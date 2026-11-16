// Mainline (BitTorrent) DHT integration: the client joins the public torrent
// DHT through community well-known nodes, announces its live ip:port under a
// key derived from the account NodeID, and finds contacts by searching their
// key.  No server of ours is involved anywhere (rules R1-R7).
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "common.h"

namespace p2p {

struct MainlineDhtHost {
    std::function<void(const std::string&)> log;
    // A contact's live endpoint arrived from the DHT (compact IPv4 value).
    std::function<void(const sockaddr_in& addr, const NodeId& expected)> on_found;
};

// Stable DHT key under which an account announces its endpoint.  Both sides
// can derive it from the contact's NodeID alone.
NodeId mainline_dht_key(const NodeId& account_id);

// Start the DHT thread.  `sock` is the node's already-bound UDP socket; the
// library sends through it and receives packets fed via mainline_dht_input.
bool mainline_dht_start(socket_t sock, uint16_t messaging_port, const NodeId& account_id,
                        const std::string& state_path, const MainlineDhtHost& host,
                        std::string& err);
void mainline_dht_stop();
bool mainline_dht_running();

// Called from the node's rx_loop for every datagram that is not our own
// protocol (demultiplexed by the first byte: 'd' starts a bencoded KRPC msg).
void mainline_dht_input(const uint8_t* data, size_t len, const sockaddr_in& from);

// Queue a lookup for a contact's announced endpoint (throttled inside).
void mainline_dht_lookup(const NodeId& contact_account_id);

// Override the port announced to the DHT (the NAT-mapped port from a STUN
// probe).  Defaults to the messaging port.
void mainline_dht_set_announce_port(uint16_t port);

// Re-publish our endpoint immediately (public address changed).
void mainline_dht_reannounce_now();

// Number of good DHT nodes in the routing table (for the UI / diagnostics).
int mainline_dht_node_count();

struct MainlineDhtStats {
    int good_nodes = 0;
    int announce_attempts = 0;           // announce searches started
    uint64_t announce_confirmed_ms = 0;  // last SEARCH_DONE (value stored), 0 = never
    uint64_t values_found = 0;           // endpoint values returned by lookups
};
MainlineDhtStats mainline_dht_stats();

}  // namespace p2p
