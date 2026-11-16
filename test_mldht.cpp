// Standalone honest test of the Mainline DHT integration against the LIVE
// BitTorrent DHT.  Two instances, same fixed "account" id:
//   test_mldht a <port>   -> announce  (instance keeps announcing under the key)
//   test_mldht s <port>   -> search    (instance looks up the key, prints found)
// If both join the real public DHT reliably, the searcher must discover the
// announcer's endpoint through on_found.  This proves announce+search work
// with zero servers of ours.
#include "mainline_dht.h"

#include <sodium.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace p2p;

static std::atomic<int> g_found{0};

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s a|s <udp_port>\n", argv[0]);
        return 2;
    }
    if (sodium_init() < 0) return 2;
    char mode = argv[1][0];
    uint16_t port = static_cast<uint16_t>(std::atoi(argv[2]));

#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == kInvalidSocket) {
        std::perror("socket");
        return 2;
    }
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (::bind(sock, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        std::perror("bind");
        return 2;
    }
#if defined(_WIN32)
    DWORD tv_ms = 100;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv_ms), sizeof(tv_ms));
#else
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 100000;
    ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // Fixed shared "account" id; the DHT key is derived from it.
    NodeId account{};
    for (auto& b : account.b) b = 0x5a;
    // The searcher uses a different own id but looks up the announcer's id.
    NodeId target = account;  // the id we look up
    if (mode == 's') {
        for (auto& b : account.b) b = 0x3c;  // searcher's own id differs from target
    }

    MainlineDhtHost host;
    host.log = [](const std::string& line) { std::printf("[log] %s\n", line.c_str()); };
    host.on_found = [&](const sockaddr_in& addr, const NodeId& expected) {
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        std::printf("[FOUND] %s -> %s:%u\n", expected.short_hex(10).c_str(), ip,
                    static_cast<unsigned>(ntohs(addr.sin_port)));
        g_found.fetch_add(1);
    };

    std::string err;
    if (!mainline_dht_start(sock, port, account, "test_mldht_nodes.bin", host, err)) {
        std::printf("dht start failed: %s\n", err.c_str());
        return 2;
    }
    std::printf("mode=%c port=%u started, node id=%s\n", mode, (unsigned)port,
                account.short_hex(8).c_str());

    if (mode == 's') mainline_dht_lookup(target);

    int run_seconds = argc > 3 ? std::atoi(argv[3]) : 60;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(run_seconds);
    int last_good = -1;
    // Announcer self-check: after joining, look up its own key to verify the
    // value is genuinely stored and retrievable from the public DHT.  This is
    // independent of the searcher's own convergence.
    bool self_check_done = false;
    int self_armed = 0;
    if (mode == 'a') { self_armed = 1; }
    // Pump the socket like the app's rx_loop does: forward KRPC datagrams
    // ('d'...) to the DHT, drive the library's periodic work.
    std::vector<uint8_t> buf(2049);
    while (std::chrono::steady_clock::now() < deadline) {
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        ssize_t n = ::recvfrom(sock, reinterpret_cast<char*>(buf.data()), buf.size(), 0,
                               reinterpret_cast<sockaddr*>(&from), &flen);
        if (n > 0 && buf[0] == 'd') mainline_dht_input(buf.data(), static_cast<size_t>(n), from);
        int good = mainline_dht_node_count();
        if (good != last_good) {
            std::printf("... good DHT nodes: %d\n", good);
            last_good = good;
        }
        if (g_found.load() > 0) {
            if (mode == 'a')
                std::printf("SELF-RETRIEVED: announcer found its own value via live DHT\n");
            else
                std::printf("RESULT: found via live Mainline DHT\n");
            mainline_dht_stop();
            return 0;
        }
        // Arm the self-lookup once we've joined.
        if (mode == 'a' && !self_check_done && good >= 1 && self_armed) {
            mainline_dht_lookup(account);
            self_check_done = true;
            self_armed = 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::printf("RESULT: no value found in %ds (good nodes=%d)\n", run_seconds,
                mainline_dht_node_count());
    mainline_dht_stop();
    return 1;
}