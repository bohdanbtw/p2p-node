// Mainline (BitTorrent) DHT driver.
//
// The node shares its one UDP socket with the vendored jech/dht library
// (third_party/dht, MIT), which speaks the real Mainline KRPC protocol.  The
// library keeps global state and is not thread-safe, so exactly one thread
// here owns every dht_* call; the node's rx_loop only hands over datagrams
// whose first byte is 'd' (bencoded KRPC), since our own protocol always
// starts with the "P2P1" magic.
//
// Discovery model (rules R1-R7, no server of ours anywhere):
//   * announce  our live ip:port under key = BLAKE2b-160(label || account id)
//   * search    that key for a contact, connect to the returned endpoint
//   * bootstrap from the community well-known routers AND the routing table
//     saved on the previous run (BEP 5: "the routing table should be saved
//     between invocations").
#include "mainline_dht.h"

#include <sodium.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#endif

#include "../third_party/dht/dht.h"

namespace p2p {
namespace {

// Community-run Mainline well-known nodes.  None of them belongs to us; the
// client needs several up and none individually (rules R1/R4).
constexpr struct {
    const char* host;
    uint16_t port;
} kRouters[] = {
    {"router.bittorrent.com", 6881},
    {"dht.transmissionbt.com", 6881},
    {"router.utorrent.com", 6881},
};

constexpr const char* kKeyLabel = "P2P-MLDHT-V1";
constexpr size_t kMaxSavedNodes = 100;
constexpr uint64_t kReannounceMs = 15 * 60 * 1000;    // values expire after 30 min
constexpr uint64_t kLookupMinIntervalMs = 20 * 1000;  // per-contact lookup throttle
constexpr uint64_t kSaveNodesMs = 5 * 60 * 1000;
constexpr int kBootstrapRefreshNodes = 8;  // re-seed from routers below this

struct DhtState {
    socket_t sock = kInvalidSocket;
    uint16_t messaging_port = 0;
    NodeId account{};
    std::string state_path;
    MainlineDhtHost host;

    std::atomic<bool> running{false};
    std::thread thread;

    std::mutex rx_mtx;
    std::condition_variable rx_cv;
    struct RxItem {
        std::vector<uint8_t> data;
        sockaddr_in from{};
    };
    std::deque<RxItem> rx;

    std::mutex lookups_mtx;
    std::vector<std::pair<NodeId, uint64_t>> lookups;  // contact id -> last lookup ms

