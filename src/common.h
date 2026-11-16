// Common types, identifiers and wire serialisation for the P2P prototype.
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace p2p {

constexpr uint32_t kMagic = 0x50325031u;  // "P2P1"
constexpr size_t kIdBytes = 20;           // 160-bit Kademlia identifier
constexpr size_t kMaxPacket = 1200;       // stay below typical MTU

enum class NatType : uint8_t {
    Unknown = 0,
    FullCone = 1,
    Restricted = 2,
    PortRestricted = 3,
    Symmetric = 4,
};

inline const char* nat_name(NatType t) {
    switch (t) {
        case NatType::FullCone: return "FullCone";
        case NatType::Restricted: return "Restricted";
        case NatType::PortRestricted: return "PortRestricted";
        case NatType::Symmetric: return "Symmetric";
        default: return "Unknown";
    }
}

inline double nat_penalty(NatType t) {
    switch (t) {
        case NatType::FullCone: return 0.0;
        case NatType::Restricted: return 0.25;
        case NatType::PortRestricted: return 0.55;
        case NatType::Symmetric: return 1.0;
        default: return 0.5;
    }
}

struct NodeId {
    std::array<uint8_t, kIdBytes> b{};

    bool operator==(const NodeId& o) const { return b == o.b; }
    bool operator!=(const NodeId& o) const { return b != o.b; }
    bool operator<(const NodeId& o) const { return b < o.b; }
    bool empty() const {
        for (uint8_t v : b)
            if (v) return false;
        return true;
    }

    std::string hex() const {
        static const char* d = "0123456789abcdef";
        std::string s;
        s.resize(kIdBytes * 2);
        for (size_t i = 0; i < kIdBytes; ++i) {
            s[2 * i] = d[b[i] >> 4];
            s[2 * i + 1] = d[b[i] & 0x0f];
        }
        return s;
    }

    std::string short_hex(size_t n = 8) const {
        std::string s = hex();
        if (s.size() <= n) return s;
        return s.substr(0, n);
    }

    static bool parse_hex(const std::string& s, NodeId& out) {
        if (s.size() != kIdBytes * 2) return false;
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < kIdBytes; ++i) {
            int hi = nib(s[2 * i]);
            int lo = nib(s[2 * i + 1]);
            if (hi < 0 || lo < 0) return false;
            out.b[i] = static_cast<uint8_t>((hi << 4) | lo);
        }
        return true;
    }
};

// Lexicographic comparison of XOR distances a^target and b^target.
inline bool xor_less(const NodeId& a, const NodeId& b, const NodeId& target) {
    for (size_t i = 0; i < kIdBytes; ++i) {
        uint8_t x = static_cast<uint8_t>(a.b[i] ^ target.b[i]);
        uint8_t y = static_cast<uint8_t>(b.b[i] ^ target.b[i]);
        if (x != y) return x < y;
    }
    return false;
}

inline uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

inline uint64_t unix_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

inline std::string wall_clock_hms() {
    using namespace std::chrono;
    auto t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

inline std::string human_uptime(uint64_t ms) {
    uint64_t s = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(s / 3600),
                  static_cast<unsigned long long>((s / 60) % 60),
                  static_cast<unsigned long long>(s % 60));
    return buf;
}

class Writer {
  public:
    std::vector<uint8_t> data;

    void u8(uint8_t v) { data.push_back(v); }
    void u16(uint16_t v) {
        data.push_back(static_cast<uint8_t>(v >> 8));
        data.push_back(static_cast<uint8_t>(v));
    }
    void u32(uint32_t v) {
        data.push_back(static_cast<uint8_t>(v >> 24));
        data.push_back(static_cast<uint8_t>(v >> 16));
        data.push_back(static_cast<uint8_t>(v >> 8));
        data.push_back(static_cast<uint8_t>(v));
    }
    void u64(uint64_t v) {
        for (int i = 7; i >= 0; --i) data.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void raw(const void* p, size_t n) {
        const uint8_t* q = static_cast<const uint8_t*>(p);
        data.insert(data.end(), q, q + n);
    }
    void bytes(const std::vector<uint8_t>& v) { raw(v.data(), v.size()); }
    void id(const NodeId& id) { raw(id.b.data(), id.b.size()); }
    void str(const std::string& s) {
        size_t n = std::min<size_t>(s.size(), 65535);
        u16(static_cast<uint16_t>(n));
        raw(s.data(), n);
    }
};

class Reader {
  public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    explicit Reader(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}

    bool ok() const { return ok_; }
    size_t remaining() const { return ok_ ? n_ - off_ : 0; }
    const uint8_t* cur() const { return p_ + off_; }

    uint8_t u8() {
        if (off_ + 1 > n_) return fail<uint8_t>();
        return p_[off_++];
    }
    uint16_t u16() {
        if (off_ + 2 > n_) return fail<uint16_t>();
        uint16_t v = static_cast<uint16_t>((p_[off_] << 8) | p_[off_ + 1]);
        off_ += 2;
        return v;
    }
    uint32_t u32() {
        if (off_ + 4 > n_) return fail<uint32_t>();
        uint32_t v = (static_cast<uint32_t>(p_[off_]) << 24) |
                     (static_cast<uint32_t>(p_[off_ + 1]) << 16) |
                     (static_cast<uint32_t>(p_[off_ + 2]) << 8) |
                     static_cast<uint32_t>(p_[off_ + 3]);
        off_ += 4;
        return v;
    }
    uint64_t u64() {
        if (off_ + 8 > n_) return fail<uint64_t>();
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | p_[off_ + i];
        off_ += 8;
        return v;
    }
    bool raw(void* out, size_t n) {
        if (off_ + n > n_) return fail_bool();
        std::memcpy(out, p_ + off_, n);
        off_ += n;
        return true;
    }
    bool id(NodeId& out) { return raw(out.b.data(), out.b.size()); }
    bool bytes(std::vector<uint8_t>& out, size_t n) {
        if (off_ + n > n_) return fail_bool();
        out.assign(p_ + off_, p_ + off_ + n);
        off_ += n;
        return true;
    }
    std::string str(size_t max_len = 4096) {
        uint16_t n = u16();
        if (!ok_ || n > max_len || off_ + n > n_) return fail<std::string>();
        std::string s(reinterpret_cast<const char*>(p_ + off_), n);
        off_ += n;
        return s;
    }

  private:
    template <typename T>
    T fail() {
        ok_ = false;
        return T{};
    }
    bool fail_bool() {
        ok_ = false;
        return false;
    }

    const uint8_t* p_;
    size_t n_;
    size_t off_ = 0;
    bool ok_ = true;
};

inline void close_socket(socket_t s) {
#ifdef _WIN32
    if (s != INVALID_SOCKET) closesocket(s);
#else
    if (s >= 0) ::close(s);
#endif
}

inline std::string endpoint_to_string(const sockaddr_in& a) {
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
    return std::string(buf) + ":" + std::to_string(ntohs(a.sin_port));
}

inline bool resolve_ipv4(const std::string& host, uint16_t port, sockaddr_in& out) {
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &out.sin_addr) == 1) return true;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    out.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

}  // namespace p2p
