// UDP transport, authenticated handshakes, peer/contact/group state, relay,
// offline envelopes and call signalling for the P2P messenger.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "crypto.h"
#include "dht.h"
#include "mainline_dht.h"

namespace p2p {

enum class PacketType : uint8_t {
    Hello = 1,
    Ping = 2,
    Pong = 3,
    FindNode = 4,
    Nodes = 5,
    Store = 6,
    FindValue = 7,
    Value = 8,
    Chat = 9,
    Ack = 10,
    Voice = 11,
    VoiceEnd = 12,
    HelloAck = 13,
    StunProbe = 14,
    StunReply = 15,
    Relay = 16,
    HolePunch = 17,
    GroupInvite = 18,
    GroupSync = 19,
    GroupChat = 20,
    CallOffer = 21,
    CallAnswer = 22,
    CallBye = 23,
    GroupVoice = 24,
    FileOffer = 25,
    FileAccept = 26,
    FileData = 27,
    FileAck = 28,
    FileDone = 29,
    FileComplete = 30,
    GroupKeyUpdate = 31,
};

constexpr uint8_t kVoiceFlag = 0x01;
constexpr uint8_t kRelayFlag = 0x02;
constexpr uint8_t kGroupForwardFlag = 0x04;

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;
    uint8_t type;
    uint8_t flags;
    uint16_t length;
    uint8_t sender[kIdBytes];
};
#pragma pack(pop)
static_assert(sizeof(PacketHeader) == 28, "unexpected packet header layout");

enum class CallState : uint8_t { Idle = 0, OutgoingRinging, IncomingRinging, ActiveCall };

struct NodeConfig {
    std::string alias = "node";
    bool alias_locked = false;  // --alias explicitly overrides the vault value
    uint16_t port = 47000;
    std::string keyfile = "p2pnode.key";
    // Multiple independent bootstrap addresses.  None of them is mandatory
    // (R1/R4 in ПРАВИЛА_ДЕЦЕНТРАЛІЗАЦІЇ.md): the client tries every entry and
    // also falls back to recently-seen saved contacts from the vault, so it
    // can re-enter the network even if every bootstrap is down (R2/R5).
    std::string bootstrap_host;   // legacy single entry (kept for compat)
    uint16_t bootstrap_port = 0;
    std::vector<std::pair<std::string, uint16_t>> bootstrap_list;
    std::string vault_path;
    bool persist = true;
    bool relay_only = false;  // headless test mode: never auto-connect directly
    std::string pairing_code;
};

struct ChatMsg {
    uint32_t id = 0;
    NodeId peer;                       // conversation id (peer or group)
    NodeId sender;
    NodeId parent_hash;
    std::string text;
    bool outgoing = false;
    bool delivered = false;
    bool queued = false;
    bool is_group = false;
    uint64_t ts_ms = 0;
};

struct FileTransferInfo {
    uint64_t id = 0;
    NodeId peer;
    std::string name;
    std::string path;
    uint64_t size = 0;
    uint64_t transferred = 0;
    bool outgoing = false;
    bool complete = false;
    bool failed = false;
    std::string error;
    uint64_t started_ms = 0;
    uint64_t updated_ms = 0;
    uint64_t unix_ms = 0;  // wall-clock start (to order cards among chat messages)
    NodeId group;          // non-empty: file belongs to this group (outgoing copy or received)
    uint64_t group_batch = 0;  // same value for all per-recipient copies of ONE group send
};

struct Contact {
    NodeId id;
    std::string alias;
    std::array<uint8_t, 32> ed_pk{};
    std::array<uint8_t, 32> x_pk{};
    bool verified = false;
    bool online = false;
    bool relayed = false;
    NatType nat = NatType::Unknown;
    sockaddr_in addr{};
    uint32_t rtt_ms = 0;
    uint64_t last_seen_ms = 0;
    bool in_contacts = true;
};

