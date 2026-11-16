#include <opus/opus.h>
#include <sodium.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "app.h"
#include "common.h"
#include "net.h"
#include "report.h"
#include "stats.h"
#include "voice.h"

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace {

void put_u16(std::ofstream& out, uint16_t v) {
    char b[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
    out.write(b, 2);
}
void put_u32(std::ofstream& out, uint32_t v) {
    char b[4] = {static_cast<char>(v), static_cast<char>(v >> 8),
                 static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
    out.write(b, 4);
}

bool write_test_wav(const std::filesystem::path& path, double seconds, double freq,
                    double amplitude) {
    const uint32_t rate = 48000;
    const uint32_t samples = static_cast<uint32_t>(rate * seconds);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write("RIFF", 4);
    put_u32(out, 36 + samples * 2);
    out.write("WAVEfmt ", 8);
    put_u32(out, 16);
    put_u16(out, 1);
    put_u16(out, 1);
    put_u32(out, rate);
    put_u32(out, rate * 2);
    put_u16(out, 2);
    put_u16(out, 16);
    out.write("data", 4);
    put_u32(out, samples * 2);
    for (uint32_t i = 0; i < samples; ++i) {
        double phase = 2.0 * 3.14159265358979323846 * freq * i / rate;
        int16_t v = static_cast<int16_t>(32767.0 * amplitude * std::sin(phase));
        put_u16(out, static_cast<uint16_t>(v));
    }
    return static_cast<bool>(out);
}

bool write_pattern(const std::filesystem::path& path, size_t bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    std::array<char, 65536> block{};
    for (size_t i = 0; i < block.size(); ++i)
        block[i] = static_cast<char>((i * 31u + 7u) & 0xffu);
    size_t left = bytes;
    while (left) {
        size_t n = std::min(left, block.size());
        out.write(block.data(), static_cast<std::streamsize>(n));
        left -= n;
    }
    return static_cast<bool>(out);
}

std::string sha256_file_hex(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    std::array<char, 65536> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize n = in.gcount();
        if (n > 0)
            crypto_hash_sha256_update(&st, reinterpret_cast<const unsigned char*>(buf.data()),
                                      static_cast<unsigned long long>(n));
    }
    std::array<unsigned char, 32> digest{};
    crypto_hash_sha256_final(&st, digest.data());
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char c : digest) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 15]);
    }
    return out;
}

// Folder of the running executable as UTF-8 (argv[0] is ANSI on Windows and loses Cyrillic).
std::string exe_dir_utf8(const char* argv0);

std::filesystem::path executable_dir(const char* argv0) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::absolute(argv0 ? argv0 : ".", ec);
    if (ec || p.empty()) return std::filesystem::current_path(ec);
    if (std::filesystem::is_directory(p, ec)) return p;
    return p.parent_path();
}

std::string exe_dir_utf8(const char* argv0) {
#ifdef _WIN32
    wchar_t buf[4096] = {0};
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n > 0 && n < sizeof(buf) / sizeof(buf[0]))
        return std::filesystem::path(buf).parent_path().u8string();
#endif
    return executable_dir(argv0).u8string();
}

void ensure_testkit(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return;

    const auto tone = dir / "tone_440_10s.wav";
    const auto chirp = dir / "chirp_5s.wav";
    const auto small = dir / "file_1mb.bin";
    const auto large = dir / "file_10mb.bin";
    if (!std::filesystem::exists(tone)) write_test_wav(tone, 10.0, 440.0, 0.12);
    if (!std::filesystem::exists(chirp)) {
        // Two quiet, pleasant tones; intentionally not an alarm/screamer.
        write_test_wav(chirp, 5.0, 523.25, 0.10);
    }
    if (!std::filesystem::exists(small)) write_pattern(small, 1024 * 1024);
    if (!std::filesystem::exists(large)) write_pattern(large, 10 * 1024 * 1024);

    const auto manifest = dir / "manifest.sha256";
    if (!std::filesystem::exists(manifest)) {
        std::ofstream m(manifest);
        if (m) {
            for (const auto& p : {tone, chirp, small, large}) {
                std::string h = sha256_file_hex(p);
                if (!h.empty()) m << h << "  " << p.filename().string() << "\n";
            }
        }
    }

    std::ofstream readme(dir / "README.txt");
    if (readme) {
        readme
            << "P2P Messenger - ready-to-use test kit\n"
            << "====================================\n\n"
            << "This folder is generated next to p2pnode.exe.  All audio files are\n"
            << "quiet sine tones; there are no screamers, random sounds or copyrighted\n"
            << "recordings.\n\n"
            << "Files:\n"
            << "  tone_440_10s.wav  - 10 s, 440 Hz, low volume\n"
            << "  chirp_5s.wav      - 5 s, 523 Hz, low volume\n"
            << "  file_1mb.bin      - deterministic 1 MiB file\n"
            << "  file_10mb.bin     - deterministic 10 MiB file\n"
            << "  manifest.sha256   - SHA-256 of the generated files\n"
            << "  selftest.ps1      - run p2pnode.exe --selftest and save output\n\n"
            << "Quick checks:\n"
            << "  1. Start p2pnode-gui.exe on both computers.\n"
            << "  2. Add the other node by Invite URI.\n"
            << "  3. Open the chat and use Attach -> file_1mb.bin.\n"
            << "  4. The receiver shows the completed file; the sender shows\n"
            << "     'all bytes received' only after the receiver's SHA-256 check.\n"
            << "  5. For microphone testing, use the microphone test in the app or run:\n"
            << "     p2pnode.exe --record-mic mic_test.wav --seconds 10\n"
            << "     The command also writes mic_test.wav.json with RMS, peak and clipping.\n"
            << "  6. After a chat/file/voice test open Inspector -> Audio and press\n"
            << "     'Save test report'; send testkit/test_report.txt with the WAV/JSON.\n"
            << "  7. For an automated self-test run testkit/selftest.ps1.\n\n"
            << "If a direct UDP path is blocked, both nodes should connect through the\n"
            << "same relay/bootstrap node; the handshake and chat then use RELAY\n"
            << "envelopes while the payload stays end-to-end encrypted.\n";
    }

    std::ofstream script(dir / "mic_test.ps1");
    if (script) {
        script << "$ErrorActionPreference = 'Stop'\n"
               << "$exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'p2pnode.exe'\n"
               << "& $exe --record-mic (Join-Path $PSScriptRoot 'mic_test.wav') --seconds 10\n"
               << "Write-Host \"Saved: $PSScriptRoot\\mic_test.wav\"\n"
               << "Write-Host \"Metrics: $PSScriptRoot\\mic_test.wav.json\"\n";
    }

    std::ofstream selftest(dir / "selftest.ps1");
    if (selftest) {
        selftest << "$ErrorActionPreference = 'Stop'\n"
                 << "$exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'p2pnode.exe'\n"
                 << "$out = Join-Path $PSScriptRoot 'selftest_report.txt'\n"
                 << "& $exe --selftest > $out 2>&1\n"
                 << "Write-Host \"Saved: $out\"\n";
    }
}

struct Args {
    AppConfig cfg;
    bool stats = false;        // --stats: write a field-test bundle next to the exe
    std::string stats_dir;     // --stats-dir DIR
    std::string cmdline;       // for environment.txt (no secrets are passed on it)
    bool selftest = false;
    bool headless = false;
    std::string export_key;
    std::string import_key;
    bool record_mic = false;
    std::string mic_path;
    int mic_seconds = 10;
    int run_seconds = 5;
    std::string peer_host;
    uint16_t peer_port = 0;
    std::string peer_id_hex;
    std::string relay_id_hex;
    std::string send_file_path;
    std::string send_chat_text;
    std::string group_create;      // headless: create a group with connected peers
    int group_members = 2;         // ...once this many peers have a session
    std::string group_send;        // headless: text to post once in a group
    int group_count = 5;           // number of group messages to post
    std::string group_file;        // --group-file PATH: send a file to every group member
    int group_interval_ms = 1000;
    std::string group_name;        // members: only post into a group with this name
    std::string report_path;
    std::string report_txt_path;
    int wait_ms = 8000;
    bool relay_only = false;
    int voice_bench = 0;
    bool auto_accept = false;
    std::string download_dir;
};

void usage() {
    std::printf(
        "P2P Node - secure C++/ImGui P2P messenger\n"
        "  --alias NAME          display name (default A)\n"
        "  --port N              local UDP port (default 47000)\n"
        "  --key FILE            identity seed file (default p2pnode.key)\n"
        "  --bootstrap HOST:PORT connect to a bootstrap peer (may be repeated)\n"
        "  --export-key FILE      export encrypted identity seed to FILE\n"
        "  --import-key FILE      import encrypted identity seed from FILE\n"
        "  --no-audio            disable SDL audio, use synthetic 440 Hz source\n"
        "  --demo                scripted scenario for screenshots\n"
        "  --contact-uri URI     add a contact and connect at startup\n"
        "  --contact-name NAME   display name for --contact-uri\n"
        "  --borderless          borderless window with custom title bar\n"
        "  --light               light theme (default); --dark for dark theme\n"
        "  --pos X,Y             window position\n"
        "  --size W,H            window size (default 1180x780)\n"
        "  --tab N               initial view (0 network, 1 chat, 2 security, 3 network)\n"
        "  --selftest            headless security/network/voice test suite\n"
        "  --headless            run a node without UI for --run-seconds\n"
        "  --run-seconds N       headless duration (default 5)\n"
        "  --record-mic FILE     capture microphone to WAV (no playback)\n"
        "  --seconds N           microphone test duration (default 10)\n"
        "  --peer HOST:PORT      headless: connect to a peer after startup\n"
        "  --peer-id HEX          headless: pin the expected 40-hex NodeID\n"
        "  --relay-id HEX         headless: use this bootstrap node as relay\n"
        "  --relay-only           headless: do not attempt a direct peer connection\n"
        "  --send-chat TEXT       headless: send one chat message after handshake\n"
        "  --group-create NAME    headless: create a group with the connected peers\n"
        "  --group-members N      headless: wait for N peer sessions before creating\n"
        "  --group-send TEXT      headless: post TEXT (numbered) after joining a group\n"
        "  --group-count K        headless: number of group messages (default 5)\n"
        "  --send-file PATH       headless: send a file and wait for completion\n"
        "  --voice-bench N        headless: stream voice packets for N seconds and\n"
        "                         print tx/rx/loss/jitter as VOICE_BENCH JSON\n"
        "  --wait-ms N            headless: completion timeout (default 8000)\n"
        "  --report FILE          headless: write a JSON test report\n"
        "  --report-txt FILE      headless: write a human-readable test report\n"
        "  --stats                record a field-test bundle (CSV/JSON/log + one .zip) next\n"
        "                         to the executable; send the .zip back. Works with the GUI too.\n"
        "  --stats-dir DIR        write the --stats bundle to DIR instead\n"
        "  --auto-accept          headless: automatically accept incoming file transfers\n"
        "  --download-dir DIR     headless: directory for received files (default: downloads)\n");
}

