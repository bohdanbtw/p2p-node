// Human-readable test report for Windows/macOS/Linux field checks.
#pragma once

#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include "net.h"

namespace p2p {

struct MicReport {
    bool present = false;
    std::string file;
    int duration_s = 10;
    uint64_t samples = 0;
    double rms_dbfs = 0.0;
    int peak = 0;
    uint64_t clipped = 0;
};

inline std::string report_one_line(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    return s;
}

inline std::string report_utc_now() {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::gmtime(&t);
    if (!tm) return "unknown";
    std::ostringstream out;
    out << std::put_time(tm, "%Y-%m-%d %H:%M:%S UTC");
    return out.str();
}

inline const char* report_call_state(CallState s) {
    switch (s) {
        case CallState::Idle: return "idle";
        case CallState::OutgoingRinging: return "outgoing-ringing";
        case CallState::IncomingRinging: return "incoming-ringing";
        case CallState::ActiveCall: return "active";
    }
    return "unknown";
}

inline std::string build_text_report(const Node& node, const MicReport* mic = nullptr) {
    std::ostringstream out;
    const auto stats = node.stats();
    const auto nat = node.nat_profile();
    const auto peers = node.peers();
    const auto groups = node.groups();
    const auto files = node.file_transfers();
    const auto call = node.call_info();
    const NodeId relay = node.relay();
    const uint64_t now = now_ms();

    out << "P2P Messenger test report\n";
    out << "==========================\n";
    out << "Generated: " << report_utc_now() << "\n";
    out << "Report version: 1\n\n";

    out << "Node\n";
    out << "  alias: " << report_one_line(node.identity().alias) << "\n";
    out << "  id: " << node.identity().id.hex() << "\n";
    out << "  udp_port: " << node.port() << "\n";
    out << "  invite: " << node.invite_uri() << "\n";
    out << "  nat: " << nat_name(nat.type)
        << " (mapped " << (nat.mapped_ip.empty() ? "unknown" : nat.mapped_ip)
        << ":" << nat.mapped_port
        << ", discovered=" << (nat.discovered ? "yes" : "no") << ")\n";
    out << "  relay: " << (relay.empty() ? std::string("none") : relay.short_hex(10)) << "\n";
    out << "  download_dir: " << node.download_dir() << "\n\n";

    out << "Peers (" << peers.size() << ")\n";
    if (peers.empty()) out << "  none\n";
    for (const auto& p : peers) {
        out << "  - " << (p.alias.empty() ? p.id.short_hex(8) : report_one_line(p.alias))
            << " | id=" << p.id.hex()
            << " | endpoint=" << endpoint_to_string(p.addr)
            << " | rtt=" << p.rtt_ms << " ms"
            << " | path=" << (p.relayed ? "relay" : (p.direct ? "direct" : "indirect"))
            << " | nat=" << nat_name(p.nat)
            << " | online=" << (p.online(now) ? "yes" : "no")
            << " | handshake=" << (p.handshake_done ? "yes" : "no") << "\n";
    }
    out << "\n";

    out << "Groups (" << groups.size() << ")\n";
    if (groups.empty()) out << "  none\n";
    for (const auto& g : groups) {
        const NodeId hub = node.group_hub(g.id);
        out << "  - " << report_one_line(g.name)
            << " | id=" << g.id.short_hex(10)
            << " | members=" << g.members.size()
            << " | hub=" << (hub.empty() ? std::string("none") : hub.short_hex(8)) << "\n";
    }
    out << "\n";

    out << "File transfers (" << files.size() << ")\n";
    if (files.empty()) out << "  none\n";
    for (const auto& f : files) {
        out << "  - " << report_one_line(f.name)
            << " | " << (f.outgoing ? "outgoing" : "incoming")
            << " | " << f.transferred << "/" << f.size << " bytes"
            << " | complete=" << (f.complete ? "yes" : "no")
            << " | failed=" << (f.failed ? "yes" : "no");
        if (f.complete && f.updated_ms >= f.started_ms)
            out << " | took_ms=" << (f.updated_ms - f.started_ms);
        if (!f.error.empty()) out << " | error=" << report_one_line(f.error);
        out << "\n";
    }
    out << "\n";

    out << "Call\n";
    out << "  state: " << report_call_state(call.state) << "\n";
    out << "  group: " << (call.is_group ? "yes" : "no") << "\n";
    out << "  peer: " << (call.peer.empty() ? std::string("none") : call.peer.short_hex(8)) << "\n";
    out << "  group_id: " << (call.group.empty() ? std::string("none") : call.group.short_hex(8)) << "\n\n";

    out << "Counters\n";
    out << "  tx_packets: " << stats.tx_packets << "\n";
    out << "  rx_packets: " << stats.rx_packets << "\n";
    out << "  tx_bytes: " << stats.tx_bytes << "\n";
    out << "  rx_bytes: " << stats.rx_bytes << "\n";
    out << "  chat_sent: " << stats.chat_sent << "\n";
    out << "  chat_recv: " << stats.chat_recv << "\n";
    out << "  chat_acked: " << stats.chat_acked << "\n";
    out << "  voice_sent: " << stats.voice_sent << "\n";
    out << "  voice_recv: " << stats.voice_recv << "\n";
    out << "  group_msgs: " << stats.group_msgs << "\n";
    out << "  group_voice: " << stats.group_voice << "\n";
    out << "  relay_forwarded: " << stats.relay_forwarded << "\n";
    out << "  dropped: " << stats.dropped << "\n";
    out << "  handshake_ok: " << stats.handshake_ok << "\n";
    out << "  handshake_rejected: " << stats.handshake_rejected << "\n";
    out << "  mitm_rejected: " << stats.mitm_rejected << "\n";
    out << "  replay_hits: " << stats.replay_hits << "\n";
    out << "  ratchet_steps: " << stats.ratchet_steps << "\n";
    out << "  queued: " << stats.queued << "\n";
    out << "  envelopes_stored: " << stats.envelopes_stored << "\n";
    out << "  envelopes_fetched: " << stats.envelopes_fetched << "\n";
    out << "  stun_replies: " << stats.stun_replies << "\n\n";

    out << "Microphone test\n";
    if (mic && mic->present) {
        out << "  file: " << report_one_line(mic->file) << "\n";
        out << "  duration_s: " << mic->duration_s << "\n";
        out << "  samples: " << mic->samples << "\n";
        out << "  rms_dbfs: " << std::fixed << std::setprecision(2) << mic->rms_dbfs << "\n";
        out << "  peak: " << mic->peak << "\n";
        out << "  clipped_samples: " << mic->clipped << "\n";
    } else {
        out << "  not run\n";
    }
    out << "\nNote: message contents, session keys and key seeds are not included.\n";
    return out.str();
}

inline bool write_text_report(const std::string& path, const Node& node,
                              const MicReport* mic = nullptr) {
    if (path.empty()) return false;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << build_text_report(node, mic);
    return static_cast<bool>(out);
}

}  // namespace p2p