struct GroupRoom {
    NodeId id;
    std::string name;
    std::array<uint8_t, 32> group_key{};
    std::vector<NodeId> members;
    NodeId last_hash;
    uint64_t created_ms = 0;
};

struct EnvelopeRecord {
    NodeId sender;
    std::vector<uint8_t> payload;
};

struct NatProfile {
    NatType type = NatType::Unknown;
    std::string mapped_ip;
    uint16_t mapped_port = 0;
    bool discovered = false;
};

struct CallInfo {
    CallState state = CallState::Idle;
    bool is_group = false;
    NodeId peer;
    NodeId group;
    uint32_t call_id = 0;
    uint64_t started_ms = 0;
    std::vector<NodeId> participants;
};

struct LookupResult {
    bool found = false;
    bool value = false;
    NodeId target;
    std::string text;
    std::vector<Peer> closest;
    int rounds = 0;
    int queries = 0;
    uint32_t time_ms = 0;
};

struct NodeStats {
    uint64_t tx_packets = 0;
    uint64_t rx_packets = 0;
    uint64_t tx_bytes = 0;
    uint64_t rx_bytes = 0;
    uint64_t chat_sent = 0;
    uint64_t chat_recv = 0;
    uint64_t chat_acked = 0;
    uint64_t voice_sent = 0;
    uint64_t voice_recv = 0;
    uint64_t dropped = 0;
    uint64_t handshake_ok = 0;
    uint64_t handshake_rejected = 0;
    uint64_t mitm_rejected = 0;
    uint64_t replay_hits = 0;
    uint64_t ratchet_steps = 0;
    uint64_t queued = 0;
    uint64_t envelopes_stored = 0;
    uint64_t envelopes_fetched = 0;
    uint64_t relay_forwarded = 0;
    uint64_t group_msgs = 0;
    uint64_t group_voice = 0;
    uint64_t stun_replies = 0;
};

class Node {
  public:
    using VoiceHandler =
        std::function<void(const NodeId&, uint32_t seq, const std::vector<uint8_t>& opus)>;
    using GroupVoiceHandler =
        std::function<void(const NodeId& group, const NodeId&, uint32_t seq,
                           const std::vector<uint8_t>& opus)>;

    Node();
    ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    bool start(const NodeConfig& cfg, std::string& err);
    void stop();

    const Identity& identity() const { return identity_; }
    void set_identity(const Identity& id) { identity_ = id; }
    uint16_t port() const { return port_; }
    const std::string& keyfile() const { return keyfile_; }
    bool running() const { return running_.load(); }
    std::string invite_uri() const;
    // "username:p2p://<id>@host:port" - what the "Copy my link" button puts on the clipboard
    std::string share_uri() const;
    static std::string parse_invite_name(const std::string& uri);

    bool connect_to(const std::string& host, uint16_t port);
    bool connect_expected(const std::string& host, uint16_t port, const NodeId& expected);
    bool connect_uri(const std::string& uri, std::string& err);
    bool connect_via_relay(const NodeId& relay, const NodeId& target,
                           std::string* err = nullptr);
    static bool parse_invite(const std::string& uri, NodeId& id, std::string& host,
                             uint16_t& port);

    void set_pairing_code(const std::string& code);
    std::string pairing_code() const;
    bool search_pairing_code(const std::string& code, std::string& err);
    bool add_contact(const NodeId& id, const std::string& alias, std::string* err = nullptr);
    bool remove_contact(const NodeId& id, std::string* err = nullptr);
    bool set_alias(const std::string& alias, std::string* err = nullptr);
    bool mark_verified(const NodeId& id, bool verified);
    std::vector<Contact> contacts() const;
    bool contact(const NodeId& id, Contact& out) const;
    bool sas(const NodeId& peer, std::string& code, std::string& words) const;
    std::string fingerprint(const NodeId& peer) const;

