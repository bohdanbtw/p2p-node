// Simplified Kademlia routing table (160 buckets, k = 20) and NAT-aware
// candidate ranking used by the overlay and by group-call hub selection.
#pragma once

#include <array>
#include <map>
#include <string>
#include <vector>

#include "common.h"
#include "crypto.h"

namespace p2p {

struct Peer {
    NodeId id;
    sockaddr_in addr{};
    std::array<uint8_t, 32> ed_pk{};
    std::array<uint8_t, 32> x_pk{};
    std::array<uint8_t, 32> shared_key{};  // legacy static DH, kept for diagnostics
    std::array<uint8_t, 32> root_key{};
    std::array<uint8_t, 32> eph_pk{};      // peer ephemeral X25519
    EphemeralKey my_eph;                   // our per-peer ephemeral key
    RatchetState text_ratchet;
    RatchetState voice_ratchet;
    ReplayWindow text_replay;
    ReplayWindow voice_replay;
    bool has_shared = false;
    bool handshake_done = false;
    bool hello_sent = false;
    bool hello_received = false;
    bool direct = false;
    bool relayed = false;
    bool verified = false;  // TOFU -> SAS-confirmed
    bool in_contacts = false;
    NatType nat = NatType::Unknown;
    uint16_t listen_port = 0;
    std::string alias;
    NodeId relay_id{};
    uint64_t last_seen_ms = 0;
    uint64_t handshake_ms = 0;
    uint64_t last_hello_sent_ms = 0;
    uint64_t last_challenge_ms = 0;
    uint64_t last_adopt_ms = 0;
    uint64_t next_redial_ms = 0;   // offline-peer recovery back-off
    uint32_t redial_attempts = 0;
    uint32_t rtt_ms = 0;
    uint32_t replay_hits = 0;

    bool online(uint64_t now, uint64_t timeout_ms = 15000) const {
        return last_seen_ms != 0 && now >= last_seen_ms && now - last_seen_ms < timeout_ms;
    }
};

struct Candidate {
    NodeId id;
    uint32_t rtt_ms = 0;
    NatType nat = NatType::Unknown;
    bool direct = false;
    double score = 0.0;
};

int bucket_index(const NodeId& a, const NodeId& b);
double xor_distance_fraction(const NodeId& a, const NodeId& b);
double candidate_score(const NodeId& self, const Candidate& candidate);
std::vector<Candidate> rank_candidates(const NodeId& self, std::vector<Candidate> candidates);

class RoutingTable {
  public:
    explicit RoutingTable(const NodeId& self) : self_(self) {}

    void add(const NodeId& id);
    void remove(const NodeId& id);
    std::vector<NodeId> closest(const NodeId& target, size_t k) const;
    size_t size() const;
    bool contains(const NodeId& id) const;

  private:
    static constexpr size_t kBuckets = kIdBytes * 8;
    static constexpr size_t kBucketSize = 20;
    NodeId self_;
    std::array<std::vector<NodeId>, kBuckets> buckets_;
};

}  // namespace p2p
