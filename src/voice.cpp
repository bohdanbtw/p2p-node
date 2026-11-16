#include "voice.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

#include "common.h"

namespace p2p {

namespace {
constexpr int kRate = 48000;
constexpr int kFrame = 960;  // 20 ms
constexpr int kChannels = 1;
// Reorder depth cap.  12 frames = 240 ms, enough to cover a WAN path whose
// jitter reaches six figures of milliseconds without unbounded growth.
constexpr size_t kMaxJitterFrames = 12;
constexpr double kPi = 3.14159265358979323846;

double rms_of(const int16_t* pcm, size_t samples) {
    if (!samples) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < samples; ++i) {
        double v = pcm[i];
        sum += v * v;
    }
    return std::sqrt(sum / static_cast<double>(samples));
}

void wav_u16(std::ofstream& out, uint16_t v) {
    char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
    out.write(b, 2);
}
void wav_u32(std::ofstream& out, uint32_t v) {
    char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8),
                 static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
    out.write(b, 4);
}
bool wav_open(std::ofstream& out, const std::string& path, uint32_t samples) {
    out.open(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write("RIFF", 4);
    wav_u32(out, 36 + samples * 2);
    out.write("WAVEfmt ", 8);
    wav_u32(out, 16);
    wav_u16(out, 1);
    wav_u16(out, 1);
    wav_u32(out, static_cast<uint32_t>(kRate));
    wav_u32(out, static_cast<uint32_t>(kRate * 2));
    wav_u16(out, 2);
    wav_u16(out, 16);
    out.write("data", 4);
    wav_u32(out, samples * 2);
    return static_cast<bool>(out);
}

}  // namespace

VoiceEngine::VoiceEngine() = default;

VoiceEngine::~VoiceEngine() { shutdown(); }

bool VoiceEngine::init(bool enable_audio, std::string& err, bool capture_only) {
    shutdown();

    int e = OPUS_OK;
    enc_ = opus_encoder_create(kRate, kChannels, OPUS_APPLICATION_VOIP, &e);
    if (!enc_ || e != OPUS_OK) {
        err = "Opus: failed to create encoder";
        return false;
    }
    opus_encoder_ctl(enc_, OPUS_SET_BITRATE(bitrate_.load()));
    opus_encoder_ctl(enc_, OPUS_SET_VBR(1));
    opus_encoder_ctl(enc_, OPUS_SET_COMPLEXITY(8));
    // In-band FEC: redundancy for the *previous* frame in every packet, so a
    // single lost packet is repaired at the receiver (conceal_loss already
    // reads it).  15 % is the expected-loss assumption we tune redundancy for.
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(15));

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_ = VoiceStats{};
        rx_window_.clear();
        rx_jitter_ = 0;
        rx_lost_ = 0;
        rx_packets_ = 0;
        rx_have_last_ = false;
        rx_last_seq_ = 0;
        last_arrival_ms_ = 0;
    }
    {
        std::lock_guard<std::mutex> lk(streams_mtx_);
        streams_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(cap_mtx_);
        cap_ring_.clear();
    }

    audio_requested_ = enable_audio;
    capture_only_ = capture_only;
    if (enable_audio) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            err = std::string("SDL audio: ") + SDL_GetError();
        } else {
            const char* drv = SDL_GetCurrentAudioDriver();
            {
                std::lock_guard<std::mutex> lk(stats_mtx_);
                stats_.driver = drv ? drv : "?";
            }
            std::string derr;
            open_devices(capture_only, derr);
        }
    }

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        if (!stats_.capture_available) stats_.capture_device = "440 Hz emulation";
        if (!stats_.playback_available) stats_.playback_device = "no device";
        if (stats_.driver.empty()) stats_.driver = "none";
    }

    running_.store(true);
    worker_ = std::thread(&VoiceEngine::worker_loop, this);
    mixer_ = std::thread(&VoiceEngine::mix_loop, this);
    return true;
}

bool VoiceEngine::open_devices(bool capture_only, std::string& err) {
    std::string cname, pname;
    {
        std::lock_guard<std::mutex> lk(dev_mtx_);
        cname = cap_name_;
        pname = play_name_;
    }
    SDL_AudioSpec want{};
    SDL_AudioSpec have{};
    want.freq = kRate;
    want.format = AUDIO_S16SYS;
    want.channels = kChannels;
    want.samples = kFrame;
    want.callback = &VoiceEngine::capture_callback;
    want.userdata = this;
    SDL_AudioDeviceID cap = SDL_OpenAudioDevice(cname.empty() ? nullptr : cname.c_str(), 1, &want,
                                                &have, 0);
    if (cap == 0 && !cname.empty()) {
        err = "capture device '" + cname + "' unavailable, using default";
        cap = SDL_OpenAudioDevice(nullptr, 1, &want, &have, 0);
    }
    capture_dev_ = cap;
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.capture_available = cap != 0;
        if (cap != 0)
            stats_.capture_device = (cname.empty() ? std::string("default microphone")
                                                   : cname) + " (" + std::to_string(have.freq) + " Hz)";
    }
    if (cap != 0) SDL_PauseAudioDevice(cap, (talking_.load() || preview_.load()) ? 0 : 1);

    if (!capture_only) {
        SDL_AudioSpec pwant{};
        SDL_AudioSpec phave{};
        pwant.freq = kRate;
        pwant.format = AUDIO_S16SYS;
        pwant.channels = kChannels;
        pwant.samples = kFrame;
        pwant.callback = nullptr;
        SDL_AudioDeviceID pl = SDL_OpenAudioDevice(pname.empty() ? nullptr : pname.c_str(), 0,
                                                   &pwant, &phave, 0);
        if (pl == 0 && !pname.empty()) {
            err += (err.empty() ? "" : "; ") + std::string("playback device '") + pname +
                   "' unavailable, using default";
            pl = SDL_OpenAudioDevice(nullptr, 0, &pwant, &phave, 0);
        }
        play_dev_ = pl;
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.playback_available = pl != 0;
        if (pl != 0) {
            SDL_PauseAudioDevice(pl, 0);
            stats_.playback_device = (pname.empty() ? std::string("default speakers") : pname) +
                                     " (" + std::to_string(phave.freq) + " Hz)";
        }
    }
    return capture_dev_ != 0 || play_dev_ != 0;
}

