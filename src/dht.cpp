#include "dht.h"

#include <algorithm>
#include <cmath>

namespace p2p {

int bucket_index(const NodeId& a, const NodeId& b) {
    for (size_t i = 0; i < kIdBytes; ++i) {
        uint8_t x = static_cast<uint8_t>(a.b[i] ^ b.b[i]);
        if (x) {
            int bit = 7;
            while (bit > 0 && !(x & (1u << bit))) --bit;
            return static_cast<int>((kIdBytes - 1 - i) * 8 + bit);
        }
    }
    return 0;
}

void RoutingTable::add(const NodeId& id) {
    if (id.empty() || id == self_) return;
    for (auto& bucket : buckets_) {
        if (std::find(bucket.begin(), bucket.end(), id) != bucket.end()) return;
    }
    int idx = bucket_index(id, self_);
    auto& bucket = buckets_[static_cast<size_t>(std::min<int>(idx, kBuckets - 1))];
    if (bucket.size() >= kBucketSize) bucket.erase(bucket.begin());
    bucket.push_back(id);
}

void RoutingTable::remove(const NodeId& id) {
    for (auto& bucket : buckets_) {
        bucket.erase(std::remove(bucket.begin(), bucket.end(), id), bucket.end());
    }
}

std::vector<NodeId> RoutingTable::closest(const NodeId& target, size_t k) const {
    std::vector<NodeId> all;
    for (const auto& bucket : buckets_) {
        all.insert(all.end(), bucket.begin(), bucket.end());
    }
    std::sort(all.begin(), all.end(), [&](const NodeId& a, const NodeId& b) {
        return xor_less(a, b, target);
    });
    if (all.size() > k) all.resize(k);
    return all;
}

size_t RoutingTable::size() const {
    size_t n = 0;
    for (const auto& bucket : buckets_) n += bucket.size();
    return n;
}

bool RoutingTable::contains(const NodeId& id) const {
    for (const auto& bucket : buckets_) {
        if (std::find(bucket.begin(), bucket.end(), id) != bucket.end()) return true;
    }
    return false;
}

double xor_distance_fraction(const NodeId& a, const NodeId& b) {
    for (size_t i = 0; i < kIdBytes; ++i) {
        uint8_t x = static_cast<uint8_t>(a.b[i] ^ b.b[i]);
        if (x == 0) continue;
        int bit = 7;
        while (bit > 0 && !(x & (1u << bit))) --bit;
        // Normalise the 160-bit distance to [0, 1) using the leading bit
        // position plus the remaining bits of this byte.
        double base = static_cast<double>(1ull << (kIdBytes * 8 - 1 - (i * 8 + bit)));
        double tail = static_cast<double>(x & ((1u << bit) - 1)) / static_cast<double>(1u << bit);
        double value = base * (1.0 + tail);
        double norm = std::ldexp(1.0, static_cast<int>(kIdBytes * 8));
        return std::min(1.0, value / norm);
    }
    return 0.0;
}

double candidate_score(const NodeId& self, const Candidate& c) {
    double distance = xor_distance_fraction(self, c.id);
    double rtt = std::min(1.0, static_cast<double>(c.rtt_ms) / 100.0);
    return 0.25 * distance + 0.15 * rtt + 0.60 * nat_penalty(c.nat);
}

std::vector<Candidate> rank_candidates(const NodeId& self, std::vector<Candidate> candidates) {
    for (auto& c : candidates) c.score = candidate_score(self, c);
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score) return a.score < b.score;
        if (a.direct != b.direct) return a.direct;
        return a.id < b.id;
    });
    return candidates;
}

}  // namespace p2p
