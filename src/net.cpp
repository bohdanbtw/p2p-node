#include "net.h"

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "mainline_dht.h"

#include <mutex>

namespace {
// Network metrics trace for the test campaign.  Enabled with
// P2P_METRICS=<path>; one CSV row per event:
//   unix_ms,event,peer,msg_id,bytes,rtt_ms,retransmits
// events: chat_send, chat_retx, chat_ack, chat_offline
void metric(const char* ev, const std::string& peer, uint32_t id, size_t bytes,
            int64_t rtt_ms, int retx) {
    static std::mutex mtx;
    static FILE* f = nullptr;
    static bool tried = false;
    std::lock_guard<std::mutex> lk(mtx);
    if (!tried) {
        tried = true;
        if (const char* path = std::getenv("P2P_METRICS")) {
            f = std::fopen(path, "a");
            if (f) std::fprintf(f, "unix_ms,event,peer,msg_id,bytes,rtt_ms,retransmits\n");
        }
    }
    if (!f) return;
    long long ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(f, "%lld,%s,%s,%u,%zu,%lld,%d\n", ts, ev, peer.c_str(), id, bytes,
                 static_cast<long long>(rtt_ms), retx);
    std::fflush(f);
}
}  // namespace

namespace p2p {

namespace {

#ifdef _WIN32
using socklen_type = int;
#else
using socklen_type = socklen_t;
#endif

using Bytes32 = std::array<uint8_t, 32>;

// How many FILE_DATA chunks the sender may put in flight before it waits for
// the receiver's cumulative acknowledgement.  The receiver acknowledges every
// contiguous chunk and re-acknowledges its current offset when a gap arrives,
// so a lost datagram costs one 500 ms stall and a rewind to remote_have rather
// than a lost byte.  One chunk per round trip would cap throughput at
// chunk_size / RTT -- 20 kB/s on a 50 ms link, which made a 8 MiB file take
// seven minutes.
constexpr uint32_t kFileWindow = 16;

// Upper bound on out-of-order chunks buffered per incoming file (4 MiB at the
// 1 kB chunk size).  Everything beyond this is dropped and re-requested by
// re-acknowledging the current offset.
constexpr size_t kFilePendingMax = 4096;

// Best-effort local interface address, used as the invite host when no NAT
// mapping has been discovered yet (plain LAN without STUN).  A connected UDP
// probe socket reveals the address of the interface used for the default
// route without sending any packet.
std::string local_udp_ip() {
    socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kInvalidSocket) return std::string("127.0.0.1");
    sockaddr_in probe{};
    probe.sin_family = AF_INET;
    probe.sin_port = htons(80);
    probe.sin_addr.s_addr = htonl(0x08080808);  // 8.8.8.8
    std::string ip = "127.0.0.1";
    if (::connect(s, reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) == 0) {
        sockaddr_in local{};
        socklen_type len = sizeof(local);
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0 &&
            local.sin_addr.s_addr != htonl(0)) {
            char buf[INET_ADDRSTRLEN];
            if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)) != nullptr)
                ip = std::string(buf);
        }
    }
    close_socket(s);
    return ip;
}

std::vector<uint8_t> make_packet(const Identity& id, PacketType type,
                                 const std::vector<uint8_t>& payload, uint8_t flags = 0) {
    PacketHeader h{};
    h.magic = htonl(kMagic);
    h.type = static_cast<uint8_t>(type);
    h.flags = flags;
    h.length = htons(static_cast<uint16_t>(payload.size()));
    std::memcpy(h.sender, id.id.b.data(), kIdBytes);
    std::vector<uint8_t> out(sizeof(h) + payload.size());
    std::memcpy(out.data(), &h, sizeof(h));
    if (!payload.empty()) std::memcpy(out.data() + sizeof(h), payload.data(), payload.size());
    return out;
}

NodeId inbox_key_of(const NodeId& id) {
    std::string label = "inbox:";
    label += id.hex();
    NodeId out{};
    crypto_generichash(out.b.data(), out.b.size(),
                       reinterpret_cast<const uint8_t*>(label.data()), label.size(), nullptr, 0);
    return out;
}

Bytes32 hash_label(const Bytes32& in, const char* label) {
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, 32);
    crypto_generichash_update(&st, in.data(), in.size());
    crypto_generichash_update(&st, reinterpret_cast<const uint8_t*>(label),
                              static_cast<unsigned long long>(std::strlen(label)));
    Bytes32 out{};
    crypto_generichash_final(&st, out.data(), out.size());
    return out;
}

NodeId group_message_hash(const NodeId& group, const NodeId& sender, uint32_t id,
                          uint64_t ts, const NodeId& parent, const std::string& text) {
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, kIdBytes);
    crypto_generichash_update(&st, group.b.data(), group.b.size());
    crypto_generichash_update(&st, sender.b.data(), sender.b.size());
    uint8_t idbuf[4] = {static_cast<uint8_t>(id >> 24), static_cast<uint8_t>(id >> 16),
                        static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id)};
    crypto_generichash_update(&st, idbuf, sizeof(idbuf));
    uint8_t tsbuf[8];
    for (int i = 0; i < 8; ++i) tsbuf[i] = static_cast<uint8_t>(ts >> (56 - 8 * i));
    crypto_generichash_update(&st, tsbuf, sizeof(tsbuf));
    crypto_generichash_update(&st, parent.b.data(), parent.b.size());
    crypto_generichash_update(&st, reinterpret_cast<const uint8_t*>(text.data()), text.size());
    NodeId out{};
    crypto_generichash_final(&st, out.b.data(), out.b.size());
    return out;
}

bool same_addr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

// All file names travel as UTF-8.  On Windows a narrow std::string path is
// interpreted in the ANSI code page, which silently mangles Cyrillic names, so
// every filesystem call goes through a UTF-8 aware path.
std::filesystem::path fs_u8(const std::string& s) { return std::filesystem::u8path(s); }

std::array<uint8_t, 32> sha256_file(const std::string& path, bool& ok) {
    std::array<uint8_t, 32> out{};
    std::ifstream in(fs_u8(path), std::ios::binary);
    if (!in) {
        ok = false;
        return out;
    }
    crypto_hash_sha256_state st;
    crypto_hash_sha256_init(&st);
    std::array<char, 64 * 1024> buf{};
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize n = in.gcount();
        if (n > 0)
            crypto_hash_sha256_update(&st, reinterpret_cast<const unsigned char*>(buf.data()),
                                      static_cast<unsigned long long>(n));
    }
    crypto_hash_sha256_final(&st, out.data());
    ok = true;
    return out;
}

std::string safe_file_name(const std::string& in) {
    std::string name = in;
    size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    for (char& c : name) {
        unsigned char u = static_cast<unsigned char>(c);
        if (u < 32 || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            c = '_';
    }
    // Windows does not allow trailing dots/spaces; strip them.
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    if (name.size() > 180) {
        size_t cut = 180;  // never cut inside a UTF-8 multi-byte sequence
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
        name.resize(cut);
    }
    // Reserved DOS device names (CON, NUL, COM1 ...) are not valid file names on Windows.
    {
        std::string stem = name.substr(0, name.find('.'));
        for (char& c : stem) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        static const char* reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3",
                                         "COM4", "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1",
                                         "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8",
                                         "LPT9"};
        for (const char* r : reserved)
            if (stem == r) {
                name = "_" + name;
                break;
            }
    }
    if (name.empty() || name == "." || name == "..") name = "received.bin";
    return name;
}

std::string unique_path(const std::string& dir, const std::string& name) {
    std::filesystem::path base = fs_u8(dir);
    std::filesystem::path candidate = base / fs_u8(name);
    std::error_code ec;
    auto taken = [&](const std::filesystem::path& p) {
        std::error_code e2;
        return std::filesystem::exists(p, e2) ||
               std::filesystem::exists(fs_u8(p.u8string() + ".part"), e2);   // in-flight transfer
    };
    if (!taken(candidate)) return candidate.u8string();
    std::filesystem::path stem = candidate.stem();
    std::filesystem::path ext = candidate.extension();
    for (int i = 1; i < 10000; ++i) {
        std::filesystem::path p =
            base / fs_u8(stem.u8string() + " (" + std::to_string(i) + ")" + ext.u8string());
        if (!taken(p)) return p.u8string();
    }
    return (base / ("received_" + std::to_string(now_ms()) + ".bin")).u8string();
}

}  // namespace

Node::Node() : rng_(std::random_device{}()) {}

Node::~Node() { stop(); }

// Readable default profile name, derived from the NodeID (stable, unique enough, no collisions
// like "Friend / Friend / Friend"): e.g. "Amber-Fox-3F9A".  Used until the user renames the profile.
static std::string default_alias_for(const NodeId& id) {
    static const char* const kAdj[16] = {"Amber", "Brave", "Calm", "Dusty", "Eager", "Frosty",
                                          "Gentle", "Happy", "Icy", "Jolly", "Kind", "Lucky",
                                          "Misty", "Noble", "Quick", "Sunny"};
    static const char* const kAnimal[16] = {"Fox", "Owl", "Wolf", "Otter", "Lynx", "Hare",
                                            "Crane", "Bear", "Seal", "Swan", "Moose", "Raven",
                                            "Panda", "Tiger", "Whale", "Zebra"};
    char tail[8];
    std::snprintf(tail, sizeof(tail), "%02X%02X", id.b[2], id.b[3]);
    return std::string(kAdj[id.b[0] & 15]) + "-" + kAnimal[id.b[1] & 15] + "-" + tail;
}

bool Node::start(const NodeConfig& cfg, std::string& err) {
    stop();

    keyfile_ = cfg.keyfile;
    if (!identity_.init(cfg.keyfile, cfg.alias, err)) return false;

    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ == kInvalidSocket) {
        err = "failed to create UDP socket";
        return false;
    }

    int yes = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
               reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
               &yes, sizeof(yes));
#endif

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(cfg.port);
    if (::bind(sock_, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        err = "failed to bind UDP port " + std::to_string(cfg.port);
        close_socket(sock_);
        sock_ = kInvalidSocket;
        return false;
    }

#ifdef _WIN32
    // Windows turns an ICMP "port unreachable" (peer closed / NAT mapping
    // gone) into WSAECONNRESET (10054) on the *next* recvfrom of a UDP
    // socket.  For a connectionless P2P socket that is meaningless and used
    // to flood the log after every handshake with a peer that went away.
    {
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
        BOOL report = FALSE;
        DWORD ret = 0;
        WSAIoctl(sock_, SIO_UDP_CONNRESET, &report, sizeof(report),
                 nullptr, 0, &ret, nullptr, nullptr);
    }
#endif

    sockaddr_in bound{};
    socklen_type blen = sizeof(bound);
    if (getsockname(sock_, reinterpret_cast<sockaddr*>(&bound), &blen) == 0)
        port_ = ntohs(bound.sin_port);
    local_ip_ = local_udp_ip();

#ifdef _WIN32
    DWORD timeout_ms = 100;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    routing_ = std::make_unique<RoutingTable>(identity_.id);
    persist_ = cfg.persist;
    relay_only_ = cfg.relay_only;
    alias_from_cli_ = cfg.alias_locked;
    // No explicit --alias (and the old placeholder "A"): use a readable name made from the NodeID.
    if (!alias_from_cli_ && (identity_.alias.empty() || identity_.alias == "A"))
        identity_.alias = default_alias_for(identity_.id);
    vault_path_ = cfg.vault_path;
    if (vault_path_.empty()) vault_path_ = "node_" + std::to_string(port_) + "_vault.dat";

    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        peers_.clear();
        contacts_.clear();
        groups_.clear();
        bootstraps_.clear();
        pending_pings_.clear();
        pending_chats_.clear();
        pending_acks_.clear();
        values_.clear();
        chat_.clear();
        offline_queue_.clear();
        inbox_seen_.clear();
        logs_.clear();
        stats_ = NodeStats{};
        call_ = CallInfo{};
        nat_ = NatProfile{};
        pairing_code_.clear();
        pairing_key_ = NodeId{};
        relay_id_ = NodeId{};
        has_strict_expected_ = false;
        pinned_by_addr_.clear();
        next_msg_id_ = 1;
        next_nonce_ = 1;
        next_call_id_ = 1;
        last_tick_ms_ = 0;
        last_save_ms_ = 0;
        last_inbox_poll_ms_ = 0;
    }

    if (persist_) {
        std::string verr;
        if (load_state(&verr)) {
            log("vault restored from " + vault_path_);
        } else if (verr != "vault not found" && !verr.empty()) {
            log("vault: " + verr);
            // Never overwrite a vault we cannot open (another identity, or a
            // different key file): keep it aside so its history is not lost.
            if (verr.find("authentication") != std::string::npos) {
                std::string aside = vault_path_ + ".unreadable-" + std::to_string(unix_ms());
                std::error_code ec;
                std::filesystem::rename(vault_path_, aside, ec);
                log(ec ? "vault: could not move unreadable vault aside"
                       : "vault: unreadable vault kept as " + aside);
            }
        }
    }

    if (!cfg.pairing_code.empty()) set_pairing_code(cfg.pairing_code);

    running_.store(true);
    started_ms_ = now_ms();
    last_save_ms_ = started_ms_;
    rx_thread_ = std::thread(&Node::rx_loop, this);

    // Join the public Mainline (BitTorrent) DHT for serverless peer discovery.
    // It shares this UDP socket, so the announced ip:port is the one peers can
    // actually reach us on.  Bootstrap comes from community routers plus the
    // routing table saved on the previous run (BEP 5); none of it is ours.
    dht_state_path_ = vault_path_;
    {
        // Derive "node_<port>_dht.bin" from the vault path, whatever suffix it
        // had, so routing-table persistence lives next to the vault.
        size_t base = dht_state_path_.find_last_of("/\\");
        size_t stem = (base == std::string::npos) ? 0 : base + 1;
        dht_state_path_ = dht_state_path_.substr(0, stem) + "node_" +
                          std::to_string(port_) + "_dht.bin";
    }
    mainline_dht_host_.log = [this](const std::string& line) { log(line); };
    mainline_dht_host_.on_found = [this](const sockaddr_in& addr, const NodeId& expected) {
        mainline_dht_on_found(addr, expected);
    };
    std::string derr;
    if (mainline_dht_start(sock_, port_, identity_.id, dht_state_path_, mainline_dht_host_,
                           derr)) {
        mainline_dht_started_.store(true);
        log("mainline DHT started (public BitTorrent DHT)");
    } else {
        log("mainline DHT failed: " + derr);
    }
    if (mainline_dht_started_.load()) {
        // R2: re-enter via known contacts / previously-saved peers without any
        // bootstrap.  Announce ourselves and look up every saved contact.
        for (const auto& kv : contacts_) mainline_dht_lookup(kv.first);
    }
    // Fast path after a restart: dial every saved contact at its last known
    // endpoint right away (DHT lookups can take tens of seconds and may
    // return stale values).  HELLO is pinned to the contact's NodeID.
    {
        std::vector<std::pair<sockaddr_in, NodeId>> dial;
        {
            std::lock_guard<std::recursive_mutex> lk(mtx_);
            for (const auto& kv : contacts_)
                if (kv.second.addr.sin_port != 0 && kv.second.addr.sin_addr.s_addr != 0)
                    dial.push_back({kv.second.addr, kv.first});
        }
        for (const auto& d : dial) {
            std::string ep = endpoint_to_string(d.first);
            size_t colon = ep.rfind(':');
            if (colon != std::string::npos)
                connect_expected(ep.substr(0, colon), ntohs(d.first.sin_port), d.second);
        }
    }

    log("node \"" + identity_.alias + "\" started, UDP port " + std::to_string(port_) +
        ", ID " + identity_.id.short_hex(12) + ", vault " +
        (persist_ ? vault_path_ : std::string("disabled")));

    {
        std::lock_guard<std::recursive_mutex> ilk(mtx_);
        infra_addrs_.clear();
        auto note = [&](const std::string& host, uint16_t port) {
            sockaddr_in a{};
            if (!host.empty() && port != 0 && resolve_ipv4(host, port, a)) infra_addrs_.push_back(a);
        };
        note(cfg.bootstrap_host, cfg.bootstrap_port);
        for (const auto& b : cfg.bootstrap_list) note(b.first, b.second);
    }
    if (!cfg.bootstrap_host.empty() && cfg.bootstrap_port != 0)
        connect_to(cfg.bootstrap_host, cfg.bootstrap_port);
    for (const auto& b : cfg.bootstrap_list)
        if (!b.first.empty() && b.second != 0) connect_to(b.first, b.second);

    return true;
}

void Node::stop() {
    bool was_running = running_.exchange(false);
    if (rx_thread_.joinable()) rx_thread_.join();
    if (stun_resolver_.joinable()) stun_resolver_.join();   // may wait for one DNS timeout
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        for (auto& kv : outgoing_files_) {
            if (kv.second) {
                kv.second->stop.store(true);
                kv.second->cancel = true;
                kv.second->info.failed = true;
                kv.second->info.error = "node stopped";
            }
        }
        cv_.notify_all();
    }
    for (auto& kv : outgoing_files_) {
        if (kv.second && kv.second->thread.joinable()) kv.second->thread.join();
    }
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        outgoing_files_.clear();
        incoming_files_.clear();
    }
    if (was_running && persist_) {
        std::string err;
        if (!save_state(&err)) log("vault save failed: " + err);
    }
    if (mainline_dht_started_.load()) {
        mainline_dht_stop();
        mainline_dht_started_.store(false);
    }
    if (sock_ != kInvalidSocket) {
        close_socket(sock_);
        sock_ = kInvalidSocket;
    }
    if (was_running) log("node stopped");
}

std::string Node::invite_uri() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::string host = nat_.discovered ? nat_.mapped_ip : local_ip_;
    if (host.empty()) host = "127.0.0.1";
    uint16_t p = nat_.discovered && nat_.mapped_port ? nat_.mapped_port : port_;
    return "p2p://" + identity_.id.hex() + "@" + host + ":" + std::to_string(p);
}

std::string Node::share_uri() const {
    std::string name;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        name = identity_.alias;
    }
    for (char& c : name)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    std::string uri = invite_uri();
    return name.empty() ? uri : name + ":" + uri;
}

std::string Node::parse_invite_name(const std::string& raw_uri) {
    size_t scheme = raw_uri.find("p2p://");
    if (scheme == std::string::npos || scheme == 0) return "";
    std::string name = raw_uri.substr(0, scheme);
    auto junk = [](unsigned char c) {
        return std::isspace(c) || c == '"' || c == '\'' || c == '<' || c == '>' || c == '`' ||
               c == ':';
    };
    size_t b = 0, e = name.size();
    while (b < e && junk(static_cast<unsigned char>(name[b]))) ++b;
    while (e > b && junk(static_cast<unsigned char>(name[e - 1]))) --e;
    name = name.substr(b, e - b);
    if (name.size() > 64) {
        size_t cut = 64;
        while (cut > 0 && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
        name.resize(cut);
    }
    return name;
}

bool Node::parse_invite(const std::string& raw_uri, NodeId& id, std::string& host,
                        uint16_t& port) {
    // Invites are pasted from messengers/browsers: tolerate surrounding
    // whitespace, line breaks and quotes/angle brackets around the URI.
    size_t b = 0, e = raw_uri.size();
    auto junk = [](unsigned char c) {
        return std::isspace(c) || c == '"' || c == '\'' || c == '<' || c == '>' || c == '`';
    };
    while (b < e && junk(static_cast<unsigned char>(raw_uri[b]))) ++b;
    while (e > b && junk(static_cast<unsigned char>(raw_uri[e - 1]))) --e;
    std::string uri = raw_uri.substr(b, e - b);
    const std::string prefix = "p2p://";
    // Shared form "username:p2p://<id>@host:port": the name in front of the link is ignored here
    // (see parse_invite_name); the link itself is validated exactly as before.
    size_t scheme = uri.find(prefix);
    if (scheme == std::string::npos) return false;
    uri.erase(0, scheme);
    std::string rest = uri.substr(prefix.size());
    size_t at = rest.find('@');
    if (at == std::string::npos) return false;
    if (!NodeId::parse_hex(rest.substr(0, at), id)) return false;
    std::string endpoint = rest.substr(at + 1);
    size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos) return false;
    host = endpoint.substr(0, colon);
    if (host.empty()) return false;
    try {
        int p = std::stoi(endpoint.substr(colon + 1));
        if (p <= 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p);
    } catch (...) {
        return false;
    }
    return true;
}

bool Node::connect_to(const std::string& host, uint16_t port) {
    return connect_expected(host, port, NodeId{});
}