void VoiceEngine::close_devices() {
    if (capture_dev_ != 0) {
        SDL_CloseAudioDevice(capture_dev_);
        capture_dev_ = 0;
    }
    if (play_dev_ != 0) {
        SDL_CloseAudioDevice(play_dev_);
        play_dev_ = 0;
    }
}

std::vector<std::string> VoiceEngine::list_devices(bool capture) {
    std::vector<std::string> out;
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return out;
    int n = SDL_GetNumAudioDevices(capture ? 1 : 0);
    for (int i = 0; i < n; ++i) {
        const char* nm = SDL_GetAudioDeviceName(i, capture ? 1 : 0);
        if (nm && *nm) out.push_back(nm);
    }
    return out;
}

bool VoiceEngine::set_devices(const std::string& capture_name, const std::string& playback_name,
                              std::string& err) {
    if (!audio_requested_) {
        err = "audio is disabled";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(dev_mtx_);
        cap_name_ = capture_name;
        play_name_ = playback_name;
    }
    if (capture_dev_ != 0) SDL_PauseAudioDevice(capture_dev_, 1);
    {
        std::lock_guard<std::mutex> lk(cap_mtx_);
        cap_ring_.clear();
    }
    close_devices();
    bool ok = open_devices(capture_only_, err);
    cap_cv_.notify_all();
    return ok;
}

void VoiceEngine::set_mic_preview(bool on, bool hear_myself) {
    hear_.store(on && hear_myself);
    preview_.store(on);
    if (capture_dev_ != 0) SDL_PauseAudioDevice(capture_dev_, (on || talking_.load()) ? 0 : 1);
    if (!on) {
        std::lock_guard<std::mutex> lk(mon_mtx_);
        mon_ring_.clear();
        mic_db_.store(-120.0);
    }
    cap_cv_.notify_all();
}

// High-pass + adaptive gate + AGC + limiter.  Returns true when the frame is speech.
bool VoiceEngine::process_input(int16_t* f, size_t n) {
    const bool filt = filter_.load();
    if (filt) {
        const double a = 0.98700;  // one-pole high-pass, ~100 Hz at 48 kHz (hum, rumble, DC)
        for (size_t i = 0; i < n; ++i) {
            double x = f[i];
            double y = a * (hp_y_ + x - hp_x_);
            hp_x_ = x;
            hp_y_ = y;
            f[i] = static_cast<int16_t>(std::clamp(y, -32768.0, 32767.0));
        }
    }
    double pre = rms_of(f, n);
    bool active;
    if (filt) {
        // Noise floor follows the quietest recent level and creeps upward slowly, so
        // the gate adapts to a hissy old microphone instead of using a fixed level.
        if (pre < noise_est_) noise_est_ = 0.9 * noise_est_ + 0.1 * pre;
        else noise_est_ *= 1.0008;
        noise_est_ = std::clamp(noise_est_, 15.0, 2500.0);
        bool speech = pre > std::max(noise_est_ * 2.8, 80.0);
        if (speech) gate_hang_ = 12;  // 240 ms hangover keeps word endings
        else if (gate_hang_ > 0) --gate_hang_;
        active = speech || gate_hang_ > 0;
        if (agc_.load()) {
            const double target = 2800.0;  // about -21 dBFS
            double desired = agc_gain_;
            if (speech) desired = std::clamp(target / std::max(pre, 1.0), 0.5, 12.0);
            agc_gain_ += (desired - agc_gain_) * (desired < agc_gain_ ? 0.30 : 0.04);
        } else {
            agc_gain_ = 1.0;
        }
    } else {
        active = pre >= vad_threshold_.load();
        agc_gain_ = 1.0;
    }
    double g = agc_gain_ * std::pow(10.0, input_gain_db_.load() / 20.0);
    if (g != 1.0 || filt) {
        for (size_t i = 0; i < n; ++i) {
            double v = f[i] * g;
            if (v > 20000.0 || v < -20000.0)  // soft limiter instead of hard clipping
                v = 32767.0 * std::tanh(v / 32767.0);
            f[i] = static_cast<int16_t>(std::clamp(v, -32768.0, 32767.0));
        }
    }
    double post = rms_of(f, n);
    double db = post > 1.0 ? 20.0 * std::log10(post / 32768.0) : -120.0;
    mic_db_.store(0.7 * mic_db_.load() + 0.3 * db);
    noise_db_.store(noise_est_ > 1.0 ? 20.0 * std::log10(noise_est_ / 32768.0) : -120.0);
    return active;
}