    std::atomic<uint16_t> announce_port{0};
    std::atomic<uint64_t> last_announce_ms{0};
    std::atomic<int> good_nodes{0};
    // Announce bookkeeping.  Starting a search is NOT a stored value: on the
    // public DHT the search often never reaches announce_peer acks, so the
    // announce is retried every 45 s until SEARCH_DONE confirms it.
    std::atomic<uint64_t> announce_next_ms{0};
    std::atomic<uint64_t> announce_confirmed_ms{0};
    std::atomic<int> announce_attempts{0};
    std::atomic<uint64_t> values_found{0};
};

DhtState* g_dht = nullptr;

std::vector<uint8_t> blake160(std::initializer_list<std::pair<const void*, size_t>> parts) {
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, kIdBytes);
    for (auto& p : parts)
        crypto_generichash_update(&st, static_cast<const uint8_t*>(p.first), p.second);
    std::vector<uint8_t> out(kIdBytes);
    crypto_generichash_final(&st, out.data(), out.size());
    return out;
}

NodeId nodeid_from_blake(const std::vector<uint8_t>& v) {
    NodeId out{};
    std::memcpy(out.b.data(), v.data(), kIdBytes);
    return out;
}

uint16_t current_announce_port() {
    if (!g_dht) return 0;
    uint16_t p = g_dht->announce_port.load();
    return p ? p : g_dht->messaging_port;
}

void dht_callback(void* /*closure*/, int event, const unsigned char* info_hash,
                  const void* data, size_t data_len) {
    if (!g_dht || !g_dht->running.load()) return;

    bool is_self = false;
    {
        auto self_key = blake160({{kKeyLabel, std::strlen(kKeyLabel)},
                                  {g_dht->account.b.data(), g_dht->account.b.size()}});
        is_self = std::memcmp(self_key.data(), info_hash, kIdBytes) == 0;
    }
    if (event == DHT_EVENT_SEARCH_DONE && is_self && g_dht->host.log) {
        // The library only reaches this after the announce_peer exchanges.
        g_dht->host.log("dht: announce search completed (value stored)");
        uint64_t t = now_ms();
        g_dht->announce_confirmed_ms.store(t);
        g_dht->announce_next_ms.store(t + kReannounceMs);
    }
    if (event == DHT_EVENT_SEARCH_DONE && !is_self && g_dht->host.log) {
        NodeId key_id{};
        std::memcpy(key_id.b.data(), info_hash, kIdBytes);
        g_dht->host.log("dht: lookup completed for key " + key_id.short_hex(10));
    }
    if (event != DHT_EVENT_VALUES || data_len < 6) return;
    // Test-only escape: allow a process to observe its own stored value.  The
    // product never needs this (we must not dial ourselves), but it is the
    // cleanest way to prove announce+retrieve in one process.
    if (is_self && std::getenv("P2P_DHT_ALLOW_SELF") == nullptr) return;

    // Recover which contact this key belongs to (we only ever search for
    // contacts whose account id we know).
    NodeId contact{};
    {
        std::lock_guard<std::mutex> lk(g_dht->lookups_mtx);
        for (auto& kv : g_dht->lookups) {
            auto key = blake160({{kKeyLabel, std::strlen(kKeyLabel)},
                                 {kv.first.b.data(), kv.first.b.size()}});
            if (std::memcmp(key.data(), info_hash, kIdBytes) == 0) {
                contact = kv.first;
                break;
            }
        }
    }
    if (contact.empty()) return;

    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t off = 0; off + 6 <= data_len; off += 6) {
        sockaddr_in ep{};
        ep.sin_family = AF_INET;
        std::memcpy(&ep.sin_addr.s_addr, bytes + off, 4);
        std::memcpy(&ep.sin_port, bytes + off + 4, 2);
        if (ep.sin_addr.s_addr == 0 || ep.sin_port == 0) continue;
        ++g_dht->values_found;
        if (g_dht->host.on_found) g_dht->host.on_found(ep, contact);
    }
}

int count_good_nodes() {
    int good = 0, dubious = 0, cached = 0, incoming = 0;
    if (dht_nodes(AF_INET, &good, &dubious, &cached, &incoming) < 0) return 0;
    return good;
}

void save_nodes() {
    if (!g_dht || g_dht->state_path.empty()) return;
    std::vector<sockaddr_in> sin(static_cast<size_t>(kMaxSavedNodes));
    int num = static_cast<int>(sin.size());
    // dht_get_nodes unconditionally writes *num6 (and fills sin6 up to the
    // capacity we hand it), so valid IPv6 out-params are required even though
    // we only persist IPv4 contacts.
    std::vector<sockaddr_in6> sin6(16);
    int num6 = static_cast<int>(sin6.size());
    int n = dht_get_nodes(sin.data(), &num, sin6.data(), &num6);
    if (n <= 0) return;
    FILE* f = std::fopen(g_dht->state_path.c_str(), "wb");
    if (!f) return;
    std::fwrite("MLDHT1", 1, 6, f);
    for (int i = 0; i < num; ++i) {
        if (sin[static_cast<size_t>(i)].sin_family != AF_INET) continue;
        std::fwrite(&sin[static_cast<size_t>(i)].sin_addr.s_addr, 4, 1, f);
        std::fwrite(&sin[static_cast<size_t>(i)].sin_port, 2, 1, f);
    }
    std::fclose(f);
}

