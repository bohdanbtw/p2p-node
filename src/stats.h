// --stats: field-test recorder.  Everything a thesis needs from a real user's
// run is written next to the executable, then packed into ONE zip the tester
// can send back.  No network upload, no chat text, no keys.
//
//   <dir>/environment.txt  OS, arch, build, command line (no secrets)
//   <dir>/events.log       every node log line (P2P_LOG_FILE mirror)
//   <dir>/metrics.csv      per-chat-message send/ack/retransmit (P2P_METRICS)
//   <dir>/timeseries.csv   2 s samples: traffic, peers, rtt, NAT, DHT, local ip
//   <dir>/peers.csv        per-peer snapshot every 5 s
//   <dir>/netevents.csv    local-ip / public-address / path / online changes,
//                          with reconnect time after an outage
//   <dir>/transfers.csv    finished file transfers with throughput
//   <dir>/voice.csv        voice quality samples while a call is active
//   <dir>/summary.json     aggregate numbers, rewritten every 30 s
//   <dir>/report.txt       the standard test report
//   <dir>.zip              all of the above (store-only zip)
#pragma once

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <net/if.h>
#endif

#include "mainline_dht.h"
#include "net.h"
#include "report.h"
#include "voice.h"

namespace p2p {

// How this machine reaches the internet right now.  No SSID or MAC address is
// recorded; "hotspot_hint" only says the gateway looks like a phone tether.
struct NetInfo {
    std::string iface;          // adapter name
    std::string type = "unknown";  // ethernet | wifi | cellular | vpn | other | unknown
    long link_mbps = 0;
    std::string gateway;
    bool hotspot_hint = false;
    int wifi_signal_dbm = 0;    // 0 = unknown (Linux only)
    std::string brief() const {
        std::string o = type + "/" + (iface.empty() ? "?" : iface);
        if (link_mbps) o += "/" + std::to_string(link_mbps) + "Mbps";
        if (hotspot_hint) o += "/hotspot?";
        return o;
    }
};

inline NetInfo query_net_info(const std::string& local_ip) {
    NetInfo ni;
    if (local_ip.empty()) return ni;
#ifdef _WIN32
    ULONG len = 32768;
    std::vector<unsigned char> buf(len);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                              GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, aa, &len);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(len);
        aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        rc = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST |
                                               GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, aa, &len);
    }
    if (rc != NO_ERROR) return ni;
    for (auto* a = aa; a; a = a->Next) {
        bool mine = false;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET) {
                char b[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr,
                          b, sizeof(b));
                if (local_ip == b) mine = true;
            }
        }
        if (!mine) continue;
        char name[260] = {0};
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, name, sizeof(name) - 1, nullptr,
                            nullptr);
        ni.iface = name;
        switch (a->IfType) {
            case IF_TYPE_ETHERNET_CSMACD: ni.type = "ethernet"; break;
            case IF_TYPE_IEEE80211: ni.type = "wifi"; break;
            case 243:  // IF_TYPE_WWANPP
            case 244:  // IF_TYPE_WWANPP2
                ni.type = "cellular"; break;
            case IF_TYPE_PPP:
            case IF_TYPE_TUNNEL: ni.type = "vpn"; break;
            default: ni.type = "other"; break;
        }
        ni.link_mbps = (a->TransmitLinkSpeed == ~0ULL) ? 0 : static_cast<long>(a->TransmitLinkSpeed / 1000000ULL);
        if (a->FirstGatewayAddress && a->FirstGatewayAddress->Address.lpSockaddr) {
            char b[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET,
                      &reinterpret_cast<sockaddr_in*>(a->FirstGatewayAddress->Address.lpSockaddr)->sin_addr,
                      b, sizeof(b));
            ni.gateway = b;
        }
        break;
    }
