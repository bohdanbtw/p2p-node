#include "crypto.h"

#include <sodium.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace p2p {

namespace {

using Bytes32 = std::array<uint8_t, 32>;

void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

Bytes32 hash_parts(const std::vector<const uint8_t*>& parts,
                   const std::vector<size_t>& sizes) {
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, 32);
    for (size_t i = 0; i < parts.size(); ++i)
        crypto_generichash_update(&st, parts[i], sizes[i]);
    Bytes32 out{};
    crypto_generichash_final(&st, out.data(), out.size());
    return out;
}

Bytes32 hash_concat(const std::vector<std::vector<uint8_t>>& chunks) {
    crypto_generichash_state st;
    crypto_generichash_init(&st, nullptr, 0, 32);
    for (const auto& c : chunks)
        if (!c.empty()) crypto_generichash_update(&st, c.data(), c.size());
    Bytes32 out{};
    crypto_generichash_final(&st, out.data(), out.size());
    return out;
}

std::vector<uint8_t> bytes_of(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::vector<uint8_t> bytes_of(const uint8_t* p, size_t n) {
    return std::vector<uint8_t>(p, p + n);
}

std::vector<uint8_t> bytes_of(const char* p, size_t n) {
    return std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(p),
                                reinterpret_cast<const uint8_t*>(p) + n);
}

bool read_seed(const std::string& path, std::array<uint8_t, 32>& seed) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(seed.data()), 32);
    return f.gcount() == 32;
}

bool write_seed(const std::string& path, const std::array<uint8_t, 32>& seed) {
    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(seed.data()), 32);
    if (!f) return false;
#ifndef _WIN32
    // The seed is the long-term identity; do not leave it world-readable.
    (void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
    return true;
}

bool dh(const std::array<uint8_t, 32>& sk, const std::array<uint8_t, 32>& pk,
        Bytes32& out) {
    return crypto_scalarmult_curve25519(out.data(), sk.data(), pk.data()) == 0;
}

Bytes32 vault_key(const std::array<uint8_t, 64>& ed_sk,
                  const std::array<uint8_t, 16>& salt) {
    return hash_parts({ed_sk.data(), salt.data(),
                       reinterpret_cast<const uint8_t*>("p2p-vault-v1")},
                      {ed_sk.size(), salt.size(), 12});
}

}  // namespace

EphemeralKey generate_ephemeral() {
    EphemeralKey k;
    randombytes_buf(k.sk.data(), k.sk.size());
    crypto_scalarmult_curve25519_base(k.pk.data(), k.sk.data());
    return k;
}

bool Identity::init(const std::string& keyfile, const std::string& alias_, std::string& err) {
    if (sodium_init() < 0) {
        err = "libsodium: initialization error";
        return false;
    }
    alias = alias_;

    std::array<uint8_t, 32> seed{};
    if (!read_seed(keyfile, seed)) {
        randombytes_buf(seed.data(), seed.size());
        if (!write_seed(keyfile, seed)) {
            err = "failed to save key " + keyfile;
            return false;
        }
    }

    if (crypto_sign_seed_keypair(ed_pk.data(), ed_sk.data(), seed.data()) != 0) {
        err = "failed to generate Ed25519 keys";
        return false;
    }
    if (crypto_sign_ed25519_sk_to_curve25519(x_sk.data(), ed_sk.data()) != 0) {
        err = "failed to generate X25519 keys";
        return false;
    }
    crypto_scalarmult_curve25519_base(x_pk.data(), x_sk.data());

    crypto_generichash(id.b.data(), id.b.size(), ed_pk.data(), ed_pk.size(), nullptr, 0);
    return true;
}

bool Identity::save_key(const std::string& keyfile, std::string& err) const {
    std::array<uint8_t, 32> seed{};
    std::copy(ed_sk.begin(), ed_sk.begin() + 32, seed.begin());
    if (!write_seed(keyfile, seed)) {
        err = "failed to save key " + keyfile;
        return false;
    }
    return true;
}

bool verify_node_id(const NodeId& id, const std::array<uint8_t, 32>& ed_pk) {
    NodeId calc{};
    crypto_generichash(calc.b.data(), calc.b.size(), ed_pk.data(), ed_pk.size(), nullptr, 0);
    return calc == id;
}

bool verify_x25519_from_ed(const std::array<uint8_t, 32>& ed_pk,
                           const std::array<uint8_t, 32>& x_pk) {
    Bytes32 calc{};
    if (crypto_sign_ed25519_pk_to_curve25519(calc.data(), ed_pk.data()) != 0) return false;
    return calc == x_pk;
}