bool Node::connect_expected(const std::string& host, uint16_t port, const NodeId& expected) {
    // Relay-only nodes talk to peers solely through the relay.  Dialing a
    // saved or DHT-found endpoint directly would create a second, differently
    // keyed session with the same peer and re-key the relayed one mid-transfer
    // (the stall seen when restarting with saved contacts).  Only unpinned
    // dials (the explicit bootstrap/relay address) are allowed.
    if (relay_only_ && !expected.empty()) {
        log("relay-only: not dialing " + host + ":" + std::to_string(port) +
            " directly (pinned contact)");
        return false;
    }
    sockaddr_in addr{};
    if (!resolve_ipv4(host, port, addr)) {
        log("failed to resolve address " + host + ":" + std::to_string(port));
        return false;
    }
    if (port == port_ && ntohl(addr.sin_addr.s_addr) == INADDR_LOOPBACK) {
        log("refused: this is the local node");
        return false;
    }

    std::lock_guard<std::recursive_mutex> lk(mtx_);
    // Already talking to this node on this address: a new HELLO would only
    // re-key the session and break in-flight file/voice traffic (DHT lookups
    // and invites re-trigger connects every few seconds).
    {
        uint64_t t = now_ms();
        for (const auto& kv : peers_) {
            const Peer& pp = kv.second;
            if (pp.handshake_done && !pp.relayed && same_addr(pp.addr, addr) &&
                pp.last_seen_ms && t - pp.last_seen_ms < 20000 &&
                (expected.empty() || pp.id == expected))
                return true;
        }
    }
    Bootstrap* found = nullptr;
    for (auto& b : bootstraps_) {
        if (same_addr(b.addr, addr)) {
            found = &b;
            break;
        }
    }
    if (!found) {
        Bootstrap b;
        b.addr = addr;
        b.eph = generate_ephemeral();
        b.last_hello_ms = 0;
        b.created_ms = now_ms();
        if (!expected.empty()) {
            // One ephemeral key per peer, whatever address we dial it on: the same
            // peer reached through DHT/hole-punch/relay then sees identical HELLOs
            // and does not re-key the session (the "HELLO storm").
            Peer& ep = ensure_peer_locked(expected, addr);
            if (!ep.my_eph.valid()) ep.my_eph = generate_ephemeral();
            b.eph = ep.my_eph;
        }
        bootstraps_.push_back(b);
        found = &bootstraps_.back();
    }
    if (!expected.empty()) {
        found->expected = expected;
        found->has_expected = true;
        pinned_by_addr_[addr_key(addr)] = expected;
    }
    if (!found->eph.valid()) found->eph = generate_ephemeral();
    send_hello(addr, &found->eph);
    log("sent signed HELLO to " + endpoint_to_string(addr));
    return true;
}

// A contact's live endpoint arrived from the public Mainline DHT: connect
// directly with the expected account id (4-DH handshake + ratchets).  This is
// the serverless path (R3/R6): no relay or bootstrap is involved.
void Node::mainline_dht_on_found(const sockaddr_in& addr, const NodeId& expected) {
    char ipbuf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr.sin_addr, ipbuf, sizeof(ipbuf));
    uint16_t p = ntohs(addr.sin_port);
    log("dht: found " + expected.short_hex(10) + " at " + ipbuf + ":" + std::to_string(p));
    connect_expected(ipbuf, p, expected);
}

bool Node::connect_uri(const std::string& uri, std::string& err) {
    NodeId id;
    std::string host;
    uint16_t port = 0;
    if (!parse_invite(uri, id, host, port)) {
        err = "invalid invite URI (expected p2p://<40-hex>@ip:port)";
        return false;
    }
    return connect_expected(host, port, id);
}

bool Node::connect_via_relay(const NodeId& relay, const NodeId& target, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (relay.empty() || target.empty() || relay == target) {
        if (err) *err = "invalid relay or target id";
        return false;
    }
    auto r = peers_.find(relay);
    if (r == peers_.end() || !r->second.handshake_done) {
        if (err) *err = "relay session is not established";
        return false;
    }
    auto it = peers_.find(target);
    if (it != peers_.end() && it->second.handshake_done && !it->second.relayed) {
        return true;  // direct session already works; keep it
    }
    Peer& p = ensure_peer_locked(target, r->second.addr);
    if (!p.my_eph.valid()) p.my_eph = generate_ephemeral();
    p.addr = r->second.addr;
    p.relayed = true;
    p.direct = false;
    p.relay_id = relay;
    p.hello_sent = true;
    p.last_hello_sent_ms = now_ms();
    relay_id_ = relay;
    if (!send_hello(r->second.addr, &p.my_eph, false, nullptr, &target)) {
        if (err) *err = "relay handshake send failed";
        return false;
    }
    log("relay-first HELLO for " + target.short_hex(8) + " via " + relay.short_hex(8));
    return true;
}

void Node::set_pairing_code(const std::string& code) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    pairing_code_ = code;
    if (code.empty()) {
        pairing_key_ = NodeId{};
        return;
    }
    pairing_key_ = pairing_dht_key(code);
    store_value(pairing_key_, invite_uri());
    log("pairing PIN/label set; DHT key " + pairing_key_.short_hex(12));
}

std::string Node::pairing_code() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return pairing_code_;
}

bool Node::search_pairing_code(const std::string& code, std::string& err) {
    if (code.empty()) {
        err = "pairing code is empty";
        return false;
    }
    LookupResult r = lookup(pairing_dht_key(code), true);
    if (!r.found || !r.value) {
        err = "pairing code was not found in DHT";
        return false;
    }
    return connect_uri(r.text, err);
}

bool Node::add_contact(const NodeId& id, const std::string& alias, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (id.empty() || id == identity_.id) {
        if (err) *err = "invalid contact id";
        return false;
    }
    Contact& c = contacts_[id];
    c.id = id;
    c.alias = alias;
    c.in_contacts = true;
    if (routing_) routing_->add(id);
    auto p = peers_.find(id);
    if (p != peers_.end()) touch_contact_locked(id, p->second);
    log("contact added: " + alias + " (" + id.short_hex(10) + ")");
    return true;
}

bool Node::remove_contact(const NodeId& id, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto c = contacts_.find(id);
    auto p = peers_.find(id);
    if (c == contacts_.end() && p == peers_.end()) {
        if (err) *err = "unknown contact";
        return false;
    }
    if (c != contacts_.end()) contacts_.erase(c);
    if (p != peers_.end()) {
        chat_.erase(std::remove_if(chat_.begin(), chat_.end(),
                                   [&](const ChatMsg& m) { return m.peer == id; }),
                    chat_.end());
        peers_.erase(p);
    }
    if (routing_) routing_->remove(id);
    // Drop queued offline messages addressed to the removed contact.
    offline_queue_.erase(std::remove_if(offline_queue_.begin(), offline_queue_.end(),
                                        [&](const OfflineItem& pc) { return pc.peer == id; }),
                         offline_queue_.end());
    log("contact removed: " + id.short_hex(10));
    std::string save_err;
    save_state(&save_err);
    return true;
}

bool Node::set_alias(const std::string& alias, std::string* err) {
    std::string clean = alias;
    while (!clean.empty() &&
           (clean.back() == '\n' || clean.back() == '\r' || clean.back() == ' '))
        clean.pop_back();
    while (!clean.empty() && (clean.front() == ' ' || clean.front() == '\t'))
        clean.erase(clean.begin());
    if (clean.empty() || clean.size() > 64) {
        if (err) *err = "profile name must be 1..64 characters";
        return false;
    }
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    const std::string old_alias = identity_.alias;
    identity_.alias = clean;
    // Persist first: a failed vault write must not leave a name that looks
    // saved in the UI but disappears after a restart.
    std::string save_err;
    if (persist_ && !save_state(&save_err)) {
        identity_.alias = old_alias;
        if (err) *err = save_err;
        return false;
    }
    for (auto& kv : peers_) {
        Peer& p = kv.second;
        if (!p.handshake_done) continue;
        if (p.relayed) {
            auto r = peers_.find(p.relay_id.empty() ? relay_id_ : p.relay_id);
            if (r != peers_.end())
                send_hello(r->second.addr, &p.my_eph, false, nullptr, &p.id);
        } else {
            send_hello(p.addr, &p.my_eph, false);
        }
    }
    log("profile name changed to \"" + clean + "\"");
    return true;
}

bool Node::mark_verified(const NodeId& id, bool verified) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = contacts_.find(id);
    if (it == contacts_.end()) return false;
    it->second.verified = verified;
    auto p = peers_.find(id);
    if (p != peers_.end()) p->second.verified = verified;
    log(std::string("contact ") + id.short_hex(8) +
        (verified ? " marked VERIFIED" : " marked TOFU"));
    if (p != peers_.end()) {
        // Persist immediately after a user security decision.
        std::string err;
        save_state(&err);
    }
    return true;
}

std::vector<Contact> Node::contacts() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    uint64_t now = now_ms();
    std::vector<Contact> out;
    out.reserve(contacts_.size() + peers_.size());
    for (const auto& kv : contacts_) {
        Contact c = kv.second;
        auto p = peers_.find(c.id);
        if (p != peers_.end()) {
            const Peer& peer = p->second;
            c.online = peer.online(now, 20000) && peer.handshake_done;
            c.relayed = peer.relayed;
            c.rtt_ms = peer.rtt_ms;
            c.nat = peer.nat;
            c.last_seen_ms = peer.last_seen_ms;
            c.addr = peer.addr;
            if (c.alias.empty()) c.alias = peer.alias;
            c.verified = c.verified || peer.verified;
            if (peer.ed_pk[0] || peer.x_pk[0]) {
                c.ed_pk = peer.ed_pk;
                c.x_pk = peer.x_pk;
            }
        }
        out.push_back(c);
    }
    for (const auto& kv : peers_) {
        if (contacts_.find(kv.first) != contacts_.end()) continue;
        Contact c;
        c.id = kv.first;
        c.alias = kv.second.alias;
        c.online = kv.second.online(now, 20000) && kv.second.handshake_done;
        c.relayed = kv.second.relayed;
        c.rtt_ms = kv.second.rtt_ms;
        c.nat = kv.second.nat;
        c.last_seen_ms = kv.second.last_seen_ms;
        c.addr = kv.second.addr;
        c.verified = kv.second.verified;
        c.ed_pk = kv.second.ed_pk;
        c.x_pk = kv.second.x_pk;
        c.in_contacts = kv.second.in_contacts;
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const Contact& a, const Contact& b) {
        if (a.online != b.online) return a.online;
        std::string an = a.alias.empty() ? a.id.hex() : a.alias;
        std::string bn = b.alias.empty() ? b.id.hex() : b.alias;
        if (an != bn) return an < bn;
        return a.id < b.id;
    });
    return out;
}

bool Node::contact(const NodeId& id, Contact& out) const {
    auto all = contacts();
    for (const auto& c : all) {
        if (c.id == id) {
            out = c;
            return true;
        }
    }
    return false;
}

bool Node::sas(const NodeId& peer, std::string& code, std::string& words) const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = peers_.find(peer);
    if (it == peers_.end() || !it->second.handshake_done) return false;
    // Safety number from the two long-term identity keys: the handshake already binds
    // the session to them, and unlike the per-session root key it does not change when
    // the session re-keys (reconnect, network change), so it can be compared once.
    const std::array<uint8_t, 32> kStable{};
    code = sas_code(kStable, identity_.ed_pk, it->second.ed_pk);
    words = sas_words(kStable, identity_.ed_pk, it->second.ed_pk);
    return true;
}

std::string Node::fingerprint(const NodeId& peer) const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = peers_.find(peer);
    if (it != peers_.end() && it->second.ed_pk[0]) return key_fingerprint(it->second.ed_pk);
    auto c = contacts_.find(peer);
    if (c != contacts_.end()) return key_fingerprint(c->second.ed_pk);
    return "-";
}

Peer& Node::ensure_peer_locked(const NodeId& id, const sockaddr_in& addr) {
    auto it = peers_.find(id);
    if (it != peers_.end()) return it->second;
    Peer p;
    p.id = id;
    p.addr = addr;
    p.last_seen_ms = now_ms();
    auto res = peers_.emplace(id, p);
    if (routing_) routing_->add(id);
    return res.first->second;
}

void Node::touch_contact_locked(const NodeId& id, const Peer& peer) {
    Contact& c = contacts_[id];
    c.id = id;
    // A profile name is authoritative for the peer and is rebroadcast in
    // every HELLO, so the address book must follow a rename.
    if (!peer.alias.empty()) c.alias = peer.alias;
    c.ed_pk = peer.ed_pk;
    c.x_pk = peer.x_pk;
    c.verified = c.verified || peer.verified;
    c.nat = peer.nat;
    c.addr = peer.addr;
    c.last_seen_ms = peer.last_seen_ms;
    c.in_contacts = true;
}

bool Node::send_to(const sockaddr_in& to, PacketType type,
                   const std::vector<uint8_t>& payload, uint8_t flags) {
    if (payload.size() + sizeof(PacketHeader) > kMaxPacket) {
        ++stats_.dropped;
        return false;
    }
    std::vector<uint8_t> packet = make_packet(identity_, type, payload, flags);
#ifdef _WIN32
    int n = ::sendto(sock_, reinterpret_cast<const char*>(packet.data()),
                     static_cast<int>(packet.size()), 0,
                     reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#else
    ssize_t n = ::sendto(sock_, packet.data(), packet.size(), 0,
                         reinterpret_cast<const sockaddr*>(&to), sizeof(to));
#endif
    if (n == static_cast<decltype(n)>(packet.size())) {
        ++stats_.tx_packets;
        stats_.tx_bytes += packet.size();
        return true;
    }
    ++stats_.dropped;
    return false;
}

bool Node::send_peer_packet(Peer& peer, PacketType type,
                            const std::vector<uint8_t>& payload, uint8_t flags) {
    if (!peer.relayed) return send_to(peer.addr, type, payload, flags);
    NodeId relay = peer.relay_id.empty() ? relay_id_ : peer.relay_id;
    auto r = peers_.find(relay);
    if (r == peers_.end()) return false;
    std::vector<uint8_t> inner = make_packet(identity_, type, payload, flags);
    return send_inner_packet(r->second.addr, peer.id, static_cast<uint8_t>(type), inner);
}

bool Node::send_hello(const sockaddr_in& to, const EphemeralKey* eph, bool ack,
                      const NodeId* expected, const NodeId* relay_target) {
    EphemeralKey local;
    const EphemeralKey* use = eph;
    if (!use) {
        for (auto& kv : peers_) {
            if (same_addr(kv.second.addr, to)) {
                if (!kv.second.my_eph.valid()) kv.second.my_eph = generate_ephemeral();
                use = &kv.second.my_eph;
                break;
            }
        }
    }
    if (!use) {
        for (auto& b : bootstraps_) {
            if (same_addr(b.addr, to)) {
                if (!b.eph.valid()) b.eph = generate_ephemeral();
                use = &b.eph;
                break;
            }
        }
    }
    if (!use) {
        local = generate_ephemeral();
        use = &local;
    }
    if (expected) pinned_by_addr_[addr_key(to)] = *expected;

    Writer body;
    body.u8(1);
    body.str(identity_.alias);
    body.raw(identity_.ed_pk.data(), identity_.ed_pk.size());
    body.raw(identity_.x_pk.data(), identity_.x_pk.size());
    body.raw(use->pk.data(), use->pk.size());
    body.u16(port_);
    body.u8(static_cast<uint8_t>(nat_.type));
    body.u64(unix_ms());
    std::array<uint8_t, 16> nonce{};
    randombytes_buf(nonce.data(), nonce.size());
    body.raw(nonce.data(), nonce.size());
    std::array<uint8_t, 32> pm{};
    if (!pairing_code_.empty()) {
        pm = pairing_mac(pairing_code_, identity_.ed_pk, use->pk);
    }
    body.raw(pm.data(), pm.size());

    std::array<uint8_t, crypto_sign_BYTES> sig{};
    crypto_sign_detached(sig.data(), nullptr, body.data.data(), body.data.size(),
                         identity_.ed_sk.data());
    Writer w;
    w.raw(body.data.data(), body.data.size());
    w.raw(sig.data(), sig.size());
    if (relay_target) {
        PacketType inner_type = ack ? PacketType::HelloAck : PacketType::Hello;
        std::vector<uint8_t> inner = make_packet(identity_, inner_type, w.data);
        return send_inner_packet(to, *relay_target, static_cast<uint8_t>(inner_type), inner);
    }
    return send_to(to, ack ? PacketType::HelloAck : PacketType::Hello, w.data);
}

void Node::send_nodes(const sockaddr_in& to, const NodeId& target) {
    std::vector<std::pair<NodeId, sockaddr_in>> list;
    if (routing_) {
        uint64_t now = now_ms();
        auto ids = routing_->closest(target, 8);
        for (const auto& id : ids) {
            auto it = peers_.find(id);
            if (it == peers_.end()) continue;
            if (!it->second.online(now, 60000)) continue;
            if (!it->second.listen_port) continue;
            list.emplace_back(id, it->second.addr);
        }
    }

    Writer w;
    w.u8(static_cast<uint8_t>(list.size()));
    for (const auto& p : list) {
        w.id(p.first);
        w.u32(ntohl(p.second.sin_addr.s_addr));
        w.u16(ntohs(p.second.sin_port));
    }
    send_to(to, PacketType::Nodes, w.data);
}

void Node::send_holepunch(const sockaddr_in& to, const NodeId& target, const sockaddr_in& ep) {
    Writer w;
    w.id(target);
    w.u32(ntohl(ep.sin_addr.s_addr));
    w.u16(ntohs(ep.sin_port));
    send_to(to, PacketType::HolePunch, w.data);
}

// Introducer-side hole punching: once both sides are known (typically after
// each completes a handshake with this relay/bootstrap node), tell every
// peer about every other peer's observed public endpoint.  Each receiver
// immediately fires a HELLO at the given endpoint, opening its own NAT pin
// simultaneously on both sides (classic UDP hole punching).
void Node::introduce_peer_locked(const NodeId& newcomer, const sockaddr_in& newcomer_addr) {
    if (relay_only_) return;
    uint64_t now = now_ms();
    for (auto& kv : peers_) {
        Peer& p = kv.second;
        if (!(p.id < newcomer) && !(newcomer < p.id)) continue;  // same id
        if (!p.handshake_done || !p.listen_port) continue;
        if (!p.online(now, 120000)) continue;
        // Skip a peer already sharing our LAN/subnet when the newcomer is local
        // too: direct addressing works there and punching adds noise.
        send_holepunch(p.addr, newcomer, newcomer_addr);
        send_holepunch(newcomer_addr, p.id, p.addr);
        log("hole-punch intro " + p.id.short_hex(8) + " <-> " + newcomer.short_hex(8));
    }
}

void Node::send_find(const Peer& peer, const NodeId& target, bool value) {
    Writer w;
    w.id(target);
    send_to(peer.addr, value ? PacketType::FindValue : PacketType::FindNode, w.data);
}

void Node::rx_loop() {
    std::vector<uint8_t> buf(2048);
    while (running_.load()) {
        sockaddr_in from{};
        socklen_type flen = sizeof(from);
#ifdef _WIN32
        int n = ::recvfrom(sock_, reinterpret_cast<char*>(buf.data()),
                           static_cast<int>(buf.size()), 0,
                           reinterpret_cast<sockaddr*>(&from), &flen);
#else
        ssize_t n = ::recvfrom(sock_, buf.data(), buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&from), &flen);
#endif
        if (n > 0) {
            // Demux: the public Mainline DHT (jech/dht) shares this socket.
            // Its KRPC messages are bencoded dictionaries, so they start with
            // 'd'; our own protocol always starts with the "P2P1" magic.  This
            // split is collision-free.
            if (n >= 20 && buf[0] == 0x01 && buf[1] == 0x01 && buf[4] == 0x21 &&
                buf[5] == 0x12 && buf[6] == 0xA4 && buf[7] == 0x42) {
                // RFC 5389 Binding Success Response (magic cookie 0x2112A442).
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                handle_public_stun(buf.data(), static_cast<size_t>(n), from);
            } else if (buf[0] == 'd' && mainline_dht_started_.load()) {
                mainline_dht_input(buf.data(), static_cast<size_t>(n), from);
            } else {
                handle_packet(buf.data(), static_cast<size_t>(n), from);
            }
        } else if (n < 0) {
#ifdef _WIN32
            int e = WSAGetLastError();
            // WSAECONNRESET/WSAENETRESET: ICMP unreachable echoed onto a UDP socket;
            // WSAEMSGSIZE: oversized datagram truncated.  None is fatal.
            if (e != WSAETIMEDOUT && e != WSAEWOULDBLOCK && e != WSAEINTR &&
                e != WSAECONNRESET && e != WSAENETRESET && e != WSAEMSGSIZE) {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                log("recvfrom error: " + std::to_string(e));
            }
#else
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                std::lock_guard<std::recursive_mutex> lk(mtx_);
                log(std::string("recvfrom error: ") + std::strerror(errno));
            }
#endif
        }
        tick();
    }
}