#else
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return ni;
    for (auto* i = ifa; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        char b[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, b, sizeof(b));
        if (local_ip != b) continue;
        ni.iface = i->ifa_name;
        break;
    }
    if (ifa) freeifaddrs(ifa);
    if (!ni.iface.empty()) {
        namespace fs = std::filesystem;
        const std::string base = "/sys/class/net/" + ni.iface;
        std::error_code ec;
        const std::string& n = ni.iface;
        if (fs::exists(base + "/wireless", ec) || n.rfind("wl", 0) == 0) ni.type = "wifi";
        else if (n.rfind("ww", 0) == 0 || n.rfind("rmnet", 0) == 0) ni.type = "cellular";
        else if (n.rfind("tun", 0) == 0 || n.rfind("wg", 0) == 0 || n.rfind("ppp", 0) == 0)
            ni.type = "vpn";
        else if (n.rfind("en", 0) == 0 || n.rfind("eth", 0) == 0) ni.type = "ethernet";
        else ni.type = "other";
        std::ifstream sp(base + "/speed");
        long sp_mbps = 0;
        if (sp >> sp_mbps && sp_mbps > 0) ni.link_mbps = sp_mbps;
        // default gateway for this interface
        std::ifstream rt("/proc/net/route");
        std::string line;
        std::getline(rt, line);
        while (std::getline(rt, line)) {
            std::istringstream ls(line);
            std::string ifn, dest, gw;
            ls >> ifn >> dest >> gw;
            if (ifn == ni.iface && dest == "00000000") {
                unsigned long g = 0;
                try { g = std::stoul(gw, nullptr, 16); } catch (...) { continue; }
                char gb[INET_ADDRSTRLEN] = {0};
                in_addr ia{};
                ia.s_addr = static_cast<in_addr_t>(g);
                inet_ntop(AF_INET, &ia, gb, sizeof(gb));
                ni.gateway = gb;
                break;
            }
        }
        if (ni.type == "wifi") {
            std::ifstream w("/proc/net/wireless");
            std::string wl;
            std::getline(w, wl);
            std::getline(w, wl);
            while (std::getline(w, wl)) {
                std::istringstream ls(wl);
                std::string ifn, st;
                double link = 0, level = 0;
                ls >> ifn >> st >> link >> level;
                if (ifn == ni.iface + ":") ni.wifi_signal_dbm = static_cast<int>(level);
            }
        }
    }
#endif
    // Android tethering uses 192.168.43.x / 192.168.49.x, iOS 172.20.10.x.
    ni.hotspot_hint = ni.gateway.rfind("192.168.43.", 0) == 0 ||
                      ni.gateway.rfind("192.168.49.", 0) == 0 ||
                      ni.gateway.rfind("172.20.10.", 0) == 0;
    return ni;
}

class StatsRecorder {
  public:
    ~StatsRecorder() { stop(); }

    // Create the output folder and arm the log/metrics mirrors.  Must run
    // BEFORE Node::start (those mirrors read their env variables lazily).
    // Never throws: tries the requested folder, then an ASCII-only fallback folder (a Cyrillic
    // user/folder name on a non-Cyrillic Windows cannot be passed through the narrow-char
    // environment variables and fopen() that the log mirrors use).
    bool start(const std::string& dir_arg, const std::string& exe_dir, const std::string& alias,
               const std::string& cmdline, std::string& err) {
        try {
            if (start_in(dir_arg, exe_dir, alias, cmdline, err)) return true;
        } catch (const std::exception& e) {
            err = e.what();
        } catch (...) {
            err = "unknown error";
        }
        try {
            std::string fb;
#ifdef _WIN32
            const char* pub = std::getenv("PUBLIC");
            fb = std::string(pub ? pub : "C:") + "\\mkr_stats";
#else
            fb = (std::filesystem::temp_directory_path() / "mkr_stats").string();
#endif
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::u8path(fb), ec);
            std::string err2;
            if (start_in("", fb, alias, cmdline, err2)) {
                std::fprintf(stderr, "[stats] using fallback folder %s (%s)\n", fb.c_str(), err.c_str());
                err.clear();
                return true;
            }
            err += "; fallback failed: " + err2;
        } catch (const std::exception& e) {
            err += std::string("; fallback failed: ") + e.what();
        } catch (...) {
        }
        return false;
    }

    bool start_in(const std::string& dir_arg, const std::string& exe_dir, const std::string& alias,
                  const std::string& cmdline, std::string& err) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path base;
        if (!dir_arg.empty()) {
            base = fs::absolute(fs::u8path(dir_arg), ec);
        } else {
            std::time_t t = std::time(nullptr);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char ts[32];
            std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &tm);
            std::string a;
            for (char c : alias) a += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            base = fs::u8path(exe_dir) / ("mkr_stats_" + a + "_" + ts);
        }