void load_and_ping_saved() {
    if (!g_dht || g_dht->state_path.empty()) return;
    FILE* f = std::fopen(g_dht->state_path.c_str(), "rb");
    if (!f) return;
    char magic[6] = {0};
    if (std::fread(magic, 1, 6, f) != 6 || std::memcmp(magic, "MLDHT1", 6) != 0) {
        std::fclose(f);
        return;
    }
    int pinged = 0;
    uint8_t rec[6];
    while (pinged < 50 && std::fread(rec, 1, 6, f) == 6) {
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        std::memcpy(&sin.sin_addr.s_addr, rec, 4);
        std::memcpy(&sin.sin_port, rec + 4, 2);
        if (sin.sin_addr.s_addr == 0 || sin.sin_port == 0) continue;
        if (dht_ping_node(reinterpret_cast<sockaddr*>(&sin), sizeof(sin)) >= 0) ++pinged;
    }
    std::fclose(f);
    if (pinged && g_dht->host.log)
        g_dht->host.log("dht: re-entering network via " + std::to_string(pinged) +
                        " saved nodes");
}

void ping_routers() {
    for (const auto& r : kRouters) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        std::string port = std::to_string(r.port);
        if (getaddrinfo(r.host, port.c_str(), &hints, &res) != 0) continue;
        for (addrinfo* ai = res; ai; ai = ai->ai_next) {
            if (ai->ai_family == AF_INET &&
                dht_ping_node(static_cast<sockaddr*>(ai->ai_addr),
                              static_cast<int>(ai->ai_addrlen)) >= 0)
                break;
        }
        freeaddrinfo(res);
    }
    if (g_dht && g_dht->host.log)
        g_dht->host.log("dht: bootstrapping from public Mainline routers");
}

void dht_loop() {
    time_t tosleep = 0;
    std::vector<uint8_t> buf(2049);

    uint64_t t_start = now_ms();
    uint64_t next_router_ping_ms = t_start + 2000;
    g_dht->announce_next_ms.store(t_start + 3000);
    uint64_t next_save_ms = t_start + kSaveNodesMs;
    load_and_ping_saved();

    while (g_dht && g_dht->running.load()) {
        DhtState::RxItem item;
        bool have_item = false;
        {
            std::unique_lock<std::mutex> lk(g_dht->rx_mtx);
            uint64_t wait_s = static_cast<uint64_t>(tosleep > 0 ? tosleep : 1);
            g_dht->rx_cv.wait_for(lk, std::chrono::seconds(wait_s),
                                  [&] { return !g_dht->rx.empty() || !g_dht->running.load(); });
            if (!g_dht->rx.empty()) {
                item = std::move(g_dht->rx.front());
                g_dht->rx.pop_front();
                have_item = true;
            }
        }
        if (!g_dht->running.load()) break;

        int rc;
        if (have_item) {
            item.data.push_back(0);  // dht_periodic requires a NUL terminator
            rc = dht_periodic(item.data.data(), item.data.size() - 1,
                              reinterpret_cast<sockaddr*>(&item.from),
                              static_cast<int>(sizeof(item.from)), &tosleep, dht_callback,
                              nullptr);
        } else {
            rc = dht_periodic(nullptr, 0, nullptr, 0, &tosleep, dht_callback, nullptr);
        }
        if (rc < 0) {
            tosleep = 1;
            continue;
        }
        if (tosleep < 0) tosleep = 0;
        if (tosleep > 5) tosleep = 5;

        uint64_t now = now_ms();

        // Keep a floor of contacts: re-seed from the community routers if the
        // table is thin and we have not entered the network yet (R1/R4).
        int good = count_good_nodes();
        g_dht->good_nodes.store(good);
        if (good < kBootstrapRefreshNodes && now >= next_router_ping_ms) {
            ping_routers();
            next_router_ping_ms = now + 60 * 1000;
        }

        // Announce (or re-announce) our endpoint under our account key.
        if (good > 0 && now >= g_dht->announce_next_ms.load()) {
            auto key = blake160({{kKeyLabel, std::strlen(kKeyLabel)},
                                 {g_dht->account.b.data(), g_dht->account.b.size()}});
            uint16_t ap = current_announce_port();
            if (ap && dht_search(key.data(), ap, AF_INET, dht_callback, nullptr) >= 0) {
                if (g_dht->last_announce_ms.load() == 0 && g_dht->host.log)
                    g_dht->host.log("dht: announced on port " + std::to_string(ap) +
                                    " under key " + nodeid_from_blake(key).short_hex(10));
                g_dht->last_announce_ms.store(now);
                ++g_dht->announce_attempts;
                // Retry soon; the SEARCH_DONE callback moves this to 15 min.
                g_dht->announce_next_ms.store(now + 45 * 1000);
            } else {
                g_dht->announce_next_ms.store(now + 30 * 1000);
            }
        }

        // Pending contact lookups (search: port 0).
        {
            std::lock_guard<std::mutex> lk(g_dht->lookups_mtx);
            for (auto& kv : g_dht->lookups) {
                if (kv.second != 1 && now - kv.second < kLookupMinIntervalMs) continue;
                auto key = blake160({{kKeyLabel, std::strlen(kKeyLabel)},
                                     {kv.first.b.data(), kv.first.b.size()}});
                if (good > 0 && dht_search(key.data(), 0, AF_INET, dht_callback, nullptr) >= 0)
                    kv.second = now;
            }
        }

        if (now >= next_save_ms) {
            save_nodes();
            next_save_ms = now + kSaveNodesMs;
        }
    }
    save_nodes();
}

