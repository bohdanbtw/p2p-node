// Standalone headless relay/bootstrap node for the P2P prototype.
//
// This binary links only the transport, crypto and DHT code: no SDL2, no
// ImGui and no Opus.  It is intended to run on a small public VPS and to be
// selected as a relay/bootstrap by ordinary p2pnode clients.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "common.h"
#include "net.h"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

void usage() {
    std::printf(
        "p2p-relay - headless relay/bootstrap node\n"
        "  --port N           UDP listen port (default 47000)\n"
        "  --key FILE         identity seed file (default relay.key)\n"
        "  --alias NAME       node alias advertised in Hello (default relay-de)\n"
        "  --public-host H    host printed in the invite URI (default 127.0.0.1)\n"
        "  --run-seconds N    stop after N seconds; 0 means run forever (default 0)\n"
        "  --stats-seconds N  print counters every N seconds (default 30)\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    p2p::NodeConfig cfg;
    cfg.alias = "relay-de";
    cfg.port = 47000;
    cfg.keyfile = "relay.key";
    cfg.persist = false;

    std::string public_host = "127.0.0.1";
    int run_seconds = 0;
    int stats_seconds = 30;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--port" && i + 1 < argc) {
            cfg.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (a == "--key" && i + 1 < argc) {
            cfg.keyfile = argv[++i];
        } else if (a == "--alias" && i + 1 < argc) {
            cfg.alias = argv[++i];
        } else if (a == "--public-host" && i + 1 < argc) {
            public_host = argv[++i];
        } else if (a == "--run-seconds" && i + 1 < argc) {
            run_seconds = std::stoi(argv[++i]);
        } else if (a == "--stats-seconds" && i + 1 < argc) {
            stats_seconds = std::max(1, std::stoi(argv[++i]));
        } else {
            std::fprintf(stderr, "unknown or incomplete argument: %s\n", a.c_str());
            usage();
            return 2;
        }
    }

    p2p::Node node;
    std::string err;
    if (!node.start(cfg, err)) {
        std::fprintf(stderr, "relay start failed: %s\n", err.c_str());
        return 1;
    }

    const p2p::Identity& id = node.identity();
    std::printf("RELAY_READY\n");
    std::printf("NODE_ID=%s\n", id.id.hex().c_str());
    std::printf("LISTEN=0.0.0.0:%u\n", static_cast<unsigned>(node.port()));
    std::printf("INVITE=p2p://%s@%s:%u\n", id.id.hex().c_str(), public_host.c_str(),
                static_cast<unsigned>(node.port()));
    std::printf("ALIAS=%s\n", id.alias.c_str());
    std::fflush(stdout);

    const auto t0 = std::chrono::steady_clock::now();
    auto last_stats = t0;
    while (!g_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto now = std::chrono::steady_clock::now();
        if (run_seconds > 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >=
                run_seconds) {
            break;
        }
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stats).count() >=
            stats_seconds) {
            last_stats = now;
            const p2p::NodeStats s = node.stats();
            std::printf(
                "STATS tx=%llu rx=%llu relay=%llu handshakes=%llu dropped=%llu\n",
                static_cast<unsigned long long>(s.tx_packets),
                static_cast<unsigned long long>(s.rx_packets),
                static_cast<unsigned long long>(s.relay_forwarded),
                static_cast<unsigned long long>(s.handshake_ok),
                static_cast<unsigned long long>(s.dropped));
            std::fflush(stdout);
        }
    }

    node.stop();
    std::printf("RELAY_STOPPED\n");
    return 0;
}
