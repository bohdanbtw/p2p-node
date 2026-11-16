// Dear ImGui messenger front-end for the P2P prototype.
#pragma once

#include <SDL2/SDL.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <set>
#include <string>
#include <vector>

#include "net.h"
#include "voice.h"

struct AppConfig {
    std::string alias = "A";
    bool alias_locked = false;
    uint16_t port = 47000;
    std::string keyfile = "p2pnode.key";
    std::string bootstrap_host;
    uint16_t bootstrap_port = 0;
    std::vector<std::pair<std::string, uint16_t>> bootstrap_list;  // extra --bootstrap entries
    bool audio = true;
    bool demo = false;
    std::string contact_uri;
    std::string contact_name;
    bool borderless = false;
    bool light = true;  // light (white) theme is the default; --dark switches
    int width = 1180;
    int height = 780;
    int x = 40;
    int y = 40;
    int tab = -1;
    std::string testkit_dir;
    std::string self_exe;  // absolute path of this executable (GUI self-test)
};

class App {
  public:
    App() = default;
    ~App();

    bool init(const AppConfig& cfg, std::string& err);
    void run();
    void shutdown();
    p2p::Node& node() { return node_; }                       // --stats recorder
    p2p::VoiceStats voice_stats() const { return voice_.stats(); }

  private:
    enum class ConversationKind { None, Direct, Group };

    void refresh();
    void demo_tick();
    void draw_ui();
    void draw_title_bar();
    void draw_sidebar();
    void draw_conversation();
    void draw_call_panel();
    void draw_inspector();
    void draw_security_tab();
    void draw_group_manage(const p2p::GroupRoom& g);  // members, add, remove, leave
    void update_ringtone(bool ringing);  // synthetic ring while a call is incoming
    void draw_identity_backup();
    void draw_network_tab();
    void draw_audio_tab();
    bool save_mic_test_metrics();
    bool save_test_report();
    void draw_audio_bar();
    void draw_add_contact_modal();
    void draw_new_group_modal();
    void draw_profile_modal();
    void draw_file_modal();
    void draw_message(const p2p::ChatMsg& msg);
    void draw_file_card(const p2p::FileTransferInfo& f, int recipients = 0, int delivered = 0);
    void refresh_audio_devices();
    void apply_audio_devices();
    void load_audio_prefs();
    void save_audio_prefs();
    void draw_avatar(const p2p::NodeId& id, const std::string& alias, float radius,
                     bool online);
    void apply_theme(bool light);
    void toggle_maximize();
    void toggle_fullscreen();
    void select_conversation(ConversationKind kind, const p2p::NodeId& id);
    bool send_text(const std::string& text);
    void start_selected_call();
    void end_call();
    void set_voice_target(const p2p::NodeId& peer, const p2p::NodeId& group);
    std::pair<p2p::NodeId, p2p::NodeId> voice_target() const;

    std::string peer_alias(const p2p::NodeId& id) const;
    std::string conversation_name() const;
    std::string conversation_subtitle() const;
    std::string format_time(uint64_t unix_ms) const;
    std::string format_duration(uint64_t ms) const;
    std::string message_preview(const p2p::NodeId& conversation) const;
    int unread_for(const p2p::NodeId& id) const;
    bool is_online(const p2p::NodeId& id) const;
    bool is_group_selected(const p2p::NodeId& id) const;
    void set_status(const std::string& text);
    void copy_text(const std::string& text, const std::string& what);

    AppConfig cfg_;
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_ = nullptr;
    p2p::Node node_;
    p2p::VoiceEngine voice_;

    bool running_ = true;
    bool voice_ok_ = false;
    bool light_ = false;
    bool fullscreen_ = false;
    bool maximized_ = false;
    bool title_dragging_ = false;
    int saved_x_ = 0;
    int saved_y_ = 0;
    int saved_w_ = 0;
    int saved_h_ = 0;
    int drag_win_x_ = 0;
    int drag_win_y_ = 0;
    float drag_mouse_x_ = 0.0f;
    float drag_mouse_y_ = 0.0f;
    std::string voice_error_;
    uint64_t started_ms_ = 0;