bool Node::is_infrastructure(const NodeId& id) const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = peers_.find(id);
    if (it == peers_.end()) return false;
    return !it->second.relayed && is_bootstrap_addr_locked(it->second.addr);
}

bool Node::is_bootstrap_addr_locked(const sockaddr_in& a) const {
    for (const auto& b : infra_addrs_)
        if (same_addr(b, a)) return true;
    return false;
}

void Node::network_changed_locked(const std::string& why) {
    log("network change detected: " + why);
    last_public_stun_ms_ = 0;  // learn the new mapped address immediately
    // HELLO the configured bootstraps/relays now; stale dial entries are not burst-probed.
    for (auto& b : bootstraps_)
        if (is_bootstrap_addr_locked(b.addr)) b.last_hello_ms = 0;
    for (auto& kv : peers_) {
        kv.second.next_redial_ms = 0;
        kv.second.redial_attempts = 0;
    }
    mainline_dht_reannounce_now();
}

// A peer that went silent (we or it changed network, laptop slept, NAT
// mapping expired) used to be forgotten after 20 s.  Keep redialling it with
// back-off 3,6,12,24,48,60 s: directly at its last known address and through
// the relay/bootstrap we are connected to, which knows its newest address.
void Node::redial_offline_peers_locked(uint64_t now) {
    if (relay_only_) return;
    for (auto& kv : peers_) {
        Peer& p = kv.second;
        if (!p.handshake_done && !p.in_contacts) continue;
        if (p.online(now, 20000)) {
            p.redial_attempts = 0;
            p.next_redial_ms = 0;
            continue;
        }
        if (p.last_seen_ms == 0 || now - p.last_seen_ms > 6ull * 3600 * 1000) continue;
        if (is_bootstrap_addr_locked(p.addr)) continue;  // bootstraps have their own loop
        if (now < p.next_redial_ms) continue;
        uint64_t backoff = std::min<uint64_t>(60000, 3000ull << std::min<uint32_t>(p.redial_attempts, 4));
        p.next_redial_ms = now + backoff;
        ++p.redial_attempts;
        if (!p.my_eph.valid()) p.my_eph = generate_ephemeral();
        if (p.addr.sin_port != 0) send_hello(p.addr, &p.my_eph, false, nullptr);
        // relay-first HELLO through any live bootstrap/relay session
        for (const auto& rk : peers_) {
            const Peer& r = rk.second;
            if (rk.first == p.id || !r.handshake_done || !r.online(now, 20000)) continue;
            if (!is_bootstrap_addr_locked(r.addr)) continue;
            send_hello(r.addr, &p.my_eph, false, nullptr, &p.id);
            break;
        }
        if (p.redial_attempts <= 2)
            log("redialling offline peer " + p.id.short_hex(8) + " (attempt " +
                std::to_string(p.redial_attempts) + ")");
    }
}

void Node::tick() {
    uint64_t now = now_ms();
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (now - last_tick_ms_ < 1000) return;
    last_tick_ms_ = now;
    public_stun_tick_locked(now);
    // Local address watch (wifi <-> ethernet <-> hotspot): a changed source IP
    // invalidates every NAT mapping we had, so refresh STUN, re-HELLO the
    // bootstraps and redial all peers right away instead of waiting.
    if (now - last_ip_check_ms_ >= 2000) {
        last_ip_check_ms_ = now;
        std::string ip = local_udp_ip();
        if (!ip.empty() && ip != local_ip_) {
            std::string old = local_ip_;
            local_ip_ = ip;
            if (!old.empty()) network_changed_locked("local address " + old + " -> " + ip);
        }
    }
    redial_offline_peers_locked(now);
    // Group fallback: when some member has no direct session (NAT that cannot
    // be punched, two members behind the same NAT without hairpinning), pull
    // the missing messages through the members we can reach.
    if (now - last_group_sync_ms_ >= 5000) {
        last_group_sync_ms_ = now;
        resend_unconfirmed_invites_locked(now);
        std::vector<NodeId> to_sync;
        for (const auto& kv : groups_) {
            for (const auto& m : kv.second.members) {
                if (m == identity_.id) continue;
                auto pp = peers_.find(m);
                if (pp == peers_.end() || !pp->second.handshake_done ||
                    !pp->second.online(now, 30000)) {
                    to_sync.push_back(kv.first);
                    break;
                }
            }
        }
        for (const auto& gid : to_sync) sync_group_quiet_locked(gid);
    }

    // Dial entries that never produced a session (stale DHT values, junk addresses) are dropped
    // after 2 minutes; otherwise every one of them is HELLOed every 3 s for the rest of the run.
    bootstraps_.erase(std::remove_if(bootstraps_.begin(), bootstraps_.end(),
        [&](const Bootstrap& b) {
            if (is_bootstrap_addr_locked(b.addr) || b.created_ms == 0 || now - b.created_ms < 120000)
                return false;
            for (const auto& kv : peers_)
                if (kv.second.handshake_done && same_addr(kv.second.addr, b.addr)) return false;
            return true;
        }), bootstraps_.end());
    for (auto& b : bootstraps_) {
        // Once a live session exists on this address, stop re-sending HELLO:
        // every HELLO re-runs the handshake and rotates the session keys,
        // which dropped in-flight file offers/chunks and voice frames.
        // Keep-alives maintain the NAT binding; HELLO resumes if the peer
        // goes silent for 20 s.
        bool live = false;
        for (const auto& kv : peers_) {
            const Peer& pp = kv.second;
            if (pp.handshake_done && !pp.relayed && same_addr(pp.addr, b.addr) &&
                pp.last_seen_ms && now - pp.last_seen_ms < 20000) {
                live = true;
                break;
            }
        }
        if (live) continue;
        if (now - b.last_hello_ms >= 3000) {
            b.last_hello_ms = now;
            send_hello(b.addr, &b.eph, false, b.has_expected ? &b.expected : nullptr);
        }
    }

    int stun_targets = 0;
    for (auto& kv : peers_) {
        Peer& p = kv.second;
        if (!p.online(now, 20000)) continue;
        if (!p.handshake_done && now - p.last_hello_sent_ms > 3000) {
            if (!p.my_eph.valid()) p.my_eph = generate_ephemeral();
            p.last_hello_sent_ms = now;
            if (p.relayed)
                send_hello(p.addr, &p.my_eph, false, nullptr, &p.id);
            else
                send_hello(p.addr, &p.my_eph);
        }
        uint32_t nonce = next_nonce_++;
        Writer w;
        w.u32(nonce);
        bool ping_sent = p.relayed ? send_peer_packet(p, PacketType::Ping, w.data)
                                   : send_to(p.addr, PacketType::Ping, w.data);
        if (ping_sent) pending_pings_[nonce] = {p.id, now};

        // A STUN probe must observe the peer's own external address; behind a
        // relay it would only measure the relay, so skip it for relayed peers.
        if (!p.relayed && p.handshake_done && stun_targets < 2 &&
            nat_.type != NatType::Symmetric) {
            auto& st = last_stun_[p.id.hex()];
            if (now - st.second > 5000) {
                st.first = next_nonce_++;
                st.second = now;
                Writer sw;
                sw.u32(st.first);
                send_to(p.addr, PacketType::StunProbe, sw.data);
                ++stun_targets;
            }
        }
    }

    for (auto it = pending_pings_.begin(); it != pending_pings_.end();) {
        if (now - it->second.second > 5000) {
            challenge_addr_.erase(it->first);
            it = pending_pings_.erase(it);
        }
        else
            ++it;
    }

    for (auto it = pending_chats_.begin(); it != pending_chats_.end();) {
        uint64_t elapsed = now - it->second.sent_ms;
        uint8_t retx = it->second.retransmit;
        if (elapsed < 1000) {
            ++it;
            continue;
        }
        if (retx == 0 && elapsed >= 1000 && elapsed < 3000) {
            // First retransmit at 1s
        } else if (retx == 1 && elapsed >= 3000 && elapsed < 6000) {
            // Second retransmit at 3s
        } else if (retx >= 2 && elapsed >= 6000) {
            // Move to offline queue after 6s
        } else {
            ++it;
            continue;
        }

        NodeId peer = it->second.peer;
        uint32_t id = it->first;
        auto p = peers_.find(peer);
        if (p != peers_.end() && p->second.handshake_done) {
            auto msg = std::find_if(chat_.begin(), chat_.end(), [&](const ChatMsg& m) {
                return m.outgoing && !m.is_group && m.id == id && m.peer == peer;
            });
            if (msg != chat_.end() && !msg->delivered && !msg->queued) {
                if (retx < 2) {
                    // Retransmit
                    if (retransmit_chat(p->second, id, msg->text)) {
                        it->second.retransmit = retx + 1;
                        it->second.sent_ms = now;  // reset timer for next retransmit
                        log("CHAT retransmit " + std::to_string(retx + 1) + " for msg " + std::to_string(id));
                        ++it;
                        continue;
                    }
                } else {
                    // Move to offline queue
                    msg->queued = true;
                    metric("chat_offline", peer.short_hex(10), id, it->second.bytes, -1, retx);
                    queue_offline_locked(peer, id, msg->text, msg->ts_ms);
                }
            }
        }
        it = pending_chats_.erase(it);
    }

    if (now - last_inbox_poll_ms_ > 3000) {
        last_inbox_poll_ms_ = now;
        poll_inbox_locked();
    }

    if (persist_ && now - last_save_ms_ > 30000) {
        last_save_ms_ = now;
        std::string err;
        save_state(&err);
    }
    cv_.notify_all();
}

void Node::handle_packet(const uint8_t* data, size_t len, const sockaddr_in& from,
                         bool via_relay) {
    if (len < sizeof(PacketHeader)) return;

    PacketHeader h{};
    std::memcpy(&h, data, sizeof(h));
    if (ntohl(h.magic) != kMagic) return;
    uint16_t plen = ntohs(h.length);
    if (plen != len - sizeof(PacketHeader) || plen > kMaxPacket) return;

    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    if (sender == identity_.id) return;

    std::lock_guard<std::recursive_mutex> lk(mtx_);
    ++stats_.rx_packets;
    stats_.rx_bytes += len;

    Reader r(data + sizeof(PacketHeader), plen);
    PacketType type = static_cast<PacketType>(h.type);
    cur_from_ = from;  // origin of the packet being processed (see adopt_path_locked)
    cur_via_relay_ = via_relay;

    // Relays forward one opaque envelope only.  A nested RELAY inside a
    // forwarded envelope would let a peer create arbitrary forwarding loops.
    if (via_relay && type == PacketType::Relay) {
        ++stats_.dropped;
        return;
    }

    if (type == PacketType::StunProbe) {
        handle_stun_probe(r, from);
        return;
    }

    auto it = peers_.find(sender);
    if (it == peers_.end()) {
        // A relayed handshake arrives inside a Relay packet before the sender
        // is known locally.  Admit that envelope; handle_hello() will create
        // and authenticate the real peer record after the target is matched.
        if (type != PacketType::Hello && type != PacketType::HelloAck &&
            type != PacketType::Relay) {
            ++stats_.dropped;
            return;
        }
        it = peers_.emplace(sender, Peer{}).first;
        it->second.id = sender;
        it->second.addr = from;
        it->second.last_seen_ms = now_ms();
        if (routing_) routing_->add(sender);
    } else {
        if (type != PacketType::Hello && type != PacketType::HelloAck)
            it->second.last_seen_ms = now_ms();
    }

    switch (type) {
        case PacketType::Hello:
            handle_hello(h, r, from, false, via_relay);
            break;
        case PacketType::HelloAck:
            handle_hello(h, r, from, true, via_relay);
            break;
        case PacketType::Ping: {
            uint32_t nonce = r.u32();
            if (!r.ok()) return;
            Writer w;
            w.u32(nonce);
            if (via_relay) {
                auto pit = peers_.find(sender);
                if (pit != peers_.end())
                    send_peer_packet(pit->second, PacketType::Pong, w.data);
            } else {
                send_to(from, PacketType::Pong, w.data);
                // Known peer pinging from an address we do not have: it probably
                // moved.  Challenge that address with a secret nonce; only a
                // Pong echoing it (see below) switches the stored endpoint.
                auto pit = peers_.find(sender);
                if (pit != peers_.end() && pit->second.handshake_done && !pit->second.relayed &&
                    !pit->second.online(now_ms(), 4000) &&   // a live peer cannot be re-pointed by a stranger
                    !same_addr(pit->second.addr, from)) {
                    uint64_t t = now_ms();
                    if (t - pit->second.last_challenge_ms > 1000) {
                        pit->second.last_challenge_ms = t;
                        uint32_t cn = next_nonce_++;
                        Writer cw;
                        cw.u32(cn);
                        if (send_to(from, PacketType::Ping, cw.data)) {
                            pending_pings_[cn] = {sender, t};
                            challenge_addr_[cn] = from;
                        }
                    }
                }
            }
            break;
        }
        case PacketType::Pong: {
            uint32_t nonce = r.u32();
            if (!r.ok()) return;
            auto p = pending_pings_.find(nonce);
            if (p == pending_pings_.end()) return;
            auto peer_it = peers_.find(p->second.first);
            if (peer_it != peers_.end()) {
                uint32_t rtt = static_cast<uint32_t>(now_ms() - p->second.second);
                peer_it->second.rtt_ms = std::max<uint32_t>(1, rtt);
                auto ca = challenge_addr_.find(nonce);
                if (ca != challenge_addr_.end()) {
                    if (!via_relay && same_addr(ca->second, from) &&
                        !same_addr(peer_it->second.addr, from)) {
                        log("peer " + peer_it->second.id.short_hex(8) + " moved: " +
                            endpoint_to_string(peer_it->second.addr) + " -> " +
                            endpoint_to_string(from) + " (ping challenge)");
                        peer_it->second.addr = from;
                        introduce_peer_locked(peer_it->second.id, from);
                    }
                    challenge_addr_.erase(ca);
                }
            }
            pending_pings_.erase(p);
            break;
        }
        case PacketType::FindNode: {
            NodeId target;
            if (!r.id(target) || !r.ok()) return;
            send_nodes(from, target);
            break;
        }
        case PacketType::Nodes:
            handle_nodes(r);
            break;
        case PacketType::Store: {
            NodeId key;
            std::string value;
            if (!r.id(key)) return;
            value = r.str(2048);
            if (!r.ok()) return;
            values_[key] = value;
            log("received STORE " + key.short_hex(10) + " (" + std::to_string(value.size()) +
                " bytes)");
            break;
        }
        case PacketType::FindValue:
            handle_find_value(r, from);
            break;
        case PacketType::Value: {
            NodeId key;
            std::string value;
            if (!r.id(key)) return;
            value = r.str(2048);
            if (!r.ok()) return;
            values_[key] = value;
            log("received VALUE " + key.short_hex(10) + " (" + std::to_string(value.size()) +
                " bytes)");
            cv_.notify_all();
            break;
        }
        case PacketType::Chat:
            handle_chat(h, r);
            break;
        case PacketType::Ack:
            handle_ack(r);
            break;
        case PacketType::Voice:
            handle_voice(h, r, false);
            break;
        case PacketType::VoiceEnd:
            handle_voice(h, r, true);
            break;
        case PacketType::GroupVoice:
            handle_group_voice(h, r);
            break;
        case PacketType::StunReply:
            handle_stun_reply(r, from);
            break;
        case PacketType::Relay:
            handle_relay(r, from);
            break;
        case PacketType::HolePunch:
            handle_holepunch(r);
            break;
        case PacketType::GroupInvite:
            handle_group_invite(h, r);
            break;
        case PacketType::GroupSync:
            handle_group_sync(h, r);
            break;
        case PacketType::GroupChat:
            handle_group_chat(h, r);
            break;
        case PacketType::CallOffer:
            handle_call_offer(h, r);
            break;
        case PacketType::CallAnswer:
            handle_call_answer(h, r);
            break;
        case PacketType::CallBye:
            handle_call_bye(h, r);
            break;
        case PacketType::FileOffer:
            handle_file_offer(h, r);
            break;
        case PacketType::FileAccept:
            handle_file_accept(h, r);
            break;
        case PacketType::FileData:
            handle_file_data(h, r);
            break;
        case PacketType::FileAck:
            handle_file_ack(h, r);
            break;
        case PacketType::FileDone:
            handle_file_done(h, r);
            break;
        case PacketType::FileComplete:
            handle_file_complete(h, r);
            break;
        case PacketType::GroupKeyUpdate:
            handle_group_key_update(h, r);
            break;
        default:
            ++stats_.dropped;
            break;
    }
}

void Node::handle_hello(const PacketHeader& h, Reader& r, const sockaddr_in& from, bool ack,
                        bool via_relay) {
    const uint8_t* signed_start = r.cur();
    uint8_t version = r.u8();
    std::string alias = r.str(64);
    std::array<uint8_t, 32> ed{};
    std::array<uint8_t, 32> x{};
    std::array<uint8_t, 32> eph{};
    if (!r.raw(ed.data(), ed.size()) || !r.raw(x.data(), x.size()) ||
        !r.raw(eph.data(), eph.size()))
        return;
    uint16_t listen_port = r.u16();
    uint8_t nat_byte = r.u8();
    uint64_t timestamp = r.u64();
    std::array<uint8_t, 16> nonce{};
    if (!r.raw(nonce.data(), nonce.size())) return;
    std::array<uint8_t, 32> pm{};
    if (!r.raw(pm.data(), pm.size())) return;
    if (!r.ok() || version != 1) return;

    size_t signed_len = static_cast<size_t>(r.cur() - signed_start);
    std::array<uint8_t, crypto_sign_BYTES> sig{};
    if (!r.raw(sig.data(), sig.size())) return;
    if (crypto_sign_verify_detached(sig.data(), signed_start, signed_len, ed.data()) != 0) {
        ++stats_.handshake_rejected;
        log("HELLO rejected: invalid Ed25519 signature");
        ++stats_.dropped;
        return;
    }

    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    if (!verify_node_id(sender, ed)) {
        ++stats_.mitm_rejected;
        ++stats_.handshake_rejected;
        log("HELLO rejected: NodeID != BLAKE2b-160(ed_pk)");
        ++stats_.dropped;
        return;
    }
    if (!verify_x25519_from_ed(ed, x)) {
        ++stats_.mitm_rejected;
        ++stats_.handshake_rejected;
        log("HELLO rejected: X25519 key is not bound to Ed25519 identity");
        ++stats_.dropped;
        return;
    }

    uint64_t nowu = unix_ms();
    uint64_t skew = timestamp > nowu ? timestamp - nowu : nowu - timestamp;
    if (timestamp == 0 || skew > 600000) {
        ++stats_.handshake_rejected;
        log("HELLO rejected: stale timestamp");
        ++stats_.dropped;
        return;
    }

    {
        // A signed HELLO stays valid for the whole timestamp window; remember its nonce so a
        // captured copy replayed from another address cannot re-point or rename the peer.
        const uint64_t tnow = now_ms();
        if (seen_hello_nonces_.size() > 4096) {
            for (auto it = seen_hello_nonces_.begin(); it != seen_hello_nonces_.end();)
                it = tnow - it->second > 600000 ? seen_hello_nonces_.erase(it) : std::next(it);
        }
        if (!seen_hello_nonces_.emplace(nonce, tnow).second) {
            ++stats_.dropped;
            return;
        }
    }
    auto pin = pinned_by_addr_.find(addr_key(from));
    if (pin != pinned_by_addr_.end() && sender != pin->second) {
        ++stats_.mitm_rejected;
        ++stats_.handshake_rejected;
        log("HELLO rejected: unexpected NodeID (invite was pinned)");
        ++stats_.dropped;
        // A stale DHT/invite endpoint now belongs to someone else.  Forget it so we stop
        // dialling it every few seconds (hundreds of rejected HELLOs per hour otherwise).
        pinned_by_addr_.erase(pin);
        if (!is_bootstrap_addr_locked(from))
            bootstraps_.erase(std::remove_if(bootstraps_.begin(), bootstraps_.end(),
                                             [&](const Bootstrap& b) { return same_addr(b.addr, from); }),
                              bootstraps_.end());
        return;
    }

    if (!pairing_code_.empty()) {
        std::array<uint8_t, 32> want = pairing_mac(pairing_code_, ed, eph);
        if (std::memcmp(want.data(), pm.data(), want.size()) != 0) {
            ++stats_.mitm_rejected;
            ++stats_.handshake_rejected;
            log("HELLO rejected: pairing MAC mismatch (possible MITM)");
            ++stats_.dropped;
            return;
        }
    }

    Peer& p = ensure_peer_locked(sender, from);
    bool first = !p.hello_received;
    bool old_handshake = p.handshake_done;
    bool eph_changed = (p.eph_pk != eph);
    NodeId relay;
    if (via_relay) {
        relay = relay_id_;
        if (relay.empty()) {
            for (const auto& kv : peers_) {
                if (kv.first != sender && same_addr(kv.second.addr, from)) {
                    relay = kv.first;
                    break;
                }
            }
        }
    }
    p.addr = from;
    p.ed_pk = ed;
    p.x_pk = x;
    p.eph_pk = eph;
    p.alias = alias;
    p.listen_port = listen_port;
    p.nat = static_cast<NatType>(nat_byte);
    p.hello_received = true;
    p.hello_sent = true;
    p.last_seen_ms = now_ms();
    p.direct = !via_relay;
    p.relayed = via_relay;
    p.relay_id = via_relay ? relay : NodeId{};
    if (via_relay && !relay.empty()) relay_id_ = relay;
    p.in_contacts = true;

    if (!p.my_eph.valid()) {
        for (auto& b : bootstraps_) {
            if (same_addr(b.addr, from) && b.eph.valid()) {
                p.my_eph = b.eph;
                break;
            }
        }
        if (!p.my_eph.valid()) p.my_eph = generate_ephemeral();
    }

    if (!old_handshake || eph_changed || !p.text_ratchet.ready) {
        std::array<uint8_t, 32> root{};
        if (!derive_root_key(identity_.x_sk, x, p.my_eph.sk, eph, identity_.id, sender,
                             root)) {
            ++stats_.handshake_rejected;
            log("HELLO rejected: X25519 exchange failed");
            ++stats_.dropped;
            return;
        }
        Bytes32 text_root = hash_label(root, "text");
        Bytes32 voice_root = hash_label(root, "voice");
        p.root_key = root;
        init_ratchet(text_root, identity_.id, sender, kTextRatchetEpoch, p.text_ratchet);
        init_ratchet(voice_root, identity_.id, sender, kVoiceRatchetEpoch, p.voice_ratchet);
        p.text_replay.reset();
        p.voice_replay.reset();
        p.handshake_done = true;
        p.handshake_ms = now_ms();
        ++stats_.handshake_ok;
        log("handshake complete with \"" + alias + "\" (" + sender.short_hex(10) +
            "), 4-DH root + ratchets ready");
    }

    touch_contact_locked(sender, p);
    pinned_by_addr_.erase(addr_key(from));
    if (first) {
        log("node \"" + alias + "\" (" + sender.short_hex(10) + ") connected from " +
            endpoint_to_string(from));
    }

    if (!ack) {
        if (via_relay && !relay.empty()) {
            auto r = peers_.find(relay);
            if (r != peers_.end())
                send_hello(r->second.addr, &p.my_eph, true, nullptr, &sender);
        } else {
            send_hello(from, &p.my_eph, true);
        }
    }
    if (!via_relay) send_nodes(from, sender);

    if (!old_handshake || eph_changed) {
        drain_offline_locked(p);
        for (auto& kv : groups_) {
            auto& g = kv.second;
            if (std::find(g.members.begin(), g.members.end(), sender) != g.members.end())
                send_group_invite_locked(g, sender);
        }
        // We sit between this newcomer and everyone we already know (bootstrap/
        // relay role): punch UDP paths so two NATed clients can reach each
        // other directly instead of relying on hairpin NAT.
        introduce_peer_locked(sender, from);
    }
    cv_.notify_all();
}

