// Identity, ephemeral 4-DH handshake, symmetric ratchet, replay window
// and local encrypted vault.  Only libsodium primitives are used.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace p2p {

constexpr uint64_t kTextRatchetEpoch = 64;    // text key rotation every 64 messages
constexpr uint64_t kVoiceRatchetEpoch = 256;  // media key rotation every 256 packets

struct Identity {
    NodeId id;
    std::array<uint8_t, 32> ed_pk{};
    std::array<uint8_t, 64> ed_sk{};
    std::array<uint8_t, 32> x_pk{};
    std::array<uint8_t, 32> x_sk{};
    std::string alias;

    bool init(const std::string& keyfile, const std::string& alias, std::string& err);
    // Save the 32-byte Ed25519 seed to a file (mode 0600 on POSIX).
    bool save_key(const std::string& keyfile, std::string& err) const;
};

struct EphemeralKey {
    std::array<uint8_t, 32> pk{};
    std::array<uint8_t, 32> sk{};
    bool valid() const {
        for (uint8_t v : pk)
            if (v) return true;
        return false;
    }
};

// 64-bit sliding replay window.  Bit 0 always corresponds to max_seq.
struct ReplayWindow {
    uint64_t max_seq = 0;
    uint64_t bitmap = 0;
    uint32_t hits = 0;
    bool initialized = false;

    bool accept(uint64_t seq);
    void reset();
};

// A symmetric epoch chain.  Keys are derived from the epoch chain, while the
// chain itself advances once per epoch (64/256 packets).  This keeps UDP loss
// inside an epoch from breaking synchronisation.
struct RatchetState {
    std::array<uint8_t, 32> tx_chain{};
    std::array<uint8_t, 32> rx_chain{};
    std::array<uint8_t, 32> rx_prev_chain{};
    uint64_t tx_epoch = 0;
    uint64_t rx_epoch = 0;
    uint64_t rx_prev_epoch = 0;
    uint64_t tx_seq = 0;
    uint64_t steps = 0;
    bool prev_valid = false;
    bool ready = false;
};

EphemeralKey generate_ephemeral();
bool verify_node_id(const NodeId& id, const std::array<uint8_t, 32>& ed_pk);
bool verify_x25519_from_ed(const std::array<uint8_t, 32>& ed_pk,
                           const std::array<uint8_t, 32>& x_pk);

bool derive_shared_key(const std::array<uint8_t, 32>& my_x_sk,
                       const std::array<uint8_t, 32>& peer_x_pk,
                       std::array<uint8_t, 32>& out_key);

// Export/import long-term identity seed encrypted with a passphrase (Argon2id + XChaCha20-Poly1305).
// Format: "P2PK1" | salt(16) | opslimit(4) | memlimit(4) | nonce(24) | ciphertext(48).
bool export_key(const Identity& id, const std::string& passphrase,
                const std::string& outfile, std::string& err);
bool import_key(const std::string& passphrase, const std::string& infile,
                Identity& out_id, std::string& err);

// root_key = BLAKE2b-256(SS || EE || SE || ES || min(ID) || max(ID)).
bool derive_root_key(const std::array<uint8_t, 32>& my_x_sk,
                     const std::array<uint8_t, 32>& peer_x_pk,
                     const std::array<uint8_t, 32>& my_eph_sk,
                     const std::array<uint8_t, 32>& peer_eph_pk,
                     const NodeId& my_id, const NodeId& peer_id,
                     std::array<uint8_t, 32>& root_key);

bool init_ratchet(const std::array<uint8_t, 32>& root_key, const NodeId& local,
                  const NodeId& remote, uint64_t epoch_size, RatchetState& out);

bool ratchet_tx_key(RatchetState& st, uint64_t epoch_size, uint64_t seq,
                    std::array<uint8_t, 32>& key);
bool ratchet_rx_key(RatchetState& st, uint64_t epoch_size, uint64_t seq,
                    std::array<uint8_t, 32>& key);
bool derive_epoch_key(const std::array<uint8_t, 32>& chain, uint64_t epoch,
                      std::array<uint8_t, 32>& key);

std::array<uint8_t, 32> pairing_mac(const std::string& code,
                                    const std::array<uint8_t, 32>& ed_pk,
                                    const std::array<uint8_t, 32>& eph_pk);
NodeId pairing_dht_key(const std::string& code);

// SAS: six decimal digits + four short words, identical on both sides.
std::string sas_code(const std::array<uint8_t, 32>& root_key,
                     const std::array<uint8_t, 32>& ed_a,
                     const std::array<uint8_t, 32>& ed_b);
std::string sas_words(const std::array<uint8_t, 32>& root_key,
                      const std::array<uint8_t, 32>& ed_a,
                      const std::array<uint8_t, 32>& ed_b);
std::string key_fingerprint(const std::array<uint8_t, 32>& ed_pk);

std::vector<uint8_t> seal_record(const std::array<uint8_t, 32>& key,
                                 const uint8_t* data, size_t len, uint8_t aad);
bool open_record(const std::array<uint8_t, 32>& key,
                 const uint8_t* data, size_t len, uint8_t aad,
                 std::vector<uint8_t>& out);

// Local vault, encrypted with ChaCha20-Poly1305 using a key derived from the
// node's Ed25519 secret and a random per-file salt.  No password prompt and no
// external database are required.
bool write_vault(const std::string& path, const std::array<uint8_t, 64>& ed_sk,
                 const std::vector<uint8_t>& plain, std::string& err);
bool read_vault(const std::string& path, const std::array<uint8_t, 64>& ed_sk,
                std::vector<uint8_t>& plain, std::string& err);

std::string hex_encode(const uint8_t* data, size_t len);
bool hex_decode(const std::string& text, std::vector<uint8_t>& out);

}  // namespace p2p