bool derive_shared_key(const std::array<uint8_t, 32>& my_x_sk,
                       const std::array<uint8_t, 32>& peer_x_pk,
                       std::array<uint8_t, 32>& out_key) {
    Bytes32 shared{};
    if (!dh(my_x_sk, peer_x_pk, shared)) return false;
    crypto_generichash(out_key.data(), out_key.size(), shared.data(), shared.size(), nullptr, 0);
    return true;
}

// Export long-term identity seed (first 32 bytes of ed_sk) encrypted with passphrase.
// Uses Argon2id (crypto_pwhash) for key derivation and XChaCha20-Poly1305 for encryption.
// File format: "P2PK1" | salt(16) | opslimit(4, le) | memlimit(4, le) | nonce(24) | ciphertext(48)
bool export_key(const Identity& id, const std::string& passphrase,
                const std::string& outfile, std::string& err) {
    // The Ed25519 seed is the first 32 bytes of ed_sk
    std::array<uint8_t, 32> seed{};
    std::copy(id.ed_sk.begin(), id.ed_sk.begin() + 32, seed.begin());

    // Generate salt and derive key with Argon2id
    std::array<uint8_t, 16> salt{};
    randombytes_buf(salt.data(), salt.size());

    // Use moderate Argon2id parameters (same as sodium defaults)
    unsigned long long opslimit = crypto_pwhash_OPSLIMIT_INTERACTIVE;
    size_t memlimit = crypto_pwhash_MEMLIMIT_INTERACTIVE;

    std::array<uint8_t, 32> key{};
    if (crypto_pwhash(key.data(), key.size(), passphrase.data(), passphrase.size(),
                      salt.data(), opslimit, memlimit, crypto_pwhash_ALG_ARGON2ID13) != 0) {
        err = "Argon2id key derivation failed";
        return false;
    }

    // Encrypt with XChaCha20-Poly1305
    std::array<uint8_t, 24> nonce{};
    randombytes_buf(nonce.data(), nonce.size());

    std::vector<uint8_t> ciphertext(32 + crypto_aead_xchacha20poly1305_ietf_ABYTES);
    unsigned long long clen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(ciphertext.data(), &clen,
                                                    seed.data(), seed.size(),
                                                    nullptr, 0, nullptr,
                                                    nonce.data(), key.data()) != 0) {
        err = "encryption failed";
        return false;
    }
    ciphertext.resize(clen);

    // Write file
    std::ofstream f(std::filesystem::u8path(outfile), std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "cannot open output file";
        return false;
    }
    f.write("P2PK1", 5);
    f.write(reinterpret_cast<const char*>(salt.data()), salt.size());
    // opslimit (4 bytes, little-endian)
    for (int i = 0; i < 4; ++i) f.put(static_cast<char>((opslimit >> (8 * i)) & 0xFF));
    // memlimit (4 bytes, little-endian)
    for (int i = 0; i < 4; ++i) f.put(static_cast<char>((memlimit >> (8 * i)) & 0xFF));
    f.write(reinterpret_cast<const char*>(nonce.data()), nonce.size());
    f.write(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());
    if (!f) {
        err = "write failed";
        return false;
    }
#ifndef _WIN32
    (void)::chmod(outfile.c_str(), S_IRUSR | S_IWUSR);
#endif
    return true;
}