void Node::handle_nodes(Reader& r) {
    uint8_t count = r.u8();
    if (!r.ok()) return;
    uint64_t now = now_ms();

    for (uint8_t i = 0; i < count; ++i) {
        NodeId id;
        uint32_t ip = 0;
        uint16_t port = 0;
        if (!r.id(id)) return;
        ip = r.u32();
        port = r.u16();
        if (!r.ok()) return;
        if (id == identity_.id || port == 0) continue;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(ip);
        addr.sin_port = htons(port);

        auto it = peers_.find(id);
        if (it != peers_.end()) {
            if (it->second.last_seen_ms == 0) it->second.last_seen_ms = now;
            continue;
        }

        Peer& p = ensure_peer_locked(id, addr);
        p.addr = addr;
        p.last_seen_ms = now;
        p.direct = false;
        if (!p.hello_sent) {
            p.hello_sent = true;
            if (relay_only_) {
                log("learned about node " + id.short_hex(10) +
                    " (relay-only, direct HELLO suppressed)");
                continue;
            }
            if (!p.my_eph.valid()) p.my_eph = generate_ephemeral();
            send_hello(addr, &p.my_eph);
            log("learned about node " + id.short_hex(10) + " (" + endpoint_to_string(addr) + ")");
        }
    }
}

void Node::handle_find_value(Reader& r, const sockaddr_in& from) {
    NodeId key;
    if (!r.id(key) || !r.ok()) return;
    auto it = values_.find(key);
    if (it == values_.end()) {
        send_nodes(from, key);
        return;
    }
    Writer w;
    w.id(key);
    w.str(it->second);
    send_to(from, PacketType::Value, w.data);
}

bool Node::seal_with_peer(Peer& peer, PacketType type, const std::vector<uint8_t>& plain,
                          std::vector<uint8_t>& payload) {
    if (!peer.handshake_done || !peer.text_ratchet.ready) return false;
    uint64_t seq = peer.text_ratchet.tx_seq;
    std::array<uint8_t, 32> key{};
    if (!ratchet_tx_key(peer.text_ratchet, kTextRatchetEpoch, seq, key)) return false;
    auto sealed = seal_record(key, plain.data(), plain.size(), static_cast<uint8_t>(type));
    Writer w;
    w.u64(seq);
    w.bytes(sealed);
    payload = std::move(w.data);
    return true;
}

// An authenticated packet arrived from a new source address: the peer moved
// (wifi -> ethernet -> hotspot) or its NAT mapping changed.  The address is
// switched only now, never on an unauthenticated header.
void Node::adopt_path_locked(Peer& peer) {
    if (cur_via_relay_ || peer.relayed) return;
    if (peer.addr.sin_addr.s_addr == cur_from_.sin_addr.s_addr &&
        peer.addr.sin_port == cur_from_.sin_port)
        return;
    if (now_ms() - peer.last_adopt_ms < 3000) return;   // two valid paths (LAN + hairpin): do not flap
    peer.last_adopt_ms = now_ms();
    log("peer " + peer.id.short_hex(8) + " moved: " + endpoint_to_string(peer.addr) + " -> " +
        endpoint_to_string(cur_from_));
    peer.addr = cur_from_;
    introduce_peer_locked(peer.id, peer.addr);  // tell the others where it is now
}

bool Node::open_with_peer(Peer& peer, PacketType type, Reader& r,
                          std::vector<uint8_t>& plain, uint64_t* out_seq) {
    uint64_t seq = r.u64();
    if (!r.ok()) return false;
    // Work on copies: a forged packet (anyone can send one, the sender id is
    // public) must not advance the ratchet or the replay window.  State is
    // committed only after the AEAD tag verified.
    RatchetState trial = peer.text_ratchet;
    ReplayWindow rwin = peer.text_replay;
    std::array<uint8_t, 32> key{};
    if (!ratchet_rx_key(trial, kTextRatchetEpoch, seq, key)) {
        ++stats_.replay_hits;
        return false;
    }
    if (!rwin.accept(seq)) {
        ++stats_.replay_hits;
        ++stats_.dropped;
        return false;
    }
    if (!open_record(key, r.cur(), r.remaining(), static_cast<uint8_t>(type), plain)) {
        ++stats_.dropped;
        return false;
    }
    peer.text_ratchet = trial;
    peer.text_replay = rwin;
    adopt_path_locked(peer);
    if (out_seq) *out_seq = seq;
    return true;
}

bool Node::send_encrypted_chat(Peer& peer, const NodeId& conv, uint32_t id,
                               const std::string& text, bool group) {
    (void)group;
    std::vector<uint8_t> payload;
    Writer pw;
    pw.u32(id);
    pw.u64(unix_ms());
    pw.str(text);
    if (!seal_with_peer(peer, PacketType::Chat, pw.data, payload)) return false;
    if (!send_peer_packet(peer, PacketType::Chat, payload)) return false;
    // Only insert if not already pending (preserve retransmit count on retransmit)
    auto pc = pending_chats_.find(id);
    if (pc == pending_chats_.end()) {
        uint64_t t = now_ms();
        pending_chats_[id] = PendingChat{conv, t, 0, t, payload.size()};
        metric("chat_send", conv.short_hex(10), id, payload.size(), -1, 0);
    } else {
        metric("chat_retx", conv.short_hex(10), id, payload.size(), -1,
               pc->second.retransmit + 1);
    }
    ++stats_.chat_sent;
    return true;
}

// Retransmit an already-pending chat message without resetting retransmit counter.
bool Node::retransmit_chat(Peer& peer, uint32_t id, const std::string& text) {
    std::vector<uint8_t> payload;
    Writer pw;
    pw.u32(id);
    pw.u64(unix_ms());
    pw.str(text);
    if (!seal_with_peer(peer, PacketType::Chat, pw.data, payload)) return false;
    if (!send_peer_packet(peer, PacketType::Chat, payload)) return false;
    ++stats_.chat_sent;
    return true;
}

void Node::handle_chat(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto it = peers_.find(sender);
    if (it == peers_.end() || !it->second.handshake_done) {
        ++stats_.dropped;
        return;
    }
    std::vector<uint8_t> plain;
    if (!open_with_peer(it->second, PacketType::Chat, r, plain)) {
        log("CHAT rejected: ratchet/auth check failed");
        return;
    }
    Reader pr(plain);
    uint32_t id = pr.u32();
    uint64_t ts = pr.u64();
    std::string text = pr.str(2048);
    if (!pr.ok()) return;

    bool duplicate = false;
    for (const auto& m : chat_) {
        if (!m.outgoing && !m.is_group && m.sender == sender && m.id == id) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate) {
        ChatMsg msg;
        msg.id = id;
        msg.peer = sender;
        msg.sender = sender;
        msg.text = text;
        msg.outgoing = false;
        msg.delivered = true;
        msg.ts_ms = ts ? ts : unix_ms();
        add_message_locked(msg);
        ++stats_.chat_recv;
        // rtt_ms column = one-way delay by sender clock (valid only with NTP-synced clocks)
        metric("chat_recv", sender.short_hex(10), id, text.size(),
               ts ? static_cast<int64_t>(unix_ms() - ts) : -1, 0);
        log("CHAT from " + sender.short_hex(8) + ": " + text.substr(0, 60));
    }
    send_ack_locked(it->second, id);
    cv_.notify_all();
}

bool Node::send_ack_locked(Peer& peer, uint32_t id) {
    Writer pw;
    pw.u32(id);
    std::vector<uint8_t> payload;
    if (!seal_with_peer(peer, PacketType::Ack, pw.data, payload)) return false;
    return send_peer_packet(peer, PacketType::Ack, payload);
}

void Node::handle_ack(Reader& r) {
    NodeId sender;
    // The caller has already resolved the peer from the header; find it again by
    // looking for the only peer whose address matches the packet source is not
    // available here, so use the pending id map below.  The authenticated
    // record already binds the ACK to the peer's ratchet.
    (void)sender;
    // This function is entered with Reader positioned at the ratchet payload.
    // We locate the peer by trying all established peers: the sequence/key and
    // AEAD tag make the correct one unique.
    std::vector<uint8_t> plain;
    Peer* matched = nullptr;
    uint32_t id = 0;
    for (auto& kv : peers_) {
        Peer& p = kv.second;
        if (!p.handshake_done) continue;
        // Copy reader state because a failed open consumes bytes.
        Reader copy = r;
        std::array<uint8_t, 32> key{};
        uint64_t seq = copy.u64();
        if (!copy.ok()) continue;
        RatchetState trial = p.text_ratchet;
        if (!ratchet_rx_key(trial, kTextRatchetEpoch, seq, key)) continue;
        std::vector<uint8_t> tmp;
        if (!open_record(key, copy.cur(), copy.remaining(), static_cast<uint8_t>(PacketType::Ack),
                         tmp))
            continue;
        Reader pr(tmp);
        uint32_t candidate = pr.u32();
        if (!pr.ok()) continue;
        // Ratchet trial succeeded only for the right key; commit state.
        p.text_ratchet = trial;
        if (!p.text_replay.accept(seq)) {
            ++stats_.replay_hits;
            return;
        }
        plain = std::move(tmp);
        id = candidate;
        matched = &p;
        break;
    }
    if (!matched) {
        ++stats_.dropped;
        return;
    }

    uint32_t rtt = 0;
    // An ACK counts only for a message that was addressed to the peer that sent the ACK;
    // otherwise any other handshaken peer could "acknowledge" (and so cancel) A's message by id.
    auto pit = pending_chats_.find(id);
    if (pit != pending_chats_.end() && pit->second.peer == matched->id) {
        uint64_t t = now_ms();
        rtt = static_cast<uint32_t>(t - pit->second.sent_ms);  // last attempt
        uint64_t first = pit->second.first_sent_ms ? pit->second.first_sent_ms
                                                   : pit->second.sent_ms;
        metric("chat_ack", pit->second.peer.short_hex(10), id, pit->second.bytes,
               static_cast<int64_t>(t - first), pit->second.retransmit);
        pending_chats_.erase(pit);
    }
    for (auto& m : chat_) {
        if (m.outgoing && !m.is_group && m.id == id && m.peer == matched->id) {
            m.delivered = true;
            m.queued = false;
            break;
        }
    }
    offline_queue_.erase(std::remove_if(offline_queue_.begin(), offline_queue_.end(),
                                        [&](const OfflineItem& i) {
                                            return i.id == id && i.peer == matched->id;
                                        }),
                         offline_queue_.end());
    matched->rtt_ms = rtt ? rtt : matched->rtt_ms;
    ++stats_.chat_acked;
    log("ACK for message " + std::to_string(id) + ", RTT " + std::to_string(rtt) + " ms");
    cv_.notify_all();
}

bool Node::send_voice(const NodeId& peer, uint32_t seq, const std::vector<uint8_t>& opus) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = peers_.find(peer);
    if (it == peers_.end() || !it->second.handshake_done) return false;
    Peer& p = it->second;
    uint64_t vseq = p.voice_ratchet.tx_seq;
    std::array<uint8_t, 32> key{};
    if (!ratchet_tx_key(p.voice_ratchet, kVoiceRatchetEpoch, vseq, key)) return false;
    Writer pw;
    pw.u32(seq);
    pw.bytes(opus);
    auto sealed = seal_record(key, pw.data.data(), pw.data.size(),
                              static_cast<uint8_t>(PacketType::Voice));
    Writer w;
    w.u64(vseq);
    w.bytes(sealed);
    if (!send_peer_packet(p, PacketType::Voice, w.data)) return false;
    ++stats_.voice_sent;
    return true;
}

bool Node::send_voice_end(const NodeId& peer) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = peers_.find(peer);
    if (it == peers_.end() || !it->second.handshake_done) return false;
    Peer& p = it->second;
    uint64_t vseq = p.voice_ratchet.tx_seq;
    std::array<uint8_t, 32> key{};
    if (!ratchet_tx_key(p.voice_ratchet, kVoiceRatchetEpoch, vseq, key)) return false;
    Writer pw;
    pw.u32(0);
    auto sealed = seal_record(key, pw.data.data(), pw.data.size(),
                              static_cast<uint8_t>(PacketType::VoiceEnd));
    Writer w;
    w.u64(vseq);
    w.bytes(sealed);
    return send_peer_packet(p, PacketType::VoiceEnd, w.data);
}

void Node::handle_voice(const PacketHeader& h, Reader& r, bool end) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto it = peers_.find(sender);
    if (it == peers_.end() || !it->second.handshake_done) {
        ++stats_.dropped;
        return;
    }
    Peer& p = it->second;
    uint64_t seq = r.u64();
    if (!r.ok()) return;
    RatchetState vtrial = p.voice_ratchet;  // commit only after authentication
    ReplayWindow vwin = p.voice_replay;
    std::array<uint8_t, 32> key{};
    if (!ratchet_rx_key(vtrial, kVoiceRatchetEpoch, seq, key)) {
        ++stats_.replay_hits;
        return;
    }
    if (!vwin.accept(seq)) {
        ++stats_.replay_hits;
        ++stats_.dropped;
        return;
    }
    std::vector<uint8_t> plain;
    if (!open_record(key, r.cur(), r.remaining(),
                     static_cast<uint8_t>(end ? PacketType::VoiceEnd : PacketType::Voice),
                     plain)) {
        ++stats_.dropped;
        return;
    }
    p.voice_ratchet = vtrial;
    p.voice_replay = vwin;
    adopt_path_locked(p);
    Reader pr(plain);
    uint32_t frame_seq = pr.u32();
    if (!pr.ok()) return;
    std::vector<uint8_t> opus;
    if (!end && pr.remaining() > 0) opus.assign(pr.cur(), pr.cur() + pr.remaining());
    if (voice_requires_call_ &&
        !(call_.state == CallState::ActiveCall && !call_.is_group && call_.peer == sender))
        return;  // not in a call with this peer: never play stray audio
    ++stats_.voice_recv;
    if (voice_handler_) voice_handler_(sender, frame_seq, opus);
}

bool Node::group_seal(const GroupRoom& group, PacketType type,
                      const std::vector<uint8_t>& plain, std::vector<uint8_t>& payload) {
    auto sealed = seal_record(group.group_key, plain.data(), plain.size(),
                              static_cast<uint8_t>(type));
    Writer w;
    w.id(group.id);
    w.bytes(sealed);
    payload = std::move(w.data);
    return true;
}

bool Node::group_open(const GroupRoom& group, PacketType type, Reader& r,
                      std::vector<uint8_t>& plain) {
    NodeId gid;
    if (!r.id(gid) || gid != group.id) return false;
    return open_record(group.group_key, r.cur(), r.remaining(), static_cast<uint8_t>(type),
                       plain);
}

bool Node::send_group_chat(const NodeId& group_id, const std::string& text,
                           std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) {
        if (err) *err = "group not found";
        return false;
    }
    if (text.empty() || text.size() > 900) {
        if (err) *err = "invalid message length";
        return false;
    }
    GroupRoom& g = it->second;
    uint32_t id = next_msg_id_++;
    uint64_t ts = unix_ms();
    NodeId parent = g.last_hash;
    NodeId hash = group_message_hash(g.id, identity_.id, id, ts, parent, text);

    Writer pw;
    pw.u32(id);
    pw.u64(ts);
    pw.id(parent);
    pw.str(text);
    std::vector<uint8_t> payload;
    group_seal(g, PacketType::GroupChat, pw.data, payload);

    int sent = 0;
    uint64_t now = now_ms();
    for (const auto& member : g.members) {
        if (member == identity_.id) continue;
        auto p = peers_.find(member);
        if (p == peers_.end() || !p->second.handshake_done) continue;
        if (!p->second.online(now, 30000)) continue;
        if (send_peer_packet(p->second, PacketType::GroupChat, payload)) ++sent;
    }
    ChatMsg msg;
    msg.id = id;
    msg.peer = g.id;
    msg.sender = identity_.id;
    msg.parent_hash = parent;
    msg.text = text;
    msg.outgoing = true;
    msg.delivered = true;
    msg.is_group = true;
    msg.ts_ms = ts;
    add_message_locked(msg);
    g.last_hash = hash;
    ++stats_.group_msgs;
    metric("group_send", g.id.short_hex(10), id, text.size(), -1, static_cast<int>(sent));
    log("GROUP \"" + g.name + "\" message sent to " + std::to_string(sent) + " peer(s)");
    (void)err;
    return true;
}

void Node::handle_group_chat(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    NodeId gid;
    if (!r.id(gid)) return;
    auto it = groups_.find(gid);
    if (it == groups_.end()) {
        ++stats_.dropped;
        return;
    }
    std::vector<uint8_t> plain;
    if (!open_record(it->second.group_key, r.cur(), r.remaining(),
                     static_cast<uint8_t>(PacketType::GroupChat), plain)) {
        ++stats_.dropped;
        return;
    }
    mark_invite_confirmed_locked(gid, sender);
    Reader pr(plain);
    uint32_t id = pr.u32();
    uint64_t ts = pr.u64();
    NodeId parent;
    if (!pr.id(parent)) return;
    std::string text = pr.str(2048);
    if (!pr.ok()) return;

    for (const auto& m : chat_) {
        if (m.is_group && m.peer == gid && m.sender == sender && m.id == id) return;
    }
    ChatMsg msg;
    msg.id = id;
    msg.peer = gid;
    msg.sender = sender;
    msg.parent_hash = parent;
    msg.text = text;
    msg.outgoing = false;
    msg.delivered = true;
    msg.is_group = true;
    msg.ts_ms = ts ? ts : unix_ms();
    add_message_locked(msg);
    it->second.last_hash = group_message_hash(gid, sender, id, msg.ts_ms, parent, text);
    ++stats_.group_msgs;
    metric("group_recv", sender.short_hex(10), id, text.size(),
           ts ? static_cast<int64_t>(unix_ms() - ts) : -1, 0);
    log("GROUP message from " + sender.short_hex(8) + " in \"" + it->second.name + "\"");
    cv_.notify_all();
}