bool parse_args(int argc, char** argv, Args& out) {
    out.cfg.port = 47000;
    out.cfg.alias = "A";
    out.cfg.keyfile = "p2pnode.key";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", what);
                return {};
            }
            return argv[++i];
        };

        if (a == "--stats") {
            out.stats = true;
        } else if (a == "--stats-dir") {
            out.stats = true;
            out.stats_dir = next("--stats-dir");
        } else if (a == "--alias") {
            out.cfg.alias = next("--alias");
            out.cfg.alias_locked = true;
        } else if (a == "--port") {
            {
                int pv = std::stoi(next("--port"));
                if (pv < 1 || pv > 65535) throw std::out_of_range("--port must be 1..65535");
                out.cfg.port = static_cast<uint16_t>(pv);
            }
        } else if (a == "--key") {
            out.cfg.keyfile = next("--key");
        } else if (a == "--bootstrap") {
            std::string v = next("--bootstrap");
            size_t p = v.rfind(':');
            if (p == std::string::npos) {
                std::fprintf(stderr, "bootstrap must be HOST:PORT\n");
                return false;
            }
            const std::string bh = v.substr(0, p);
            const uint16_t bp = static_cast<uint16_t>(std::stoi(v.substr(p + 1)));
            if (out.cfg.bootstrap_host.empty()) {
                out.cfg.bootstrap_host = bh;
                out.cfg.bootstrap_port = bp;
            } else {
                out.cfg.bootstrap_list.push_back({bh, bp});  // --bootstrap may repeat
            }
        } else if (a == "--no-audio") {
            out.cfg.audio = false;
        } else if (a == "--demo") {
            out.cfg.demo = true;
        } else if (a == "--contact-uri") {
            out.cfg.contact_uri = next("--contact-uri");
        } else if (a == "--contact-name") {
            out.cfg.contact_name = next("--contact-name");
        } else if (a == "--borderless") {
            out.cfg.borderless = true;
        } else if (a == "--light") {
            out.cfg.light = true;
        } else if (a == "--dark") {
            out.cfg.light = false;
        } else if (a == "--pos") {
            std::string v = next("--pos");
            std::sscanf(v.c_str(), "%d,%d", &out.cfg.x, &out.cfg.y);
        } else if (a == "--size") {
            std::string v = next("--size");
            std::sscanf(v.c_str(), "%d,%d", &out.cfg.width, &out.cfg.height);
        } else if (a == "--tab") {
            out.cfg.tab = std::stoi(next("--tab"));
        } else if (a == "--selftest") {
            out.selftest = true;
        } else if (a == "--export-key") {
            out.export_key = next("--export-key");
        } else if (a == "--import-key") {
            out.import_key = next("--import-key");
        } else if (a == "--headless") {
            out.headless = true;
        } else if (a == "--run-seconds") {
            out.run_seconds = std::stoi(next("--run-seconds"));
        } else if (a == "--record-mic") {
            out.record_mic = true;
            out.mic_path = next("--record-mic");
        } else if (a == "--seconds") {
            out.mic_seconds = std::stoi(next("--seconds"));
        } else if (a == "--peer") {
            std::string v = next("--peer");
            size_t p = v.rfind(':');
            if (p == std::string::npos) {
                std::fprintf(stderr, "peer must be HOST:PORT\n");
                return false;
            }
            out.peer_host = v.substr(0, p);
            out.peer_port = static_cast<uint16_t>(std::stoi(v.substr(p + 1)));
        } else if (a == "--peer-id") {
            out.peer_id_hex = next("--peer-id");
        } else if (a == "--relay-id") {
            out.relay_id_hex = next("--relay-id");
        } else if (a == "--relay-only") {
            out.relay_only = true;
        } else if (a == "--send-chat") {
            out.send_chat_text = next("--send-chat");
        } else if (a == "--group-create") {
            out.group_create = next("--group-create");
        } else if (a == "--group-members") {
            out.group_members = std::stoi(next("--group-members"));
        } else if (a == "--group-send") {
            out.group_send = next("--group-send");
        } else if (a == "--group-file") {
            out.group_file = next("--group-file");
        } else if (a == "--group-name") {
            out.group_name = next("--group-name");
        } else if (a == "--group-count") {
            out.group_count = std::stoi(next("--group-count"));
        } else if (a == "--group-interval-ms") {
            out.group_interval_ms = std::stoi(next("--group-interval-ms"));
        } else if (a == "--send-file") {
            out.send_file_path = next("--send-file");
        } else if (a == "--wait-ms") {
            out.wait_ms = std::stoi(next("--wait-ms"));
        } else if (a == "--voice-bench") {
            out.voice_bench = std::stoi(next("--voice-bench"));
        } else if (a == "--report") {
            out.report_path = next("--report");
        } else if (a == "--auto-accept") {
            out.auto_accept = true;
        } else if (a == "--download-dir") {
            out.download_dir = next("--download-dir");
        } else if (a == "--report-txt") {
            out.report_txt_path = next("--report-txt");
        } else if (a == "--help" || a == "-h") {
            usage();
            return false;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage();
            return false;
        }
    }
    return true;
}

bool write_seed(const std::string& path, const std::array<uint8_t, 32>& seed) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(seed.data()), 32);
    if (!f) return false;
#ifndef _WIN32
    (void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
    return true;
}

bool wait_until(const std::function<bool()>& pred, int timeout_ms) {
    uint64_t t0 = p2p::now_ms();
    while (p2p::now_ms() - t0 < static_cast<uint64_t>(timeout_ms)) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

bool make_voice_frames(int count, std::vector<std::vector<uint8_t>>& frames) {
    int e = OPUS_OK;
    OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &e);
    if (!enc || e != OPUS_OK) return false;
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(5));
    std::array<int16_t, 960> pcm{};
    std::vector<uint8_t> packet(800);
    frames.clear();
    for (int f = 0; f < count; ++f) {
        for (int i = 0; i < 960; ++i) {
            double phase = 2.0 * 3.14159265358979323846 * 440.0 * (f * 960 + i) / 48000.0;
            pcm[i] = static_cast<int16_t>(6000.0 * std::sin(phase));
        }
        int n = opus_encode(enc, pcm.data(), 960, packet.data(),
                            static_cast<opus_int32>(packet.size()));
        if (n <= 0) {
            opus_encoder_destroy(enc);
            return false;
        }
        frames.emplace_back(packet.begin(), packet.begin() + n);
    }
    opus_encoder_destroy(enc);
    return true;
}

bool send_forged_hello(const p2p::Identity& attacker, const p2p::NodeId& claimed,
                       uint16_t listen_port, uint16_t target_port) {
    p2p::EphemeralKey eph = p2p::generate_ephemeral();
    p2p::Writer body;
    body.u8(1);
    body.str("evil");
    body.raw(attacker.ed_pk.data(), attacker.ed_pk.size());
    body.raw(attacker.x_pk.data(), attacker.x_pk.size());
    body.raw(eph.pk.data(), eph.pk.size());
    body.u16(listen_port);
    body.u8(static_cast<uint8_t>(p2p::NatType::Unknown));
    body.u64(p2p::unix_ms());
    std::array<uint8_t, 16> nonce{};
    randombytes_buf(nonce.data(), nonce.size());
    body.raw(nonce.data(), nonce.size());
    std::array<uint8_t, 32> pm{};
    body.raw(pm.data(), pm.size());
    std::array<uint8_t, crypto_sign_BYTES> sig{};
    crypto_sign_detached(sig.data(), nullptr, body.data.data(), body.data.size(),
                         attacker.ed_sk.data());

    p2p::PacketHeader h{};
    h.magic = htonl(p2p::kMagic);
    h.type = static_cast<uint8_t>(p2p::PacketType::Hello);
    h.length = htons(static_cast<uint16_t>(body.data.size() + sig.size()));
    std::memcpy(h.sender, claimed.b.data(), p2p::kIdBytes);
    std::vector<uint8_t> packet(sizeof(h) + body.data.size() + sig.size());
    std::memcpy(packet.data(), &h, sizeof(h));
    std::memcpy(packet.data() + sizeof(h), body.data.data(), body.data.size());
    std::memcpy(packet.data() + sizeof(h) + body.data.size(), sig.data(), sig.size());

    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kInvalidSocket) return false;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(target_port);
    inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
