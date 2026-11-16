// Opus voice pipeline: SDL2 capture/playback, per-peer decoders, jitter
// buffers, PCM mixing, VAD/PTT and adaptive in-band FEC.
#pragma once

#include <SDL2/SDL.h>
#include <opus/opus.h>

#include <array>
#include <atomic>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.h"

namespace p2p {

struct VoiceStats {
    uint64_t tx_packets = 0;
    uint64_t rx_packets = 0;
    uint64_t session_tx = 0;
    // 20 ms frames the listener never heard, each counted exactly once.  This
    // is the audio-impairing figure: a frame that is dropped by the buffer and
    // a frame that is lost on the network both end up here, but never twice.
    uint32_t lost = 0;
    // Sequence gaps observed on arrival.  A network-layer estimate; it also
    // counts a packet that merely arrived out of order as missing.
    uint32_t rx_gap_lost = 0;
    // Frames discarded because the reorder buffer hit its depth cap.
    uint32_t jitter_overflow = 0;
    // Times playout had to skip forward over one or more missing frames.
    uint32_t playout_skips = 0;
    // Frames repaired by decoder-side FEC reconstruction of a lost packet.
    uint32_t fec_recovered = 0;
    // Frames synthesized by the Opus PLC (packet loss concealment) instead of
    // being replaced with silence.
    uint32_t plc_frames = 0;
    // Current adaptive jitter-buffer depth target, in 20 ms frames.
    uint32_t buffer_target = 0;
    uint32_t last_seq = 0;
    uint32_t peers = 0;
    uint32_t mix_frames = 0;
    uint32_t clipping = 0;
    uint32_t vad_skipped = 0;
    double bitrate_kbps = 0.0;
    double jitter_ms = 0.0;
    double last_snr_db = 0.0;
    double mic_level_db = -120.0;  // processed microphone level (dBFS)
    double packet_loss_pct = 0.0;
    bool talking = false;
    bool vad_active = false;
    bool muted = false;
    bool deafened = false;
    bool vad_enabled = true;
    bool ptt_mode = false;
    bool fec_enabled = false;
    bool capture_available = false;
    bool playback_available = false;
    std::string capture_device;
    std::string playback_device;
    std::string driver;
    std::string topology = "1-on-1 Direct";
};

class VoiceEngine {
  public:
    using Sender = std::function<void(uint32_t seq, const std::vector<uint8_t>& opus)>;
    using EndSender = std::function<void()>;
    // Headless hook: receives the final mixed playout frames (used by the
    // self-test to verify loss concealment without a sound card).
    using MixSink = std::function<void(const int16_t*, size_t)>;

    VoiceEngine();
    ~VoiceEngine();

    VoiceEngine(const VoiceEngine&) = delete;
    VoiceEngine& operator=(const VoiceEngine&) = delete;

    bool init(bool enable_audio, std::string& err, bool capture_only = false);
    void shutdown();
    bool start_mic_test(const std::string& path, int seconds, std::string& err);
    void stop_mic_test();
    struct MicTestStats {
        uint32_t samples = 0;
        double rms_dbfs = -120.0;
        int peak = 0;
        uint64_t clipped = 0;
    };
    MicTestStats mic_test_stats() const;

    void set_sender(Sender s) { sender_ = std::move(s); }
    void set_end_sender(EndSender s) { end_sender_ = std::move(s); }
    void set_mix_sink(MixSink s) { mix_sink_ = std::move(s); }
    void set_bitrate(int bps);
    int bitrate() const { return bitrate_.load(); }

    bool start_talk();
    void stop_talk();
    bool talking() const { return talking_.load(); }

    void on_packet(const NodeId& peer, uint32_t seq, const std::vector<uint8_t>& opus);
    void on_packet(uint32_t seq, const std::vector<uint8_t>& opus);
    void on_end();
    void on_peer_end(const NodeId& peer);
    void clear_peers();

    void set_vad(bool enabled);
    void set_ptt(bool enabled);
    void set_ptt_pressed(bool pressed);
    void set_muted(bool muted);
    void set_deafened(bool deafened);
    void set_packet_loss(double pct);
    void set_topology(const std::string& topology);
    bool muted() const { return muted_.load(); }
    bool deafened() const { return deafened_.load(); }
    bool vad_enabled() const { return vad_.load(); }
    bool ptt_mode() const { return ptt_.load(); }
    int peer_count() const;

    VoiceStats stats() const;

    // --- microphone processing for old / weak / noisy devices -------------------
    // High-pass (100 Hz), adaptive noise-floor gate, AGC and a soft limiter run on
    // every captured frame before voice detection and encoding.
    void set_filter_enabled(bool on) { filter_.store(on); }
    bool filter_enabled() const { return filter_.load(); }
    void set_agc(bool on) { agc_.store(on); }
    bool agc() const { return agc_.load(); }
    void set_input_gain_db(int db) { input_gain_db_.store(std::clamp(db, -12, 30)); }
    int input_gain_db() const { return input_gain_db_.load(); }
    // Mic preview for the Audio tab: level meter always, "hear myself" optionally.
    void set_mic_preview(bool on, bool hear_myself);
    bool mic_preview() const { return preview_.load(); }
    bool hear_myself() const { return hear_.load(); }
    double mic_level_db() const { return mic_db_.load(); }   // processed level, dBFS
    double noise_floor_db() const { return noise_db_.load(); }