    bool send_chat(const NodeId& peer, const std::string& text, std::string* err = nullptr);
    bool send_chat_offline(const NodeId& peer, const std::string& text,
                           std::string* err = nullptr);
    bool send_group_chat(const NodeId& group, const std::string& text,
                         std::string* err = nullptr);
    bool send_chat_relay(const NodeId& relay, const NodeId& peer, const std::string& text,
                         std::string* err = nullptr);
    bool send_file(const NodeId& peer, const std::string& path, std::string* err = nullptr,
                   const NodeId& group = NodeId{}, uint64_t batch = 0);
    // Send one file to every online member of a group (one pairwise encrypted transfer each).
    int send_file_group(const NodeId& group, const std::string& path, std::string* err = nullptr);
    // GUI: play incoming voice only while a call is active (otherwise a peer that is still
    // talking after we hung up would stay audible).  Tests/headless keep the old behaviour.
    void set_voice_requires_call(bool v) { std::lock_guard<std::recursive_mutex> lk(mtx_); voice_requires_call_ = v; }
    // True for bootstrap/relay servers (they are infrastructure, not conversation partners).
    bool is_infrastructure(const NodeId& id) const;
    void set_download_dir(const std::string& dir);
    std::string download_dir() const;
    bool set_auto_accept_files(bool enabled, std::string* err = nullptr);
    std::vector<FileTransferInfo> file_transfers() const;
    bool file_transfer(uint64_t id, FileTransferInfo& out) const;
    bool cancel_file_transfer(uint64_t id);
    bool send_voice(const NodeId& peer, uint32_t seq, const std::vector<uint8_t>& opus);
    bool send_voice_end(const NodeId& peer);
    bool send_group_voice(const NodeId& group, uint32_t seq,
                          const std::vector<uint8_t>& opus);

    bool create_group(const std::string& name, const std::vector<NodeId>& members,
                      NodeId& out_id, std::string* err = nullptr);
    std::vector<GroupRoom> groups() const;
    bool group(const NodeId& id, GroupRoom& out) const;
    bool invite_to_group(const NodeId& group, const NodeId& peer);
    bool sync_group(const NodeId& group);
    // Remove a member from the group and rotate the group key (admin only).
    bool exclude_from_group(const NodeId& group, const NodeId& peer, std::string* err = nullptr);
    // Leave a group voluntarily.
    bool leave_group(const NodeId& group, std::string* err = nullptr);

    bool set_relay(const NodeId& relay);
    NodeId relay() const;
    bool probe_nat(const NodeId& peer);
    NatProfile nat_profile() const;

    bool start_call(const NodeId& peer, std::string* err = nullptr);
    bool start_group_call(const NodeId& group, std::string* err = nullptr);
    bool answer_call(bool accept, std::string* err = nullptr);
    void end_call();
    CallInfo call_info() const;
    NodeId group_hub(const NodeId& group) const;

    bool store_value(const NodeId& key, const std::string& value);
    bool local_value(const NodeId& key, std::string& value) const;
    LookupResult lookup(const NodeId& target, bool want_value = false);

    std::vector<Peer> peers() const;
    bool get_peer(const NodeId& id, Peer& out) const;
    std::vector<ChatMsg> chat() const;
    std::vector<ChatMsg> messages() const { return chat(); }
    std::vector<ChatMsg> messages_for(const NodeId& conversation) const;
    NodeStats stats() const;
    std::vector<std::string> take_logs();
    void log(const std::string& line);

    bool save_state(std::string* err = nullptr);
    bool load_state(std::string* err = nullptr);

    void set_voice_handler(VoiceHandler h) { voice_handler_ = std::move(h); }
    void set_group_voice_handler(GroupVoiceHandler h) { group_voice_handler_ = std::move(h); }