// The library only keeps the socket handle as an int and passes it back to
// dht_sendto.  On Windows SOCKET is 64-bit, so we do not round-trip it through
// the library: dht_init gets a dummy non-negative id and we send on our own
// stored handle.
socket_t dht_socket_handle() { return g_dht ? g_dht->sock : kInvalidSocket; }

}  // namespace

NodeId mainline_dht_key(const NodeId& account_id) {
    return nodeid_from_blake(blake160({{kKeyLabel, std::strlen(kKeyLabel)},
                                       {account_id.b.data(), account_id.b.size()}}));
}

bool mainline_dht_start(socket_t sock, uint16_t messaging_port, const NodeId& account_id,
                        const std::string& state_path, const MainlineDhtHost& host,
                        std::string& err) {
    if (g_dht) {
        err = "dht already started";
        return false;
    }
    // Optional protocol debug: P2P_DHT_DEBUG=<path> enables the library's
    // built-in debugf() trace (off by default in the product).
    if (const char* dbg = std::getenv("P2P_DHT_DEBUG")) {
        if (FILE* f = std::fopen(dbg, "w")) dht_debug = f;
    }
    auto* st = new DhtState();
    st->sock = sock;
    st->messaging_port = messaging_port;
    st->account = account_id;
    st->state_path = state_path;
    st->host = host;
    st->announce_port.store(messaging_port);
    g_dht = st;

    unsigned char dht_id[kIdBytes];
    std::memcpy(dht_id, account_id.b.data(), kIdBytes);  // BLAKE2b of the ed25519 pk: uniform
    unsigned char version[4] = {'M', 'K', 'R', '1'};
    // Our own protocol reads this same socket; the library only keeps the id
    // and routes sends through dht_sendto, so a dummy non-negative id is fine.
    if (dht_init(static_cast<int>(sock), -1, dht_id, version) < 0) {
        delete st;
        g_dht = nullptr;
        err = "dht_init failed";
        return false;
    }

    st->running.store(true);
    st->thread = std::thread(dht_loop);
    return true;
}

void mainline_dht_stop() {
    if (!g_dht) return;
    g_dht->running.store(false);
    g_dht->rx_cv.notify_all();
    if (g_dht->thread.joinable()) g_dht->thread.join();
    save_nodes();
    dht_uninit();
    if (dht_debug) {
        std::fclose(dht_debug);
        dht_debug = nullptr;
    }
    delete g_dht;
    g_dht = nullptr;
}

bool mainline_dht_running() { return g_dht && g_dht->running.load(); }