bool Node::create_group(const std::string& name, const std::vector<NodeId>& members,
                        NodeId& out_id, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (name.empty()) {
        if (err) *err = "group name is empty";
        return false;
    }
    GroupRoom g;
    randombytes_buf(g.id.b.data(), g.id.b.size());
    if (g.id.empty()) g.id.b[0] = 1;
    randombytes_buf(g.group_key.data(), g.group_key.size());
    g.name = name;
    g.created_ms = unix_ms();
    g.members.push_back(identity_.id);
    for (const auto& m : members) {
        if (m == identity_.id || m.empty()) continue;
        if (std::find(g.members.begin(), g.members.end(), m) == g.members.end())
            g.members.push_back(m);
    }
    out_id = g.id;
    groups_[g.id] = g;
    for (const auto& m : g.members) {
        if (m == identity_.id) continue;
        send_group_invite_locked(groups_[g.id], m);
    }
    log("group \"" + name + "\" created (" + g.id.short_hex(10) + ", " +
        std::to_string(g.members.size()) + " members)");
    return true;
}

std::vector<GroupRoom> Node::groups() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<GroupRoom> out;
    for (const auto& kv : groups_) out.push_back(kv.second);
    std::sort(out.begin(), out.end(), [](const GroupRoom& a, const GroupRoom& b) {
        return a.name < b.name;
    });
    return out;
}

bool Node::group(const NodeId& id, GroupRoom& out) const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = groups_.find(id);
    if (it == groups_.end()) return false;
    out = it->second;
    return true;
}

// Send a group key update to a specific member (admin only).
bool Node::send_group_key_update_locked(const GroupRoom& group, const NodeId& member) {
    auto it = peers_.find(member);
    if (it == peers_.end() || !it->second.handshake_done) return false;
    Writer pw;
    pw.id(group.id);
    pw.raw(group.group_key.data(), group.group_key.size());
    pw.u16(static_cast<uint16_t>(group.members.size()));
    for (const auto& m : group.members) pw.id(m);
    std::vector<uint8_t> payload;
    if (!seal_with_peer(it->second, PacketType::GroupKeyUpdate, pw.data, payload)) return false;
    if (!send_peer_packet(it->second, PacketType::GroupKeyUpdate, payload)) return false;
    log("GROUP_KEY_UPDATE sent to " + member.short_hex(8) + " for \"" + group.name + "\"");
    return true;
}

bool Node::send_group_invite_locked(const GroupRoom& group, const NodeId& member, bool fresh) {
    auto it = peers_.find(member);
    if (it == peers_.end() || !it->second.handshake_done) return false;
    Writer pw;
    pw.id(group.id);
    pw.str(group.name);
    pw.raw(group.group_key.data(), group.group_key.size());
    pw.u16(static_cast<uint16_t>(group.members.size()));
    for (const auto& m : group.members) pw.id(m);
    std::vector<uint8_t> payload;
    if (!seal_with_peer(it->second, PacketType::GroupInvite, pw.data, payload)) return false;
    if (!send_peer_packet(it->second, PacketType::GroupInvite, payload)) return false;
    {
        InviteState& st = invite_state_[group.id][member];
        if (fresh) st = InviteState{};
        st.last_ms = now_ms();
        ++st.tries;
    }
    log("GROUP_INVITE sent to " + member.short_hex(8) + " for \"" + group.name + "\"");
    return true;
}

void Node::mark_invite_confirmed_locked(const NodeId& group, const NodeId& member) {
    auto g = invite_state_.find(group);
    if (g == invite_state_.end()) return;
    auto m = g->second.find(member);
    if (m != g->second.end()) m->second.confirmed = true;
}

void Node::resend_unconfirmed_invites_locked(uint64_t now) {
    for (auto& gk : invite_state_) {
        auto git = groups_.find(gk.first);
        if (git == groups_.end()) continue;
        for (auto mit = gk.second.begin(); mit != gk.second.end();) {
            // A member who was excluded meanwhile must never receive the rotated key again.
            const auto& members = git->second.members;
            if (std::find(members.begin(), members.end(), mit->first) == members.end()) {
                mit = gk.second.erase(mit);
                continue;
            }
            InviteState& st = mit->second;
            const NodeId member = mit->first;
            ++mit;
            if (st.confirmed || st.tries >= 24 || now - st.last_ms < 5000) continue;
            auto p = peers_.find(member);
            if (p == peers_.end() || !p->second.handshake_done) continue;
            send_group_invite_locked(git->second, member, false);
        }
    }
}

void Node::handle_group_invite(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto p = peers_.find(sender);
    if (p == peers_.end() || !p->second.handshake_done) return;
    std::vector<uint8_t> plain;
    if (!open_with_peer(p->second, PacketType::GroupInvite, r, plain)) return;
    Reader pr(plain);
    GroupRoom g;
    if (!pr.id(g.id)) return;
    g.name = pr.str(128);
    if (!pr.raw(g.group_key.data(), g.group_key.size())) return;
    uint16_t count = pr.u16();
    if (!pr.ok() || count > 256) return;
    for (uint16_t i = 0; i < count; ++i) {
        NodeId m;
        if (!pr.id(m)) return;
        if (std::find(g.members.begin(), g.members.end(), m) == g.members.end())
            g.members.push_back(m);
    }
    // The inviter must itself be listed as a member of the group it invites us to.
    const bool sender_listed =
        std::find(g.members.begin(), g.members.end(), sender) != g.members.end();
    if (std::find(g.members.begin(), g.members.end(), identity_.id) == g.members.end())
        g.members.push_back(identity_.id);
    g.created_ms = unix_ms();
    auto existing = groups_.find(g.id);
    if (existing == groups_.end()) {
        if (left_groups_.count(g.id)) {
            ++stats_.dropped;
            return;  // we left this group on purpose: a stale member list must not pull us back
        }
        if (!sender_listed || groups_.size() >= 200) {
            ++stats_.dropped;
            log("GROUP_INVITE ignored from " + sender.short_hex(8) + " (not a member / too many groups)");
            return;
        }
        groups_[g.id] = g;
        log("joined group \"" + g.name + "\" (" + g.id.short_hex(10) + ")");
    } else {
        GroupRoom& cur = existing->second;
        const bool from_admin = !cur.members.empty() && cur.members.front() == sender;
        const bool same_key = cur.group_key == g.group_key;
        if (from_admin) {
            // The creator/admin is authoritative for name, key and member list.
            cur.name = g.name;
            cur.group_key = g.group_key;
            cur.members = g.members;
            log("group \"" + g.name + "\" keys/members updated");
        } else if (same_key &&
                   std::find(cur.members.begin(), cur.members.end(), sender) != cur.members.end()) {
            // A member re-sends its (possibly stale) view: it may only ADD members, never remove
            // them or change the key; otherwise a stale peer would undo a key rotation or drop
            // somebody the admin had just invited.
            for (const auto& m : g.members)
                if (std::find(cur.members.begin(), cur.members.end(), m) == cur.members.end())
                    cur.members.push_back(m);
        } else {
            ++stats_.dropped;
            log("GROUP_INVITE ignored from " + sender.short_hex(8) + " for \"" + cur.name +
                "\" (not the admin, different key)");
            return;
        }
    }
    sync_group(g.id);
    cv_.notify_all();
}

bool Node::invite_to_group(const NodeId& group_id, const NodeId& peer_id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return false;
    if (std::find(it->second.members.begin(), it->second.members.end(), peer_id) ==
        it->second.members.end())
        it->second.members.push_back(peer_id);
    const bool ok = send_group_invite_locked(it->second, peer_id);
    // Tell the other members about the newcomer too; otherwise they keep a stale member list and
    // never address him (members may only ADD people, see handle_group_invite).
    for (const auto& m : it->second.members) {
        if (m == identity_.id || m == peer_id) continue;
        send_group_invite_locked(it->second, m);
    }
    return ok;
}

// Remove a member from the group and rotate the group key (admin only).
bool Node::exclude_from_group(const NodeId& group, const NodeId& peer,
                              std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = groups_.find(group);
    if (it == groups_.end()) {
        if (err) *err = "group not found";
        return false;
    }
    GroupRoom& g = it->second;
    // Only the admin (first member) can exclude.
    if (g.members.empty() || g.members[0] != identity_.id) {
        if (err) *err = "only group admin can exclude members";
        return false;
    }
    auto mem_it = std::find(g.members.begin(), g.members.end(), peer);
    if (mem_it == g.members.end()) {
        if (err) *err = "member not in group";
        return false;
    }
    // Don't let admin exclude themselves via this path.
    if (peer == identity_.id) {
        if (err) *err = "admin should use leave_group";
        return false;
    }
    g.members.erase(mem_it);
    // Rotate the group key and send it to remaining members.
    randombytes_buf(g.group_key.data(), g.group_key.size());
    log("group \"" + g.name + "\" key rotated after excluding " +
        peer.short_hex(8));
    // Send the new key to each remaining member via pairwise ratchet.
    for (const auto& m : g.members) {
        if (m == identity_.id) continue;
        send_group_key_update_locked(g, m);
    }
    return true;
}

// Leave a group voluntarily.
bool Node::leave_group(const NodeId& group, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = groups_.find(group);
    if (it == groups_.end()) {
        if (err) *err = "group not found";
        return false;
    }
    GroupRoom& g = it->second;
    auto mem_it = std::find(g.members.begin(), g.members.end(), identity_.id);
    if (mem_it == g.members.end()) {
        if (err) *err = "not a member of this group";
        return false;
    }
    // If we're the admin, rotate the key before leaving so we can't read
    // future messages.
    bool was_admin = (!g.members.empty() && g.members[0] == identity_.id);
    g.members.erase(mem_it);
    if (was_admin) {
        if (!g.members.empty()) {
            randombytes_buf(g.group_key.data(), g.group_key.size());
            log("group \"" + g.name + "\" key rotated after admin left");
            for (const auto& m : g.members) {
                send_group_key_update_locked(g, m);
            }
        }
    }
    // The group disappears from OUR list in every case (there is no server to delete it for the
    // others; they keep their copy, and after an admin leaves they get a new key).
    log("left group \"" + g.name + "\"");
    left_groups_.insert(group);  // ignore re-invites from members that still list us (this session)
    invite_state_.erase(group);
    groups_.erase(it);
    return true;
}

void Node::sync_group_quiet_locked(const NodeId& group_id) {
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return;
    GroupRoom& g = it->second;
    Writer pw;
    pw.u8(0);
    pw.id(g.last_hash);
    std::vector<uint8_t> payload;
    group_seal(g, PacketType::GroupSync, pw.data, payload);
    uint64_t now = now_ms();
    for (const auto& member : g.members) {
        if (member == identity_.id) continue;
        auto p = peers_.find(member);
        if (p == peers_.end() || !p->second.handshake_done || !p->second.online(now, 30000))
            continue;
        send_peer_packet(p->second, PacketType::GroupSync, payload);
    }
}

bool Node::sync_group(const NodeId& group_id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return false;
    GroupRoom& g = it->second;
    Writer pw;
    pw.u8(0);
    pw.id(g.last_hash);
    std::vector<uint8_t> payload;
    group_seal(g, PacketType::GroupSync, pw.data, payload);
    int sent = 0;
    for (const auto& member : g.members) {
        if (member == identity_.id) continue;
        auto p = peers_.find(member);
        if (p == peers_.end() || !p->second.handshake_done) continue;
        if (send_peer_packet(p->second, PacketType::GroupSync, payload)) ++sent;
    }
    log("GROUP_SYNC request for \"" + g.name + "\" sent to " + std::to_string(sent) +
        " peer(s)");
    return true;
}

void Node::handle_group_sync(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    NodeId gid;
    if (!r.id(gid)) return;
    auto it = groups_.find(gid);
    auto p = peers_.find(sender);
    if (it == groups_.end() || p == peers_.end()) return;
    std::vector<uint8_t> plain;
    if (!open_record(it->second.group_key, r.cur(), r.remaining(),
                     static_cast<uint8_t>(PacketType::GroupSync), plain))
        return;
    mark_invite_confirmed_locked(gid, sender);
    Reader pr(plain);
    uint8_t mode = pr.u8();
    if (!pr.ok()) return;
    if (mode == 0) {
        NodeId last;
        if (!pr.id(last)) return;
        (void)last;
        Writer out;
        out.u8(1);
        std::vector<ChatMsg> list;
        for (const auto& m : chat_)
            if (m.is_group && m.peer == gid) list.push_back(m);
        // Keep the reply inside one UDP datagram: newest messages first until
        // ~1000 bytes of payload (the old cap of 100 x 2 KB could never fit).
        size_t keep = 0, bytes = 0;
        for (auto rit = list.rbegin(); rit != list.rend() && keep < 40; ++rit) {
            size_t sz = 4 + 8 + 20 + 20 + 2 + rit->text.size();
            if (bytes + sz > 1000) break;
            bytes += sz;
            ++keep;
        }
        list.erase(list.begin(), list.end() - static_cast<std::ptrdiff_t>(keep));
        out.u16(static_cast<uint16_t>(list.size()));
        for (const auto& m : list) {
            out.u32(m.id);
            out.u64(m.ts_ms);
            out.id(m.sender);
            out.id(m.parent_hash);
            out.str(m.text);
        }
        std::vector<uint8_t> payload;
        group_seal(it->second, PacketType::GroupSync, out.data, payload);
        send_peer_packet(p->second, PacketType::GroupSync, payload);
        return;
    }
    uint16_t count = pr.u16();
    if (!pr.ok() || count > 256) return;
    int merged = 0;
    for (uint16_t i = 0; i < count; ++i) {
        ChatMsg m;
        m.id = pr.u32();
        m.ts_ms = pr.u64();
        if (!pr.id(m.sender) || !pr.id(m.parent_hash)) return;
        m.text = pr.str(2048);
        if (!pr.ok()) return;
        m.peer = gid;
        m.is_group = true;
        m.delivered = true;
        bool exists = false;
        for (const auto& old : chat_) {
            if (old.is_group && old.peer == gid && old.sender == m.sender && old.id == m.id) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            m.outgoing = m.sender == identity_.id;
            add_message_locked(m);
            ++merged;
            if (!m.outgoing)
                metric("group_recv_sync", m.sender.short_hex(10), m.id, m.text.size(),
                       static_cast<int64_t>(unix_ms() - m.ts_ms), 0);
        }
    }
    if (merged) log("GROUP_SYNC merged " + std::to_string(merged) + " message(s)");
    cv_.notify_all();
}

bool Node::set_relay(const NodeId& relay) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    relay_id_ = relay;
    log(relay.empty() ? "relay disabled" : "relay selected: " + relay.short_hex(10));
    return true;
}

NodeId Node::relay() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return relay_id_;
}

bool Node::send_inner_packet(const sockaddr_in& to, const NodeId& target, uint8_t inner_type,
                             const std::vector<uint8_t>& inner_packet) {
    (void)inner_type;
    if (inner_packet.size() > 1100) return false;
    Writer w;
    w.id(target);
    w.u16(static_cast<uint16_t>(inner_packet.size()));
    w.raw(inner_packet.data(), inner_packet.size());
    return send_to(to, PacketType::Relay, w.data, kRelayFlag);
}

bool Node::send_chat_relay(const NodeId& relay, const NodeId& peer, const std::string& text,
                           std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (text.empty() || text.size() > 900) {
        if (err) *err = "invalid message length";
        return false;
    }
    auto p = peers_.find(peer);
    if (p == peers_.end() || !p->second.handshake_done) {
        if (err) *err = "peer session is not established";
        return false;
    }
    auto r = peers_.find(relay);
    if (r == peers_.end()) {
        if (err) *err = "relay node is unknown";
        return false;
    }
    uint32_t id = next_msg_id_++;
    Writer pw;
    pw.u32(id);
    pw.u64(unix_ms());
    pw.str(text);
    std::vector<uint8_t> sealed_payload;
    if (!seal_with_peer(p->second, PacketType::Chat, pw.data, sealed_payload)) {
        if (err) *err = "ratchet key derivation failed";
        return false;
    }
    std::vector<uint8_t> inner = make_packet(identity_, PacketType::Chat, sealed_payload);
    if (!send_inner_packet(r->second.addr, peer, static_cast<uint8_t>(PacketType::Chat),
                           inner)) {
        if (err) *err = "relay send failed";
        return false;
    }
    { uint64_t t = now_ms(); pending_chats_[id] = PendingChat{peer, t, 0, t, text.size()}; }
    metric("chat_send_relay", peer.short_hex(10), id, text.size(), -1, 0);
    ChatMsg msg;
    msg.id = id;
    msg.peer = peer;
    msg.sender = identity_.id;
    msg.text = text;
    msg.outgoing = true;
    msg.delivered = false;
    msg.ts_ms = unix_ms();
    add_message_locked(msg);
    ++stats_.chat_sent;
    log("CHAT sent via relay " + relay.short_hex(8) + " to " + peer.short_hex(8));
    return true;
}

bool Node::send_file_payload_locked(Peer& peer, PacketType type,
                                    const std::vector<uint8_t>& plain) {
    std::vector<uint8_t> payload;
    if (!seal_with_peer(peer, type, plain, payload)) return false;
    return send_peer_packet(peer, type, payload);
}

int Node::send_file_group(const NodeId& gid, const std::string& path, std::string* err) {
    std::vector<NodeId> members;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto g = groups_.find(gid);
        if (g == groups_.end()) {
            if (err) *err = "unknown group";
            return 0;
        }
        const uint64_t now = now_ms();
        for (const auto& m : g->second.members) {
            if (m == identity_.id) continue;
            auto pit = peers_.find(m);
            if (pit != peers_.end() && !pit->second.relayed && is_bootstrap_addr_locked(pit->second.addr))
                continue;  // relay/bootstrap servers are infrastructure, not recipients
            if (pit == peers_.end() || !pit->second.handshake_done || !pit->second.online(now, 30000))
                continue;  // offline members cannot answer an offer
            members.push_back(m);
        }
    }
    if (members.empty()) {
        if (err)
            *err = "no other person in this group is online (relay servers do not receive files)";
        return 0;
    }
    int started = 0;
    std::string last;
    uint64_t batch = 0;
    randombytes_buf(&batch, sizeof(batch));
    if (batch == 0) batch = 1;
    for (const auto& m : members) {
        std::string e;
        if (send_file(m, path, &e, gid, batch)) ++started;
        else last = e;
    }
    if (started == 0 && err) *err = last.empty() ? "no group member is reachable" : last;
    return started;
}

bool Node::send_file(const NodeId& peer, const std::string& path, std::string* err,
                     const NodeId& group, uint64_t batch) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(fs_u8(path), ec) || ec) {
        if (err) *err = "file does not exist";
        return false;
    }
    uint64_t size = std::filesystem::file_size(fs_u8(path), ec);
    if (ec) {
        if (err) *err = "cannot read file size";
        return false;
    }
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    {
        auto rp = peers_.find(peer);
        if (rp != peers_.end() && !rp->second.relayed && is_bootstrap_addr_locked(rp->second.addr)) {
            if (err) *err = "this is a relay server, not a person: it does not receive files";
            return false;
        }
    }
    auto it = peers_.find(peer);
    if (it == peers_.end() || !it->second.handshake_done) {
        if (err) *err = "peer session is not established";
        return false;
    }
    {
        size_t active = 0;
        for (const auto& kv : outgoing_files_)
            if (!kv.second->info.complete && !kv.second->info.failed) ++active;
        if (active >= (batch ? 64u : 16u)) {  // one group send = one transfer per member
            if (err) *err = "too many active outgoing transfers";
            return false;
        }
    }
    auto job = std::make_shared<FileJob>();
    uint64_t file_id = 0;
    randombytes_buf(&file_id, sizeof(file_id));
    if (file_id == 0) file_id = next_file_id_++;
    job->info.id = file_id;
    job->info.peer = peer;
    job->info.name = safe_file_name(path);
    job->info.path = path;
    job->info.size = size;
    job->info.outgoing = true;
    job->info.group = group;
    job->info.group_batch = batch;
    job->info.unix_ms = unix_ms();
    job->info.started_ms = now_ms();
    job->info.updated_ms = job->info.started_ms;
    job->source = path;
    job->size = size;
    job->chunk_size = 1000;
    uint64_t id = job->info.id;
    outgoing_files_[id] = job;
    job->thread = std::thread(&Node::file_sender_loop, this, id);
    log("FILE offer started: " + job->info.name + " (" + std::to_string(size) +
        " bytes) to " + peer.short_hex(8));
    return true;
}