void VoiceEngine::shutdown() {
    talking_.store(false);
    running_.store(false);
    cap_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (mixer_.joinable()) mixer_.join();

    record_capture_.store(false);
    {
        std::lock_guard<std::mutex> lk(record_mtx_);
        if (record_out_.is_open()) {
            record_out_.flush();
            record_out_.close();
        }
    }

    close_devices();
    preview_.store(false);
    hear_.store(false);
    if (audio_requested_ && SDL_WasInit(SDL_INIT_AUDIO)) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }

    audio_requested_ = false;

    {
        std::lock_guard<std::mutex> lk(codec_mtx_);
        if (enc_) {
            opus_encoder_destroy(enc_);
            enc_ = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lk(streams_mtx_);
        for (auto& kv : streams_) {
            if (kv.second.dec) opus_decoder_destroy(kv.second.dec);
        }
        streams_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(cap_mtx_);
        cap_ring_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.talking = false;
        stats_.capture_available = false;
        stats_.playback_available = false;
    }
}

void VoiceEngine::set_bitrate(int bps) {
    int v = std::clamp(bps, 6000, 128000);
    bitrate_.store(v);
    std::lock_guard<std::mutex> lk(codec_mtx_);
    if (enc_) opus_encoder_ctl(enc_, OPUS_SET_BITRATE(v));
}

bool VoiceEngine::start_talk() {
    if (talking_.load()) return true;
    tx_seq_.store(0);
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.session_tx = 0;
        stats_.talking = true;
    }
    {
        std::lock_guard<std::mutex> lk(codec_mtx_);
        if (enc_) opus_encoder_ctl(enc_, OPUS_RESET_STATE);
    }
    {
        std::lock_guard<std::mutex> lk(cap_mtx_);
        cap_ring_.clear();
    }
    talking_.store(true);
    if (capture_dev_ != 0) SDL_PauseAudioDevice(capture_dev_, 0);
    cap_cv_.notify_all();
    return true;
}

void VoiceEngine::stop_talk() {
    if (!talking_.exchange(false)) return;
    if (capture_dev_ != 0) SDL_PauseAudioDevice(capture_dev_, preview_.load() ? 0 : 1);  // keep a running mic check alive
    if (end_sender_) end_sender_();
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        stats_.talking = false;
    }
    cap_cv_.notify_all();
}

bool VoiceEngine::start_mic_test(const std::string& path, int seconds, std::string& err) {
    if (capture_dev_ == 0) {
        err = "no microphone capture device";
        return false;
    }
    if (seconds <= 0 || seconds > 600) {
        err = "recording duration must be 1..600 seconds";
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(record_mtx_);
        if (record_out_.is_open()) record_out_.close();
        record_limit_ = static_cast<uint32_t>(kRate) * static_cast<uint32_t>(seconds);
        record_samples_ = 0;
        record_sum_sq_ = 0.0;
        record_peak_ = 0;
        record_clipped_ = 0;
        if (!wav_open(record_out_, path, record_limit_)) {
            err = "cannot create WAV file";
            return false;
        }
    }
    record_capture_.store(true);
    if (!start_talk()) {
        record_capture_.store(false);
        std::lock_guard<std::mutex> lk(record_mtx_);
        record_out_.close();
        err = "cannot start capture";
        return false;
    }
    return true;
}

void VoiceEngine::stop_mic_test() {
    record_capture_.store(false);
    stop_talk();
    std::lock_guard<std::mutex> lk(record_mtx_);
    if (record_out_.is_open()) {
        // Keep the RIFF size deterministic even if the caller stops a little early.
        std::vector<int16_t> zeros(960, 0);
        while (record_samples_ < record_limit_) {
            size_t n = std::min<size_t>(zeros.size(), record_limit_ - record_samples_);
            record_out_.write(reinterpret_cast<const char*>(zeros.data()),
                              static_cast<std::streamsize>(n * sizeof(int16_t)));
            record_samples_ += static_cast<uint32_t>(n);
        }
        record_out_.flush();
        record_out_.close();
    }
}

void VoiceEngine::record_pcm(const int16_t* pcm, size_t samples) {
    if (!record_capture_.load() || !pcm || !samples) return;
    std::lock_guard<std::mutex> lk(record_mtx_);
    if (!record_out_.is_open() || record_samples_ >= record_limit_) return;
    size_t n = std::min<size_t>(samples, record_limit_ - record_samples_);
    record_out_.write(reinterpret_cast<const char*>(pcm),
                      static_cast<std::streamsize>(n * sizeof(int16_t)));
    for (size_t i = 0; i < n; ++i) {
        int v = pcm[i];
        int a = v < 0 ? -v : v;
        if (a > record_peak_) record_peak_ = a;
        record_sum_sq_ += static_cast<double>(v) * static_cast<double>(v);
        if (v >= 32767 || v <= -32768) ++record_clipped_;
    }
    record_samples_ += static_cast<uint32_t>(n);
    if (record_samples_ >= record_limit_) record_capture_.store(false);
}