#ifdef _WIN32
        for (unsigned char ch : base.u8string())
            if (ch >= 0x80) {   // narrow env/fopen would mangle it
                err = "path is not ASCII: " + base.u8string();
                return false;
            }
#endif
        fs::create_directories(base, ec);
        if (ec) {
            err = "cannot create " + base.u8string() + ": " + ec.message();
            return false;
        }
        dir_ = base;
        exe_dir_ = exe_dir;
        started_unix_ms_ = unix_ms();
        started_ms_ = now_ms();
        set_env("P2P_LOG_FILE", (dir_ / "events.log").u8string());
        set_env("P2P_METRICS", (dir_ / "metrics.csv").u8string());
        write_environment(alias, cmdline);
        write_readme();
        open_csv(ts_, "timeseries.csv",
                 "unix_ms,uptime_s,local_ip,conn_type,iface,link_mbps,gateway,hotspot_hint,wifi_dbm,public_ip,public_port,nat,peers_total,peers_online,"
                 "peers_relayed,rtt_min_ms,rtt_max_ms,tx_packets,rx_packets,tx_bytes,rx_bytes,"
                 "chat_sent,chat_recv,chat_acked,voice_sent,voice_recv,group_msgs,dropped,"
                 "handshake_ok,handshake_rejected,mitm_rejected,replay_hits,relay_forwarded,"
                 "dht_nodes,dht_announce_attempts,dht_announce_confirmed,dht_values_found");
        open_csv(peers_csv_, "peers.csv",
                 "unix_ms,peer,endpoint,rtt_ms,path,nat,online,handshake");
        open_csv(ev_, "netevents.csv", "unix_ms,uptime_s,event,subject,detail");
        open_csv(xfer_, "transfers.csv",
                 "unix_ms,direction,peer,size_bytes,transferred_bytes,duration_ms,"
                 "throughput_kbps,ok,error");
        open_csv(voice_csv_, "voice.csv",
                 "unix_ms,tx,rx,lost,rx_gap_lost,jitter_ms,bitrate_kbps,loss_pct,fec_recovered,"
                 "plc_frames,playout_skips,jitter_overflow,buffer_target,vad_skipped,mic_db,capture,playback");
        return true;
    }

    // Start sampling.  `voice` may be empty (headless without audio).
    void attach(Node* node, std::function<VoiceStats()> voice = {}) {
        node_ = node;
        voice_fn_ = std::move(voice);
        running_ = true;
        th_ = std::thread([this] { loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        try {
            uint64_t dummy = 0;
            sample(dummy);  // final sample so short runs still record their totals
            snapshot(true);
        } catch (...) {
        }
        std::fprintf(stderr, "[stats] saved: %s.zip\n", dir_.u8string().c_str());
    }

    const std::filesystem::path& dir() const { return dir_; }

  private:
    struct PeerTrack {
        bool online = false;
        uint64_t offline_since_ms = 0;
        bool seen_online = false;
        std::string endpoint;
        std::string path;
        uint64_t rtt_n = 0;
        double rtt_sum = 0;
        uint32_t rtt_min = 0, rtt_max = 0;
        std::string alias;
    };

    static void set_env(const char* k, const std::string& v) {
#ifdef _WIN32
        _putenv_s(k, v.c_str());
#else
        ::setenv(k, v.c_str(), 1);
#endif
    }

    static std::string csv_escape(const std::string& s) {
        std::string o;
        for (char c : s) o += (c == ',' || c == '\n' || c == '\r') ? ' ' : c;
        return o;
    }

    void open_csv(std::ofstream& f, const char* name, const char* header) {
        f.open(dir_ / name, std::ios::trunc);
        if (f) f << header << "\n";
    }

    std::string local_ip() const {
        // The address the OS would use to reach the internet: changes when the
        // user moves between ethernet, wifi and a hotspot.  UDP connect sends
        // nothing.
        socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (s == kInvalidSocket) return "";
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(53);
        inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);  // TEST-NET, never contacted
        std::string out;
        if (::connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
            sockaddr_in me{};
#ifdef _WIN32
            int len = sizeof(me);
#else
            socklen_t len = sizeof(me);
#endif
            if (::getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
                char buf[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf));
                out = buf;
            }
        }
#ifdef _WIN32
        ::closesocket(s);
#else
        ::close(s);
#endif
        return out;
    }

    void event(const std::string& ev, const std::string& subject, const std::string& detail) {
        if (!ev_) return;
        ev_ << unix_ms() << "," << (now_ms() - started_ms_) / 1000.0 << "," << ev << ","
            << csv_escape(subject) << "," << csv_escape(detail) << "\n";
        ev_.flush();
    }

    void loop() {
        uint64_t last_peers = 0, last_snap = 0;
        std::unique_lock<std::mutex> lk(cv_mtx_);
        while (running_) {
            try {   // the recorder must never take the app down
                sample(last_peers);
                uint64_t t = now_ms();
                if (t - last_snap > 30000) {
                    last_snap = t;
                    snapshot(false);
                }
            } catch (...) {
            }
            cv_.wait_for(lk, std::chrono::seconds(2), [&] { return !running_.load(); });
        }
    }

    void sample(uint64_t& last_peers_ms) {
        if (!node_) return;
        const uint64_t now = now_ms();
        const NodeStats st = node_->stats();
        const NatProfile nat = node_->nat_profile();
        const auto peers = node_->peers();
        const auto dht = mainline_dht_stats();
        const std::string lip = local_ip();

        NetInfo ni = query_net_info(lip);
        if (lip.empty() && !last_local_ip_.empty() && !net_down_) {
            net_down_ = true;   // Wi-Fi off / cable pulled: record the start of the outage
            event("network_down", last_local_ip_, "no usable local address");
        }
        if (!lip.empty() && net_down_) net_down_ = false;
        if (!lip.empty() && (lip != last_local_ip_ || ni.iface != last_ni_.iface)) {
            event("local_ip_change", last_local_ip_.empty() ? "start" : last_local_ip_,
                  lip + " via " + ni.brief() + (ni.gateway.empty() ? "" : " gw=" + ni.gateway) +
                      (last_ni_.iface.empty() ? "" : " (was " + last_ni_.brief() + ")"));
            last_local_ip_ = lip;
        }
        last_ni_ = ni;
        const std::string pub = nat.mapped_ip + ":" + std::to_string(nat.mapped_port);
        if (nat.discovered && pub != last_public_) {
            event("public_addr_change", last_public_.empty() ? "start" : last_public_, pub);
            last_public_ = pub;
        }
        if (st.handshake_ok > last_handshake_) {
            event("handshake", "", "total=" + std::to_string(st.handshake_ok));
            last_handshake_ = st.handshake_ok;
        }

        int online = 0, relayed = 0;
        uint32_t rmin = 0, rmax = 0;
        bool dump_peers = now - last_peers_ms > 5000;
        if (dump_peers) last_peers_ms = now;
        for (const auto& p : peers) {
            if (!p.handshake_done) continue;
            auto& tr = track_[p.id.hex()];
            tr.alias = p.alias;
            const bool on = p.online(now);
            const std::string ep = endpoint_to_string(p.addr);
            const std::string path = p.relayed ? "relay" : (p.direct ? "direct" : "indirect");
            if (on) {
                ++online;
                if (p.relayed) ++relayed;
                if (p.rtt_ms) {
                    rmin = rmin ? std::min(rmin, p.rtt_ms) : p.rtt_ms;
                    rmax = std::max(rmax, p.rtt_ms);
                    ++tr.rtt_n;
                    tr.rtt_sum += p.rtt_ms;
                    tr.rtt_min = tr.rtt_min ? std::min(tr.rtt_min, p.rtt_ms) : p.rtt_ms;
                    tr.rtt_max = std::max(tr.rtt_max, p.rtt_ms);
                }
            }
            if (on && !tr.online) {
                std::string d = ep + " " + path;
                if (tr.seen_online && tr.offline_since_ms)
                    d += " outage_ms=" + std::to_string(now - tr.offline_since_ms);
                event(tr.seen_online ? "peer_reconnected" : "peer_online", p.id.short_hex(10), d);
                tr.online = true;
                tr.seen_online = true;
            } else if (!on && tr.online) {
                event("peer_offline", p.id.short_hex(10), ep + " " + path);
                tr.online = false;
                tr.offline_since_ms = now;
            }
            if (tr.seen_online && !tr.endpoint.empty() && ep != tr.endpoint)
                event("peer_endpoint_change", p.id.short_hex(10), tr.endpoint + " -> " + ep);
            if (tr.seen_online && !tr.path.empty() && path != tr.path)
                event("peer_path_change", p.id.short_hex(10), tr.path + " -> " + path);
            tr.endpoint = ep;
            tr.path = path;
            if (dump_peers && peers_csv_)
                peers_csv_ << unix_ms() << "," << p.id.short_hex(10) << "," << ep << ","
                           << p.rtt_ms << "," << path << "," << nat_name(p.nat) << ","
                           << (on ? 1 : 0) << ",1\n";
        }
        if (dump_peers && peers_csv_) peers_csv_.flush();

        ts_ << unix_ms() << "," << (now - started_ms_) / 1000.0 << "," << lip << ","
            << last_ni_.type << "," << csv_escape(last_ni_.iface) << "," << last_ni_.link_mbps
            << "," << last_ni_.gateway << "," << (last_ni_.hotspot_hint ? 1 : 0) << ","
            << last_ni_.wifi_signal_dbm << "," << nat.mapped_ip << "," << nat.mapped_port << "," << nat_name(nat.type) << ","
            << peers.size() << "," << online << "," << relayed << "," << rmin << "," << rmax
            << "," << st.tx_packets << "," << st.rx_packets << "," << st.tx_bytes << ","
            << st.rx_bytes << "," << st.chat_sent << "," << st.chat_recv << ","
            << st.chat_acked << "," << st.voice_sent << "," << st.voice_recv << ","
            << st.group_msgs << "," << st.dropped << "," << st.handshake_ok << ","
            << st.handshake_rejected << "," << st.mitm_rejected << "," << st.replay_hits << ","
            << st.relay_forwarded << "," << dht.good_nodes << "," << dht.announce_attempts
            << "," << dht.announce_confirmed_ms << "," << dht.values_found << "\n";
        ts_.flush();
        last_stats_ = st;
        last_dht_ = dht;

        for (const auto& f : node_->file_transfers()) {
            if (!(f.complete || f.failed) || logged_files_.count(f.id)) continue;
            logged_files_[f.id] = true;
            uint64_t dur = f.updated_ms >= f.started_ms ? f.updated_ms - f.started_ms : 0;
            double kbps = dur ? (f.transferred * 8.0) / dur : 0;  // bits/ms == kbit/s
            xfer_ << unix_ms() << "," << (f.outgoing ? "out" : "in") << ","
                  << f.peer.short_hex(10) << "," << f.size << "," << f.transferred << ","
                  << dur << "," << kbps << "," << (f.complete ? 1 : 0) << ","
                  << csv_escape(f.error) << "\n";
            xfer_.flush();
            ++files_done_;
            if (f.complete) {
                ++files_ok_;
                bytes_ok_ += f.size;
                ms_ok_ += dur;
            }
        }

        if (voice_fn_) {
            VoiceStats v = voice_fn_();
            if (v.tx_packets || v.rx_packets) {
                voice_csv_ << unix_ms() << "," << v.tx_packets << "," << v.rx_packets << ","
                           << v.lost << "," << v.rx_gap_lost << "," << v.jitter_ms << ","
                           << v.bitrate_kbps << "," << v.packet_loss_pct << ","
                           << v.fec_recovered << "," << v.plc_frames << "," << v.playout_skips
                           << "," << v.jitter_overflow << "," << v.buffer_target << ","
                           << v.vad_skipped << "," << v.mic_level_db << ","
                           << csv_escape(v.capture_device) << "," << csv_escape(v.playback_device) << "\n";
                voice_csv_.flush();
                last_voice_ = v;
                voice_seen_ = true;
            }
        }
    }

    void write_summary() {
        std::ostringstream o;
        const double up = (now_ms() - started_ms_) / 1000.0;
        o << "{\n  \"tool\": \"p2pnode --stats\",\n  \"format\": 1,\n";
        o << "  \"started_unix_ms\": " << started_unix_ms_ << ",\n";
        o << "  \"uptime_s\": " << up << ",\n";
        o << "  \"local_ip_changes\": " << count_events("local_ip_change") << ",\n";
        o << "  \"public_addr_changes\": " << count_events("public_addr_change") << ",\n";
        const NodeStats& s = last_stats_;
        o << "  \"counters\": {\"tx_packets\": " << s.tx_packets << ", \"rx_packets\": "
          << s.rx_packets << ", \"tx_bytes\": " << s.tx_bytes << ", \"rx_bytes\": "
          << s.rx_bytes << ", \"chat_sent\": " << s.chat_sent << ", \"chat_recv\": "
          << s.chat_recv << ", \"chat_acked\": " << s.chat_acked << ", \"voice_sent\": "
          << s.voice_sent << ", \"voice_recv\": " << s.voice_recv << ", \"group_msgs\": "
          << s.group_msgs << ", \"dropped\": " << s.dropped << ", \"handshake_ok\": "
          << s.handshake_ok << ", \"handshake_rejected\": " << s.handshake_rejected
          << ", \"mitm_rejected\": " << s.mitm_rejected << ", \"replay_hits\": "
          << s.replay_hits << ", \"relay_forwarded\": " << s.relay_forwarded << "},\n";
        o << "  \"dht\": {\"good_nodes\": " << last_dht_.good_nodes
          << ", \"announce_attempts\": " << last_dht_.announce_attempts
          << ", \"announce_confirmed\": " << (last_dht_.announce_confirmed_ms ? "true" : "false")
          << ", \"values_found\": " << last_dht_.values_found << "},\n";
        o << "  \"files\": {\"finished\": " << files_done_ << ", \"ok\": " << files_ok_
          << ", \"avg_throughput_kbps\": "
          << (ms_ok_ ? (bytes_ok_ * 8.0) / ms_ok_ : 0.0) << "},\n";
        if (voice_seen_) {
            const VoiceStats& v = last_voice_;
            o << "  \"voice_last\": {\"tx\": " << v.tx_packets << ", \"rx\": " << v.rx_packets
              << ", \"lost\": " << v.lost << ", \"loss_pct\": " << v.packet_loss_pct
              << ", \"jitter_ms\": " << v.jitter_ms << ", \"bitrate_kbps\": "
              << v.bitrate_kbps << "},\n";
        }
        o << "  \"peers\": [";
        bool first = true;
        for (const auto& kv : track_) {
            const auto& t = kv.second;
            if (!t.seen_online) continue;
            o << (first ? "\n    " : ",\n    ") << "{\"id\": \"" << kv.first.substr(0, 10)
              << "\", \"rtt_avg_ms\": " << (t.rtt_n ? t.rtt_sum / t.rtt_n : 0.0)
              << ", \"rtt_min_ms\": " << t.rtt_min << ", \"rtt_max_ms\": " << t.rtt_max
              << ", \"path\": \"" << t.path << "\"}";
            first = false;
        }
        o << (first ? "" : "\n  ") << "]\n}\n";
        std::ofstream f(dir_ / "summary.json", std::ios::trunc);
        f << o.str();
    }

    int count_events(const std::string& name) {
        std::ifstream in(dir_ / "netevents.csv");
        std::string line;
        int n = 0;
        while (std::getline(in, line))
            if (line.find("," + name + ",") != std::string::npos) ++n;
        return n;
    }

    void snapshot(bool final_pass) {
        if (node_) {
            write_summary();
            write_text_report((dir_ / "report.txt").u8string(), *node_);
        }
        for (auto* f : {&ts_, &peers_csv_, &ev_, &xfer_, &voice_csv_})
            if (*f) f->flush();
        // Operator markers ("wifi off", "started call"): mark.bat / mark.sh append
        // "unix_ms,text" to marks.csv next to the exe; keep a copy in the bundle.
        {
            std::error_code mec;
            auto mk = std::filesystem::path(exe_dir_) / "marks.csv";
            if (std::filesystem::exists(mk, mec))
                std::filesystem::copy_file(mk, dir_ / "marks.csv",
                                           std::filesystem::copy_options::overwrite_existing, mec);
        }
        write_zip();
        (void)final_pass;
    }

    void write_environment(const std::string& alias, const std::string& cmdline) {
        std::ofstream f(dir_ / "environment.txt", std::ios::trunc);
        f << "p2pnode --stats environment\n";
        f << "generated: " << report_utc_now() << "\n";
        f << "alias: " << report_one_line(alias) << "\n";
#if defined(_WIN32)
        f << "os: windows\n";
#elif defined(__APPLE__)
        f << "os: macos\n";
#else
        f << "os: linux\n";
#endif
#if defined(__x86_64__) || defined(_M_X64)
        f << "arch: x86_64\n";
#elif defined(__aarch64__)
        f << "arch: aarch64\n";
#else
        f << "arch: other\n";
#endif
        f << "build: " << __DATE__ << " " << __TIME__ << "\n";
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char z[16];
        std::strftime(z, sizeof(z), "%z", &tm);
        f << "utc_offset: " << z << "\n";
        f << "cmdline: " << cmdline << "\n";
        {
            NetInfo ni = query_net_info(local_ip());
            f << "connection: " << ni.type << " iface=" << ni.iface << " link_mbps=" << ni.link_mbps
              << " gateway=" << ni.gateway << " hotspot_hint=" << (ni.hotspot_hint ? "yes" : "no")
              << " wifi_dbm=" << ni.wifi_signal_dbm << "\n";
        }
        f << "note: no chat text, keys or key files are recorded.\n";
    }

    void write_readme() {
        std::ofstream f(dir_ / "README_SEND_THIS.txt", std::ios::trunc);
        f << "This folder (and the .zip next to it) was written by 'p2pnode --stats'.\n"
             "Send the .zip to the person who asked you to test.\n\n"
             "It contains: timing and traffic counters, RTT, NAT type, your local and\n"
             "public IP addresses, the node IDs and IP addresses of peers you talked to,\n"
             "file transfer speeds, voice quality numbers and the program's event log.\n"
             "It does NOT contain chat messages, passwords, key files or audio.\n"
             "Delete the folder if you do not want to send it.\n";
    }

    // ---- store-only zip ------------------------------------------------
    static uint32_t crc32(const std::string& d) {
        static uint32_t table[256];
        static bool init = false;
        if (!init) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            init = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (unsigned char ch : d) c = table[(c ^ ch) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }
    static void le16(std::string& s, uint16_t v) { s += char(v & 0xFF); s += char(v >> 8); }
    static void le32(std::string& s, uint32_t v) {
        for (int i = 0; i < 4; ++i) s += char((v >> (8 * i)) & 0xFF);
    }

    void write_zip() {
        namespace fs = std::filesystem;
        const std::string root = dir_.filename().u8string();
        std::string out, central;
        std::time_t t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        uint16_t dtime = uint16_t((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
        uint16_t ddate = uint16_t(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
        uint32_t count = 0;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir_, ec)) {
            if (!e.is_regular_file()) continue;
            std::ifstream in(e.path(), std::ios::binary);
            std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            std::string name = root + "/" + e.path().filename().u8string();
            uint32_t crc = crc32(data), off = uint32_t(out.size());
            std::string lh;
            le32(lh, 0x04034b50); le16(lh, 20); le16(lh, 0x0800); le16(lh, 0);
            le16(lh, dtime); le16(lh, ddate); le32(lh, crc);
            le32(lh, uint32_t(data.size())); le32(lh, uint32_t(data.size()));
            le16(lh, uint16_t(name.size())); le16(lh, 0);
            out += lh + name + data;
            le32(central, 0x02014b50); le16(central, 20); le16(central, 20);
            le16(central, 0x0800); le16(central, 0); le16(central, dtime); le16(central, ddate);
            le32(central, crc); le32(central, uint32_t(data.size()));
            le32(central, uint32_t(data.size())); le16(central, uint16_t(name.size()));
            le16(central, 0); le16(central, 0); le16(central, 0); le16(central, 0);
            le32(central, 0); le32(central, off);
            central += name;
            ++count;
        }
        uint32_t cd_off = uint32_t(out.size());
        out += central;
        le32(out, 0x06054b50); le16(out, 0); le16(out, 0); le16(out, uint16_t(count));
        le16(out, uint16_t(count)); le32(out, uint32_t(central.size())); le32(out, cd_off);
        le16(out, 0);
        // write beside, then rename over the old zip: a crash mid-write never leaves a truncated zip
        const fs::path final_zip = fs::u8path(dir_.u8string() + ".zip");
        const fs::path tmp_zip = fs::u8path(dir_.u8string() + ".zip.tmp");
        {
            std::ofstream z(tmp_zip, std::ios::binary | std::ios::trunc);
            z.write(out.data(), std::streamsize(out.size()));
        }
        std::error_code rec;
        fs::rename(tmp_zip, final_zip, rec);
        if (rec) {   // some filesystems refuse to replace: copy over instead
            fs::copy_file(tmp_zip, final_zip, fs::copy_options::overwrite_existing, rec);
            fs::remove(tmp_zip, rec);
        }
    }

    std::filesystem::path dir_;
    std::string exe_dir_;
    Node* node_ = nullptr;
    std::function<VoiceStats()> voice_fn_;
    std::thread th_;
    std::atomic<bool> running_{false};
    std::condition_variable cv_;
    std::mutex cv_mtx_;
    std::ofstream ts_, peers_csv_, ev_, xfer_, voice_csv_;
    uint64_t started_unix_ms_ = 0, started_ms_ = 0;
    std::string last_local_ip_, last_public_;
    NetInfo last_ni_;
    bool net_down_ = false;
    uint64_t last_handshake_ = 0;
    std::map<std::string, PeerTrack> track_;
    std::map<uint64_t, bool> logged_files_;
    NodeStats last_stats_;
    MainlineDhtStats last_dht_;
    VoiceStats last_voice_;
    bool voice_seen_ = false;
    uint64_t files_done_ = 0, files_ok_ = 0, bytes_ok_ = 0, ms_ok_ = 0;
};

}  // namespace p2p