bool import_key(const std::string& passphrase, const std::string& infile,
                Identity& out_id, std::string& err) {
    std::ifstream f(std::filesystem::u8path(infile), std::ios::binary);
    if (!f) {
        err = "cannot open input file";
        return false;
    }

    // Read magic
    char magic[5];
    f.read(magic, 5);
    if (!f || std::string(magic, 5) != "P2PK1") {
        err = "invalid file format (bad magic)";
        return false;
    }

    // Read salt
    std::array<uint8_t, 16> salt{};
    f.read(reinterpret_cast<char*>(salt.data()), salt.size());
    if (!f || f.gcount() != 16) {
        err = "truncated file (salt)";
        return false;
    }

    // Read opslimit (4 bytes LE)
    unsigned long long opslimit = 0;
    for (int i = 0; i < 4; ++i) {
        char c;
        f.read(&c, 1);
        if (!f) { err = "truncated file (opslimit)"; return false; }
        opslimit |= static_cast<unsigned long long>(static_cast<unsigned char>(c)) << (8 * i);
    }

    // Read memlimit (4 bytes LE)
    size_t memlimit = 0;
    for (int i = 0; i < 4; ++i) {
        char c;
        f.read(&c, 1);
        if (!f) { err = "truncated file (memlimit)"; return false; }
        memlimit |= static_cast<size_t>(static_cast<unsigned char>(c)) << (8 * i);
    }

    // Read nonce (24 bytes)
    std::array<uint8_t, 24> nonce{};
    f.read(reinterpret_cast<char*>(nonce.data()), nonce.size());
    if (!f || f.gcount() != 24) {
        err = "truncated file (nonce)";
        return false;
    }

    // Read remaining ciphertext
    std::vector<uint8_t> ciphertext((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (ciphertext.size() != 32 + crypto_aead_xchacha20poly1305_ietf_ABYTES) {
        err = "unexpected ciphertext length";
        return false;
    }

    // Derive key
    std::array<uint8_t, 32> key{};
    if (crypto_pwhash(key.data(), key.size(), passphrase.data(), passphrase.size(),
                      salt.data(), opslimit, memlimit, crypto_pwhash_ALG_ARGON2ID13) != 0) {
        err = "Argon2id key derivation failed";
        return false;
    }

    // Decrypt
    std::array<uint8_t, 32> seed{};
    unsigned long long dlen = 0;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(seed.data(), &dlen, nullptr,
                                                    ciphertext.data(), ciphertext.size(),
                                                    nullptr, 0, nonce.data(), key.data()) != 0) {
        err = "decryption failed (wrong passphrase?)";
        return false;
    }

    // Reconstruct identity from seed
    if (crypto_sign_seed_keypair(out_id.ed_pk.data(), out_id.ed_sk.data(), seed.data()) != 0) {
        err = "failed to generate Ed25519 keys from seed";
        return false;
    }
    if (crypto_sign_ed25519_sk_to_curve25519(out_id.x_sk.data(), out_id.ed_sk.data()) != 0) {
        err = "failed to generate X25519 keys from seed";
        return false;
    }
    crypto_scalarmult_curve25519_base(out_id.x_pk.data(), out_id.x_sk.data());
    crypto_generichash(out_id.id.b.data(), out_id.id.b.size(),
                       out_id.ed_pk.data(), out_id.ed_pk.size(), nullptr, 0);
    out_id.alias.clear();
    return true;
}

bool derive_root_key(const std::array<uint8_t, 32>& my_x_sk,
                     const std::array<uint8_t, 32>& peer_x_pk,
                     const std::array<uint8_t, 32>& my_eph_sk,
                     const std::array<uint8_t, 32>& peer_eph_pk,
                     const NodeId& my_id, const NodeId& peer_id,
                     std::array<uint8_t, 32>& root_key) {
    Bytes32 ss{}, ee{}, se{}, es{};
    if (!dh(my_x_sk, peer_x_pk, ss)) return false;
    if (!dh(my_eph_sk, peer_eph_pk, ee)) return false;
    if (!dh(my_x_sk, peer_eph_pk, se)) return false;
    if (!dh(my_eph_sk, peer_x_pk, es)) return false;

    const NodeId& lo = my_id < peer_id ? my_id : peer_id;
    const NodeId& hi = my_id < peer_id ? peer_id : my_id;
    // The two cross-DH terms are computed in opposite order on the two peers:
    // A has (A_static,B_eph) then (A_eph,B_static), B has the same two values
    // swapped.  Sort them so both sides hash an identical transcript.
    const Bytes32& cross_lo = std::lexicographical_compare(
                                  se.begin(), se.end(), es.begin(), es.end())
                                  ? se
                                  : es;
    const Bytes32& cross_hi = (&cross_lo == &se) ? es : se;
    std::vector<std::vector<uint8_t>> chunks;
    chunks.push_back(bytes_of(ss.data(), ss.size()));
    chunks.push_back(bytes_of(ee.data(), ee.size()));
    chunks.push_back(bytes_of(cross_lo.data(), cross_lo.size()));
    chunks.push_back(bytes_of(cross_hi.data(), cross_hi.size()));
    chunks.push_back(bytes_of(lo.b.data(), lo.b.size()));
    chunks.push_back(bytes_of(hi.b.data(), hi.b.size()));
    root_key = hash_concat(chunks);
    return true;
}

bool init_ratchet(const std::array<uint8_t, 32>& root_key, const NodeId& local,
                  const NodeId& remote, uint64_t epoch_size, RatchetState& out) {
    (void)epoch_size;
    const NodeId& lo = local < remote ? local : remote;
    const NodeId& hi = local < remote ? remote : local;
    auto derive = [&](const char* label) {
        std::vector<std::vector<uint8_t>> chunks;
        chunks.push_back(bytes_of(root_key.data(), root_key.size()));
        chunks.push_back(bytes_of(label, std::strlen(label)));
        chunks.push_back(bytes_of(lo.b.data(), lo.b.size()));
        chunks.push_back(bytes_of(hi.b.data(), hi.b.size()));
        return hash_concat(chunks);
    };
    Bytes32 a = derive("chain-a");
    Bytes32 b = derive("chain-b");
    out = RatchetState{};
    bool local_low = local < remote;
    out.tx_chain = local_low ? a : b;
    out.rx_chain = local_low ? b : a;
    out.rx_prev_chain = out.rx_chain;
    out.ready = true;
    return true;
}

bool derive_epoch_key(const std::array<uint8_t, 32>& chain, uint64_t epoch,
                      std::array<uint8_t, 32>& key) {
    std::vector<std::vector<uint8_t>> chunks;
    chunks.push_back(bytes_of(chain.data(), chain.size()));
    chunks.push_back(bytes_of("epoch", 5));
    std::vector<uint8_t> e;
    put_u64(e, epoch);
    chunks.push_back(e);
    key = hash_concat(chunks);
    return true;
}

namespace {

void step_chain(Bytes32& chain) {
    std::vector<std::vector<uint8_t>> chunks;
    chunks.push_back(bytes_of(chain.data(), chain.size()));
    chunks.push_back(bytes_of("step", 4));
    chain = hash_concat(chunks);
}

Bytes32 packet_key(const Bytes32& epoch_key, uint64_t seq) {
    std::vector<std::vector<uint8_t>> chunks;
    chunks.push_back(bytes_of(epoch_key.data(), epoch_key.size()));
    chunks.push_back(bytes_of("msg", 3));
    std::vector<uint8_t> s;
    put_u64(s, seq);
    chunks.push_back(s);
    return hash_concat(chunks);
}

}  // namespace

bool ratchet_tx_key(RatchetState& st, uint64_t epoch_size, uint64_t seq,
                    std::array<uint8_t, 32>& key) {
    if (!st.ready || epoch_size == 0) return false;
    uint64_t epoch = seq / epoch_size;
    if (epoch < st.tx_epoch) return false;
    while (st.tx_epoch < epoch) {
        step_chain(st.tx_chain);
        ++st.tx_epoch;
        ++st.steps;
    }
    Bytes32 ek{};
    derive_epoch_key(st.tx_chain, st.tx_epoch, ek);
    key = packet_key(ek, seq);
    st.tx_seq = seq + 1;
    return true;
}

bool ratchet_rx_key(RatchetState& st, uint64_t epoch_size, uint64_t seq,
                    std::array<uint8_t, 32>& key) {
    if (!st.ready || epoch_size == 0) return false;
    uint64_t epoch = seq / epoch_size;
    Bytes32 chain = st.rx_chain;
    if (epoch < st.rx_epoch) {
        if (!(st.prev_valid && epoch == st.rx_prev_epoch)) return false;
        chain = st.rx_prev_chain;
    } else {
        if (epoch - st.rx_epoch > 1024) return false;
        while (st.rx_epoch < epoch) {
            st.rx_prev_chain = st.rx_chain;
            st.rx_prev_epoch = st.rx_epoch;
            st.prev_valid = true;
            step_chain(st.rx_chain);
            ++st.rx_epoch;
            ++st.steps;
        }
        chain = st.rx_chain;
    }
    Bytes32 ek{};
    derive_epoch_key(chain, epoch, ek);
    key = packet_key(ek, seq);
    return true;
}

bool ReplayWindow::accept(uint64_t seq) {
    if (!initialized) {
        initialized = true;
        max_seq = seq;
        bitmap = 1;
        return true;
    }
    if (seq > max_seq) {
        uint64_t diff = seq - max_seq;
        bitmap = diff >= 64 ? 1 : ((bitmap << diff) | 1);
        max_seq = seq;
        return true;
    }
    uint64_t diff = max_seq - seq;
    if (diff >= 64) {
        ++hits;
        return false;
    }
    uint64_t mask = uint64_t{1} << diff;
    if (bitmap & mask) {
        ++hits;
        return false;
    }
    bitmap |= mask;
    return true;
}

void ReplayWindow::reset() {
    max_seq = 0;
    bitmap = 0;
    hits = 0;
    initialized = false;
}

std::array<uint8_t, 32> pairing_mac(const std::string& code,
                                    const std::array<uint8_t, 32>& ed_pk,
                                    const std::array<uint8_t, 32>& eph_pk) {
    std::vector<std::vector<uint8_t>> chunks;
    chunks.push_back(bytes_of(code));
    chunks.push_back(bytes_of(ed_pk.data(), ed_pk.size()));
    chunks.push_back(bytes_of(eph_pk.data(), eph_pk.size()));
    return hash_concat(chunks);
}

NodeId pairing_dht_key(const std::string& code) {
    std::string label = "pairing:" + code;
    NodeId out{};
    crypto_generichash(out.b.data(), out.b.size(),
                       reinterpret_cast<const uint8_t*>(label.data()), label.size(),
                       nullptr, 0);
    return out;
}

namespace {

Bytes32 sas_material(const std::array<uint8_t, 32>& root_key,
                     const std::array<uint8_t, 32>& ed_a,
                     const std::array<uint8_t, 32>& ed_b) {
    const auto& lo = std::lexicographical_compare(ed_a.begin(), ed_a.end(), ed_b.begin(),
                                                  ed_b.end()) ? ed_a : ed_b;
    const auto& hi = (&lo == &ed_a) ? ed_b : ed_a;
    std::vector<std::vector<uint8_t>> chunks;
    chunks.push_back(bytes_of(root_key.data(), root_key.size()));
    chunks.push_back(bytes_of("sas", 3));
    chunks.push_back(bytes_of(lo.data(), lo.size()));
    chunks.push_back(bytes_of(hi.data(), hi.size()));
    return hash_concat(chunks);
}

const char* kWords[] = {
    "amber",  "anchor", "apple",  "arrow",  "atlas",  "baker",  "beacon", "birch",
    "bloom",  "breeze", "cabin",  "canyon", "cedar",  "cloud",  "coral",  "crystal",
    "dawn",   "delta",  "ember",  "falcon", "fern",   "field",  "frost",  "garden",
    "glade",  "harbor", "hazel",  "island", "ivory",  "jade",   "lantern","maple",
    "meadow", "mint",   "moon",   "oak",    "ocean",  "olive",  "pearl",  "pine",
    "rain",   "reef",   "river",  "robin",  "sage",   "shore",  "silver", "sky",
    "snow",   "sparrow","stone",  "storm",  "sun",    "terra",  "tide",   "topaz",
    "valley", "willow", "wind",   "winter", "wolf",   "wood",   "yarrow", "zephyr",
};

}  // namespace

std::string sas_code(const std::array<uint8_t, 32>& root_key,
                     const std::array<uint8_t, 32>& ed_a,
                     const std::array<uint8_t, 32>& ed_b) {
    Bytes32 h = sas_material(root_key, ed_a, ed_b);
    uint32_t v = (static_cast<uint32_t>(h[0]) << 24) |
                 (static_cast<uint32_t>(h[1]) << 16) |
                 (static_cast<uint32_t>(h[2]) << 8) | static_cast<uint32_t>(h[3]);
    v %= 1000000u;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06u", v);
    return buf;
}

std::string sas_words(const std::array<uint8_t, 32>& root_key,
                      const std::array<uint8_t, 32>& ed_a,
                      const std::array<uint8_t, 32>& ed_b) {
    Bytes32 h = sas_material(root_key, ed_a, ed_b);
    std::string out;
    for (int i = 0; i < 4; ++i) {
        uint8_t v = h[4 + static_cast<size_t>(i)];
        if (!out.empty()) out += " ";
        out += kWords[v % (sizeof(kWords) / sizeof(kWords[0]))];
    }
    return out;
}

std::string key_fingerprint(const std::array<uint8_t, 32>& ed_pk) {
    std::array<uint8_t, 32> h{};
    crypto_generichash(h.data(), h.size(), ed_pk.data(), ed_pk.size(), nullptr, 0);
    std::string s = hex_encode(h.data(), 16);
    for (size_t i = 4; i < s.size(); i += 5) s.insert(i, ":");
    return s;
}

std::vector<uint8_t> seal_record(const std::array<uint8_t, 32>& key,
                                 const uint8_t* data, size_t len, uint8_t aad) {
    std::vector<uint8_t> out(12 + len + crypto_aead_chacha20poly1305_ietf_ABYTES);
    randombytes_buf(out.data(), 12);
    unsigned long long clen = 0;
    crypto_aead_chacha20poly1305_ietf_encrypt(
        out.data() + 12, &clen, data, len, &aad, 1, nullptr, out.data(), key.data());
    out.resize(12 + static_cast<size_t>(clen));
    return out;
}

bool open_record(const std::array<uint8_t, 32>& key,
                 const uint8_t* data, size_t len, uint8_t aad,
                 std::vector<uint8_t>& out) {
    constexpr size_t kOverhead = 12 + crypto_aead_chacha20poly1305_ietf_ABYTES;
    if (len < kOverhead) return false;
    out.resize(len - kOverhead);
    unsigned long long mlen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            out.data(), &mlen, nullptr, data + 12, len - 12, &aad, 1, data, key.data()) != 0) {
        out.clear();
        return false;
    }
    out.resize(static_cast<size_t>(mlen));
    return true;
}