void Node::set_download_dir(const std::string& dir) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    download_dir_ = dir.empty() ? "downloads" : dir;
}

std::string Node::download_dir() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return download_dir_;
}

bool Node::set_auto_accept_files(bool enabled, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto_accept_files_ = enabled;
    if (err) err->clear();
    return true;
}

std::vector<FileTransferInfo> Node::file_transfers() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<FileTransferInfo> out;
    for (const auto& kv : outgoing_files_) out.push_back(kv.second->info);
    for (const auto& kv : incoming_files_) out.push_back(kv.second.info);
    std::sort(out.begin(), out.end(), [](const FileTransferInfo& a, const FileTransferInfo& b) {
        return a.started_ms > b.started_ms;
    });
    return out;
}

bool Node::file_transfer(uint64_t id, FileTransferInfo& out) const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto o = outgoing_files_.find(id);
    if (o != outgoing_files_.end()) {
        out = o->second->info;
        return true;
    }
    auto i = incoming_files_.find(id);
    if (i != incoming_files_.end()) {
        out = i->second.info;
        return true;
    }
    return false;
}

bool Node::cancel_file_transfer(uint64_t id) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto o = outgoing_files_.find(id);
    if (o != outgoing_files_.end()) {
        o->second->stop.store(true);
        o->second->cancel = true;
        o->second->info.failed = true;
        o->second->info.error = "cancelled";
        cv_.notify_all();
        return true;
    }
    auto i = incoming_files_.find(id);
    if (i != incoming_files_.end()) {
        i->second.info.failed = true;
        i->second.info.error = "cancelled";
        i->second.out.close();
        return true;
    }
    return false;
}

void Node::file_sender_loop(uint64_t transfer_id) {
    std::shared_ptr<FileJob> job;
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        auto it = outgoing_files_.find(transfer_id);
        if (it == outgoing_files_.end()) return;
        job = it->second;
    }

    bool hash_ok = false;
    std::array<uint8_t, 32> sha = sha256_file(job->source, hash_ok);
    if (!hash_ok) {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        job->info.failed = true;
        job->info.error = "cannot hash source file";
        job->info.updated_ms = now_ms();
        cv_.notify_all();
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(mtx_);
        job->sha = sha;
    }

    std::unique_lock<std::recursive_mutex> lk(mtx_);
    uint64_t wait_start = now_ms();
    while (!job->stop.load()) {
        auto p = peers_.find(job->info.peer);
        if (p != peers_.end() && p->second.handshake_done) break;
        if (now_ms() - wait_start > 15000) {
            job->info.failed = true;
            job->info.error = "peer is offline";
            job->info.updated_ms = now_ms();
            cv_.notify_all();
            return;
        }
        cv_.wait_for(lk, std::chrono::milliseconds(200));
    }
    if (job->stop.load()) return;
    auto pit = peers_.find(job->info.peer);
    if (pit == peers_.end()) return;
    // The peer entry can disappear while we wait (contact removed mid-transfer): never hold
    // a reference across a wait, look it up for every packet.
    auto send_fp = [&](PacketType t, const std::vector<uint8_t>& plain) -> bool {
        auto q = peers_.find(job->info.peer);
        if (q == peers_.end()) return false;
        return send_file_payload_locked(q->second, t, plain);
    };

    Writer offer;
    offer.u64(transfer_id);
    offer.str(job->info.name);
    offer.u64(job->size);
    offer.u32(job->chunk_size);
    offer.raw(sha.data(), sha.size());
    offer.id(job->info.group);  // trailing: the receiver files the transfer under this group
    if (!send_fp(PacketType::FileOffer, offer.data)) {
        job->info.failed = true;
        job->info.error = "cannot send file offer";
        job->info.updated_ms = now_ms();
        cv_.notify_all();
        return;
    }
    job->info.updated_ms = now_ms();
    // The offer and its ACCEPT travel over UDP: resend the offer once a second
    // for up to 10 s instead of failing on a single lost datagram.
    for (int attempt = 0; attempt < 10 && !job->accepted && !job->info.failed &&
                          !job->stop.load();
         ++attempt) {
        cv_.wait_for(lk, std::chrono::milliseconds(1000),
                     [&] { return job->accepted || job->info.failed || job->stop.load(); });
        if (job->accepted || job->info.failed || job->stop.load()) break;
        send_fp(PacketType::FileOffer, offer.data);
    }
    if (job->stop.load()) return;
    if (!job->accepted) {
        if (!job->info.failed) {
            job->info.failed = true;
            job->info.error = "no answer from the other side (it may be offline, need a reconnect, or run an older version)";
        }
        job->info.updated_ms = now_ms();
        cv_.notify_all();
        return;
    }

    std::ifstream in(fs_u8(job->source), std::ios::binary);
    if (!in) {
        job->info.failed = true;
        job->info.error = "cannot open source file";
        job->info.updated_ms = now_ms();
        cv_.notify_all();
        return;
    }

    uint64_t offset = std::min(job->remote_have, job->size);
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<char> buf(job->chunk_size);
    uint32_t since_ack = 0;

    while (offset < job->size && !job->stop.load()) {
        // No rewind check here: with more than one chunk in flight, offset is
        // legitimately ahead of remote_have.  Recovery happens after the
        // window wait, where a stalled acknowledgement is unambiguous.
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize n = in.gcount();
        if (n <= 0) break;
        Writer data;
        data.u64(transfer_id);
        data.u64(offset);
        data.raw(buf.data(), static_cast<size_t>(n));
        if (!send_fp(PacketType::FileData, data.data)) {
            job->info.failed = true;
            job->info.error = "network send failed";
            job->info.updated_ms = now_ms();
            cv_.notify_all();
            return;
        }
        offset += static_cast<uint64_t>(n);
        job->info.transferred = offset;
        job->info.updated_ms = now_ms();
        ++since_ack;
        // Yield the node mutex between chunks so the receive thread can
        // process FILE_ACK while a large file is being sent.
        lk.unlock();
        lk.lock();
        if (since_ack >= kFileWindow || offset == job->size) {
            uint64_t target = offset;
            cv_.wait_for(lk, std::chrono::milliseconds(150),
                         [&] { return job->remote_have >= target || job->stop.load(); });
            if (job->stop.load()) return;
            if (job->remote_have < offset) {
                offset = job->remote_have;
                in.clear();
                in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
            }
            since_ack = 0;
        }
    }

    // Retransmit any tail that the receiver did not acknowledge (UDP loss).
    int retries = 0;
    while (job->remote_have < job->size && retries < 600 && !job->stop.load()) {
        uint64_t target = job->size;
        cv_.wait_for(lk, std::chrono::milliseconds(200),
                     [&] { return job->remote_have >= target || job->stop.load(); });
        if (job->remote_have >= job->size || job->stop.load()) break;
        offset = std::min(job->remote_have, job->size);
        in.clear();
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        int burst = 0;
        while (offset < job->size && !job->stop.load() && burst < static_cast<int>(kFileWindow)) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize n = in.gcount();
            if (n <= 0) break;
            Writer data;
            data.u64(transfer_id);
            data.u64(offset);
            data.raw(buf.data(), static_cast<size_t>(n));
            if (!send_fp(PacketType::FileData, data.data)) break;
            offset += static_cast<uint64_t>(n);
            job->info.transferred = offset;
            job->info.updated_ms = now_ms();
            ++burst;
            lk.unlock();
            lk.lock();
        }
        ++retries;
    }
    if (job->stop.load()) return;
    if (job->remote_have < job->size) {
        job->info.failed = true;
        job->info.error = "receiver did not acknowledge all data";
        job->info.updated_ms = now_ms();
        cv_.notify_all();
        return;
    }

    Writer done;
    done.u64(transfer_id);
    done.u64(job->size);
    done.raw(sha.data(), sha.size());
    const uint64_t done_deadline = now_ms() + 20000;
    while (!job->info.complete && !job->info.failed && !job->stop.load()) {
        if (!send_fp(PacketType::FileDone, done.data)) {
            job->info.failed = true;
            job->info.error = "cannot send completion marker";
            break;
        }
        job->done_sent = true;
        if (now_ms() >= done_deadline) break;
        cv_.wait_for(lk, std::chrono::milliseconds(500),
                     [&] { return job->info.complete || job->info.failed || job->stop.load(); });
    }
    if (!job->info.complete && !job->info.failed && !job->stop.load()) {
        job->info.failed = true;
        job->info.error = "receiver did not confirm completion";
    }
    job->info.updated_ms = now_ms();
    cv_.notify_all();
}

void Node::handle_file_offer(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto p = peers_.find(sender);
    if (p == peers_.end() || !p->second.handshake_done) return;
    std::vector<uint8_t> plain;
    if (!open_with_peer(p->second, PacketType::FileOffer, r, plain)) return;
    Reader pr(plain);
    uint64_t id = pr.u64();
    std::string name = pr.str(255);
    uint64_t size = pr.u64();
    uint32_t chunk = pr.u32();
    std::array<uint8_t, 32> sha{};
    if (!pr.raw(sha.data(), sha.size()) || !pr.ok() || chunk < 128 || chunk > 4000) return;
    // Optional trailing group id (older senders do not send it).  It is honoured only when we
    // and the sender are both members of that group; otherwise the file stays a private one.
    NodeId offer_group;
    if (pr.remaining() >= kIdBytes) {
        NodeId gid;
        if (pr.id(gid) && !gid.empty()) {
            auto git = groups_.find(gid);
            if (git != groups_.end()) {
                const auto& mem = git->second.members;
                if (std::find(mem.begin(), mem.end(), sender) != mem.end() &&
                    std::find(mem.begin(), mem.end(), identity_.id) != mem.end())
                    offer_group = gid;
            }
        }
    }

    // Duplicate offer (the sender retransmits it when our ACCEPT was lost):
    // answer again with the current offset instead of restarting the transfer.
    {
        auto dup = incoming_files_.find(id);
        if (dup != incoming_files_.end() && dup->second.info.peer == sender &&
            !dup->second.info.complete && !dup->second.info.failed) {
            Writer w;
            w.u64(id);
            w.u8(1);
            w.u64(dup->second.have);
            w.str("");
            send_file_payload_locked(p->second, PacketType::FileAccept, w.data);
            return;
        }
    }
    auto reject = [&](const std::string& why) {
        Writer w;
        w.u64(id);
        w.u8(0);
        w.u64(0);
        w.str(why);
        send_file_payload_locked(p->second, PacketType::FileAccept, w.data);
    };
    if (!auto_accept_files_) {
        reject("automatic file acceptance is disabled");
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(fs_u8(download_dir_), ec);
    if (ec) {
        reject("cannot create download directory");
        return;
    }
    std::string final_path = unique_path(download_dir_, safe_file_name(name));
    std::string part_path = final_path + ".part";
    uint64_t resume = 0;
    if (std::filesystem::exists(fs_u8(part_path), ec) && !ec)
        resume = std::filesystem::file_size(fs_u8(part_path), ec);
    if (ec || resume > size) {
        std::filesystem::remove(fs_u8(part_path), ec);
        resume = 0;
    }
    std::ofstream out;
    if (resume > 0) {
        out.open(fs_u8(part_path), std::ios::binary | std::ios::in | std::ios::out);
        if (out) out.seekp(static_cast<std::streamoff>(resume), std::ios::beg);
    } else {
        out.open(fs_u8(part_path), std::ios::binary | std::ios::trunc);
    }
    if (!out) {
        reject("cannot open destination file");
        return;
    }
    FileIncoming inc;
    inc.info.id = id;
    inc.info.peer = sender;
    inc.info.name = safe_file_name(name);
    inc.info.path = final_path;
    inc.info.size = size;
    inc.info.transferred = resume;
    inc.info.outgoing = false;
    inc.info.group = offer_group;
    inc.info.unix_ms = unix_ms();
    inc.info.started_ms = now_ms();
    inc.info.updated_ms = inc.info.started_ms;
    inc.have = resume;
    inc.chunk_size = chunk;
    inc.sha = sha;
    inc.out = std::move(out);
    incoming_files_[id] = std::move(inc);

    Writer w;
    w.u64(id);
    w.u8(1);
    w.u64(resume);
    w.str("");
    send_file_payload_locked(p->second, PacketType::FileAccept, w.data);
    log("FILE incoming: " + safe_file_name(name) + " (" + std::to_string(size) +
        " bytes), resume " + std::to_string(resume));
    cv_.notify_all();
}

void Node::handle_file_accept(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto p = peers_.find(sender);
    if (p == peers_.end()) return;
    std::vector<uint8_t> plain;
    if (!open_with_peer(p->second, PacketType::FileAccept, r, plain)) return;
    Reader pr(plain);
    uint64_t id = pr.u64();
    uint8_t accepted = pr.u8();
    uint64_t resume = pr.u64();
    std::string why = pr.str(255);
    if (!pr.ok()) return;
    auto it = outgoing_files_.find(id);
    if (it == outgoing_files_.end()) return;
    auto& job = *it->second;
    if (job.info.peer != sender) return;
    if (!accepted) {
        job.info.failed = true;
        job.info.error = why.empty() ? "file rejected" : why;
        job.info.updated_ms = now_ms();
        cv_.notify_all();
        return;
    }
    job.accepted = true;
    job.remote_have = std::max(job.remote_have, std::min(resume, job.size));
    job.info.transferred = job.remote_have;
    job.info.updated_ms = now_ms();
    cv_.notify_all();
}

void Node::handle_file_data(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto p = peers_.find(sender);
    if (p == peers_.end()) return;
    std::vector<uint8_t> plain;
    if (!open_with_peer(p->second, PacketType::FileData, r, plain)) return;
    Reader pr(plain);
    uint64_t id = pr.u64();
    uint64_t offset = pr.u64();
    if (!pr.ok()) return;
    std::vector<uint8_t> data(pr.cur(), pr.cur() + pr.remaining());
    auto it = incoming_files_.find(id);
    if (it == incoming_files_.end()) return;
    FileIncoming& in = it->second;
    if (in.info.peer != sender) return;
    if (offset > in.have) {
        // Ahead of a gap: keep it for later instead of forcing a retransmit.
        // The cap is 4 MiB of 1 kB chunks; beyond that the offer is abusive
        // or the sender is hopelessly out of order, so fall back to re-ACKing
        // the current offset and let it rewind.
        if (in.pending.size() < kFilePendingMax &&
            in.pending.find(offset) == in.pending.end() &&
            offset + data.size() <= in.info.size)
            in.pending.emplace(offset, std::move(data));
        Writer w;
        w.u64(id);
        w.u64(in.have);
        send_file_payload_locked(p->second, PacketType::FileAck, w.data);
        return;
    }
    if (offset < in.have) {  // duplicate after resume/retransmit
        Writer w;
        w.u64(id);
        w.u64(in.have);
        send_file_payload_locked(p->second, PacketType::FileAck, w.data);
        return;
    }
    if (in.have + data.size() > in.info.size) {
        in.info.failed = true;
        in.info.error = "received more data than announced";
        return;
    }
    in.out.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    if (!in.out) {
        in.info.failed = true;
        in.info.error = "cannot write destination file";
        return;
    }
    in.have += data.size();
    // Drain whatever run of buffered chunks now becomes contiguous.
    while (true) {
        auto pit = in.pending.find(in.have);
        if (pit == in.pending.end()) break;
        const auto& raw = pit->second;
        if (in.have + raw.size() > in.info.size) {
            in.pending.erase(pit);
            break;
        }
        in.out.write(reinterpret_cast<const char*>(raw.data()),
                     static_cast<std::streamsize>(raw.size()));
        if (!in.out) {
            in.info.failed = true;
            in.info.error = "cannot write destination file";
            return;
        }
        in.have += raw.size();
        in.pending.erase(pit);
    }
    in.info.transferred = in.have;
    in.info.updated_ms = now_ms();
    // Acknowledge every contiguous chunk.  ACKs are small, and frequent
    // feedback keeps the sender's 16-chunk window from stalling when one
    // datagram is lost or reordered.
    {
        Writer w;
        w.u64(id);
        w.u64(in.have);
        if (send_file_payload_locked(p->second, PacketType::FileAck, w.data)) {
            in.last_ack_have = in.have;
            in.last_ack_ms = now_ms();
        }
    }
    cv_.notify_all();
}

void Node::handle_file_ack(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto p = peers_.find(sender);
    if (p == peers_.end()) return;
    std::vector<uint8_t> plain;
    if (!open_with_peer(p->second, PacketType::FileAck, r, plain)) return;
    Reader pr(plain);
    uint64_t id = pr.u64();
    uint64_t have = pr.u64();
    if (!pr.ok()) return;
    auto it = outgoing_files_.find(id);
    if (it == outgoing_files_.end()) return;
    auto& job = *it->second;
    if (job.info.peer != sender) return;
    if (have > job.remote_have) job.remote_have = std::min(have, job.size);
    job.info.updated_ms = now_ms();
    cv_.notify_all();
}

void Node::handle_file_done(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto p = peers_.find(sender);
    if (p == peers_.end()) return;
    std::vector<uint8_t> plain;
    if (!open_with_peer(p->second, PacketType::FileDone, r, plain)) return;
    Reader pr(plain);
    uint64_t id = pr.u64();
    uint64_t size = pr.u64();
    std::array<uint8_t, 32> sha{};
    if (!pr.raw(sha.data(), sha.size()) || !pr.ok()) return;
    auto it = incoming_files_.find(id);
    if (it == incoming_files_.end()) return;
    FileIncoming& in = it->second;
    if (in.info.peer != sender) return;
    // A retransmitted DONE must not re-open or re-hash an already completed
    // transfer.  Re-send the cached verdict so a lost COMPLETE is recoverable.
    if (in.info.complete || in.info.failed) {
        Writer w;
        w.u64(id);
        w.u8(in.info.complete ? 1 : 0);
        w.u64(in.have);
        w.str(in.info.error);
        send_file_payload_locked(p->second, PacketType::FileComplete, w.data);
        return;
    }
    in.out.flush();
    in.out.close();
    bool ok = (size == in.info.size && in.have == in.info.size);
    std::string why;
    if (!ok) {
        why = "size mismatch or incomplete data";
    } else {
        bool hok = false;
        auto got = sha256_file(in.info.path + ".part", hok);
        if (!hok || std::memcmp(got.data(), sha.data(), got.size()) != 0) {
            ok = false;
            why = "SHA-256 mismatch";
            std::error_code rmec;
            std::filesystem::remove(fs_u8(in.info.path + ".part"), rmec);
        }
    }
    if (ok) {
        std::error_code ec;
        // Never delete an existing file (it may be open in a viewer or be the
        // user's own): if the target is taken or locked, finish under a new name.
        if (std::filesystem::exists(fs_u8(in.info.path), ec)) {
            ec = std::make_error_code(std::errc::file_exists);   // rename() would silently replace it
        } else {
            std::filesystem::rename(fs_u8(in.info.path + ".part"), fs_u8(in.info.path), ec);
        }
        if (ec || (std::filesystem::exists(fs_u8(in.info.path + ".part")) && !ec)) {
            std::error_code ec2;
            std::filesystem::path cur = fs_u8(in.info.path);
            std::string alt = unique_path(cur.parent_path().u8string(), cur.filename().u8string());
            std::filesystem::rename(fs_u8(in.info.path + ".part"), fs_u8(alt), ec2);
            if (!ec2) {
                in.info.path = alt;
                ec.clear();
            }
        }
        if (ec) {
            ok = false;
            why = "cannot rename completed file";
        }
    }
    in.info.complete = ok;
    in.info.failed = !ok;
    in.info.error = why;
    in.info.transferred = in.have;
    in.info.updated_ms = now_ms();
    Writer w;
    w.u64(id);
    w.u8(ok ? 1 : 0);
    w.u64(in.have);
    w.str(why);
    send_file_payload_locked(p->second, PacketType::FileComplete, w.data);
    log("FILE " + std::string(ok ? "complete: " : "failed: ") + in.info.name +
        (why.empty() ? "" : " (" + why + ")"));
    cv_.notify_all();
}

void Node::handle_group_key_update(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto p = peers_.find(sender);
    if (p == peers_.end() || !p->second.handshake_done) return;
    std::vector<uint8_t> plain;
    if (!open_with_peer(p->second, PacketType::GroupKeyUpdate, r, plain)) return;
    Reader pr(plain);
    NodeId gid;
    if (!pr.id(gid)) return;
    auto it = groups_.find(gid);
    if (it == groups_.end()) return;
    // The sender must be the group admin (first member) in our view, and the
    // updated list must contain the sender but not necessarily the leaver.
    if (it->second.members.empty() || it->second.members[0] != sender) return;
    std::array<uint8_t, 32> new_key{};
    if (!pr.raw(new_key.data(), new_key.size())) return;
    uint16_t count = pr.u16();
    if (!pr.ok() || count == 0 || count > 256) return;
    std::vector<NodeId> members;
    members.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        NodeId m;
        if (!pr.id(m)) return;
        members.push_back(m);
    }
    if (std::find(members.begin(), members.end(), identity_.id) == members.end()) return;
    it->second.group_key = new_key;
    it->second.members = std::move(members);
    log("GROUP_KEY_UPDATE applied for \"" + it->second.name +
        "\" (new fingerprint " + key_fingerprint(it->second.group_key).substr(0, 8) + ")");
    cv_.notify_all();
}