    // --- device selection ---------------------------------------------------------
    static std::vector<std::string> list_devices(bool capture);
    // Empty name = system default.  Re-opens the SDL devices (safe during a call).
    bool set_devices(const std::string& capture_name, const std::string& playback_name,
                     std::string& err);
    std::string capture_name() const { std::lock_guard<std::mutex> lk(dev_mtx_); return cap_name_; }
    std::string playback_name() const { std::lock_guard<std::mutex> lk(dev_mtx_); return play_name_; }
    bool loopback_test(double& snr_db, double& kbps, std::string& err);
    static NodeId local_stream_id() {
        NodeId id;
        id.b[kIdBytes - 1] = 1;
        return id;
    }

  private:
    struct PeerVoiceStream {
        NodeId id;
        OpusDecoder* dec = nullptr;
        // Raw Opus packets keyed by sequence number; decoded lazily at playout
        // so a lost frame can be reconstructed with decoder-side FEC.
        std::map<uint32_t, std::vector<uint8_t>> jitter;
        uint32_t next_seq = 0;
        bool have_next = false;
        uint32_t last_seq = 0;
        uint64_t last_arrival_ms = 0;
        uint64_t last_play_ms = 0;
        uint64_t lost = 0;      // missing frames, counted once
        uint64_t overflow = 0;  // frames dropped by the depth cap
        uint64_t skips = 0;     // playout jumps
        uint64_t packets = 0;
        uint64_t fec_rec = 0;   // frames recovered by decoder FEC
        uint64_t plc = 0;       // frames synthesized by PLC
        uint32_t buffer_target = 8;  // adaptive depth target (2..12 frames, 60 ms floor)
        bool needs_prime = true;  // wait for target depth (stream start / after skip)
        uint64_t last_loss_ms = 0;
        uint64_t last_decay_ms = 0;
        double jitter_ms = 0.0;
        bool vorbi = false;
    };

    bool open_devices(bool capture_only, std::string& err);
    void close_devices();
    // Returns true when the frame carries speech (adaptive gate).
    bool process_input(int16_t* f, size_t n);

    void worker_loop();
    void mix_loop();
    void encode_and_send(const int16_t* pcm, size_t samples);
    void queue_output(const int16_t* pcm, size_t samples);
    void generate_sim_frame(std::array<int16_t, 960>& out);
    void update_fec_locked();
    // Hide a missing frame: decoder-side FEC from the next packet when
    // available, otherwise Opus PLC. Returns the repair method used.
    enum class LossRepair { None, Fec, Plc };
    LossRepair conceal_loss(PeerVoiceStream& s, const std::vector<uint8_t>* next_pkt,
                            std::vector<int16_t>& pcm);
    void record_pcm(const int16_t* pcm, size_t samples);

    static void SDLCALL capture_callback(void* userdata, Uint8* stream, int len);
    void on_capture(const Uint8* stream, int len);

    OpusEncoder* enc_ = nullptr;
    std::atomic<int> bitrate_{32000};
    std::atomic<double> packet_loss_{0.0};
    mutable std::mutex codec_mtx_;

    std::atomic<SDL_AudioDeviceID> capture_dev_{0};
    std::atomic<SDL_AudioDeviceID> play_dev_{0};
    bool audio_requested_ = false;

    std::atomic<bool> running_{false};
    std::atomic<bool> talking_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> deafened_{false};
    std::atomic<bool> vad_{true};
    std::atomic<bool> ptt_{false};
    std::atomic<bool> ptt_pressed_{false};
    std::atomic<int> vad_threshold_{450};
    std::atomic<bool> filter_{true};
    std::atomic<bool> agc_{true};
    std::atomic<int> input_gain_db_{0};
    std::atomic<bool> preview_{false};
    std::atomic<bool> hear_{false};
    std::atomic<double> mic_db_{-120.0};
    std::atomic<double> noise_db_{-120.0};
    // filter state (worker thread only)
    double hp_x_ = 0.0, hp_y_ = 0.0, agc_gain_ = 1.0, noise_est_ = 150.0;
    int gate_hang_ = 0;
    mutable std::mutex dev_mtx_;
    std::string cap_name_, play_name_;
    bool capture_only_ = false;
    mutable std::mutex mon_mtx_;
    std::deque<int16_t> mon_ring_;
    std::atomic<uint32_t> tx_seq_{0};
    std::thread worker_;
    std::thread mixer_;

    mutable std::mutex cap_mtx_;
    std::condition_variable cap_cv_;
    std::vector<int16_t> cap_ring_;
    double sim_phase_ = 0.0;
    std::atomic<bool> last_vad_active_{false};
    std::atomic<bool> record_capture_{false};
    mutable std::mutex record_mtx_;
    std::ofstream record_out_;
    uint32_t record_samples_ = 0;
    uint32_t record_limit_ = 0;
    double record_sum_sq_ = 0.0;
    int record_peak_ = 0;
    uint64_t record_clipped_ = 0;

    mutable std::mutex streams_mtx_;
    std::map<NodeId, PeerVoiceStream> streams_;

    mutable std::mutex stats_mtx_;
    VoiceStats stats_;
    std::deque<std::pair<uint64_t, uint32_t>> rx_window_;
    uint64_t last_arrival_ms_ = 0;
    bool rx_have_last_ = false;
    uint32_t rx_last_seq_ = 0;
    double rx_jitter_ = 0.0;
    uint64_t rx_lost_ = 0;
    uint64_t rx_packets_ = 0;
    std::string topology_ = "1-on-1 Direct";

    Sender sender_;
    EndSender end_sender_;
    MixSink mix_sink_;
};

}  // namespace p2p