void mainline_dht_input(const uint8_t* data, size_t len, const sockaddr_in& from) {
    if (!g_dht || !g_dht->running.load() || len == 0 || len > 2048) return;
    DhtState::RxItem item;
    item.data.assign(data, data + len);
    item.from = from;
    {
        std::lock_guard<std::mutex> lk(g_dht->rx_mtx);
        if (g_dht->rx.size() > 1024) g_dht->rx.pop_front();  // bursts of get_peers replies
        g_dht->rx.push_back(std::move(item));
    }
    g_dht->rx_cv.notify_one();
}

void mainline_dht_lookup(const NodeId& contact_account_id) {
    if (!g_dht || contact_account_id.empty()) return;
    std::lock_guard<std::mutex> lk(g_dht->lookups_mtx);
    for (auto& kv : g_dht->lookups) {
        if (kv.first == contact_account_id) return;  // already tracked, loop re-searches
    }
    g_dht->lookups.push_back({contact_account_id, 1});  // 1 = "immediately"
}

void mainline_dht_set_announce_port(uint16_t port) {
    if (g_dht && port) {
        // A new mapped port must reach the DHT now, not after the 15 min cycle.
        if (g_dht->announce_port.exchange(port) != port) g_dht->announce_next_ms.store(0);
    }
}

void mainline_dht_reannounce_now() {
    if (g_dht) g_dht->announce_next_ms.store(0);
}

int mainline_dht_node_count() { return g_dht ? g_dht->good_nodes.load() : 0; }

MainlineDhtStats mainline_dht_stats() {
    MainlineDhtStats st;
    if (!g_dht) return st;
    st.good_nodes = g_dht->good_nodes.load();
    st.announce_attempts = g_dht->announce_attempts.load();
    st.announce_confirmed_ms = g_dht->announce_confirmed_ms.load();
    st.values_found = g_dht->values_found.load();
    return st;
}

}  // namespace p2p

// ---------------------------------------------------------------------------
// Callbacks the jech/dht library expects the application to provide.
// ---------------------------------------------------------------------------
extern "C" {

int dht_sendto(int sockfd, const void* buf, int len, int flags, const struct sockaddr* to,
               int tolen) {
    (void)sockfd;
    socket_t s = p2p::dht_socket_handle();
    if (s == kInvalidSocket || !p2p::mainline_dht_running()) return -1;
#ifdef _WIN32
    return ::sendto(s, static_cast<const char*>(buf), len, flags, to, tolen);
#else
    return static_cast<int>(::sendto(s, buf, static_cast<size_t>(len), flags, to,
                                     static_cast<socklen_t>(tolen)));
#endif
}

int dht_blacklisted(const struct sockaddr* sa, int salen) {
    (void)sa;
    (void)salen;
    return 0;
}

void dht_hash(void* hash_return, int hash_size, const void* v1, int len1, const void* v2,
              int len2, const void* v3, int len3) {
    // BLAKE2b-256 over the three parts, truncated to the requested size
    // (libsodium's generic hash refuses outputs < 16 bytes, so hash 32).
    unsigned char full[32];
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, sizeof(full));
    if (len1 > 0) crypto_generichash_update(&st, static_cast<const unsigned char*>(v1), len1);
    if (len2 > 0) crypto_generichash_update(&st, static_cast<const unsigned char*>(v2), len2);
    if (len3 > 0) crypto_generichash_update(&st, static_cast<const unsigned char*>(v3), len3);
    crypto_generichash_final(&st, full, sizeof(full));
    int n = hash_size > static_cast<int>(sizeof(full)) ? static_cast<int>(sizeof(full))
                                                       : hash_size;
    std::memcpy(hash_return, full, n);
    if (hash_size > n) std::memset(static_cast<char*>(hash_return) + n, 0, hash_size - n);
}

int dht_random_bytes(void* buf, size_t size) {
    randombytes_buf(buf, size);
    return static_cast<int>(size);
}

}  // extern "C"