  private:
    struct PendingChat {
        NodeId peer;
        uint64_t sent_ms = 0;
        uint8_t retransmit = 0;  // 0=first send, 1=1s, 2=3s, then offline
        uint64_t first_sent_ms = 0;  // for end-to-end delivery latency
        size_t bytes = 0;            // encrypted packet size (metrics)
    };
    struct PendingAck {
        NodeId peer;
        uint32_t msg_id = 0;
        uint64_t sent_ms = 0;
    };
    struct Bootstrap {
        sockaddr_in addr{};
        uint64_t last_hello_ms = 0;
        uint64_t created_ms = 0;
        EphemeralKey eph;
        NodeId expected;
        bool has_expected = false;
    };
    struct OfflineItem {
        uint32_t id = 0;
        NodeId peer;
        std::string text;
        uint64_t ts_ms = 0;
    };
    struct FileIncoming {
        FileTransferInfo info;
        uint64_t have = 0;
        uint32_t chunk_size = 0;
        std::array<uint8_t, 32> sha{};
        std::ofstream out;
        uint64_t last_ack_ms = 0;
        uint64_t last_ack_have = 0;
        // Chunks that arrived ahead of a gap, keyed by byte offset.  The
        // sender keeps kFileWindow datagrams in flight, and a relay or a
        // reordering WAN path can deliver them out of order; buffering here
        // turns that into a short stall instead of a retransmission storm.
        std::map<uint64_t, std::vector<uint8_t>> pending;
    };
    struct FileJob {
        FileTransferInfo info;
        uint64_t have = 0;
        uint64_t remote_have = 0;
        uint64_t size = 0;
        std::string source;
        std::array<uint8_t, 32> sha{};
        uint32_t chunk_size = 1000;
        bool accepted = false;
        bool done_sent = false;
        std::atomic<bool> stop{false};
        bool cancel = false;
        std::thread thread;
    };

    void rx_loop();
    void tick();
    void handle_packet(const uint8_t* data, size_t len, const sockaddr_in& from,
                       bool via_relay = false);
    void handle_hello(const PacketHeader& h, Reader& r, const sockaddr_in& from, bool ack,
                      bool via_relay);
    void handle_nodes(Reader& r);
    void handle_chat(const PacketHeader& h, Reader& r);
    void handle_ack(Reader& r);
    void handle_voice(const PacketHeader& h, Reader& r, bool end);
    void handle_group_voice(const PacketHeader& h, Reader& r);
    void handle_find_value(Reader& r, const sockaddr_in& from);
    void handle_stun_probe(Reader& r, const sockaddr_in& from);
    void handle_stun_reply(Reader& r, const sockaddr_in& from);
    // Public RFC 5389 STUN (first-contact address discovery for the invite).
    void public_stun_tick_locked(uint64_t now);
    bool handle_public_stun(const uint8_t* data, size_t len, const sockaddr_in& from);
    void handle_relay(Reader& r, const sockaddr_in& from);
    void handle_holepunch(Reader& r);
    void handle_group_invite(const PacketHeader& h, Reader& r);
    void handle_group_key_update(const PacketHeader& h, Reader& r);
    void handle_group_sync(const PacketHeader& h, Reader& r);
    void handle_group_chat(const PacketHeader& h, Reader& r);
    void handle_call_offer(const PacketHeader& h, Reader& r);
    void handle_call_answer(const PacketHeader& h, Reader& r);
    void handle_call_bye(const PacketHeader& h, Reader& r);
    void handle_file_offer(const PacketHeader& h, Reader& r);
    void handle_file_accept(const PacketHeader& h, Reader& r);
    void handle_file_data(const PacketHeader& h, Reader& r);
    void handle_file_ack(const PacketHeader& h, Reader& r);
    void handle_file_done(const PacketHeader& h, Reader& r);
    void handle_file_complete(const PacketHeader& h, Reader& r);