void Node::handle_file_complete(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto p = peers_.find(sender);
    if (p == peers_.end()) return;
    std::vector<uint8_t> plain;
    if (!open_with_peer(p->second, PacketType::FileComplete, r, plain)) return;
    Reader pr(plain);
    uint64_t id = pr.u64();
    uint8_t ok = pr.u8();
    uint64_t bytes = pr.u64();
    std::string why = pr.str(255);
    if (!pr.ok()) return;
    auto it = outgoing_files_.find(id);
    if (it == outgoing_files_.end()) return;
    auto& job = *it->second;
    if (job.info.peer != sender) return;
    job.info.transferred = bytes;
    job.info.complete = ok != 0;
    job.info.failed = ok == 0;
    job.info.error = why;
    job.info.updated_ms = now_ms();
    log("FILE receiver status: " + std::string(ok ? "all bytes received" : "failed") +
        (why.empty() ? "" : " (" + why + ")"));
    cv_.notify_all();
}

void Node::handle_relay(Reader& r, const sockaddr_in& from) {
    NodeId target;
    if (!r.id(target)) return;
    uint16_t inner_len = r.u16();
    if (!r.ok() || inner_len == 0 || inner_len > kMaxPacket || r.remaining() < inner_len)
        return;
    std::vector<uint8_t> inner(inner_len);
    if (!r.raw(inner.data(), inner.size())) return;

    if (target == identity_.id) {
        handle_packet(inner.data(), inner.size(), from, true);
        return;
    }
    auto it = peers_.find(target);
    if (it == peers_.end()) {
        ++stats_.dropped;
        return;
    }
    // The relay only observes the opaque inner packet; it has no session key.
    Writer w;
    w.id(target);
    w.u16(inner_len);
    w.raw(inner.data(), inner.size());
    if (send_to(it->second.addr, PacketType::Relay, w.data, kRelayFlag)) {
        ++stats_.relay_forwarded;
        log("RELAY forwarded " + std::to_string(inner_len) + " bytes to " +
            target.short_hex(8));
    }
}

void Node::handle_holepunch(Reader& r) {
    NodeId target;
    uint32_t ip = 0;
    uint16_t port = 0;
    if (!r.id(target) || !(ip = r.u32()) || !(port = r.u16())) return;
    // `target` is the peer the introducer wants us to dial (not us).  The old
    // check `target != identity_.id` dropped every genuine intro, so members
    // of a group never punched a direct path to each other.
    if (target == identity_.id) return;
    if (relay_only_) return;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(ip);
    addr.sin_port = htons(port);
    std::string host = endpoint_to_string(addr);
    size_t colon = host.rfind(':');
    if (colon == std::string::npos) return;
    connect_expected(host.substr(0, colon), port, target);
}

bool Node::probe_nat(const NodeId& peer) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = peers_.find(peer);
    if (it == peers_.end() || !it->second.handshake_done) return false;
    uint32_t nonce = next_nonce_++;
    last_stun_[peer.hex()] = {nonce, now_ms()};
    Writer w;
    w.u32(nonce);
    return send_to(it->second.addr, PacketType::StunProbe, w.data);
}

NatProfile Node::nat_profile() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return nat_;
}

void Node::handle_stun_probe(Reader& r, const sockaddr_in& from) {
    uint32_t nonce = r.u32();
    if (!r.ok()) return;
    Writer w;
    w.u32(nonce);
    w.u32(ntohl(from.sin_addr.s_addr));
    w.u16(ntohs(from.sin_port));
    send_to(from, PacketType::StunReply, w.data);
}

void Node::handle_stun_reply(Reader& r, const sockaddr_in& from) {
    uint32_t nonce = r.u32();
    uint32_t ip = r.u32();
    uint16_t port = r.u16();
    if (!r.ok()) return;
    (void)from;
    char ipbuf[INET_ADDRSTRLEN] = {0};
    in_addr a{};
    a.s_addr = htonl(ip);
    inet_ntop(AF_INET, &a, ipbuf, sizeof(ipbuf));
    std::string mapped = ipbuf;

    // A probe answered by a peer on the same LAN only reports our private
    // address.  It says nothing about the NAT and must not overwrite the
    // public mapping (that flipped the invite and the DHT announce port back
    // to 192.168.x.x and misclassified the NAT as symmetric).
    {
        uint32_t h = ip;
        bool priv = (h >> 24) == 10 || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8 ||
                    (h >> 24) == 127 || (h >> 16) == 0xA9FE || (h >> 22) == 0x191;
        if (priv) {
            ++stats_.stun_replies;
            return;
        }
    }

    if (!nat_.discovered) {
        nat_.mapped_ip = mapped;
        nat_.mapped_port = port;
        nat_.type = NatType::FullCone;
        nat_.discovered = true;
    } else if (port != nat_.mapped_port) {
        nat_.type = NatType::Symmetric;
        nat_.mapped_ip = mapped;
        nat_.mapped_port = port;
    } else if (mapped != nat_.mapped_ip) {
        nat_.type = NatType::Restricted;
        nat_.mapped_ip = mapped;
    }
    ++stats_.stun_replies;
    (void)nonce;
    log("STUN reply: mapped " + mapped + ":" + std::to_string(port) + ", NAT " +
        nat_name(nat_.type));
    // Publish the NAT-mapped port to the DHT so announced peers reach us on it.
    mainline_dht_set_announce_port(port);
}

void Node::public_stun_tick_locked(uint64_t now) {
    // Ask a public STUN server for our external ip:port every 60 s until a
    // peer-based probe has taken over, then every 5 min to follow re-mapping.
    // Opt out with P2P_NO_PUBLIC_STUN=1.
    if (std::getenv("P2P_NO_PUBLIC_STUN")) return;
    uint64_t interval = nat_.discovered ? 300000 : 60000;
    if (last_public_stun_ms_ && now - last_public_stun_ms_ < interval) return;
    last_public_stun_ms_ = now;
    if (!public_stun_resolved_) {
        // DNS can block for many seconds while the network is down; never do it under mtx_ (the
        // GUI takes the same lock every frame).  Resolve in a helper thread and pick the result up.
        if (stun_ready_.load()) {
            public_stun_addr_ = stun_tmp_;
            public_stun_resolved_ = true;
            if (stun_resolver_.joinable()) stun_resolver_.join();
        } else {
            if (!stun_resolving_.exchange(true)) {
                if (stun_resolver_.joinable()) stun_resolver_.join();
                const char* host = std::getenv("P2P_STUN_SERVER");
                std::string h = host ? host : "stun.l.google.com:19302";
                std::string port = "3478";
                size_t c = h.rfind(':');
                if (c != std::string::npos) { port = h.substr(c + 1); h = h.substr(0, c); }
                stun_resolver_ = std::thread([this, h, port]() {
                    addrinfo hints{};
                    hints.ai_family = AF_INET;
                    hints.ai_socktype = SOCK_DGRAM;
                    addrinfo* res = nullptr;
                    if (getaddrinfo(h.c_str(), port.c_str(), &hints, &res) == 0 && res) {
                        std::memcpy(&stun_tmp_, res->ai_addr, sizeof(stun_tmp_));
                        freeaddrinfo(res);
                        stun_ready_.store(true);
                    }
                    stun_resolving_.store(false);
                });
            }
            return;
        }
    }
    uint8_t req[20] = {0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
    randombytes_buf(public_stun_txid_.data(), public_stun_txid_.size());
    std::memcpy(req + 8, public_stun_txid_.data(), 12);
    ::sendto(sock_, reinterpret_cast<const char*>(req), sizeof(req), 0,
             reinterpret_cast<const sockaddr*>(&public_stun_addr_), sizeof(public_stun_addr_));
}

bool Node::handle_public_stun(const uint8_t* d, size_t len, const sockaddr_in& from) {
    if (!public_stun_resolved_ || from.sin_addr.s_addr != public_stun_addr_.sin_addr.s_addr)
        return false;
    if (std::memcmp(d + 8, public_stun_txid_.data(), 12) != 0) return false;
    size_t mlen = (static_cast<size_t>(d[2]) << 8) | d[3];
    size_t end = std::min(len, 20 + mlen);
    for (size_t off = 20; off + 4 <= end;) {
        uint16_t type = static_cast<uint16_t>((d[off] << 8) | d[off + 1]);
        uint16_t alen = static_cast<uint16_t>((d[off + 2] << 8) | d[off + 3]);
        const uint8_t* v = d + off + 4;
        if (off + 4 + alen > end) break;
        bool xored = type == 0x0020;
        if ((xored || type == 0x0001) && alen >= 8 && v[1] == 0x01) {
            uint16_t port = static_cast<uint16_t>((v[2] << 8) | v[3]);
            uint32_t ip = (uint32_t(v[4]) << 24) | (uint32_t(v[5]) << 16) |
                          (uint32_t(v[6]) << 8) | v[7];
            if (xored) {
                port ^= 0x2112;
                ip ^= 0x2112A442u;
            }
            char ipbuf[INET_ADDRSTRLEN] = {0};
            in_addr a{};
            a.s_addr = htonl(ip);
            inet_ntop(AF_INET, &a, ipbuf, sizeof(ipbuf));
            bool changed = !nat_.discovered || nat_.mapped_ip != ipbuf || nat_.mapped_port != port;
            const bool was_discovered = nat_.discovered;
            nat_.mapped_ip = ipbuf;
            nat_.mapped_port = port;
            if (!nat_.discovered) nat_.type = NatType::FullCone;
            nat_.discovered = true;
            ++stats_.stun_replies;
            if (changed) {
                log("public STUN: external address " + nat_.mapped_ip + ":" +
                    std::to_string(port));
                mainline_dht_set_announce_port(port);
                mainline_dht_reannounce_now();  // public IP may have changed too
                if (was_discovered) network_changed_locked("public address -> " + nat_.mapped_ip + ":" + std::to_string(port));
            }
            return true;
        }
        off += 4 + ((alen + 3u) & ~3u);
    }
    return false;
}

bool Node::send_group_voice(const NodeId& group_id, uint32_t seq,
                            const std::vector<uint8_t>& opus) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return false;
    GroupRoom& g = it->second;
    Writer pw;
    pw.id(identity_.id);
    pw.u32(seq);
    pw.bytes(opus);
    std::vector<uint8_t> payload;
    group_seal(g, PacketType::GroupVoice, pw.data, payload);

    NodeId hub;
    bool use_hub = g.members.size() > 4;
    if (!use_hub) {
        uint64_t now = now_ms();
        for (const auto& member : g.members) {
            if (member == identity_.id) continue;
            auto p = peers_.find(member);
            if (p == peers_.end() || !p->second.handshake_done) continue;
            if (!p->second.online(now, 20000)) continue;
            if (!p->second.direct) {
                use_hub = true;
                break;
            }
        }
    }
    if (use_hub) choose_group_hub_locked(g, hub);
    bool self_hub = use_hub && hub == identity_.id;
    uint64_t now = now_ms();
    int sent = 0;
    uint8_t flags = (use_hub && !self_hub) ? kGroupForwardFlag : 0;
    for (const auto& member : g.members) {
        if (member == identity_.id) continue;
        if (use_hub && !self_hub && member != hub) continue;
        auto p = peers_.find(member);
        if (p == peers_.end() || !p->second.handshake_done) continue;
        if (!p->second.online(now, 30000)) continue;
        if (send_peer_packet(p->second, PacketType::GroupVoice, payload, flags)) ++sent;
    }
    ++stats_.group_voice;
    return sent > 0 || g.members.size() <= 1;
}

void Node::handle_group_voice(const PacketHeader& h, Reader& r) {
    NodeId gid;
    if (!r.id(gid)) return;
    auto it = groups_.find(gid);
    if (it == groups_.end()) return;
    const uint8_t* sealed_ptr = r.cur();
    size_t sealed_len = r.remaining();
    std::vector<uint8_t> plain;
    if (!open_record(it->second.group_key, sealed_ptr, sealed_len,
                     static_cast<uint8_t>(PacketType::GroupVoice), plain)) {
        ++stats_.dropped;
        return;
    }
    Reader pr(plain);
    NodeId sender;
    if (!pr.id(sender)) return;
    uint32_t seq = pr.u32();
    if (!pr.ok()) return;
    if (std::find(it->second.members.begin(), it->second.members.end(), sender) ==
        it->second.members.end())
        return;
    std::vector<uint8_t> opus;
    if (pr.remaining()) opus.assign(pr.cur(), pr.cur() + pr.remaining());
    ++stats_.group_voice;
    const bool in_group_call = call_.state == CallState::ActiveCall && call_.is_group &&
                               call_.group == gid;
    if (!voice_requires_call_ || in_group_call) {
        ++stats_.voice_recv;
        if (group_voice_handler_) group_voice_handler_(gid, sender, seq, opus);
    }

    if (!(h.flags & kGroupForwardFlag)) return;
    NodeId hub;
    if (!choose_group_hub_locked(it->second, hub) || hub != identity_.id) return;
    Writer fw;
    fw.id(gid);
    fw.raw(sealed_ptr, sealed_len);
    uint64_t now = now_ms();
    for (const auto& member : it->second.members) {
        if (member == identity_.id || member == sender) continue;
        auto p = peers_.find(member);
        if (p == peers_.end() || !p->second.online(now, 30000)) continue;
        send_peer_packet(p->second, PacketType::GroupVoice, fw.data, kGroupForwardFlag);
    }
}

bool Node::choose_group_hub_locked(const GroupRoom& group, NodeId& hub) const {
    if (group.members.empty()) return false;
    uint64_t now = now_ms();
    double best = 1e9;
    NodeId best_id = identity_.id;
    NatType self_nat = nat_.type;
    for (const auto& candidate : group.members) {
        double max_rtt = 0.0;
        double penalty = 0.0;
        if (candidate == identity_.id) {
            penalty = nat_penalty(self_nat);
            for (const auto& other : group.members) {
                if (other == identity_.id) continue;
                auto p = peers_.find(other);
                if (p == peers_.end()) {
                    max_rtt = std::max(max_rtt, 400.0);
                    continue;
                }
                max_rtt = std::max(max_rtt, static_cast<double>(std::max<uint32_t>(20, p->second.rtt_ms)));
                penalty = std::max(penalty, nat_penalty(p->second.nat));
                if (!p->second.direct) max_rtt = std::max(max_rtt, 300.0);
            }
        } else {
            auto cp = peers_.find(candidate);
            if (cp == peers_.end() || !cp->second.online(now, 30000)) continue;
            penalty = nat_penalty(cp->second.nat);
            max_rtt = static_cast<double>(std::max<uint32_t>(20, cp->second.rtt_ms));
            for (const auto& other : group.members) {
                if (other == candidate || other == identity_.id) continue;
                auto p = peers_.find(other);
                if (p == peers_.end()) {
                    max_rtt = std::max(max_rtt, 400.0);
                    continue;
                }
                max_rtt = std::max(max_rtt, static_cast<double>(std::max<uint32_t>(20, p->second.rtt_ms)));
                penalty = std::max(penalty, nat_penalty(p->second.nat));
            }
        }
        double score = 0.40 * std::min(1.0, max_rtt / 300.0) + 0.60 * penalty;
        if (score < best || (score == best && candidate < best_id)) {
            best = score;
            best_id = candidate;
        }
    }
    hub = best_id;
    return true;
}

NodeId Node::group_hub(const NodeId& group_id) const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) return NodeId{};
    NodeId hub;
    choose_group_hub_locked(it->second, hub);
    return hub;
}

bool Node::start_call(const NodeId& peer, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = peers_.find(peer);
    if (it == peers_.end() || !it->second.handshake_done) {
        if (err) *err = "peer handshake is not ready";
        return false;
    }
    if (call_.state != CallState::Idle) {
        if (err) *err = "another call is already in progress";
        return false;
    }
    call_ = CallInfo{};
    call_.state = CallState::OutgoingRinging;
    call_.peer = peer;
    call_.call_id = next_call_id_++;
    call_.started_ms = now_ms();
    call_.participants = {identity_.id, peer};
    Writer pw;
    pw.u32(call_.call_id);
    pw.u8(0);
    pw.id(NodeId{});
    std::vector<uint8_t> payload;
    if (!seal_with_peer(it->second, PacketType::CallOffer, pw.data, payload)) {
        if (err) *err = "cannot encrypt call offer";
        call_.state = CallState::Idle;
        return false;
    }
    send_peer_packet(it->second, PacketType::CallOffer, payload);
    log("CALL_OFFER sent to " + peer.short_hex(8));
    return true;
}

bool Node::start_group_call(const NodeId& group_id, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = groups_.find(group_id);
    if (it == groups_.end()) {
        if (err) *err = "group not found";
        return false;
    }
    if (call_.state != CallState::Idle) {
        if (err) *err = "another call is already in progress";
        return false;
    }
    call_ = CallInfo{};
    call_.state = CallState::OutgoingRinging;
    call_.is_group = true;
    call_.group = group_id;
    call_.call_id = next_call_id_++;
    call_.started_ms = now_ms();
    call_.participants = it->second.members;
    int sent = 0;
    for (const auto& member : it->second.members) {
        if (member == identity_.id) continue;
        auto p = peers_.find(member);
        if (p == peers_.end() || !p->second.handshake_done) continue;
        Writer pw;
        pw.u32(call_.call_id);
        pw.u8(1);
        pw.id(group_id);
        std::vector<uint8_t> payload;
        if (seal_with_peer(p->second, PacketType::CallOffer, pw.data, payload) &&
            send_peer_packet(p->second, PacketType::CallOffer, payload))
            ++sent;
    }
    log("GROUP CALL_OFFER sent to " + std::to_string(sent) + " member(s)");
    (void)err;
    return sent > 0;
}

bool Node::answer_call(bool accept, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (call_.state != CallState::IncomingRinging) {
        if (err) *err = "no incoming call";
        return false;
    }
    auto p = peers_.find(call_.peer);
    if (p == peers_.end()) {
        call_ = CallInfo{};
        cv_.notify_all();
        if (err) *err = "caller disappeared";
        return false;
    }
    Writer pw;
    pw.u32(call_.call_id);
    pw.u8(accept ? 1 : 0);
    std::vector<uint8_t> payload;
    if (!seal_with_peer(p->second, PacketType::CallAnswer, pw.data, payload)) {
        if (err) *err = "cannot encrypt call answer";
        return false;
    }
    send_peer_packet(p->second, PacketType::CallAnswer, payload);
    call_.state = accept ? CallState::ActiveCall : CallState::Idle;
    if (accept) call_.started_ms = now_ms();
    log(accept ? "CALL answered" : "CALL declined");
    return true;
}

void Node::end_call() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (call_.state == CallState::Idle) return;
    uint32_t id = call_.call_id;
    auto send_bye = [&](const NodeId& member) {
        auto p = peers_.find(member);
        if (p == peers_.end() || !p->second.handshake_done) return;
        Writer pw;
        pw.u32(id);
        std::vector<uint8_t> payload;
        if (seal_with_peer(p->second, PacketType::CallBye, pw.data, payload))
            send_peer_packet(p->second, PacketType::CallBye, payload);
    };
    if (call_.is_group) {
        for (const auto& m : call_.participants)
            if (m != identity_.id) send_bye(m);
    } else {
        send_bye(call_.peer);
    }
    call_ = CallInfo{};
    log("CALL ended");
}

CallInfo Node::call_info() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return call_;
}

void Node::handle_call_offer(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto p = peers_.find(sender);
    if (p == peers_.end() || !p->second.handshake_done) return;
    std::vector<uint8_t> plain;
    if (!open_with_peer(p->second, PacketType::CallOffer, r, plain)) return;
    Reader pr(plain);
    uint32_t id = pr.u32();
    uint8_t is_group = pr.u8();
    NodeId gid;
    if (!pr.id(gid) || !pr.ok()) return;
    if (call_.state != CallState::Idle) {
        Writer pw;
        pw.u32(id);
        pw.u8(0);
        std::vector<uint8_t> payload;
        if (seal_with_peer(p->second, PacketType::CallAnswer, pw.data, payload))
            send_peer_packet(p->second, PacketType::CallAnswer, payload);
        return;
    }
    call_ = CallInfo{};
    call_.state = CallState::IncomingRinging;
    call_.is_group = is_group != 0;
    call_.group = call_.is_group ? gid : NodeId{};
    // Keep the offer sender for both direct and group calls: answer_call() must
    // encrypt the CallAnswer to the peer that actually sent the offer.
    call_.peer = sender;
    call_.call_id = id;
    call_.participants = {sender};
    if (call_.is_group) {
        auto g = groups_.find(gid);
        if (g != groups_.end()) call_.participants = g->second.members;
    }
    log("incoming " + std::string(call_.is_group ? "group " : "") + "call from " +
        sender.short_hex(8));
    cv_.notify_all();
}