VoiceEngine::MicTestStats VoiceEngine::mic_test_stats() const {
    std::lock_guard<std::mutex> lk(record_mtx_);
    MicTestStats out;
    out.samples = record_samples_;
    out.peak = record_peak_;
    out.clipped = record_clipped_;
    if (record_samples_ > 0 && record_sum_sq_ > 0.0) {
        double rms = std::sqrt(record_sum_sq_ / static_cast<double>(record_samples_)) / 32768.0;
        out.rms_dbfs = 20.0 * std::log10(std::max(rms, 1e-6));
    }
    return out;
}

void VoiceEngine::capture_callback(void* userdata, Uint8* stream, int len) {
    auto* self = static_cast<VoiceEngine*>(userdata);
    if (!self || !(self->talking_.load() || self->preview_.load())) return;  // preview = mic check
    self->on_capture(stream, len);
}

void VoiceEngine::on_capture(const Uint8* stream, int len) {
    std::lock_guard<std::mutex> lk(cap_mtx_);
    const int16_t* pcm = reinterpret_cast<const int16_t*>(stream);
    size_t n = static_cast<size_t>(len / 2);
    cap_ring_.insert(cap_ring_.end(), pcm, pcm + n);
    const size_t cap = static_cast<size_t>(kRate) * 2;  // 2 s
    if (cap_ring_.size() > cap)
        cap_ring_.erase(cap_ring_.begin(), cap_ring_.begin() + (cap_ring_.size() - cap));
    cap_cv_.notify_one();
}

void VoiceEngine::generate_sim_frame(std::array<int16_t, 960>& out) {
    for (size_t i = 0; i < out.size(); ++i) {
        out[i] = static_cast<int16_t>(6000.0 * std::sin(sim_phase_));
        sim_phase_ += 2.0 * kPi * 440.0 / kRate;
        if (sim_phase_ > 2.0 * kPi) sim_phase_ -= 2.0 * kPi;
    }
}