    bool send_to(const sockaddr_in& to, PacketType type, const std::vector<uint8_t>& payload,
                 uint8_t flags = 0);
    bool send_hello(const sockaddr_in& to, const EphemeralKey* eph = nullptr, bool ack = false,
                    const NodeId* expected = nullptr,
                    const NodeId* relay_target = nullptr);
    bool send_peer_packet(Peer& peer, PacketType type, const std::vector<uint8_t>& payload,
                          uint8_t flags = 0);
    bool send_file_payload_locked(Peer& peer, PacketType type,
                                  const std::vector<uint8_t>& plain);
    void file_sender_loop(uint64_t transfer_id);
    void send_nodes(const sockaddr_in& to, const NodeId& target);
    void send_holepunch(const sockaddr_in& to, const NodeId& target, const sockaddr_in& ep);
    void introduce_peer_locked(const NodeId& newcomer, const sockaddr_in& newcomer_addr);
    void send_find(const Peer& peer, const NodeId& target, bool value);
    Peer& ensure_peer_locked(const NodeId& id, const sockaddr_in& addr);

    bool seal_with_peer(Peer& peer, PacketType type, const std::vector<uint8_t>& plain,
                        std::vector<uint8_t>& payload);
    void adopt_path_locked(Peer& peer);
    sockaddr_in cur_from_{};
    bool cur_via_relay_ = false;
    bool open_with_peer(Peer& peer, PacketType type, Reader& r, std::vector<uint8_t>& plain,
                        uint64_t* out_seq = nullptr);
    bool send_encrypted_chat(Peer& peer, const NodeId& conv, uint32_t id,
                             const std::string& text, bool group);
    bool retransmit_chat(Peer& peer, uint32_t id, const std::string& text);
    bool send_group_relay(const NodeId& target, const std::vector<uint8_t>& payload);
    bool send_inner_packet(const sockaddr_in& to, const NodeId& target, uint8_t inner_type,
                           const std::vector<uint8_t>& inner);
    bool send_ack_locked(Peer& peer, uint32_t id);

    ChatMsg& add_message_locked(const ChatMsg& msg);
    void touch_contact_locked(const NodeId& id, const Peer& peer);
    void queue_offline_locked(const NodeId& peer, uint32_t id, const std::string& text,
                              uint64_t ts);
    bool publish_envelope_locked(const NodeId& peer, const OfflineItem& item);
    void drain_offline_locked(Peer& peer);
    void poll_inbox_locked();
    bool handle_envelope_locked(const NodeId& sender, const std::vector<uint8_t>& plain);
    bool choose_group_hub_locked(const GroupRoom& group, NodeId& hub) const;
    bool send_group_invite_locked(const GroupRoom& group, const NodeId& member, bool fresh = true);
    bool send_group_key_update_locked(const GroupRoom& group, const NodeId& member);
    bool group_seal(const GroupRoom& group, PacketType type, const std::vector<uint8_t>& plain,
                    std::vector<uint8_t>& payload);
    bool group_open(const GroupRoom& group, PacketType type, Reader& r,
                    std::vector<uint8_t>& plain);
    std::vector<uint8_t> serialize_state_locked() const;
    bool deserialize_state_locked(const std::vector<uint8_t>& data, std::string& err);
    NodeId inbox_key(const NodeId& id) const;

    Identity identity_;
    std::unique_ptr<RoutingTable> routing_;
    socket_t sock_ = kInvalidSocket;
    uint64_t last_public_stun_ms_ = 0;
    uint64_t last_group_sync_ms_ = 0;
    void sync_group_quiet_locked(const NodeId& group_id);
    std::array<uint8_t, 12> public_stun_txid_{};
    sockaddr_in public_stun_addr_{};
    bool public_stun_resolved_ = false;
    uint16_t port_ = 0;
    std::string keyfile_;
    uint64_t last_ip_check_ms_ = 0;
    std::thread stun_resolver_;
    std::atomic<bool> stun_resolving_{false};
    std::atomic<bool> stun_ready_{false};
    sockaddr_in stun_tmp_{};
    std::map<std::array<uint8_t, 16>, uint64_t> seen_hello_nonces_;  // replayed signed HELLOs are dropped
    bool voice_requires_call_ = false;
    bool is_bootstrap_addr_locked(const sockaddr_in& a) const;  // configured bootstrap/relay servers only
    std::vector<sockaddr_in> infra_addrs_;
    void network_changed_locked(const std::string& why);
    void redial_offline_peers_locked(uint64_t now);
    std::string local_ip_;  // non-loopback address of this host probe (LAN-safe invites)
    std::atomic<bool> running_{false};
    std::thread rx_thread_;