bool write_vault(const std::string& path, const std::array<uint8_t, 64>& ed_sk,
                 const std::vector<uint8_t>& plain, std::string& err) {
    static const uint8_t kMagicVault[8] = {'P', '2', 'P', 'V', 'L', 'T', '0', '1'};
    std::array<uint8_t, 16> salt{};
    randombytes_buf(salt.data(), salt.size());
    Bytes32 key = vault_key(ed_sk, salt);
    std::vector<uint8_t> sealed(12 + plain.size() + crypto_aead_chacha20poly1305_ietf_ABYTES);
    randombytes_buf(sealed.data(), 12);
    unsigned long long clen = 0;
    if (crypto_aead_chacha20poly1305_ietf_encrypt(
            sealed.data() + 12, &clen, plain.data(), plain.size(), kMagicVault,
            sizeof(kMagicVault), nullptr, sealed.data(), key.data()) != 0) {
        err = "vault encryption failed";
        return false;
    }
    sealed.resize(12 + static_cast<size_t>(clen));

    std::ofstream f(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
    if (!f) {
        err = "cannot write vault " + path;
        return false;
    }
    f.write(reinterpret_cast<const char*>(kMagicVault), sizeof(kMagicVault));
    f.write(reinterpret_cast<const char*>(salt.data()), salt.size());
    f.write(reinterpret_cast<const char*>(sealed.data()),
            static_cast<std::streamsize>(sealed.size()));
    if (!f) {
        err = "vault write error";
        return false;
    }
#ifndef _WIN32
    (void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
    return true;
}

bool read_vault(const std::string& path, const std::array<uint8_t, 64>& ed_sk,
                std::vector<uint8_t>& plain, std::string& err) {
    static const uint8_t kMagicVault[8] = {'P', '2', 'P', 'V', 'L', 'T', '0', '1'};
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f) {
        err = "vault not found";
        return false;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
    if (file.size() < sizeof(kMagicVault) + 16 + 12 + crypto_aead_chacha20poly1305_ietf_ABYTES ||
        std::memcmp(file.data(), kMagicVault, sizeof(kMagicVault)) != 0) {
        err = "vault header is invalid";
        return false;
    }
    std::array<uint8_t, 16> salt{};
    std::memcpy(salt.data(), file.data() + sizeof(kMagicVault), salt.size());
    Bytes32 key = vault_key(ed_sk, salt);
    const uint8_t* sealed = file.data() + sizeof(kMagicVault) + salt.size();
    size_t sealed_len = file.size() - sizeof(kMagicVault) - salt.size();
    plain.resize(sealed_len - 12 - crypto_aead_chacha20poly1305_ietf_ABYTES);
    unsigned long long mlen = 0;
    if (crypto_aead_chacha20poly1305_ietf_decrypt(
            plain.data(), &mlen, nullptr, sealed + 12, sealed_len - 12, kMagicVault,
            sizeof(kMagicVault), sealed, key.data()) != 0) {
        plain.clear();
        err = "vault authentication failed";
        return false;
    }
    plain.resize(static_cast<size_t>(mlen));
    return true;
}

std::string hex_encode(const uint8_t* data, size_t len) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s[2 * i] = d[data[i] >> 4];
        s[2 * i + 1] = d[data[i] & 0x0f];
    }
    return s;
}

bool hex_decode(const std::string& text, std::vector<uint8_t>& out) {
    if (text.size() % 2 != 0) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        int hi = nib(text[i]);
        int lo = nib(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

}  // namespace p2p