void VoiceEngine::worker_loop() {
    while (running_.load()) {
        if (capture_dev_ == 0) {
            // No microphone (yet): synthetic 440 Hz source.  Re-checked on every iteration,
            // so a device chosen later in the Audio tab takes over without a restart.
            if (talking_.load()) {
                std::array<int16_t, 960> frame{};
                generate_sim_frame(frame);
                record_pcm(frame.data(), frame.size());
                double level = rms_of(frame.data(), frame.size());
                bool active = level >= vad_threshold_.load();
                last_vad_active_.store(active);
                bool blocked = muted_.load() || (ptt_.load() && !ptt_pressed_.load()) ||
                               (vad_.load() && !active);
                if (blocked) {
                    std::lock_guard<std::mutex> lk(stats_mtx_);
                    ++stats_.vad_skipped;
                } else {
                    encode_and_send(frame.data(), frame.size());
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        std::unique_lock<std::mutex> lk(cap_mtx_);
        cap_cv_.wait_for(lk, std::chrono::milliseconds(50), [&] {
            return !running_.load() || (!cap_ring_.empty() && cap_ring_.size() >= kFrame);
        });
        if (!running_.load()) break;
        if (!talking_.load() && !preview_.load()) {
            cap_ring_.clear();
            continue;
        }
        if (cap_ring_.size() < static_cast<size_t>(kFrame)) continue;

        std::array<int16_t, 960> frame{};
        std::copy(cap_ring_.begin(), cap_ring_.begin() + kFrame, frame.begin());
        cap_ring_.erase(cap_ring_.begin(), cap_ring_.begin() + kFrame);
        size_t ring_after = cap_ring_.size();
        lk.unlock();

        bool active = process_input(frame.data(), frame.size());
        if (hear_.load()) {  // "hear myself": feed the processed mic into the mixer
            std::lock_guard<std::mutex> ml(mon_mtx_);
            mon_ring_.insert(mon_ring_.end(), frame.begin(), frame.end());
            while (mon_ring_.size() > static_cast<size_t>(kFrame) * 6)
                mon_ring_.erase(mon_ring_.begin(), mon_ring_.begin() + kFrame);
        }
        if (!talking_.load()) continue;  // preview only: nothing is sent or recorded
        record_pcm(frame.data(), frame.size());
        last_vad_active_.store(active);
        bool blocked = muted_.load() || (ptt_.load() && !ptt_pressed_.load()) ||
                       (vad_.load() && !active);
        if (blocked) {
            std::lock_guard<std::mutex> st(stats_mtx_);
            ++stats_.vad_skipped;
            continue;
        }
        encode_and_send(frame.data(), frame.size());
        // Pace at one frame per ~16 ms: SDL capture callbacks on Windows
        // (winmm backend) arrive in bursts, so without pacing the packets
        // reach the peer faster than the 20 ms mixer drains them, the small
        // adaptive jitter buffer overflows and whole frames are dropped —
        // audible as static.  Only throttle while the ring still holds a
        // full frame; when it runs dry the condition-variable wait resumes.
        if (ring_after >= static_cast<size_t>(kFrame))
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

void VoiceEngine::encode_and_send(const int16_t* pcm, size_t samples) {
    if (!enc_ || !sender_) return;
    std::array<uint8_t, 1500> packet{};
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(codec_mtx_);
        n = opus_encode(enc_, pcm, static_cast<int>(samples), packet.data(),
                        static_cast<opus_int32>(packet.size()));
    }
    if (n <= 0) return;

    uint32_t seq = tx_seq_.fetch_add(1);
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        ++stats_.tx_packets;
        ++stats_.session_tx;
    }
    sender_(seq, std::vector<uint8_t>(packet.begin(), packet.begin() + n));
}

void VoiceEngine::queue_output(const int16_t* pcm, size_t samples) {
    if (play_dev_ == 0) {
        // Headless: hand the frame to the test sink, if any.
        if (mix_sink_ && samples > 0) mix_sink_(pcm, samples);
        return;
    }
    if (deafened_.load() || samples == 0) return;
    const Uint32 bytes = static_cast<Uint32>(samples * sizeof(int16_t));
    const Uint32 queued = SDL_GetQueuedAudioSize(play_dev_);
    // Only drop the backlog on a hard overrun (~400 ms).  The SDL queue on
    // Windows (winmm) already carries ~90 ms of device latency, so clearing at
    // ~100 ms chopped live audio every few callback periods — audible as
    // static/clicking.  Hard-clear only when we are truly far behind.
    if (queued > static_cast<Uint32>(kRate * sizeof(int16_t) * 2 / 5))
        SDL_ClearQueuedAudio(play_dev_);
    SDL_QueueAudio(play_dev_, pcm, bytes);

}

void VoiceEngine::mix_loop() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    std::vector<int16_t> mixed(static_cast<size_t>(kFrame));
    std::vector<int16_t> scratch;  // playout decode scratch, mix_loop thread only

    while (running_.load()) {
        next += std::chrono::milliseconds(20);
        std::this_thread::sleep_until(next);
        if (!running_.load()) break;

        std::array<int32_t, kFrame> acc{};
        bool any = false;
        size_t active_peers = 0;
        uint64_t lost_total = 0;
        double jitter_max = 0.0;
        uint32_t target_max = 0;

        {
            std::lock_guard<std::mutex> lk(streams_mtx_);
            uint64_t now = now_ms();
            for (auto it = streams_.begin(); it != streams_.end();) {
                PeerVoiceStream& s = it->second;
                if (s.jitter.empty()) {
                    if (s.last_arrival_ms != 0 && now - s.last_arrival_ms > 4000) {
                        if (s.dec) opus_decoder_destroy(s.dec);
                        it = streams_.erase(it);
                        continue;
                    }
                    ++it;
                    continue;
                }
                if (!s.have_next) {
                    s.next_seq = s.jitter.begin()->first;
                    s.have_next = true;
                }

                // Adaptive depth: after a stable stretch of 10 s the target
                // decays back toward the 4-frame floor (80 ms), at most once a
                // second.  A 40 ms floor proved too tight on real clients:
                // any hiccup in the mixer thread or NIC driver caused gaps.
                if (s.buffer_target > 4 && now - s.last_decay_ms >= 1000 &&
                    (s.last_loss_ms == 0 || now - s.last_loss_ms >= 10000)) {
                    --s.buffer_target;
                    s.last_decay_ms = now;
                }
                target_max = std::max(target_max, s.buffer_target);

                // Prefill: wait for the adaptive target depth on stream start
                // or after a skip, so the buffer can absorb reordering before
                // the first playout.
                if (s.needs_prime && s.jitter.size() < s.buffer_target) {
                    ++it;
                    continue;
                }
                s.needs_prime = false;

                auto frame = s.jitter.find(s.next_seq);
                if (frame == s.jitter.end()) {
                    // The next frame to play is missing.  If we're still in
                    // prefill, wait.  Otherwise a newer frame is already in the
                    // buffer, so the gap is real — conceal immediately (FEC
                    // from the next packet, or PLC for longer gaps).
                    auto nxt = s.jitter.lower_bound(s.next_seq);
                    if (nxt == s.jitter.end() || s.needs_prime) {
                        ++it;
                        continue;
                    }
                    const uint32_t gap = nxt->first - s.next_seq;
                    bool concealed = false;
                    // Conceal at most the last two missing frames.  The gap comes from a sequence
                    // number inside the (authenticated) packet, so a peer can make it ~4 billion;
                    // looping over all of it froze the mixer, the voice stats lock and the node.
                    const uint32_t conceal_from = nxt->first - std::min<uint32_t>(gap, 2);
                    for (uint32_t q = conceal_from; q < nxt->first; ++q) {
                        scratch.assign(kFrame, 0);
                        const std::vector<uint8_t>* repair_src =
                            (q + 1 == nxt->first) ? &nxt->second : nullptr;
                        LossRepair r = conceal_loss(s, repair_src, scratch);
                        if (r == LossRepair::Fec) ++s.fec_rec;
                        if (r == LossRepair::Plc) ++s.plc;
                        if (r == LossRepair::None) continue;
                        for (size_t i = 0; i < scratch.size() && i < acc.size(); ++i)
                            acc[i] += scratch[i];
                        concealed = true;
                    }
                    if (concealed) {
                        any = true;
                        ++active_peers;
                    }
                    s.lost += gap;
                    ++s.skips;
                    s.last_loss_ms = now;
                    s.buffer_target =
                        std::min<uint32_t>(kMaxJitterFrames, s.buffer_target + 2);
                    s.next_seq = nxt->first;
                    frame = s.jitter.find(s.next_seq);
                }
                if (frame != s.jitter.end()) {
                    scratch.assign(kFrame, 0);
                    int n = opus_decode(s.dec, frame->second.data(),
                                        static_cast<opus_int32>(frame->second.size()),
                                        scratch.data(), kFrame, 0);
                    s.jitter.erase(frame);
                    if (n > 0) {
                        size_t cnt = std::min(static_cast<size_t>(n), acc.size());
                        for (size_t i = 0; i < cnt; ++i) acc[i] += scratch[i];
                        any = true;
                        ++active_peers;
                        s.last_play_ms = now;
                    } else {
                        ++s.lost;
                    }
                    ++s.next_seq;
                }
                lost_total += s.lost;
                jitter_max = std::max(jitter_max, s.jitter_ms);
                ++it;
            }
        }

        if (hear_.load()) {
            std::lock_guard<std::mutex> ml(mon_mtx_);
            if (mon_ring_.size() >= static_cast<size_t>(kFrame)) {
                for (size_t i = 0; i < static_cast<size_t>(kFrame); ++i) acc[i] += mon_ring_[i];
                mon_ring_.erase(mon_ring_.begin(), mon_ring_.begin() + kFrame);
                any = true;
            }
        }
        if (any) {
            uint32_t clipping = 0;
            for (size_t i = 0; i < mixed.size(); ++i) {
                int32_t v = acc[i];
                if (v > 32767) {
                    v = 32767;
                    ++clipping;
                } else if (v < -32768) {
                    v = -32768;
                    ++clipping;
                }
                mixed[i] = static_cast<int16_t>(v);
            }
            queue_output(mixed.data(), mixed.size());
            std::lock_guard<std::mutex> lk(stats_mtx_);
            ++stats_.mix_frames;
            stats_.clipping += clipping;
        } else {
            // Keep the playback queue draining even during silence; do not
            // generate audible comfort noise.
            if (play_dev_ != 0 && deafened_.load()) SDL_ClearQueuedAudio(play_dev_);
        }

        {
            std::lock_guard<std::mutex> lk(streams_mtx_);
            std::lock_guard<std::mutex> st(stats_mtx_);
            stats_.peers = static_cast<uint32_t>(active_peers);
            stats_.jitter_ms = jitter_max;
            // Holes heard by the listener only; network gaps are reported
            // separately as rx_gap_lost by stats().
            stats_.lost = static_cast<uint32_t>(lost_total);
            stats_.buffer_target = target_max;
        }
    }
}

void VoiceEngine::on_packet(const NodeId& peer, uint32_t seq,
                            const std::vector<uint8_t>& opus) {
    if (opus.empty()) {
        on_peer_end(peer);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(streams_mtx_);
        auto it = streams_.find(peer);
        if (it == streams_.end()) {
            int e = OPUS_OK;
            OpusDecoder* dec = opus_decoder_create(kRate, kChannels, &e);
            if (!dec || e != OPUS_OK) {
                if (dec) opus_decoder_destroy(dec);
                return;
            }
            PeerVoiceStream s;
            s.id = peer;
            s.dec = dec;
            s.needs_prime = true;
            it = streams_.emplace(peer, s).first;
        }
        PeerVoiceStream& s = it->second;

        uint64_t now = now_ms();
        if (s.have_next && seq < s.next_seq) return;  // already played or duplicate
        s.jitter[seq] = opus;
        // The eviction ceiling is the adaptive target: it grows by 2 frames on
        // every overrun toward the hard cap of 12 and decays back to 2 in the
        // mixer once the stream is stable again.
        if (s.jitter.size() > s.buffer_target) {
            s.last_loss_ms = now;
            s.buffer_target = std::min<uint32_t>(kMaxJitterFrames, s.buffer_target + 2);
        }
        while (s.jitter.size() > s.buffer_target) {
            // Overrun: the buffer holds more audio than the cap allows, so the
            // oldest frame has to go.  We do NOT advance next_seq here — the
            // mixer is the single source of truth for playout position.  It
            // will skip over the evicted frame when it reaches it (using
            // FEC/PLC for the gap).
            const uint32_t dropped = s.jitter.begin()->first;
            s.jitter.erase(s.jitter.begin());
            ++s.overflow;
        }
        if (s.last_arrival_ms != 0) {
            double d = static_cast<double>(now - s.last_arrival_ms) - 20.0;
            s.jitter_ms += (std::abs(d) - s.jitter_ms) / 16.0;
        }
        s.last_arrival_ms = now;
        s.last_seq = seq;
        ++s.packets;
    }

    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        uint64_t now = now_ms();
        if (seq == 0) {
            rx_have_last_ = true;
            rx_last_seq_ = 0;
            rx_lost_ = 0;
        } else if (!rx_have_last_) {
            rx_have_last_ = true;
            rx_last_seq_ = seq;
        } else if (seq <= rx_last_seq_) {
            return;
        } else {
            if (seq > rx_last_seq_ + 1) rx_lost_ += seq - rx_last_seq_ - 1;
            rx_last_seq_ = seq;
        }
        ++rx_packets_;
        if (last_arrival_ms_ != 0) {
            double d = static_cast<double>(now - last_arrival_ms_) - 20.0;
            rx_jitter_ += (std::abs(d) - rx_jitter_) / 16.0;
        }
        last_arrival_ms_ = now;
        rx_window_.emplace_back(now, static_cast<uint32_t>(opus.size()));
        while (!rx_window_.empty() && now - rx_window_.front().first > 3000)
            rx_window_.pop_front();
    }
}

void VoiceEngine::on_packet(uint32_t seq, const std::vector<uint8_t>& opus) {
    on_packet(local_stream_id(), seq, opus);
}

void VoiceEngine::on_peer_end(const NodeId& peer) {
    std::lock_guard<std::mutex> lk(streams_mtx_);
    auto it = streams_.find(peer);
    if (it == streams_.end()) return;
    if (it->second.dec) opus_decoder_destroy(it->second.dec);
    streams_.erase(it);
}

void VoiceEngine::on_end() {
    clear_peers();
    std::lock_guard<std::mutex> lk(stats_mtx_);
    rx_have_last_ = false;
    rx_last_seq_ = 0;
    last_arrival_ms_ = 0;
}

void VoiceEngine::clear_peers() {
    std::lock_guard<std::mutex> lk(streams_mtx_);
    for (auto& kv : streams_)
        if (kv.second.dec) opus_decoder_destroy(kv.second.dec);
    streams_.clear();
}

void VoiceEngine::set_vad(bool enabled) {
    vad_.store(enabled);
    if (enabled) {
        ptt_.store(false);
        ptt_pressed_.store(false);
    }
}

void VoiceEngine::set_ptt(bool enabled) {
    ptt_.store(enabled);
    if (enabled) vad_.store(false);
}

void VoiceEngine::set_ptt_pressed(bool pressed) { ptt_pressed_.store(pressed); }

void VoiceEngine::set_muted(bool muted) { muted_.store(muted); }

void VoiceEngine::set_deafened(bool deafened) {
    deafened_.store(deafened);
    if (deafened && play_dev_ != 0) SDL_ClearQueuedAudio(play_dev_);
}

void VoiceEngine::update_fec_locked() {
    if (!enc_) return;
    double loss = packet_loss_.load();
    bool fec = loss > 2.0;
    int ctl_loss = static_cast<int>(std::min(30.0, loss * 1.5));
    opus_encoder_ctl(enc_, OPUS_SET_INBAND_FEC(fec ? 1 : 0));
    opus_encoder_ctl(enc_, OPUS_SET_PACKET_LOSS_PERC(ctl_loss));
}

void VoiceEngine::set_packet_loss(double pct) {
    packet_loss_.store(std::clamp(pct, 0.0, 30.0));
    std::lock_guard<std::mutex> lk(codec_mtx_);
    update_fec_locked();
}

// Hide one missing 20 ms frame.  When the sender had in-band FEC enabled
// (the same >2 % loss threshold that switches it on in update_fec_locked) and
// the packet following the hole is available, decoding that packet with the
// FEC flag reconstructs the lost frame.  Otherwise the decoder synthesizes a
// concealment frame from its internal history (Opus PLC).  A hard failure
// leaves silence, reported as LossRepair::None.
VoiceEngine::LossRepair VoiceEngine::conceal_loss(PeerVoiceStream& s,
                                                  const std::vector<uint8_t>* next_pkt,
                                                  std::vector<int16_t>& pcm) {
    if (!s.dec) return LossRepair::None;
    const bool fec_on = packet_loss_.load() > 2.0;
    if (fec_on && next_pkt && !next_pkt->empty()) {
        int n = opus_decode(s.dec, next_pkt->data(),
                            static_cast<opus_int32>(next_pkt->size()),
                            pcm.data(), kFrame, 1);
        if (n > 0) {
            pcm.resize(static_cast<size_t>(n));
            return LossRepair::Fec;
        }
    }
    int n = opus_decode(s.dec, nullptr, 0, pcm.data(), kFrame, 0);
    if (n > 0) {
        pcm.resize(static_cast<size_t>(n));
        return LossRepair::Plc;
    }
    return LossRepair::None;
}

void VoiceEngine::set_topology(const std::string& topology) {
    std::lock_guard<std::mutex> lk(stats_mtx_);
    topology_ = topology;
    stats_.topology = topology;
}

int VoiceEngine::peer_count() const {
    std::lock_guard<std::mutex> lk(streams_mtx_);
    int n = 0;
    for (const auto& kv : streams_)
        if (kv.second.dec) ++n;
    return n;
}

VoiceStats VoiceEngine::stats() const {
    VoiceStats s;
    uint64_t now = now_ms();
    {
        std::lock_guard<std::mutex> lk(stats_mtx_);
        s = stats_;
    }
    s.mic_level_db = mic_db_.load();
    {
        std::lock_guard<std::mutex> lk(streams_mtx_);
        // each missing frame is counted once, at playout; do not add the
        // arrival-gap estimate on top or the total exceeds the packets sent
        s.peers = 0;
        s.lost = 0;
        s.jitter_overflow = 0;
        s.playout_skips = 0;
        s.fec_recovered = 0;
        s.plc_frames = 0;
        s.rx_gap_lost = static_cast<uint32_t>(std::min<uint64_t>(rx_lost_, UINT32_MAX));
        s.jitter_ms = rx_jitter_;
        for (const auto& kv : streams_) {
            if (kv.second.dec) ++s.peers;
            s.lost += static_cast<uint32_t>(kv.second.lost);
            s.jitter_overflow += static_cast<uint32_t>(kv.second.overflow);
            s.playout_skips += static_cast<uint32_t>(kv.second.skips);
            s.fec_recovered += static_cast<uint32_t>(kv.second.fec_rec);
            s.plc_frames += static_cast<uint32_t>(kv.second.plc);
            s.buffer_target = std::max(s.buffer_target, kv.second.buffer_target);
            s.jitter_ms = std::max(s.jitter_ms, kv.second.jitter_ms);
        }
    }
    s.talking = talking_.load();
    s.muted = muted_.load();
    s.deafened = deafened_.load();
    s.vad_enabled = vad_.load();
    s.ptt_mode = ptt_.load();
    s.vad_active = last_vad_active_.load();
    s.packet_loss_pct = packet_loss_.load();
    s.fec_enabled = s.packet_loss_pct > 2.0;
    uint32_t bytes = 0;
    uint64_t oldest = now;
    {
        // topology_, rx_* and rx_window_ are written by the network thread under stats_mtx_;
        // reading them without it was a data race (std::deque / std::string UB).
        std::lock_guard<std::mutex> lk(stats_mtx_);
        s.topology = topology_;
        s.last_seq = rx_last_seq_;
        s.rx_packets = rx_packets_;
        for (const auto& v : rx_window_) {
            bytes += v.second;
            oldest = std::min(oldest, v.first);
        }
    }
    double span = static_cast<double>(now - oldest) / 1000.0;
    if (span > 0.25) s.bitrate_kbps = static_cast<double>(bytes) * 8.0 / span / 1000.0;
    return s;
}

bool VoiceEngine::loopback_test(double& snr_db, double& kbps, std::string& err) {
    if (talking_.load()) {
        err = "finish the current call first";
        return false;
    }
    std::lock_guard<std::mutex> lk(codec_mtx_);
    if (!enc_) {
        err = "Opus is not initialized";
        return false;
    }
    int e = OPUS_OK;
    OpusDecoder* dec = opus_decoder_create(kRate, kChannels, &e);
    if (!dec || e != OPUS_OK) {
        if (dec) opus_decoder_destroy(dec);
        err = "Opus decoder creation failed";
        return false;
    }

    constexpr int kSamples = kRate;  // 1 s
    constexpr int kFrames = kSamples / kFrame;
    std::vector<int16_t> original(kSamples);
    for (int i = 0; i < kSamples; ++i)
        original[i] = static_cast<int16_t>(8000.0 * std::sin(2.0 * kPi * 440.0 * i / kRate));

    std::vector<int16_t> decoded(kSamples, 0);
    std::vector<uint8_t> packet(1500);
    size_t total_bytes = 0;

    opus_encoder_ctl(enc_, OPUS_RESET_STATE);
    opus_decoder_ctl(dec, OPUS_RESET_STATE);

    for (int f = 0; f < kFrames; ++f) {
        int n = opus_encode(enc_, original.data() + f * kFrame, kFrame, packet.data(),
                            static_cast<opus_int32>(packet.size()));
        if (n <= 0) {
            opus_decoder_destroy(dec);
            err = "Opus encode error";
            return false;
        }
        total_bytes += static_cast<size_t>(n);
        int m = opus_decode(dec, packet.data(), n, decoded.data() + f * kFrame, kFrame, 0);
        if (m <= 0) {
            opus_decoder_destroy(dec);
            err = "Opus decode error";
            return false;
        }
    }

    double best_err = 1e100;
    int best_lag = 0;
    for (int lag = 0; lag <= 600; ++lag) {
        double err_sum = 0.0;
        for (int i = lag; i < kSamples; ++i) {
            double d = static_cast<double>(decoded[i]) - static_cast<double>(original[i - lag]);
            err_sum += d * d;
        }
        if (err_sum < best_err) {
            best_err = err_sum;
            best_lag = lag;
        }
    }

    double sig = 0.0;
    for (int i = best_lag; i < kSamples; ++i) {
        double v = original[i - best_lag];
        sig += v * v;
    }
    snr_db = (best_err <= 1e-9 || sig <= 1e-9)
                 ? 99.0
                 : 10.0 * std::log10(sig / best_err);
    kbps = static_cast<double>(total_bytes) * 8.0 / 1000.0;

    opus_encoder_ctl(enc_, OPUS_RESET_STATE);
    opus_decoder_ctl(dec, OPUS_RESET_STATE);
    opus_decoder_destroy(dec);

    {
        std::lock_guard<std::mutex> slk(stats_mtx_);
        stats_.last_snr_db = snr_db;
    }
    return true;
}

}  // namespace p2p