void Node::handle_call_answer(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto p = peers_.find(sender);
    if (p == peers_.end()) return;
    std::vector<uint8_t> plain;
    if (!open_with_peer(p->second, PacketType::CallAnswer, r, plain)) return;
    Reader pr(plain);
    uint32_t id = pr.u32();
    uint8_t accept = pr.u8();
    if (!pr.ok() || id != call_.call_id) return;
    call_.state = accept ? CallState::ActiveCall : CallState::Idle;
    if (accept) call_.started_ms = now_ms();
    log(accept ? "CALL established" : "CALL declined by peer");
    cv_.notify_all();
}

void Node::handle_call_bye(const PacketHeader& h, Reader& r) {
    NodeId sender;
    std::memcpy(sender.b.data(), h.sender, kIdBytes);
    auto p = peers_.find(sender);
    if (p == peers_.end()) return;
    std::vector<uint8_t> plain;
    if (!open_with_peer(p->second, PacketType::CallBye, r, plain)) return;
    Reader pr(plain);
    uint32_t id = pr.u32();
    if (!pr.ok() || id != call_.call_id) return;
    call_ = CallInfo{};
    log("CALL ended by peer");
    cv_.notify_all();
}

ChatMsg& Node::add_message_locked(const ChatMsg& msg) {
    chat_.push_back(msg);
    if (chat_.size() > 2000) chat_.erase(chat_.begin(), chat_.begin() + 500);
    return chat_.back();
}

void Node::queue_offline_locked(const NodeId& peer, uint32_t id, const std::string& text,
                                uint64_t ts) {
    for (auto& item : offline_queue_) {
        if (item.peer == peer && item.id == id) return;
    }
    OfflineItem item{id, peer, text, ts};
    offline_queue_.push_back(item);
    ++stats_.queued;
    publish_envelope_locked(peer, item);
}

NodeId Node::inbox_key(const NodeId& id) const { return inbox_key_of(id); }

bool Node::publish_envelope_locked(const NodeId& peer, const OfflineItem& item) {
    std::array<uint8_t, 32> x_pk{};
    bool have = false;
    auto c = contacts_.find(peer);
    if (c != contacts_.end() && c->second.x_pk[0]) {
        x_pk = c->second.x_pk;
        have = true;
    }
    auto p = peers_.find(peer);
    if (p != peers_.end() && p->second.x_pk[0]) {
        x_pk = p->second.x_pk;
        have = true;
    }
    if (!have) return false;
    std::array<uint8_t, 32> key{};
    if (!derive_shared_key(identity_.x_sk, x_pk, key)) return false;
    Writer pw;
    pw.u32(item.id);
    pw.u64(item.ts_ms ? item.ts_ms : unix_ms());
    pw.str(item.text);
    auto sealed = seal_record(key, pw.data.data(), pw.data.size(), 0xF1);
    Writer env;
    env.u8(1);
    env.id(identity_.id);
    env.bytes(sealed);
    std::string hex = hex_encode(env.data.data(), env.data.size());
    if (hex.size() > 2000) return false;
    store_value(inbox_key(peer), hex);
    ++stats_.envelopes_stored;
    log("offline envelope for " + peer.short_hex(8) + " stored in DHT (" +
        std::to_string(hex.size()) + " hex chars)");
    return true;
}

void Node::drain_offline_locked(Peer& peer) {
    if (!peer.handshake_done) return;
    for (auto it = offline_queue_.begin(); it != offline_queue_.end();) {
        if (it->peer != peer.id) {
            ++it;
            continue;
        }
        if (!send_encrypted_chat(peer, peer.id, it->id, it->text, false)) {
            ++it;
            continue;
        }
        for (auto& m : chat_) {
            if (m.outgoing && !m.is_group && m.id == it->id && m.peer == peer.id) {
                m.queued = false;
                break;
            }
        }
        log("offline message " + std::to_string(it->id) + " flushed to " +
            peer.id.short_hex(8));
        it = offline_queue_.erase(it);
    }
}

bool Node::handle_envelope_locked(const NodeId& sender, const std::vector<uint8_t>& plain) {
    Reader pr(plain);
    uint32_t id = pr.u32();
    uint64_t ts = pr.u64();
    std::string text = pr.str(2048);
    if (!pr.ok()) return false;
    for (const auto& m : chat_) {
        if (!m.outgoing && !m.is_group && m.sender == sender && m.id == id) return true;
    }
    ChatMsg msg;
    msg.id = id;
    msg.peer = sender;
    msg.sender = sender;
    msg.text = text;
    msg.outgoing = false;
    msg.delivered = true;
    msg.ts_ms = ts ? ts : unix_ms();
    add_message_locked(msg);
    ++stats_.chat_recv;
    ++stats_.envelopes_fetched;
    auto p = peers_.find(sender);
    if (p != peers_.end() && p->second.handshake_done) send_ack_locked(p->second, id);
    log("offline envelope from " + sender.short_hex(8) + " delivered: " +
        text.substr(0, 50));
    cv_.notify_all();
    return true;
}

void Node::poll_inbox_locked() {
    const NodeId key = inbox_key(identity_.id);
    auto vit = values_.find(key);
    if (vit != values_.end() && inbox_seen_.find(vit->second) == inbox_seen_.end()) {
        std::vector<uint8_t> raw;
        if (hex_decode(vit->second, raw) && raw.size() > 1 + kIdBytes) {
            Reader er(raw);
            uint8_t version = er.u8();
            NodeId sender;
            if (version == 1 && er.id(sender) && er.ok()) {
                std::vector<uint8_t> sealed;
                if (er.bytes(sealed, er.remaining())) {
                    std::array<uint8_t, 32> x_pk{};
                    bool have = false;
                    auto c = contacts_.find(sender);
                    if (c != contacts_.end() && c->second.x_pk[0]) {
                        x_pk = c->second.x_pk;
                        have = true;
                    }
                    auto p = peers_.find(sender);
                    if (p != peers_.end() && p->second.x_pk[0]) {
                        x_pk = p->second.x_pk;
                        have = true;
                    }
                    if (have) {
                        std::array<uint8_t, 32> k{};
                        std::vector<uint8_t> plain;
                        if (derive_shared_key(identity_.x_sk, x_pk, k) &&
                            open_record(k, sealed.data(), sealed.size(), 0xF1, plain)) {
                            handle_envelope_locked(sender, plain);
                        }
                    }
                }
            }
        }
        inbox_seen_.insert(vit->second);
    }

    uint64_t now = now_ms();
    if (now - last_inbox_poll_ms_ < 2500) return;
    last_inbox_poll_ms_ = now;
    int queried = 0;
    for (auto& kv : peers_) {
        if (queried >= 3) break;
        if (!kv.second.handshake_done || !kv.second.online(now, 30000)) continue;
        send_find(kv.second, key, true);
        ++queried;
    }
}

bool Node::send_chat(const NodeId& peer, const std::string& text, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (text.empty() || text.size() > 900) {
        if (err) *err = "invalid message length";
        return false;
    }
    uint32_t id = next_msg_id_++;
    ChatMsg msg;
    msg.id = id;
    msg.peer = peer;
    msg.sender = identity_.id;
    msg.text = text;
    msg.outgoing = true;
    msg.ts_ms = unix_ms();

    auto it = peers_.find(peer);
    bool ready = it != peers_.end() && it->second.handshake_done;
    if (ready && it->second.online(now_ms(), 20000)) {
        if (send_encrypted_chat(it->second, peer, id, text, false)) {
            add_message_locked(msg);
            return true;
        }
    }
    msg.queued = true;
    add_message_locked(msg);
    queue_offline_locked(peer, id, text, msg.ts_ms);
    if (err) *err = "queued (offline)";
    log("CHAT queued for offline peer " + peer.short_hex(8));
    return true;
}

bool Node::send_chat_offline(const NodeId& peer, const std::string& text, std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (text.empty() || text.size() > 900) {
        if (err) *err = "invalid message length";
        return false;
    }
    uint32_t id = next_msg_id_++;
    ChatMsg msg;
    msg.id = id;
    msg.peer = peer;
    msg.sender = identity_.id;
    msg.text = text;
    msg.outgoing = true;
    msg.queued = true;
    msg.ts_ms = unix_ms();
    add_message_locked(msg);
    queue_offline_locked(peer, id, text, msg.ts_ms);
    if (err) *err = "queued";
    return true;
}

bool Node::send_group_relay(const NodeId& target, const std::vector<uint8_t>& payload) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto r = peers_.find(relay_id_);
    if (r == peers_.end()) return false;
    std::vector<uint8_t> inner = make_packet(identity_, PacketType::GroupVoice, payload);
    return send_inner_packet(r->second.addr, target, static_cast<uint8_t>(PacketType::GroupVoice),
                             inner);
}

bool Node::store_value(const NodeId& key, const std::string& value) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    values_[key] = value;
    uint64_t now = now_ms();
    int sent = 0;
    auto ids = routing_ ? routing_->closest(key, 3) : std::vector<NodeId>{};
    for (const auto& id : ids) {
        auto it = peers_.find(id);
        if (it == peers_.end() || !it->second.online(now)) continue;
        Writer w;
        w.id(key);
        w.str(value);
        if (send_to(it->second.addr, PacketType::Store, w.data)) ++sent;
    }
    log("STORE " + key.short_hex(10) + " sent to " + std::to_string(sent) + " nodes");
    return sent > 0 || !ids.empty();
}

bool Node::local_value(const NodeId& key, std::string& value) const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = values_.find(key);
    if (it == values_.end()) return false;
    value = it->second;
    return true;
}

LookupResult Node::lookup(const NodeId& target, bool want_value) {
    LookupResult res;
    res.target = target;
    uint64_t t0 = now_ms();

    std::unique_lock<std::recursive_mutex> lk(mtx_);
    std::vector<NodeId> queried;

    for (int round = 0; round < 6; ++round) {
        uint64_t now = now_ms();
        std::vector<Peer> candidates;
        auto ids = routing_ ? routing_->closest(target, 16) : std::vector<NodeId>{};
        for (const auto& id : ids) {
            auto it = peers_.find(id);
            if (it == peers_.end()) continue;
            if (!it->second.online(now, 30000)) continue;
            if (std::find(queried.begin(), queried.end(), id) != queried.end()) continue;
            candidates.push_back(it->second);
        }
        if (candidates.empty()) break;

        size_t n = std::min<size_t>(3, candidates.size());
        for (size_t i = 0; i < n; ++i) {
            send_find(candidates[i], target, want_value);
            queried.push_back(candidates[i].id);
            ++res.queries;
        }
        ++res.rounds;

        if (want_value) {
            cv_.wait_for(lk, std::chrono::milliseconds(140),
                         [&] { return values_.find(target) != values_.end(); });
            if (values_.find(target) != values_.end()) break;
        } else {
            cv_.wait_for(lk, std::chrono::milliseconds(100));
        }
    }

    auto vit = values_.find(target);
    if (want_value && vit != values_.end()) {
        res.found = true;
        res.value = true;
        res.text = vit->second;
    }

    uint64_t now = now_ms();
    auto ids = routing_ ? routing_->closest(target, 8) : std::vector<NodeId>{};
    for (const auto& id : ids) {
        auto it = peers_.find(id);
        if (it == peers_.end()) continue;
        if (!it->second.online(now, 60000)) continue;
        res.closest.push_back(it->second);
    }
    if (!want_value) res.found = !res.closest.empty();
    res.time_ms = static_cast<uint32_t>(now_ms() - t0);
    return res;
}

std::vector<Peer> Node::peers() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<Peer> out;
    out.reserve(peers_.size());
    uint64_t now = now_ms();
    for (const auto& kv : peers_) {
        Peer p = kv.second;
        if (!p.online(now, 60000)) p.direct = false;
        out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [&](const Peer& a, const Peer& b) {
        bool ao = a.online(now, 20000);
        bool bo = b.online(now, 20000);
        if (ao != bo) return ao;
        if (a.alias != b.alias) return a.alias < b.alias;
        return a.id < b.id;
    });
    return out;
}

bool Node::get_peer(const NodeId& id, Peer& out) const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    auto it = peers_.find(id);
    if (it == peers_.end()) return false;
    out = it->second;
    return true;
}

std::vector<ChatMsg> Node::chat() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    return chat_;
}

std::vector<ChatMsg> Node::messages_for(const NodeId& conversation) const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<ChatMsg> out;
    for (const auto& m : chat_)
        if (m.peer == conversation) out.push_back(m);
    return out;
}

NodeStats Node::stats() const {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    NodeStats s = stats_;
    uint64_t steps = 0;
    uint64_t replay = 0;
    for (const auto& kv : peers_) {
        steps += kv.second.text_ratchet.steps + kv.second.voice_ratchet.steps;
        replay += kv.second.text_replay.hits + kv.second.voice_replay.hits;
    }
    s.ratchet_steps += steps;
    s.replay_hits += replay;
    return s;
}

std::vector<std::string> Node::take_logs() {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<std::string> out(logs_.begin(), logs_.end());
    logs_.clear();
    return out;
}

void Node::log(const std::string& line) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    logs_.push_back("[" + wall_clock_hms() + "] " + line);
    if (logs_.size() > 400) logs_.pop_front();
    // P2P_LOG_FILE=<path>: mirror every log line to a file immediately
    // (live inspection during multi-machine tests, also for the GUI build).
    static FILE* lf = nullptr;
    static bool lf_tried = false;
    if (!lf_tried) {
        lf_tried = true;
        if (const char* path = std::getenv("P2P_LOG_FILE")) lf = std::fopen(path, "a");
    }
    if (lf) {
        std::fprintf(lf, "%s\n", logs_.back().c_str());
        std::fflush(lf);
    }
}

std::vector<uint8_t> Node::serialize_state_locked() const {
    Writer w;
    w.u8(3);
    w.str(identity_.alias);
    // Every section writes exactly the number of records it declares (the old code declared
    // min(size, cap) but looped over everything, which shifted all following sections on reload).
    w.u16(static_cast<uint16_t>(std::min<size_t>(contacts_.size(), 1000)));
    size_t written_contacts = 0;
    for (const auto& kv : contacts_) {
        if (written_contacts++ >= 1000) break;
        const Contact& c = kv.second;
        w.id(c.id);
        w.str(c.alias);
        w.raw(c.ed_pk.data(), c.ed_pk.size());
        w.raw(c.x_pk.data(), c.x_pk.size());
        w.u8(c.verified ? 1 : 0);
        w.u8(c.in_contacts ? 1 : 0);
        w.u32(ntohl(c.addr.sin_addr.s_addr));
        w.u16(ntohs(c.addr.sin_port));
    }
    w.u16(static_cast<uint16_t>(std::min<size_t>(groups_.size(), 1000)));
    size_t written_groups = 0;
    for (const auto& kv : groups_) {
        if (written_groups++ >= 1000) break;
        const GroupRoom& g = kv.second;
        w.id(g.id);
        w.str(g.name);
        w.raw(g.group_key.data(), g.group_key.size());
        w.id(g.last_hash);
        w.u64(g.created_ms);
        w.u16(static_cast<uint16_t>(std::min<size_t>(g.members.size(), 256)));
        size_t written_members = 0;
        for (const auto& m : g.members) {
            if (written_members++ >= 256) break;
            w.id(m);
        }
    }
    // Keep the NEWEST 1000 messages.
    w.u16(static_cast<uint16_t>(std::min<size_t>(chat_.size(), 1000)));
    const size_t chat_skip = chat_.size() > 1000 ? chat_.size() - 1000 : 0;
    for (size_t chat_i = chat_skip; chat_i < chat_.size(); ++chat_i) {
        const ChatMsg& m = chat_[chat_i];
        w.u32(m.id);
        w.id(m.peer);
        w.id(m.sender);
        w.id(m.parent_hash);
        w.str(m.text);
        uint8_t flags = 0;
        if (m.outgoing) flags |= 1;
        if (m.delivered) flags |= 2;
        if (m.queued) flags |= 4;
        if (m.is_group) flags |= 8;
        w.u8(flags);
        w.u64(m.ts_ms);
    }
    w.u16(static_cast<uint16_t>(std::min<size_t>(offline_queue_.size(), 500)));
    size_t written_queue = 0;
    for (const auto& i : offline_queue_) {
        if (written_queue++ >= 500) break;
        w.u32(i.id);
        w.id(i.peer);
        w.str(i.text);
        w.u64(i.ts_ms);
    }
    w.id(relay_id_);
    return w.data;
}

bool Node::deserialize_state_locked(const std::vector<uint8_t>& data, std::string& err) {
    Reader r(data);
    uint8_t version = r.u8();
    if (!r.ok() || (version != 2 && version != 3)) {
        err = "unsupported vault version";
        return false;
    }
    if (version >= 3) {
        std::string saved_alias = r.str(64);
        if (!r.ok()) return false;
        if (!alias_from_cli_ && !saved_alias.empty() && saved_alias != "A") identity_.alias = saved_alias;
    }
    contacts_.clear();
    groups_.clear();
    chat_.clear();
    offline_queue_.clear();
    uint16_t n = r.u16();
    if (!r.ok() || n > 1000) return false;
    for (uint16_t i = 0; i < n; ++i) {
        Contact c;
        if (!r.id(c.id)) return false;
        c.alias = r.str(128);
        if (!r.raw(c.ed_pk.data(), c.ed_pk.size()) ||
            !r.raw(c.x_pk.data(), c.x_pk.size()))
            return false;
        c.verified = r.u8() != 0;
        c.in_contacts = r.u8() != 0;
        uint32_t ip = r.u32();
        uint16_t port = r.u16();
        if (!r.ok() || c.id == identity_.id) continue;
        c.addr.sin_family = AF_INET;
        c.addr.sin_addr.s_addr = htonl(ip);
        c.addr.sin_port = htons(port);
        contacts_[c.id] = c;
    }
    n = r.u16();
    if (!r.ok() || n > 1000) return false;
    for (uint16_t i = 0; i < n; ++i) {
        GroupRoom g;
        if (!r.id(g.id)) return false;
        g.name = r.str(128);
        if (!r.raw(g.group_key.data(), g.group_key.size())) return false;
        if (!r.id(g.last_hash)) return false;
        g.created_ms = r.u64();
        uint16_t mc = r.u16();
        if (!r.ok() || mc > 256) return false;
        for (uint16_t j = 0; j < mc; ++j) {
            NodeId m;
            if (!r.id(m)) return false;
            g.members.push_back(m);
        }
        groups_[g.id] = g;
    }
    n = r.u16();
    if (!r.ok() || n > 1000) return false;
    for (uint16_t i = 0; i < n; ++i) {
        ChatMsg m;
        m.id = r.u32();
        if (!r.id(m.peer) || !r.id(m.sender) || !r.id(m.parent_hash)) return false;
        m.text = r.str(4096);
        uint8_t flags = r.u8();
        m.outgoing = flags & 1;
        m.delivered = flags & 2;
        m.queued = flags & 4;
        m.is_group = flags & 8;
        m.ts_ms = r.u64();
        if (!r.ok()) return false;
        chat_.push_back(m);
        next_msg_id_ = std::max(next_msg_id_, m.id + 1);
    }
    n = r.u16();
    if (!r.ok() || n > 500) return false;
    for (uint16_t i = 0; i < n; ++i) {
        OfflineItem item;
        item.id = r.u32();
        if (!r.id(item.peer)) return false;
        item.text = r.str(4096);
        item.ts_ms = r.u64();
        if (!r.ok()) return false;
        offline_queue_.push_back(item);
    }
    if (r.remaining() >= kIdBytes) r.id(relay_id_);
    return r.ok();
}

bool Node::save_state(std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    if (!persist_) return true;
    if (vault_path_.empty()) {
        if (err) *err = "vault path is empty";
        return false;
    }
    std::vector<uint8_t> data = serialize_state_locked();
    std::string e;
    if (!write_vault(vault_path_, identity_.ed_sk, data, e)) {
        if (err) *err = e;
        return false;
    }
    return true;
}

bool Node::load_state(std::string* err) {
    std::lock_guard<std::recursive_mutex> lk(mtx_);
    std::vector<uint8_t> data;
    std::string e;
    if (!read_vault(vault_path_, identity_.ed_sk, data, e)) {
        if (err) *err = e;
        return false;
    }
    std::string de;
    if (!deserialize_state_locked(data, de)) {
        if (err) *err = de;
        return false;
    }
    return true;
}

}  // namespace p2p