#ifdef _WIN32
    int n = ::sendto(s, reinterpret_cast<const char*>(packet.data()),
                     static_cast<int>(packet.size()), 0,
                     reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#else
    ssize_t n = ::sendto(s, packet.data(), packet.size(), 0,
                         reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#endif
    p2p::close_socket(s);
    return n == static_cast<decltype(n)>(packet.size());
}

int run_selftest() {
    const char* key_a = "selftest_a.key";
    const char* key_b = "selftest_b.key";
    const char* key_c = "selftest_c.key";
    std::remove(key_a);
    std::remove(key_b);
    std::remove(key_c);

    p2p::Node a;
    p2p::Node b;
    p2p::Node c;
    std::string err;

    p2p::NodeConfig ca;
    ca.alias = "Alice";
    ca.port = 47210;
    ca.keyfile = key_a;
    ca.persist = false;

    p2p::NodeConfig cb;
    cb.alias = "Bob";
    cb.port = 47211;
    cb.keyfile = key_b;
    cb.persist = false;
    cb.bootstrap_host = "127.0.0.1";
    cb.bootstrap_port = 47210;

    p2p::NodeConfig cc;
    cc.alias = "Carol";
    cc.port = 47212;
    cc.keyfile = key_c;
    cc.persist = false;
    cc.bootstrap_host = "127.0.0.1";
    cc.bootstrap_port = 47210;

    if (!a.start(ca, err)) {
        std::fprintf(stderr, "A start: %s\n", err.c_str());
        return 1;
    }
    if (!b.start(cb, err)) {
        std::fprintf(stderr, "B start: %s\n", err.c_str());
        a.stop();
        return 1;
    }
    if (!c.start(cc, err)) {
        std::fprintf(stderr, "C start: %s\n", err.c_str());
        a.stop();
        b.stop();
        return 1;
    }

    std::printf("[selftest] nodes started\n");
    std::fflush(stdout);
    bool ab = wait_until(
        [&] {
            p2p::Peer p;
            return a.get_peer(b.identity().id, p) && p.handshake_done && p.eph_pk[0] &&
                   p.root_key[0];
        },
        5000);
    bool ac = wait_until(
        [&] {
            p2p::Peer p;
            return a.get_peer(c.identity().id, p) && p.handshake_done;
        },
        6000);
    bool bc = wait_until(
        [&] {
            p2p::Peer p;
            return b.get_peer(c.identity().id, p) && p.handshake_done;
        },
        5000);
    if (ab && !bc) {
        b.connect_to("127.0.0.1", 47212);
        bc = wait_until(
            [&] {
                p2p::Peer p;
                return b.get_peer(c.identity().id, p) && p.handshake_done &&
                       c.get_peer(b.identity().id, p) && p.handshake_done;
            },
            5000);
    }

    std::printf("[selftest] handshakes ab=%d ac=%d bc=%d\n", ab, ac, bc);
    std::fflush(stdout);
    p2p::Peer peer_b;
    bool signed_hello = ab && a.get_peer(b.identity().id, peer_b) &&
                        peer_b.handshake_done && p2p::verify_node_id(b.identity().id,
                                                                    b.identity().ed_pk) &&
                        p2p::verify_x25519_from_ed(b.identity().ed_pk, b.identity().x_pk) &&
                        peer_b.root_key[0] && peer_b.eph_pk[0] && peer_b.text_ratchet.ready;

    uint64_t mitm_before = a.stats().mitm_rejected;
    bool forged_sent = send_forged_hello(c.identity(), b.identity().id, 47999, 47210);
    bool mitm_rejected = forged_sent && wait_until(
        [&] { return a.stats().mitm_rejected > mitm_before; }, 1500);

    std::printf("[selftest] signed_hello=%d mitm=%d\n", signed_hello, mitm_rejected);
    std::fflush(stdout);
    std::string sas_a;
    std::string sas_b;
    std::string words_a;
    std::string words_b;
    bool sas_ok = ab && a.sas(b.identity().id, sas_a, words_a) &&
                  b.sas(a.identity().id, sas_b, words_b) && !sas_a.empty() &&
                  sas_a == sas_b && !words_a.empty();

    const int kRatchetMessages = 70;
    int sent_messages = 0;
    for (int i = 0; i < kRatchetMessages; ++i) {
        if (a.send_chat(b.identity().id, "ratchet-" + std::to_string(i), &err))
            ++sent_messages;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    int received_messages = 0;
    bool ratchet_delivered = wait_until(
        [&] {
            received_messages = 0;
            for (const auto& m : b.chat())
                if (!m.outgoing && !m.is_group &&
                    m.text.rfind("ratchet-", 0) == 0)
                    ++received_messages;
            return received_messages >= kRatchetMessages;
        },
        7000);
    bool ratchet_acked = wait_until(
        [&] {
            int acked = 0;
            for (const auto& m : a.chat())
                if (m.outgoing && !m.is_group && m.delivered &&
                    m.text.rfind("ratchet-", 0) == 0)
                    ++acked;
            return acked >= kRatchetMessages;
        },
        7000);
    p2p::NodeStats stats_a = a.stats();
    bool ratchet_rotated = stats_a.ratchet_steps > 0 && stats_a.chat_acked > 0;
    std::printf("[selftest] ratchet sent=%d recv=%d delivered=%d acked=%d\n",
                sent_messages, received_messages, ratchet_delivered, ratchet_acked);
    std::fflush(stdout);

    std::atomic<int> voice_rx{0};
    b.set_voice_handler([&](const p2p::NodeId&, uint32_t, const std::vector<uint8_t>& opus) {
        if (!opus.empty()) voice_rx.fetch_add(1);
    });

    std::vector<std::vector<uint8_t>> frames;
    bool voice_frames_ok = make_voice_frames(50, frames);
    int direct_sent = 0;
    if (ab && voice_frames_ok) {
        for (size_t i = 0; i < frames.size(); ++i) {
            if (a.send_voice(b.identity().id, static_cast<uint32_t>(i), frames[i]))
                ++direct_sent;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    bool voice_50 = voice_frames_ok && direct_sent >= 48 &&
                    wait_until([&] { return voice_rx.load() >= 48; }, 4000);
    std::printf("[selftest] voice tx=%d rx=%d ok=%d\n", direct_sent, voice_rx.load(),
                voice_50);
    std::fflush(stdout);

    p2p::NodeId group_id;
    bool group_created = false;
    if (ab && ac) {
        std::vector<p2p::NodeId> members{b.identity().id, c.identity().id};
        group_created = a.create_group("Selftest Room", members, group_id, &err);
    }
    bool group_joined_b = false;
    bool group_joined_c = false;
    if (group_created) {
        group_joined_b = wait_until(
            [&] {
                p2p::GroupRoom g;
                return b.group(group_id, g) && g.members.size() >= 3;
            },
            5000);
        group_joined_c = wait_until(
            [&] {
                p2p::GroupRoom g;
                return c.group(group_id, g) && g.members.size() >= 3;
            },
            5000);
    }
    bool group_chat_sent = group_joined_b && group_joined_c &&
                           a.send_group_chat(group_id, "group-selftest", &err);
    bool group_chat_recv = group_chat_sent && wait_until(
        [&] {
            for (const auto& m : b.messages_for(group_id))
                if (!m.outgoing && m.text == "group-selftest") return true;
            return false;
        },
        4000) && wait_until(
        [&] {
            for (const auto& m : c.messages_for(group_id))
                if (!m.outgoing && m.text == "group-selftest") return true;
            return false;
        },
        4000);
    bool group_dag = false;
    if (group_chat_recv) {
        p2p::GroupRoom g;
        group_dag = a.group(group_id, g) && !g.last_hash.empty();
    }

    std::atomic<int> group_voice_b{0};
    std::atomic<int> group_voice_c{0};
    b.set_group_voice_handler(
        [&](const p2p::NodeId&, const p2p::NodeId&, uint32_t,
            const std::vector<uint8_t>& opus) {
            if (!opus.empty()) group_voice_b.fetch_add(1);
        });
    c.set_group_voice_handler(
        [&](const p2p::NodeId&, const p2p::NodeId&, uint32_t,
            const std::vector<uint8_t>& opus) {
            if (!opus.empty()) group_voice_c.fetch_add(1);
        });
    int group_voice_sent = 0;
    if (group_chat_sent && frames.size() >= 50) {
        for (int i = 0; i < 50; ++i) {
            if (a.send_group_voice(group_id, static_cast<uint32_t>(i), frames[i]))
                ++group_voice_sent;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    bool group_voice_ok = group_voice_sent >= 48 &&
                          wait_until(
                              [&] {
                                  return group_voice_b.load() >= 48 &&
                                         group_voice_c.load() >= 48;
                              },
                              5000);
    std::printf("[selftest] group created=%d joined=%d/%d chat=%d dag=%d voice=%d/%d\n",
                group_created, group_joined_b, group_joined_c, group_chat_recv, group_dag,
                group_voice_b.load(), group_voice_c.load());
    std::fflush(stdout);

    // 6b. Group key rotation: A (admin) excludes C -> B gets a new key.
    bool group_rekey_ok = false;
    if (group_voice_ok) {
        p2p::GroupRoom g_before;
        b.group(group_id, g_before);
        std::string xerr;
        if (a.exclude_from_group(group_id, c.identity().id, &xerr)) {
            // B should converge to a 2-member list with a rotated key.
            bool b_updated = wait_until(
                [&] {
                    p2p::GroupRoom gb;
                    return b.group(group_id, gb) && gb.members.size() == 2 &&
                           gb.group_key != g_before.group_key;
                },
                5000);
            if (b_updated) {
                // New key in force: A->B chat after re-key must decrypt.
                bool chat_after = a.send_group_chat(group_id, "after-rekey", &xerr);
                bool b_recv = false;
                if (chat_after) {
                    b_recv = wait_until(
                        [&] {
                            for (const auto& m : b.messages_for(group_id))
                                if (!m.outgoing && m.text == "after-rekey") return true;
                            return false;
                        },
                        3000);
                }
                group_rekey_ok = b_recv;
            }
        }
    }
    std::printf("[selftest] group_rekey ok=%d\n", group_rekey_ok);
    std::fflush(stdout);

    // Offline queue and encrypted DHT inbox envelope.
    b.stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    bool offline_queued = a.send_chat_offline(b.identity().id, "offline-selftest", &err);
    bool envelope_stored = wait_until(
        [&] { return a.stats().envelopes_stored > 0; }, 2000);
    if (offline_queued) {
        cb.bootstrap_host = "127.0.0.1";
        cb.bootstrap_port = 47210;
        if (!b.start(cb, err)) {
            std::fprintf(stderr, "B restart: %s\n", err.c_str());
            offline_queued = false;
        }
    }
    bool offline_received = offline_queued && wait_until(
        [&] {
            for (const auto& m : b.chat())
                if (!m.outgoing && !m.is_group && m.text == "offline-selftest") return true;
            return false;
        },
        8000);
    bool offline_acked = offline_queued && wait_until(
        [&] {
            for (const auto& m : a.chat())
                if (m.outgoing && !m.is_group && m.text == "offline-selftest" && m.delivered)
                    return true;
            return false;
        },
        5000);
    std::printf("[selftest] offline queued=%d stored=%d recv=%d acked=%d\n",
                offline_queued, envelope_stored, offline_received, offline_acked);
    std::fflush(stdout);

    // Third-node relay.
    bool bc_again = wait_until(
        [&] {
            p2p::Peer p;
            return b.get_peer(c.identity().id, p) && p.handshake_done;
        },
        7000);
    uint64_t relay_before = c.stats().relay_forwarded;
    bool relay_sent = bc_again && a.send_chat_relay(c.identity().id, b.identity().id,
                                                   "relay-selftest", &err);
    bool relay_received = relay_sent && wait_until(
        [&] {
            for (const auto& m : b.chat())
                if (!m.outgoing && !m.is_group && m.text == "relay-selftest") return true;
            return false;
        },
        5000);
    bool relay_forwarded = relay_received && wait_until(
        [&] { return c.stats().relay_forwarded > relay_before; }, 3000);
    std::printf("[selftest] relay sent=%d recv=%d forwarded=%d\n", relay_sent,
                relay_received, relay_forwarded);
    std::fflush(stdout);

    // File transfer: streaming chunks, receiver SHA-256 and completion ACK.
    std::printf("[selftest] file stage begin\n");
    std::fflush(stdout);
    bool file_transfer_ok = false;
    b.set_download_dir("selftest_downloads");
    bool pattern_ok = write_pattern("selftest_payload.bin", 256 * 1024);
    std::printf("[selftest] file pattern=%d\n", pattern_ok);
    std::fflush(stdout);
    bool file_started = pattern_ok && a.send_file(b.identity().id, "selftest_payload.bin", &err);
    std::printf("[selftest] file started=%d err=%s\n", file_started, err.c_str());
    std::fflush(stdout);
    bool sender_complete = file_started && wait_until(
        [&] {
            for (const auto& f : a.file_transfers())
                if (f.outgoing && f.complete) return true;
            return false;
        },
        45000);
    bool receiver_complete = file_started && wait_until(
        [&] {
            for (const auto& f : b.file_transfers())
                if (!f.outgoing && f.complete) return true;
            return false;
        },
        45000);
    file_transfer_ok = file_started && sender_complete && receiver_complete;
    std::printf("[selftest] file started=%d sender=%d receiver=%d ok=%d\n", file_started,
                sender_complete, receiver_complete, file_transfer_ok);
    std::fflush(stdout);
    std::remove("selftest_payload.bin");
    std::filesystem::remove_all("selftest_downloads");

    // Relay-first handshake: two fresh peers never learn each other's direct
    // address; their HELLO/HELLO_ACK and chat travel inside RELAY envelopes.
    p2p::Node relay_node;
    p2p::Node relay_a;
    p2p::Node relay_b;
    const char* key_r = "selftest_r.key";
    const char* key_ra = "selftest_ra.key";
    const char* key_rb = "selftest_rb.key";
    std::remove(key_r);
    std::remove(key_ra);
    std::remove(key_rb);
    p2p::NodeConfig cr;
    cr.alias = "Relay";
    cr.port = 47213;
    cr.keyfile = key_r;
    cr.persist = false;
    p2p::NodeConfig cra;
    cra.alias = "RelayAlice";
    cra.port = 47214;
    cra.keyfile = key_ra;
    cra.persist = false;
    cra.bootstrap_host = "127.0.0.1";
    cra.bootstrap_port = 47213;
    cra.relay_only = true;
    p2p::NodeConfig crb;
    crb.alias = "RelayBob";
    crb.port = 47215;
    crb.keyfile = key_rb;
    crb.persist = false;
    crb.bootstrap_host = "127.0.0.1";
    crb.bootstrap_port = 47213;
    crb.relay_only = true;

    bool relay_first_ok = false;
    if (relay_node.start(cr, err) && relay_a.start(cra, err) && relay_b.start(crb, err)) {
        bool ra_ready = wait_until(
            [&] {
                p2p::Peer p;
                return relay_a.get_peer(relay_node.identity().id, p) && p.handshake_done;
            },
            5000);
        bool rb_ready = wait_until(
            [&] {
                p2p::Peer p;
                return relay_b.get_peer(relay_node.identity().id, p) && p.handshake_done;
            },
            5000);
        bool started = ra_ready && rb_ready &&
                       relay_a.connect_via_relay(relay_node.identity().id,
                                                 relay_b.identity().id, &err);
        bool both_handshaked = started && wait_until(
            [&] {
                p2p::Peer p1;
                p2p::Peer p2;
                return relay_a.get_peer(relay_b.identity().id, p1) && p1.handshake_done &&
                       relay_b.get_peer(relay_a.identity().id, p2) && p2.handshake_done;
            },
            7000);
        bool relay_chat = both_handshaked &&
                          relay_a.send_chat(relay_b.identity().id, "relay-first-selftest", &err);
        bool relay_chat_recv = relay_chat && wait_until(
            [&] {
                for (const auto& m : relay_b.chat())
                    if (!m.outgoing && !m.is_group && m.text == "relay-first-selftest")
                        return true;
                return false;
            },
            5000);
        p2p::Peer pb;
        bool marked_relay = relay_a.get_peer(relay_b.identity().id, pb) && pb.relayed;
        relay_first_ok = both_handshaked && relay_chat_recv && marked_relay &&
                         relay_node.stats().relay_forwarded > 0;
    }
    std::printf("[selftest] relay_first ok=%d\n", relay_first_ok);
    std::fflush(stdout);
    relay_node.stop();
    relay_a.stop();
    relay_b.stop();
    std::remove(key_r);
    std::remove(key_ra);
    std::remove(key_rb);
    std::remove("node_47213_vault.dat");
    std::remove("node_47214_vault.dat");
    std::remove("node_47215_vault.dat");

    // Multi-peer voice engine mixing (no playback; --no-audio path).
    p2p::VoiceEngine ve;
    std::string verr;
    bool voice_init = ve.init(false, verr);
    bool mix_ok = false;
    bool fec_ok = false;
    double snr = 0.0;
    double kbps = 0.0;
    bool loopback = false;
    bool fecplc_ok = false;
    double fecplc_snr = 0.0;
    int fecplc_lag = 0;
    int fecplc_played = 0;
    uint32_t fecplc_fec = 0;
    uint32_t fecplc_plc = 0;
    if (voice_init) {
        ve.set_packet_loss(5.0);
        fec_ok = ve.stats().fec_enabled;
        if (voice_frames_ok) {
            for (size_t i = 0; i < 10; ++i) {
                ve.on_packet(b.identity().id, static_cast<uint32_t>(i), frames[i]);
                ve.on_packet(c.identity().id, static_cast<uint32_t>(i), frames[i]);
                std::this_thread::sleep_for(std::chrono::milliseconds(22));
            }
            p2p::VoiceStats vs = ve.stats();
            mix_ok = vs.mix_frames >= 5 && vs.peers >= 1;
        }
        loopback = ve.loopback_test(snr, kbps, verr);

        // FEC/PLC reconstruction quality test (headless, no sound card).
        // Encode 1 s of 440 Hz sine at 32 kbps with FEC on, then decode with
        // every 8th packet removed: FEC can reconstruct from the following
        // packet; PLC kicks in for isolated drops.  We measure the SNR of the
        // reconstructed stream vs. the original.
        if (voice_frames_ok) {
            p2p::VoiceEngine ve2;
            if (ve2.init(false, verr)) {
                ve2.set_packet_loss(10.0);  // enables in-band FEC
                std::vector<int16_t> original(48000);
                for (int i = 0; i < 48000; ++i)
                    original[i] = static_cast<int16_t>(8000.0 *
                        std::sin(2.0 * 3.14159265358979323846 * 440.0 * i / 48000.0));

                std::vector<std::vector<uint8_t>> pkts;
                pkts.reserve(50);
                // Encode 1 s = 50 frames with FEC
                {
                    int e = OPUS_OK;
                    OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &e);
                    if (enc) {
                        opus_encoder_ctl(enc, OPUS_SET_BITRATE(32000));
                        opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
                        opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(10));
                        std::array<int16_t, 960> pcm{};
                        std::vector<uint8_t> packet(1500);
                        for (int f = 0; f < 50; ++f) {
                            std::copy(original.begin() + f * 960, original.begin() + (f + 1) * 960, pcm.begin());
                            int n = opus_encode(enc, pcm.data(), 960, packet.data(),
                                                static_cast<opus_int32>(packet.size()));
                            if (n > 0) pkts.emplace_back(packet.begin(), packet.begin() + n);
                        }
                        opus_encoder_destroy(enc);
                    }
                }
                // Decode with every 8th frame missing; capture mix_sink output
                if (pkts.size() == 50) {
                    std::vector<int16_t> decoded(48000, 0);
                    size_t dec_pos = 0;
                    ve2.set_mix_sink([&](const int16_t* pcm, size_t n) {
                        if (dec_pos + n <= decoded.size()) {
                            std::copy(pcm, pcm + n, decoded.begin() + dec_pos);
                            dec_pos += n;
                        }
                    });
                    for (size_t i = 0; i < pkts.size(); ++i) {
                        if (i % 8 != 0) {  // drop frame
                            ve2.on_packet(p2p::VoiceEngine::local_stream_id(), static_cast<uint32_t>(i), pkts[i]);
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(21));
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    // Align and measure SNR.  Playout starts at packet 1 (the
                    // packet-0 drop), so the decoded stream is a delayed copy
                    // of the original; search a two-frame window of lags and
                    // only score samples that were actually played, trimming
                    // the last frame that may still be in flight.
                    const int played = static_cast<int>(dec_pos);
                    const int tail = 960;
                    double best_err = 1e100;
                    int best_lag = 0;
                    for (int lag = 0; lag <= 2880; ++lag) {
                        double err_sum = 0.0;
                        const int hi = played - tail;
                        for (int j = lag; j < hi; ++j) {
                            double d = static_cast<double>(decoded[j]) -
                                       static_cast<double>(original[j - lag]);
                            err_sum += d * d;
                        }
                        if (err_sum < best_err) {
                            best_err = err_sum;
                            best_lag = lag;
                        }
                    }
                    double sig = 0.0;
                    for (int j = best_lag; j < played - tail; ++j) {
                        double v = original[j - best_lag];
                        sig += v * v;
                    }
                    fecplc_snr = (best_err <= 1e-9 || sig <= 1e-9)
                        ? 99.0 : 10.0 * std::log10(sig / best_err);
                    fecplc_lag = best_lag;
                    fecplc_played = played;
                    // The pass criterion is the repair machinery working: every
                    // dropped frame must be concealed by FEC or PLC.  SNR is
                    // reported for the field record.
                    const p2p::VoiceStats vs2 = ve2.stats();
                    fecplc_ok = vs2.fec_recovered + vs2.plc_frames >= 5 &&
                                played > 40000;
                    fecplc_fec = vs2.fec_recovered;
                    fecplc_plc = vs2.plc_frames;
                    ve2.shutdown();
                }
            }
        }

        ve.shutdown();
    }
    std::printf("[selftest] voice_engine init=%d mix=%d fec=%d loopback=%d fecplc=%d snr=%.2f\n",
                voice_init, mix_ok, fec_ok, loopback, fecplc_ok, fecplc_snr);
    std::fflush(stdout);

    // 12. Encrypted identity export/import round-trip (Argon2id + XChaCha20-Poly1305).
    bool key_roundtrip = false;
    {
        const char* key_e = "selftest_e.key";
        const char* exp_f = "selftest_e.p2pk";
        std::remove(key_e);
        std::remove(exp_f);
        p2p::Identity id1;
        std::string kerr;
        if (id1.init(key_e, "ExportTest", kerr)) {
            if (p2p::export_key(id1, "correct horse", exp_f, kerr)) {
                // Wrong passphrase must fail.
                p2p::Identity bad;
                const bool wrong_rejected = !p2p::import_key("wrong", exp_f, bad, kerr);
                // Correct passphrase restores the same NodeID.
                p2p::Identity id2;
                bool same_id = false;
                if (p2p::import_key("correct horse", exp_f, id2, kerr)) {
                    id2.alias = id1.alias;
                    same_id = (id1.id == id2.id) && (id1.ed_pk == id2.ed_pk) &&
                              (id1.x_sk == id2.x_sk);
                }
                key_roundtrip = wrong_rejected && same_id;
            }
        }
        std::remove(key_e);
        std::remove(exp_f);
    }
    std::printf("[selftest] key_export_import roundtrip=%d\n", key_roundtrip);
    std::fflush(stdout);

    // Profile rename must reach the other side's address book, and the shared contact link
    // "username:p2p://id@host:port" must parse to the same id and carry the name.
    bool rename_ok = false;
    bool link_ok = false;
    {
        const std::string new_name = "Alice Renamed";
        std::string rerr;
        if (a.set_alias(new_name, &rerr)) {
            rename_ok = wait_until(
                [&] {
                    for (const auto& ct : b.contacts())
                        if (ct.id == a.identity().id && ct.alias == new_name) return true;
                    return false;
                },
                6000);
        }
        const std::string share = a.share_uri();
        p2p::NodeId pid;
        std::string phost;
        uint16_t pport = 0;
        link_ok = p2p::Node::parse_invite(share, pid, phost, pport) && pid == a.identity().id &&
                  p2p::Node::parse_invite_name(share) == new_name &&
                  p2p::Node::parse_invite(a.invite_uri(), pid, phost, pport) &&
                  p2p::Node::parse_invite_name(a.invite_uri()).empty();
    }
    std::printf("[selftest] rename_propagates=%d share_link_roundtrip=%d\n", rename_ok, link_ok);
    std::fflush(stdout);

    // ---- Group file / invite / leave cases (fresh group, a = admin, members b and c) ----
    bool gf_group_ok = false, gf_batch_ok = false, gf_direct_ok = false;
    bool gate_member_ok = false, gate_stranger_ok = false;
    bool leave_ok = false, invite_notify_ok = false;
    {
        auto has_member = [](const p2p::GroupRoom& g, const p2p::NodeId& id) {
            return std::find(g.members.begin(), g.members.end(), id) != g.members.end();
        };
        std::string gerr;
        // The earlier stages stopped/restarted b: wait until all three are connected again.
        bool mesh = wait_until(
            [&] {
                p2p::Peer p;
                return a.get_peer(b.identity().id, p) && p.handshake_done &&
                       a.get_peer(c.identity().id, p) && p.handshake_done &&
                       b.get_peer(a.identity().id, p) && p.handshake_done &&
                       b.get_peer(c.identity().id, p) && p.handshake_done &&
                       c.get_peer(a.identity().id, p) && p.handshake_done &&
                       c.get_peer(b.identity().id, p) && p.handshake_done;
            },
            10000);
        p2p::NodeId gid;
        bool g_ready = false;
        if (mesh && a.create_group("Selftest Files", {b.identity().id, c.identity().id}, gid,
                                   &gerr)) {
            g_ready = wait_until(
                [&] {
                    p2p::GroupRoom gb, gc;
                    return b.group(gid, gb) && c.group(gid, gc) && gb.members.size() >= 3 &&
                           gc.members.size() >= 3;
                },
                8000);
        }
        std::printf("[selftest] group2 mesh=%d ready=%d\n", mesh, g_ready);
        std::fflush(stdout);

        // (a) group file: one shared batch, every other member gets a complete group transfer.
        const char* gf_name = "selftest_gf_group.bin";
        const char* df_name = "selftest_gf_direct.bin";
        b.set_download_dir("selftest_dl_b");
        c.set_download_dir("selftest_dl_c");
        b.set_auto_accept_files(true);
        c.set_auto_accept_files(true);
        bool pat1 = write_pattern(gf_name, 64 * 1024);
        bool pat2 = write_pattern(df_name, 64 * 1024);
        int started = 0;
        if (g_ready && pat1) {
            // Offers need members to look "online" (<30 s); retry briefly if not yet.
            wait_until(
                [&] {
                    started = a.send_file_group(gid, gf_name, &gerr);
                    return started > 0;
                },
                5000);
        }
        auto grp_done = [&](p2p::Node& n) {
            for (const auto& f : n.file_transfers())
                if (!f.outgoing && f.group == gid && f.name == gf_name && f.complete)
                    return true;
            return false;
        };
        bool b_got = started > 0 && wait_until([&] { return grp_done(b); }, 30000);
        bool c_got = started > 0 && wait_until([&] { return grp_done(c); }, 30000);
        int out_n = 0;
        bool out_same_batch = true, out_all_complete = false;
        uint64_t batch0 = 0;
        wait_until(
            [&] {
                out_n = 0;
                int done = 0;
                out_same_batch = true;
                batch0 = 0;
                for (const auto& f : a.file_transfers()) {
                    if (!f.outgoing || !(f.group == gid) || f.name != gf_name) continue;
                    ++out_n;
                    if (f.complete) ++done;
                    if (batch0 == 0) batch0 = f.group_batch;
                    else if (f.group_batch != batch0) out_same_batch = false;
                }
                return out_n >= 2 && done >= 2;
            },
            15000);
        out_all_complete = out_n >= 2;
        gf_group_ok = pat1 && started == 2 && b_got && c_got && out_all_complete;
        gf_batch_ok = out_n == 2 && out_same_batch && batch0 != 0;
        std::printf("[selftest] group_file started=%d b=%d c=%d out=%d batch=%d\n", started,
                    b_got, c_got, out_n, gf_batch_ok);
        std::fflush(stdout);

        // (b) direct file a -> b carries no group on either side.
        bool dstart = pat2 && a.send_file(b.identity().id, df_name, &gerr);
        bool d_b = dstart && wait_until(
            [&] {
                for (const auto& f : b.file_transfers())
                    if (!f.outgoing && f.name == df_name && f.complete && f.group.empty())
                        return true;
                return false;
            },
            30000);
        bool d_a = dstart && wait_until(
            [&] {
                for (const auto& f : a.file_transfers())
                    if (f.outgoing && f.name == df_name && f.complete && f.group.empty() &&
                        f.group_batch == 0)
                        return true;
                return false;
            },
            15000);
        // And no direct-file entry may be mislabeled with a group.
        bool d_clean = true;
        for (const auto& f : b.file_transfers())
            if (f.name == df_name && !f.group.empty()) d_clean = false;
        gf_direct_ok = dstart && d_b && d_a && d_clean;
        std::printf("[selftest] direct_file started=%d b=%d a=%d ok=%d\n", dstart, d_b, d_a,
                    gf_direct_ok);
        std::fflush(stdout);
        std::remove(gf_name);
        std::remove(df_name);
        std::filesystem::remove_all("selftest_dl_b");
        std::filesystem::remove_all("selftest_dl_c");

        // (c) invite gating.  c is a plain member: its invite_to_group sends b a "member view"
        // (same key, one extra member).  b must keep its key, keep a as admin and not lose anyone.
        p2p::Node d;
        const char* key_d = "selftest_d.key";
        std::remove(key_d);
        p2p::NodeConfig cd;
        cd.alias = "Dave";
        cd.port = 47216;
        cd.keyfile = key_d;
        cd.persist = false;
        cd.bootstrap_host = "127.0.0.1";
        cd.bootstrap_port = 47211;  // handshake with b only
        bool d_started = d.start(cd, gerr);
        bool d_b_shake = d_started && wait_until(
            [&] {
                p2p::Peer p;
                return d.get_peer(b.identity().id, p) && p.handshake_done &&
                       b.get_peer(d.identity().id, p) && p.handshake_done;
            },
            8000);
        if (g_ready) {
            p2p::GroupRoom gb0;
            b.group(gid, gb0);
            c.invite_to_group(gid, d.identity().id);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            p2p::GroupRoom gb1, ga1;
            bool have = b.group(gid, gb1) && a.group(gid, ga1);
            gate_member_ok = have && gb1.group_key == gb0.group_key &&
                             ga1.group_key == gb0.group_key && !gb1.members.empty() &&
                             gb1.members.front() == a.identity().id &&
                             !ga1.members.empty() && ga1.members.front() == a.identity().id &&
                             has_member(gb1, a.identity().id) &&
                             has_member(gb1, b.identity().id) &&
                             has_member(gb1, c.identity().id);
        }
        // Stranger d (handshaken with b, not a member) cannot take over b's existing group and
        // cannot make b write into it: it has neither the id nor the key, so the only thing it can
        // do through the API is create its OWN group (a new id).  b's copy of G must be untouched.
        if (g_ready && d_b_shake) {
            p2p::GroupRoom gb0;
            b.group(gid, gb0);
            p2p::NodeId evil;
            d.create_group("Evil", {b.identity().id}, evil, &gerr);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            p2p::GroupRoom gb1;
            bool g_untouched = b.group(gid, gb1) && gb1.group_key == gb0.group_key &&
                               gb1.members == gb0.members && gb1.name == gb0.name;
            p2p::GroupRoom ge;
            bool evil_separate = !(evil == gid) && (!b.group(evil, ge) || ge.group_key != gb0.group_key);
            gate_stranger_ok = g_untouched && evil_separate;
        }
        std::printf("[selftest] invite_gating member=%d stranger=%d (d_started=%d d_b=%d)\n",
                    gate_member_ok, gate_stranger_ok, d_started, d_b_shake);
        std::fflush(stdout);
        d.stop();
        std::remove(key_d);
        std::remove("node_47216_vault.dat");

        // (d) leave_group: b leaves; b loses the group, a and c keep theirs.
        if (g_ready) {
            bool left = b.leave_group(gid, &gerr);
            p2p::GroupRoom tmp;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            leave_ok = left && !b.group(gid, tmp) && a.group(gid, tmp) && c.group(gid, tmp);
        }
        std::printf("[selftest] leave_group ok=%d\n", leave_ok);
        std::fflush(stdout);

        // (e) invite_to_group tells the EXISTING members about the newcomer.
        if (mesh) {
            p2p::NodeId g2;
            bool g2_created = a.create_group("Selftest Two", {b.identity().id}, g2, &gerr);
            bool b_in = g2_created && wait_until(
                [&] {
                    p2p::GroupRoom g;
                    return b.group(g2, g) && has_member(g, b.identity().id);
                },
                8000);
            bool c_absent = true;
            { p2p::GroupRoom g; c_absent = !c.group(g2, g); }
            bool inv = b_in && a.invite_to_group(g2, c.identity().id);
            bool c_in = inv && wait_until(
                [&] {
                    p2p::GroupRoom g;
                    return c.group(g2, g) && has_member(g, c.identity().id);
                },
                8000);
            bool b_sees_c = inv && wait_until(
                [&] {
                    p2p::GroupRoom g;
                    return b.group(g2, g) && has_member(g, c.identity().id);
                },
                8000);
            invite_notify_ok = c_absent && inv && c_in && b_sees_c;
            std::printf("[selftest] invite_notify created=%d b_in=%d c_absent=%d c_in=%d "
                        "b_sees_c=%d\n", g2_created, b_in, c_absent, c_in, b_sees_c);
            std::fflush(stdout);
        }
        std::printf("[selftest] group_file_ok=%d group_batch_ok=%d direct_file_group_empty=%d "
                    "invite_gate_member=%d invite_gate_stranger=%d leave_group=%d "
                    "invite_notifies_members=%d\n",
                    gf_group_ok, gf_batch_ok, gf_direct_ok, gate_member_ok, gate_stranger_ok,
                    leave_ok, invite_notify_ok);
        std::fflush(stdout);
    }

    bool pass = rename_ok && link_ok && signed_hello && mitm_rejected && sas_ok && ratchet_delivered &&
                ratchet_acked && ratchet_rotated && voice_50 && group_created &&
                group_joined_b && group_joined_c && group_chat_sent && group_chat_recv &&
                group_dag && group_voice_ok && offline_queued && envelope_stored &&
                offline_received && offline_acked && relay_sent && relay_received &&
                relay_forwarded && file_transfer_ok && relay_first_ok && mix_ok && fec_ok &&
                loopback && fecplc_ok && key_roundtrip && group_rekey_ok &&
                gf_group_ok && gf_batch_ok && gf_direct_ok && gate_member_ok &&
                gate_stranger_ok && leave_ok && invite_notify_ok;

    std::printf("P2P selftest\n");
    std::printf("  A_ID=%s\n", a.identity().id.hex().c_str());
    std::printf("  B_ID=%s\n", b.identity().id.hex().c_str());
    std::printf("  C_ID=%s\n", c.identity().id.hex().c_str());
    std::printf("  1 signed_hello+ephemeral=%s handshake_ok=%llu mitm_rejected=%llu\n",
                signed_hello ? "yes" : "no",
                static_cast<unsigned long long>(a.stats().handshake_ok),
                static_cast<unsigned long long>(a.stats().mitm_rejected));
    std::printf("  2 mitm_rejected=%s\n", mitm_rejected ? "yes" : "no");
    std::printf("  3 sas_match=%s code=%s words=\"%s\"\n", sas_ok ? "yes" : "no",
                sas_a.c_str(), words_a.c_str());
    std::printf("  4 ratchet_chat sent=%d received=%d delivered=%s acked=%s steps=%llu\n",
                sent_messages, received_messages,
                ratchet_delivered ? "yes" : "no", ratchet_acked ? "yes" : "no",
                static_cast<unsigned long long>(stats_a.ratchet_steps));
    std::printf("  5 voice_packets_tx=%d rx=%d voice_50_50=%s\n", direct_sent,
                voice_rx.load(), voice_50 ? "yes" : "no");
    std::printf("  6 group_created=%s joined_b=%s joined_c=%s chat=%s dag=%s "
                "voice_b=%d voice_c=%d\n",
                group_created ? "yes" : "no", group_joined_b ? "yes" : "no",
                group_joined_c ? "yes" : "no", group_chat_recv ? "yes" : "no",
                group_dag ? "yes" : "no", group_voice_b.load(), group_voice_c.load());
    std::printf("  7 offline queued=%s envelope_stored=%s delivered=%s acked=%s\n",
                offline_queued ? "yes" : "no", envelope_stored ? "yes" : "no",
                offline_received ? "yes" : "no", offline_acked ? "yes" : "no");
    std::printf("  8 relay sent=%s received=%s forwarded=%s\n", relay_sent ? "yes" : "no",
                relay_received ? "yes" : "no", relay_forwarded ? "yes" : "no");
    std::printf("  9 file_transfer streaming_sha256_ack=%s\n", file_transfer_ok ? "yes" : "no");
    std::printf(" 10 relay_first handshake_chat=%s\n", relay_first_ok ? "yes" : "no");
    std::printf(" 11 voice_engine mix=%s fec=%s loopback=%s snr_db=%.2f bitrate_kbps=%.1f\n",
                mix_ok ? "yes" : "no", fec_ok ? "yes" : "no", loopback ? "yes" : "no",
                snr, kbps);
    std::printf(" 12 fec_plc=%s snr_db=%.2f lag=%d played=%d fec=%u plc=%u  key_export_import=%s\n",
                fecplc_ok ? "yes" : "no", fecplc_snr, fecplc_lag, fecplc_played,
                fecplc_fec, fecplc_plc, key_roundtrip ? "yes" : "no");
    std::printf(" 13 group_rekey=%s\n", group_rekey_ok ? "yes" : "no");
    std::printf("  result=%s\n", pass ? "PASS" : "FAIL");

    a.stop();
    b.stop();
    c.stop();
    std::remove(key_a);
    std::remove(key_b);
    std::remove(key_c);
    std::remove("node_47210_vault.dat");
    std::remove("node_47211_vault.dat");
    std::remove("node_47212_vault.dat");
    return pass ? 0 : 1;
}

int run_mic_test(const Args& args) {
    p2p::VoiceEngine voice;
    std::string err;
    if (!voice.init(true, err, true)) {
        std::fprintf(stderr, "mic init: %s\n", err.c_str());
        return 1;
    }
    if (!voice.start_mic_test(args.mic_path, args.mic_seconds, err)) {
        std::fprintf(stderr, "mic test: %s\n", err.c_str());
        voice.shutdown();
        return 1;
    }
    std::printf("Recording %d s of microphone audio to %s (no playback)...\n",
                args.mic_seconds, args.mic_path.c_str());
    std::this_thread::sleep_for(std::chrono::seconds(args.mic_seconds + 1));
    voice.stop_mic_test();
    auto stats = voice.mic_test_stats();
    voice.shutdown();

    const std::string meta_path = args.mic_path + ".json";
    std::ofstream meta(meta_path);
    if (meta) {
        meta << "{\n"
             << "  \"wav\": \""
             << std::filesystem::path(args.mic_path).filename().string() << "\",\n"
             << "  \"duration_s\": " << args.mic_seconds << ",\n"
             << "  \"sample_rate\": 48000,\n"
             << "  \"channels\": 1,\n"
             << "  \"samples\": " << stats.samples << ",\n"
             << "  \"rms_dbfs\": " << std::fixed << std::setprecision(2)
             << stats.rms_dbfs << ",\n"
             << "  \"peak\": " << stats.peak << ",\n"
             << "  \"clipped_samples\": " << stats.clipped << "\n"
             << "}\n";
    }
    std::printf("Microphone test finished: %s\n", args.mic_path.c_str());
    std::printf("  RMS %.2f dBFS, peak %d, clipped %llu; metrics: %s\n",
                stats.rms_dbfs, stats.peak,
                static_cast<unsigned long long>(stats.clipped), meta_path.c_str());
    return 0;
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

void write_report(const std::string& path, p2p::Node& node, const p2p::NodeId& peer,
                  bool handshake, bool file_ok, const std::string& note,
                  const std::vector<std::string>& logs) {
    if (path.empty()) return;
    std::ofstream out(path);
    if (!out) return;
    const auto s = node.stats();
    out << "{\n";
    out << "  \"node_id\": \"" << node.identity().id.hex() << "\",\n";
    out << "  \"peer_id\": \"" << peer.hex() << "\",\n";
    out << "  \"handshake\": " << (handshake ? "true" : "false") << ",\n";
    out << "  \"file_complete\": " << (file_ok ? "true" : "false") << ",\n";
    out << "  \"note\": \"" << json_escape(note) << "\",\n";
    out << "  \"stats\": {\"tx\": " << s.tx_packets << ", \"rx\": " << s.rx_packets
        << ", \"relay_forwarded\": " << s.relay_forwarded
        << ", \"chat_sent\": " << s.chat_sent << ", \"chat_recv\": " << s.chat_recv
        << ", \"chat_acked\": " << s.chat_acked << ", \"voice_sent\": " << s.voice_sent
        << ", \"voice_recv\": " << s.voice_recv << "},\n";
    out << "  \"files\": [";
    auto files = node.file_transfers();
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        if (i) out << ",";
        out << "{\"name\":\"" << json_escape(f.name) << "\",\"size\":" << f.size
            << ",\"transferred\":" << f.transferred << ",\"outgoing\":"
            << (f.outgoing ? "true" : "false") << ",\"complete\":"
            << (f.complete ? "true" : "false") << ",\"failed\":"
            << (f.failed ? "true" : "false") << ",\"error\":\""
            << json_escape(f.error) << "\",\"started_ms\":" << f.started_ms
            << ",\"duration_ms\":"
            << (f.started_ms && f.updated_ms >= f.started_ms ? f.updated_ms - f.started_ms : 0)
            << "}";
    }
    out << "],\n";
    out << "  \"logs\": [";
    for (size_t i = 0; i < logs.size(); ++i) {
        if (i) out << ",";
        out << "\"" << json_escape(logs[i]) << "\"";
    }
    out << "]\n}\n";
}

int run_headless(const Args& args) {
    p2p::Node node;
    p2p::StatsRecorder rec;  // declared after node: stops (and snapshots) first
    std::string err;
    bool stats_ok = false;
    if (args.stats) {   // a recorder problem must never stop the app itself
        stats_ok = rec.start(args.stats_dir,
                             exe_dir_utf8(args.cfg.self_exe.empty() ? "." : args.cfg.self_exe.c_str()),
                             args.cfg.alias, args.cmdline, err);
        if (!stats_ok) std::fprintf(stderr, "--stats disabled: %s\n", err.c_str());
    }
    p2p::NodeConfig nc;
    nc.alias = args.cfg.alias;
    nc.alias_locked = args.cfg.alias_locked;
    nc.port = args.cfg.port;
    nc.keyfile = args.cfg.keyfile;
    nc.bootstrap_host = args.cfg.bootstrap_host;
    nc.bootstrap_port = args.cfg.bootstrap_port;
    nc.bootstrap_list = args.cfg.bootstrap_list;
    nc.relay_only = args.relay_only;
    if (!node.start(nc, err)) {
        std::fprintf(stderr, "start: %s\n", err.c_str());
        return 1;
    }
    if (stats_ok) rec.attach(&node);
    if (args.auto_accept) {
        node.set_auto_accept_files(true, &err);
    }
    if (!args.download_dir.empty()) {
        node.set_download_dir(args.download_dir);
    }
    std::printf("NODE_ID=%s\n", node.identity().id.hex().c_str());
    std::printf("INVITE=%s\n", node.invite_uri().c_str());
    std::fflush(stdout);

    p2p::NodeId peer;
    if (!args.peer_id_hex.empty() && !p2p::NodeId::parse_hex(args.peer_id_hex, peer)) {
        std::fprintf(stderr, "invalid --peer-id\n");
        rec.stop();
        node.stop();
        return 2;
    }
    p2p::NodeId relay;
    if (!args.relay_id_hex.empty() && !p2p::NodeId::parse_hex(args.relay_id_hex, relay)) {
        std::fprintf(stderr, "invalid --relay-id\n");
        rec.stop();
        node.stop();
        return 2;
    }

    // --run-seconds governs passive nodes (for example a relay or a peer
    // waiting for a GUI); an active send/connect extends the budget by
    // --wait-ms so the operation can finish.
    const bool active = !args.peer_host.empty() || !args.send_chat_text.empty() ||
                        !args.send_file_path.empty() || !args.relay_id_hex.empty() ||
                        !args.cfg.bootstrap_host.empty() || args.voice_bench > 0;
    uint64_t budget_ms = static_cast<uint64_t>(std::max(1, args.run_seconds)) * 1000;
    if (active) budget_ms = std::max(budget_ms, static_cast<uint64_t>(std::max(1000, args.wait_ms)));
    if (args.voice_bench > 0)
        budget_ms = std::max(budget_ms,
                             static_cast<uint64_t>(args.voice_bench + 15) * 1000);
    const uint64_t deadline = p2p::now_ms() + budget_ms;
    auto peer_ready = [&]() {
        if (!peer.empty()) {
            p2p::Peer p;
            return node.get_peer(peer, p) && p.handshake_done;
        }
        int handshakes = 0;
        for (const auto& p : node.peers()) {
            if (!p.handshake_done) continue;
            ++handshakes;
            // A relayed peer is the conversation partner, not the bootstrap
            // relay that only forwards the handshake.
            if (p.relayed) return true;
        }
        // With a bootstrap the first handshake is normally the bootstrap
        // node; a passive receiver must wait for the second (the caller).
        return handshakes >= (args.cfg.bootstrap_host.empty() ? 1 : 2);
    };

    if (!args.peer_host.empty() && args.peer_port && !(args.relay_only && !relay.empty())) {
        if (peer.empty())
            node.connect_to(args.peer_host, args.peer_port);
        else
            node.connect_expected(args.peer_host, args.peer_port, peer);
    }

    if (!relay.empty()) {
        while (p2p::now_ms() < deadline) {
            p2p::Peer p;
            if (node.get_peer(relay, p) && p.handshake_done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        node.set_relay(relay);
        if (!peer.empty() && !peer_ready()) {
            std::string rerr;
            if (!node.connect_via_relay(relay, peer, &rerr))
                std::fprintf(stderr, "relay connect: %s\n", rerr.c_str());
        }
    }

    while (p2p::now_ms() < deadline && !peer_ready() &&
           (!args.peer_host.empty() || !args.cfg.bootstrap_host.empty() || !peer.empty() ||
            !args.send_chat_text.empty() || !args.send_file_path.empty() ||
            args.voice_bench > 0))
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    bool handshake = peer_ready();
    if (peer.empty()) {
        // Prefer a relayed conversation peer over the bootstrap relay.
        for (const auto& p : node.peers()) {
            if (p.handshake_done && p.relayed && p.id != relay) {
                peer = p.id;
                handshake = true;
                break;
            }
        }
        if (peer.empty()) {
            for (const auto& p : node.peers()) {
                if (p.handshake_done && p.id != relay) {
                    peer = p.id;
                    handshake = true;
                    break;
                }
            }
        }
    }
    bool file_ok = false;
    bool chat_ok = true;
    std::string note;
    if (!args.send_chat_text.empty()) {
        if (peer.empty() || !handshake) {
            chat_ok = false;
            note += "chat not sent: peer is not ready; ";
        } else {
            std::string serr;
            if (!node.send_chat(peer, args.send_chat_text, &serr)) {
                chat_ok = false;
                note += "chat send failed: " + serr + "; ";
            } else {
                note += "chat sent; ";
            }
        }
    }
    if (!args.send_file_path.empty()) {
        if (peer.empty() || !handshake) {
            note += "file not sent: peer is not ready; ";
        } else {
            std::string ferr;
            if (!node.send_file(peer, args.send_file_path, &ferr)) {
                note += "file start failed: " + ferr + "; ";
            } else {
                while (p2p::now_ms() < deadline) {
                    bool any = false;
                    bool done = false;
                    bool failed = false;
                    for (const auto& f : node.file_transfers()) {
                        if (!f.outgoing) continue;
                        any = true;
                        if (f.complete) done = true;
                        if (f.failed) failed = true;
                    }
                    if (done || failed) {
                        file_ok = done;
                        note += done ? "file complete; " : "file failed; ";
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    (void)any;
                }
                if (!file_ok) {
                    // Receiver-side report is also useful when the sender is remote.
                    for (const auto& f : node.file_transfers())
                        if (!f.outgoing && f.complete) file_ok = true;
                }
            }
        }
    }

    // Group chat scenario.  The creator waits for --group-members sessions and
    // invites them; every node with --group-send waits until it is in a group
    // with the expected size, posts K numbered messages and keeps running so
    // it receives the others' messages until the deadline.
    if (!args.group_create.empty()) {
        while (p2p::now_ms() < deadline) {
            std::vector<p2p::NodeId> members;
            for (const auto& p : node.peers())
                if (p.handshake_done) members.push_back(p.id);
            if (static_cast<int>(members.size()) >= args.group_members) {
                p2p::NodeId gid;
                std::string gerr;
                if (node.create_group(args.group_create, members, gid, &gerr))
                    std::printf("GROUP_CREATED %s members=%zu\n", gid.hex().c_str(),
                                members.size() + 1);
                else
                    std::printf("GROUP_CREATE_FAILED %s\n", gerr.c_str());
                std::fflush(stdout);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
    if (!args.group_file.empty()) {
        p2p::NodeId gid;
        while (p2p::now_ms() < deadline && gid.empty()) {
            const std::string want = !args.group_name.empty() ? args.group_name : args.group_create;
            for (const auto& g : node.groups())
                if (static_cast<int>(g.members.size()) >= args.group_members + 1 &&
                    (want.empty() || g.name == want))
                    gid = g.id;
            if (gid.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!gid.empty()) {
            std::string ferr;
            int n = node.send_file_group(gid, args.group_file, &ferr);
            std::printf("GROUP_FILE started=%d %s\n", n, ferr.c_str());
            std::fflush(stdout);
            while (p2p::now_ms() < deadline) {
                bool pending = false;
                for (const auto& f : node.file_transfers())
                    if (f.outgoing && f.group == gid && !f.complete && !f.failed) pending = true;
                if (!pending) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            int ok = 0, bad = 0;
            for (const auto& f : node.file_transfers())
                if (f.outgoing && f.group == gid) (f.complete ? ok : bad)++;
            std::printf("GROUP_FILE_DONE ok=%d failed=%d\n", ok, bad);
            std::fflush(stdout);
        }
    }
    if (!args.group_send.empty()) {
        p2p::NodeId gid;
        while (p2p::now_ms() < deadline && gid.empty()) {
            const std::string want = !args.group_name.empty() ? args.group_name : args.group_create;
            for (const auto& g : node.groups())
                if (static_cast<int>(g.members.size()) >= args.group_members + 1 &&
                    (want.empty() || g.name == want))
                    gid = g.id;
            if (gid.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!gid.empty()) {
            // Small random offset so all members do not post in the same tick.
            std::this_thread::sleep_for(std::chrono::milliseconds(500 + randombytes_uniform(1500)));
            for (int i = 1; i <= args.group_count && p2p::now_ms() < deadline; ++i) {
                std::string gerr;
                node.send_group_chat(gid, args.group_send + " #" + std::to_string(i), &gerr);
                std::this_thread::sleep_for(std::chrono::milliseconds(args.group_interval_ms));
            }
            std::printf("GROUP_SENT %d\n", args.group_count);
        } else {
            std::printf("GROUP_SEND_SKIPPED no group\n");
        }
        std::fflush(stdout);
    }

    // Voice transport benchmark: stream real Opus voice packets through the
    // node's encrypted voice path and decode them on the receiving side, so
    // jitter/loss come from VoiceEngine exactly as in a live call.
    if (args.voice_bench > 0 && !peer.empty() && handshake) {
        p2p::VoiceEngine ve;
        std::string verr;
        if (!ve.init(false, verr)) {
            std::fprintf(stderr, "voice-bench: engine init failed: %s\n", verr.c_str());
        } else {
            // Ordering diagnostic.  VoiceStats::lost cannot tell a gap from a
            // frame that arrived after its playout slot, so track the highest
            // sequence seen and count anything that arrives below it.
            std::atomic<int64_t> rx_max_seq{-1};
            std::atomic<uint64_t> rx_reordered{0};
            node.set_voice_handler([&](const p2p::NodeId& id, uint32_t seq,
                                       const std::vector<uint8_t>& opus) {
                if (opus.empty()) return;
                const int64_t prev = rx_max_seq.load(std::memory_order_relaxed);
                if (prev >= 0 && static_cast<int64_t>(seq) < prev)
                    rx_reordered.fetch_add(1, std::memory_order_relaxed);
                else if (static_cast<int64_t>(seq) > prev)
                    rx_max_seq.store(seq, std::memory_order_relaxed);
                ve.on_packet(id, seq, opus);
            });

            std::vector<std::vector<uint8_t>> frames;
            if (!make_voice_frames(200, frames)) {
                std::fprintf(stderr, "voice-bench: cannot encode Opus frames\n");
                ve.shutdown();
                rec.stop();
        node.stop();
                return 1;
            }

            uint64_t tx = 0;
            const int total = args.voice_bench * 50;  // 20 ms cadence
            auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < total; ++i) {
                std::this_thread::sleep_until(t0 + std::chrono::milliseconds(20 * i));
                const auto& fr = frames[static_cast<size_t>(i) % frames.size()];
                if (node.send_voice(peer, static_cast<uint32_t>(i), fr)) ++tx;
            }
            // Let in-flight packets arrive before reading the counters.
            for (int i = 0; i < 25; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));

            const p2p::VoiceStats vs = ve.stats();
            // Network loss is tx - rx.  VoiceStats::lost also counts local
            // jitter-buffer overflows, so keep it separate from link loss.
            const uint64_t net_lost =
                tx > vs.rx_packets ? tx - vs.rx_packets : 0;
            const double net_loss_pct =
                tx ? 100.0 * static_cast<double>(net_lost) / static_cast<double>(tx)
                   : 0.0;
            std::printf(
                "VOICE_BENCH {\"seconds\":%d,\"tx\":%llu,\"rx\":%llu,"
                "\"net_lost\":%llu,\"net_loss_pct\":%.2f,\"reordered\":%llu,"
                "\"frames_lost\":%u,\"rx_gap_lost\":%u,\"jitter_overflow\":%u,"
                "\"playout_skips\":%u,\"fec_recovered\":%u,\"plc_frames\":%u,"
                "\"peers\":%u,\"last_seq\":%u,"
                "\"jitter_ms\":%.2f,\"bitrate_kbps\":%.2f}\n",
                args.voice_bench, static_cast<unsigned long long>(tx),
                static_cast<unsigned long long>(vs.rx_packets),
                static_cast<unsigned long long>(net_lost), net_loss_pct,
                static_cast<unsigned long long>(rx_reordered.load()),
                vs.lost, vs.rx_gap_lost, vs.jitter_overflow, vs.playout_skips,
                vs.fec_recovered, vs.plc_frames,
                vs.peers, vs.last_seq, vs.jitter_ms, vs.bitrate_kbps);
            std::fflush(stdout);
            ve.shutdown();
        }
    } else if (args.voice_bench > 0) {
        std::fprintf(stderr, "voice-bench: skipped, no handshake with the peer\n");
    }

    // Passive nodes run for the whole budget; an active send that already
    // finished (or failed) exits right away so timings are not inflated.
    uint64_t now = p2p::now_ms();
    const bool group_mode = !args.group_create.empty() || !args.group_send.empty();
    // A node that only connects (--bootstrap / --peer) but sends nothing is a
    // receiver: it must stay up so relayed chat/file traffic can reach it.
    const bool has_send_job = !args.send_chat_text.empty() || !args.send_file_path.empty() ||
                              args.voice_bench > 0;
    if (now < deadline && (!active || group_mode || !has_send_job))
        std::this_thread::sleep_for(std::chrono::milliseconds(deadline - now));

    for (const auto& f : node.file_transfers())
        if (!f.outgoing && f.complete) file_ok = true;

    const auto logs = node.take_logs();
    write_report(args.report_path, node, peer, handshake, file_ok, note, logs);
    if (!args.report_txt_path.empty() &&
        !p2p::write_text_report(args.report_txt_path, node)) {
        std::fprintf(stderr, "cannot write text report: %s\n",
                     args.report_txt_path.c_str());
    }
    for (const auto& line : logs) std::printf("%s\n", line.c_str());
    rec.stop();
        node.stop();
    return ((args.send_chat_text.empty() || chat_ok) &&
            (args.send_file_path.empty() || file_ok))
               ? 0
               : 3;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
#endif

    Args args;
    try {
        if (!parse_args(argc, argv, args)) return 1;
    } catch (const std::exception& e) {
        // std::stoi & co. used to terminate the program with "terminate called ..." on a bad value
        std::fprintf(stderr, "invalid command-line value: %s\n", e.what());
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--key" || a == "--import-key" || a == "--export-key") {
            args.cmdline += a + " <hidden> ";
            ++i;
        } else {
            args.cmdline += a + " ";
        }
    }

    if (!args.selftest && args.export_key.empty() && args.import_key.empty()) {
        std::filesystem::path kit = executable_dir(argv[0]) / "testkit";
        ensure_testkit(kit);
        args.cfg.testkit_dir = kit.string();
        std::error_code ec;
        args.cfg.self_exe = std::filesystem::absolute(argv[0], ec).string();
#ifdef _WIN32
        char mod[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, mod, MAX_PATH) > 0) args.cfg.self_exe = mod;
#else
        auto real = std::filesystem::read_symlink("/proc/self/exe", ec);
        if (!ec) args.cfg.self_exe = real.string();
#endif
    }

    int rc = 0;
    if (!args.export_key.empty()) {
        p2p::Identity id;
        std::string err;
        if (!id.init(args.cfg.keyfile, args.cfg.alias, err)) {
            std::fprintf(stderr, "init: %s\n", err.c_str());
            return 1;
        }
        std::string passphrase;
        std::fprintf(stderr, "Enter passphrase: ");
        std::getline(std::cin, passphrase);
        if (!p2p::export_key(id, passphrase, args.export_key, err)) {
            std::fprintf(stderr, "export failed: %s\n", err.c_str());
            return 1;
        }
        std::printf("Identity exported to %s\n", args.export_key.c_str());
    } else if (!args.import_key.empty()) {
        std::string passphrase;
        std::fprintf(stderr, "Enter passphrase: ");
        std::getline(std::cin, passphrase);
        p2p::Identity id;
        std::string err;
        if (!p2p::import_key(passphrase, args.import_key, id, err)) {
            std::fprintf(stderr, "import failed: %s\n", err.c_str());
            return 1;
        }
        // Save as regular keyfile
        std::array<uint8_t, 32> seed{};
        std::copy(id.ed_sk.begin(), id.ed_sk.begin() + 32, seed.begin());
        if (!write_seed(args.cfg.keyfile, seed)) {
            std::fprintf(stderr, "failed to save keyfile\n");
            return 1;
        }
        std::printf("Identity imported and saved to %s\n", args.cfg.keyfile.c_str());
    } else if (args.selftest) {
        rc = run_selftest();
    } else if (args.record_mic) {
        rc = run_mic_test(args);
    } else if (args.headless) {
        rc = run_headless(args);
    } else {
        App app;
        std::string err;
        p2p::StatsRecorder rec;
        bool stats_ok = false;
        if (args.stats) {
            stats_ok = rec.start(args.stats_dir, exe_dir_utf8(argv[0]), args.cfg.alias, args.cmdline, err);
            if (!stats_ok) std::fprintf(stderr, "--stats disabled: %s\n", err.c_str());
        }
        if (!app.init(args.cfg, err)) {
            std::fprintf(stderr, "init: %s\n", err.c_str());
            app.shutdown();
            rc = 1;
        } else {
            if (stats_ok) rec.attach(&app.node(), [&app] { return app.voice_stats(); });
            app.run();
            rec.stop();
            app.shutdown();
        }
    }

#ifdef _WIN32
    WSACleanup();
#endif

    return rc;
}