    std::vector<p2p::Contact> contacts_;
    std::vector<p2p::Peer> peers_;
    std::vector<p2p::GroupRoom> groups_;
    std::vector<p2p::ChatMsg> messages_;
    p2p::NodeStats stats_;
    p2p::VoiceStats vstats_;
    p2p::CallInfo call_;
    p2p::NatProfile nat_;
    p2p::NodeId relay_;
    std::vector<p2p::FileTransferInfo> files_;
    p2p::NodeId pending_relay_id_;
    uint64_t pending_relay_at_ = 0;
    bool pending_relay_ = false;
    bool mic_test_active_ = false;
    uint64_t mic_test_until_ = 0;
    std::string mic_test_file_;
    std::string testkit_dir_;
    std::string self_exe_;
    // GUI self-test: runs "<self_exe> --selftest" in a child process.
    std::thread selftest_thread_;
    std::mutex selftest_mtx_;
    std::atomic<int> selftest_state_{0};  // 0 idle, 1 running, 2 done
    int selftest_rc_ = 0;
    std::vector<std::string> selftest_lines_;
    void start_gui_selftest();
    std::vector<std::string> log_lines_;
    uint64_t last_refresh_ms_ = 0;

    ConversationKind selected_kind_ = ConversationKind::None;
    ConversationKind file_target_kind_ = ConversationKind::None;  // target of the open Send-file dialog
    p2p::NodeId file_target_id_;
    std::map<std::string, std::string> drafts_;  // unsent text per conversation
    uint32_t ring_dev_ = 0;               // SDL audio device used for the ringtone (0 = closed)
    std::vector<int16_t> ring_pcm_;       // one ring cycle, 48 kHz mono
    uint64_t ringing_selected_call_ = 0;  // call id we already switched the chat for
    p2p::NodeId selected_id_;
    std::map<p2p::NodeId, int> unread_;
    std::set<std::string> seen_messages_;
    bool inspector_open_ = true;
    int inspector_tab_ = 0;
    bool auto_scroll_ = true;

    mutable std::mutex voice_target_mtx_;
    p2p::NodeId voice_peer_;
    p2p::NodeId voice_group_;
    bool voice_active_by_ui_ = false;
    uint32_t voice_call_id_ = 0;
    double last_fec_loss_ = -1.0;

    char chat_input_[1200] = {0};
    char search_input_[64] = {0};
    char invite_input_[256] = {0};
    std::string add_contact_error_;
    bool focus_composer_ = false;
    char contact_alias_[64] = {0};
    char pin_input_[64] = {0};
    char group_name_[64] = {0};
    char pair_code_[64] = {0};
    char profile_input_[64] = {0};
    char export_path_[512] = {0};
    char import_path_[512] = {0};
    char export_pass_[64] = {0};
    char import_pass_[64] = {0};
    char file_path_[512] = {0};
    bool show_add_contact_ = false;
    bool show_new_group_ = false;
    bool show_profile_ = false;
    bool show_file_ = false;
    // audio device / filter UI state
    std::vector<std::string> in_devs_, out_devs_;
    int in_sel_ = 0, out_sel_ = 0;  // 0 = system default
    bool devs_loaded_ = false;
    bool mic_preview_ui_ = false, hear_myself_ui_ = false;
    std::string backup_msg_;
    bool backup_ok_ = true;
    bool add_by_pin_ = false;
    std::set<p2p::NodeId> group_selected_;
    int bitrate_index_ = 2;
    bool ptt_mode_ = false;
    bool ptt_button_active_ = false;

    std::string status_ = "Ready";
    uint64_t status_ms_ = 0;
    bool demo_ = false;
    uint64_t demo_t0_ = 0;
    bool demo_msg1_ = false;
    bool demo_msg2_ = false;
    bool demo_select_ = false;
    bool demo_inspector_ = false;
    int next_tab_ = -1;
};