    mutable std::recursive_mutex mtx_;
    std::condition_variable_any cv_;
    std::map<NodeId, Peer> peers_;
    std::map<NodeId, Contact> contacts_;
    std::map<NodeId, GroupRoom> groups_;
    // Group invites are single UDP packets: remember who has not answered yet and resend (every
    // 5 s, at most 24 times) until a packet for that group comes back from the member.
    struct InviteState {
        uint64_t last_ms = 0;
        int tries = 0;
        bool confirmed = false;
    };
    std::map<NodeId, std::map<NodeId, InviteState>> invite_state_;  // group id -> member
    std::set<NodeId> left_groups_;  // groups the user left (not persisted)
    void resend_unconfirmed_invites_locked(uint64_t now);
    void mark_invite_confirmed_locked(const NodeId& group, const NodeId& member);
    std::vector<Bootstrap> bootstraps_;
    std::map<uint32_t, std::pair<NodeId, uint64_t>> pending_pings_;
    std::map<uint32_t, sockaddr_in> challenge_addr_;  // nonce -> address being verified
    std::map<uint32_t, PendingChat> pending_chats_;
    std::deque<PendingAck> pending_acks_;
    std::map<NodeId, std::string> values_;
    std::vector<ChatMsg> chat_;
    std::deque<OfflineItem> offline_queue_;
    std::set<std::string> inbox_seen_;
    std::deque<std::string> logs_;
    std::map<uint64_t, FileIncoming> incoming_files_;
    std::map<uint64_t, std::shared_ptr<FileJob>> outgoing_files_;
    std::string download_dir_ = "downloads";
    bool auto_accept_files_ = true;
    uint64_t next_file_id_ = 1;
    bool alias_from_cli_ = false;
    NodeStats stats_;
    CallInfo call_;
    NatProfile nat_;
    std::string pairing_code_;
    NodeId pairing_key_{};
    NodeId relay_id_{};
    NodeId strict_expected_{};
    bool has_strict_expected_ = false;
    // Per-address identity pins: a HELLO coming from an address we dialled
    // expecting NodeID X must carry X (MITM check).  The old single global
    // pin rejected every other contact once a second lookup was dialled.
    std::map<uint64_t, NodeId> pinned_by_addr_;
    static uint64_t addr_key(const sockaddr_in& a) {
        return (static_cast<uint64_t>(a.sin_addr.s_addr) << 16) | a.sin_port;
    }
    bool persist_ = true;
    bool relay_only_ = false;
    std::string vault_path_;
    std::string last_seed_;
    uint32_t next_msg_id_ = 1;
    uint32_t next_nonce_ = 1;
    uint32_t next_call_id_ = 1;
    uint64_t started_ms_ = 0;
    uint64_t last_tick_ms_ = 0;
    uint64_t last_save_ms_ = 0;
    uint64_t last_inbox_poll_ms_ = 0;
    std::mt19937 rng_;
    std::map<std::string, std::pair<uint32_t, uint64_t>> last_stun_;

    // Mainline DHT integration
    MainlineDhtHost mainline_dht_host_;
    std::string dht_state_path_;
    void mainline_dht_on_found(const sockaddr_in& addr, const NodeId& expected);
    std::atomic<bool> mainline_dht_started_{false};

    VoiceHandler voice_handler_;
    GroupVoiceHandler group_voice_handler_;
};

}  // namespace p2p
