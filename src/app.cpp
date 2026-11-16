#include "app.h"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <SDL2/SDL_opengl.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "report.h"

namespace {

ImVec4 c_accent(0.30f, 0.72f, 0.98f, 1.0f);
ImVec4 c_green(0.35f, 0.85f, 0.55f, 1.0f);
ImVec4 c_amber(0.98f, 0.72f, 0.30f, 1.0f);
ImVec4 c_red(0.95f, 0.38f, 0.38f, 1.0f);
ImVec4 c_muted(0.55f, 0.60f, 0.68f, 1.0f);
ImVec4 c_sidebar(0.070f, 0.080f, 0.103f, 1.0f);
ImVec4 c_panel(0.085f, 0.095f, 0.120f, 1.0f);
ImVec4 c_bubble_in(0.115f, 0.130f, 0.165f, 1.0f);
ImVec4 c_bubble_out(0.080f, 0.360f, 0.610f, 1.0f);
ImVec4 c_input_bg(0.105f, 0.125f, 0.160f, 1.0f);

// Escape a value before embedding it in a hand-written JSON document.  The
// GUI writes the microphone-test metadata itself; a file name containing a
// quote or backslash would otherwise produce invalid JSON.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

const char* find_font() {
    const char* paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
    };
    for (const char* p : paths) {
        std::ifstream f(p, std::ios::binary);
        if (f.good()) return p;
    }
    return nullptr;
}

// Per-user application data directory, created if missing.  The GUI stores
// its identity key and encrypted vault here so both survive a relaunch from
// any working directory (and do not litter the desktop or the repo folder):
//   Windows: %APPDATA%\P2PMessenger\
//   Linux:   $XDG_DATA_HOME/p2pmessenger  (or ~/.local/share/p2pmessenger)
// Headless / selftest mode deliberately ignores this and keeps files in the
// current directory, so test runs stay isolated and disposable.
std::string app_data_dir() {
#ifdef _WIN32
    const char* env = std::getenv("APPDATA");
    std::string base = env && *env ? env : "";
    if (base.empty()) {
        const char* ud = std::getenv("USERPROFILE");
        if (ud && *ud) base = std::string(ud) + "\\AppData\\Roaming";
    }
    if (base.empty()) return "";
    base += "\\P2PMessenger";
#else
    const char* env = std::getenv("XDG_DATA_HOME");
    std::string base = env && *env ? env : "";
    if (base.empty()) {
        const char* home = std::getenv("HOME");
        if (home && *home) base = std::string(home) + "/.local/share";
    }
    if (base.empty()) return "";
    base += "/p2pmessenger";
#endif
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    return base;
}

const ImWchar* glyph_ranges() {
    static const ImWchar ranges[] = {
        0x0020, 0x00FF,
        0x0400, 0x04FF,
        0x2000, 0x206F,
        0x2100, 0x214F,  // letterlike symbols (№)
        0x2190, 0x21FF,
        0x25A0, 0x25FF,
        0x2600, 0x26FF,
        0x2700, 0x27BF,
        0,
    };
    return ranges;
}

double r_factor(double delay_ms, double loss_pct) {
    delay_ms = std::max(0.0, delay_ms);
    loss_pct = std::min(100.0, std::max(0.0, loss_pct));
    const double ie = 8.0;
    const double bpl = 15.0;
    double id = 0.024 * delay_ms;
    if (delay_ms > 177.3) id += 0.11 * (delay_ms - 177.3);
    double ie_eff = loss_pct > 0 ? ie + (95.0 - ie) * loss_pct / (loss_pct + bpl) : ie;
    return std::max(0.0, std::min(100.0, 93.2 - id - ie_eff));
}

double mos_from_r(double r) {
    if (r <= 0) return 1.0;
    if (r >= 100) return 4.5;
    double v = 1.0 + 0.035 * r + 7e-6 * r * (r - 60.0) * (100.0 - r);
    return std::max(1.0, std::min(4.5, v));
}

std::string lower_copy(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// n counts bytes, but the cut never lands inside a multi-byte UTF-8 character (Cyrillic names).
std::string truncate(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    size_t cut = n;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

// First full UTF-8 character of the name (upper-cased when it is plain ASCII).
std::string initial_of(const std::string& alias) {
    if (alias.empty()) return "?";
    const unsigned char first = static_cast<unsigned char>(alias[0]);
    size_t len = first < 0x80 ? 1 : (first >> 5) == 0x6 ? 2 : (first >> 4) == 0xE ? 3 : (first >> 3) == 0x1E ? 4 : 1;
    if (len > alias.size()) len = 1;
    std::string out = alias.substr(0, len);
    if (len == 1) out[0] = static_cast<char>(std::toupper(first));
    return out;
}

std::string fmt1(double v) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f", v);
    return b;
}

}  // namespace

App::~App() { shutdown(); }

namespace {
std::string user_downloads_dir();
}

bool App::init(const AppConfig& cfg, std::string& err) {
    cfg_ = cfg;
    light_ = cfg.light;
    self_exe_ = cfg.self_exe;
    testkit_dir_ = cfg.testkit_dir.empty() ? "testkit" : cfg.testkit_dir;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        err = std::string("SDL: ") + SDL_GetError();
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
    if (cfg.borderless) flags |= SDL_WINDOW_BORDERLESS;
    window_ = SDL_CreateWindow(("P2P Messenger - " + cfg.alias).c_str(), cfg.x, cfg.y,
                               cfg.width, cfg.height, flags);
    if (!window_) {
        err = std::string("SDL window: ") + SDL_GetError();
        return false;
    }
    if (cfg.borderless) SDL_SetWindowBordered(window_, SDL_FALSE);

    gl_ = SDL_GL_CreateContext(window_);
    if (!gl_) {
        err = std::string("OpenGL context: ") + SDL_GetError();
        return false;
    }
    SDL_GL_MakeCurrent(window_, gl_);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.FontGlobalScale = 1.0f;
    apply_theme(cfg.light);

    const char* font_path = find_font();
    if (font_path) {
        ImFontConfig fc;
        fc.OversampleH = 2;
        fc.OversampleV = 2;
        fc.PixelSnapH = true;
        if (!io.Fonts->AddFontFromFileTTF(font_path, 16.0f, &fc, glyph_ranges()))
            io.Fonts->AddFontDefault();
    } else {
        io.Fonts->AddFontDefault();
    }

    ImGui_ImplSDL2_InitForOpenGL(window_, gl_);
    ImGui_ImplOpenGL3_Init("#version 330");

    if (!cfg.contact_uri.empty()) {
        p2p::NodeId cid;
        std::string host;
        uint16_t port = 0;
        if (p2p::Node::parse_invite(cfg.contact_uri, cid, host, port)) {
            std::string calias = cfg.contact_name.empty() ? cid.short_hex(8) : cfg.contact_name;
            node_.add_contact(cid, calias);
            std::string cerr;
            if (!node_.connect_uri(cfg.contact_uri, cerr))
                node_.log("contact connect failed: " + cerr);
        } else {
            node_.log("invalid --contact-uri");
        }
    }

    p2p::NodeConfig nc;
    nc.alias = cfg.alias;
    nc.alias_locked = cfg.alias_locked;
    nc.port = cfg.port;
    nc.keyfile = cfg.keyfile;
    nc.bootstrap_host = cfg.bootstrap_host;
    nc.bootstrap_port = cfg.bootstrap_port;
    nc.bootstrap_list = cfg.bootstrap_list;
    // Persist identity + history in the per-user app-data directory so a
    // relaunch from any folder still finds the same account and messages.
    std::string data = app_data_dir();
    if (!data.empty()) {
        if (nc.keyfile == "p2pnode.key")
            nc.keyfile = data + "/p2pnode.key";
        if (nc.vault_path.empty()) {
            // One vault per identity: the default account keeps vault.dat,
            // a custom --key gets its own file instead of fighting over it.
            if (cfg.keyfile == "p2pnode.key") {
                nc.vault_path = data + "/vault.dat";
            } else {
                std::string stem = std::filesystem::path(cfg.keyfile).stem().string();
                for (auto& ch : stem)
                    if (!std::isalnum(static_cast<unsigned char>(ch))) ch = '_';
                nc.vault_path = data + "/vault_" + stem + ".dat";
            }
        }
    }
    if (!node_.start(nc, err)) return false;
    node_.set_download_dir(user_downloads_dir());  // visible, per-user, opened from the chat

    if (!cfg.contact_uri.empty()) {
        p2p::NodeId cid;
        std::string host;
        uint16_t port = 0;
        if (p2p::Node::parse_invite(cfg.contact_uri, cid, host, port)) {
            std::string calias = cfg.contact_name.empty() ? cid.short_hex(8) : cfg.contact_name;
            node_.add_contact(cid, calias);
            std::string cerr;
            if (!node_.connect_uri(cfg.contact_uri, cerr))
                node_.log("contact connect failed: " + cerr);
        } else {
            node_.log("invalid --contact-uri");
        }
    }

    std::string verr;
    voice_ok_ = voice_.init(cfg.audio, verr);
    if (!voice_ok_) {
        voice_error_ = verr;
        node_.log("voice unavailable: " + verr);
    } else {
        voice_.set_vad(true);
        voice_.set_bitrate(32000);
        node_.set_voice_requires_call(true);  // never play audio outside an active call
        load_audio_prefs();
        voice_.set_sender([this](uint32_t seq, const std::vector<uint8_t>& opus) {
            auto target = voice_target();
            if (!target.second.empty())
                node_.send_group_voice(target.second, seq, opus);
            else if (!target.first.empty())
                node_.send_voice(target.first, seq, opus);
        });
        voice_.set_end_sender([this]() {
            auto target = voice_target();
            if (!target.second.empty())
                return;
            if (!target.first.empty()) node_.send_voice_end(target.first);
        });
        node_.set_voice_handler(
            [this](const p2p::NodeId& sender, uint32_t seq, const std::vector<uint8_t>& opus) {
                if (opus.empty())
                    voice_.on_peer_end(sender);
                else
                    voice_.on_packet(sender, seq, opus);
            });
        node_.set_group_voice_handler(
            [this](const p2p::NodeId&, const p2p::NodeId& sender, uint32_t seq,
                   const std::vector<uint8_t>& opus) {
                if (!opus.empty()) voice_.on_packet(sender, seq, opus);
            });
    }

    std::snprintf(pair_code_, sizeof(pair_code_), "%s", node_.pairing_code().c_str());
    started_ms_ = p2p::now_ms();
    demo_ = cfg.demo;
    demo_t0_ = started_ms_;
    demo_inspector_ = cfg.tab == 0 || cfg.tab == 2 || cfg.tab == 3;
    next_tab_ = cfg.tab;
    set_status("Ready");
    return true;
}

namespace {
// Strip whitespace and the quotes Windows "Copy as path" adds.
std::string clean_path(const std::string& in) {
    size_t b = 0, e = in.size();
    while (b < e && (std::isspace(static_cast<unsigned char>(in[b])) || in[b] == '"' || in[b] == '\'')) ++b;
    while (e > b && (std::isspace(static_cast<unsigned char>(in[e - 1])) || in[e - 1] == '"' || in[e - 1] == '\'')) --e;
    std::string out = in.substr(b, e - b);
    if (out.rfind("file://", 0) == 0) out = out.substr(7);
    return out;
}

// Native "open file" dialog.  Returns an empty string when cancelled.
std::string pick_file_dialog() {
#ifdef _WIN32
    wchar_t buf[2048] = {0};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buf;
    ofn.nMaxFile = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    ofn.lpstrFilter = L"All files\0*.*\0";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? std::filesystem::path(buf).u8string() : std::string();
#else
    const char* cmds[] = {"zenity --file-selection 2>/dev/null",
                          "kdialog --getopenfilename 2>/dev/null"};
    for (const char* c : cmds) {
        FILE* f = popen(c, "r");
        if (!f) continue;
        char buf[1024] = {0};
        bool got = std::fgets(buf, sizeof(buf), f) != nullptr;
        int rc = pclose(f);
        if (got && rc == 0) return clean_path(buf);
    }
    return {};
#endif
}

// ---- opening files from the chat -------------------------------------------------
std::string path_sep() {
#ifdef _WIN32
    return "\\";
#else
    return "/";
#endif
}

std::string user_downloads_dir() {
    std::string base = ".";
#ifdef _WIN32
    if (const wchar_t* w = _wgetenv(L"USERPROFILE"))
        base = std::filesystem::path(w).u8string() + "\\Downloads";
#else
    if (const char* h = std::getenv("HOME")) {
        base = std::string(h) + "/Downloads";
        std::error_code ec;
        if (!std::filesystem::exists(base, ec)) base = h;
    }
#endif
    return base + path_sep() + "MKR-received";
}

void spawn_detached(const char* prog, const std::string& arg) {
#ifndef _WIN32
    pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {  // double fork: the opener never becomes a zombie
            setsid();
            int nul = open("/dev/null", O_WRONLY);   // only async-signal-safe calls after fork()
            if (nul >= 0) {
                dup2(nul, 1);
                dup2(nul, 2);
            }
            execlp(prog, prog, arg.c_str(), static_cast<char*>(nullptr));
        }
        _exit(0);
    } else if (pid > 0) {
        int st = 0;
        waitpid(pid, &st, 0);
    }
#else
    (void)prog;
    (void)arg;
#endif
}

bool open_path(const std::string& utf8) {
    std::error_code ec;
    if (utf8.empty() || !std::filesystem::exists(std::filesystem::u8path(utf8), ec)) return false;
#ifdef _WIN32
    std::wstring w = std::filesystem::u8path(utf8).wstring();
    return reinterpret_cast<INT_PTR>(
               ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
#else
    spawn_detached("xdg-open", utf8);
    return true;
#endif
}

bool reveal_path(const std::string& utf8) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::u8path(utf8);
    if (utf8.empty() || !std::filesystem::exists(p, ec)) return false;
#ifdef _WIN32
    std::wstring args = L"/select,\"" + p.wstring() + L"\"";
    return reinterpret_cast<INT_PTR>(
               ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr,
                             SW_SHOWNORMAL)) > 32;
#else
    spawn_detached("xdg-open", p.parent_path().u8string());
    return true;
#endif
}

// Received files with an executable/scriptable type are shown in the folder, never launched by a click.
bool risky_file_type(const std::string& name) {
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string e = name.substr(dot + 1);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const char* bad[] = {"exe", "bat", "cmd", "com", "scr", "pif", "ps1", "psm1", "vbs", "vbe",
                                "js", "jse", "wsf", "wsh", "lnk", "msi", "msp", "jar", "sh", "desktop",
                                "appimage", "reg", "hta", "cpl", "dll", "run", "bin", "py", "pl", "rb",
                                "app", "deb", "rpm", "command", "url"};
    for (const char* b : bad)
        if (e == b) return true;
    return false;
}

std::string human_size(uint64_t b) {
    char buf[32];
    if (b < 1024) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(b));
    else if (b < 1024ull * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", b / 1024.0);
    else if (b < 1024ull * 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f MB", b / 1048576.0);
    else std::snprintf(buf, sizeof(buf), "%.2f GB", b / 1073741824.0);
    return buf;
}

// Shorten UTF-8 text to fit `max_w` pixels, appending an ellipsis.
std::string fit_text(const std::string& s, float max_w) {
    if (ImGui::CalcTextSize(s.c_str()).x <= max_w) return s;
    std::string out = s;
    while (!out.empty()) {
        do { out.pop_back(); } while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80);
        if (ImGui::CalcTextSize((out + "\xE2\x80\xA6").c_str()).x <= max_w) break;
    }
    return out + "\xE2\x80\xA6";
}
}  // namespace

void App::apply_theme(bool light) {
    light_ = light;
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 0.0f;
    st.ChildRounding = 12.0f;
    st.FrameRounding = 9.0f;
    st.PopupRounding = 12.0f;
    st.ScrollbarRounding = 10.0f;
    st.GrabRounding = 9.0f;
    st.TabRounding = 9.0f;
    st.WindowBorderSize = 0.0f;
    st.ChildBorderSize = 1.0f;
    st.FrameBorderSize = 0.0f;
    st.WindowPadding = ImVec2(16.0f, 14.0f);
    st.FramePadding = ImVec2(12.0f, 7.0f);
    st.ItemSpacing = ImVec2(10.0f, 9.0f);
    st.ScrollbarSize = 11.0f;

    if (light) {
        ImGui::StyleColorsLight();
        st.Colors[ImGuiCol_WindowBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        st.Colors[ImGuiCol_ChildBg] = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        st.Colors[ImGuiCol_PopupBg] = ImVec4(1.0f, 1.0f, 1.0f, 0.99f);
        st.Colors[ImGuiCol_Border] = ImVec4(0.84f, 0.87f, 0.91f, 1.0f);
        st.Colors[ImGuiCol_FrameBg] = ImVec4(0.955f, 0.966f, 0.978f, 1.0f);
        st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.915f, 0.940f, 0.968f, 1.0f);
        st.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.870f, 0.912f, 0.955f, 1.0f);
        st.Colors[ImGuiCol_Button] = ImVec4(0.900f, 0.938f, 0.978f, 1.0f);
        st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.820f, 0.895f, 0.970f, 1.0f);
        st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.740f, 0.850f, 0.955f, 1.0f);
        st.Colors[ImGuiCol_Header] = ImVec4(0.880f, 0.925f, 0.975f, 1.0f);
        st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.810f, 0.885f, 0.965f, 1.0f);
        st.Colors[ImGuiCol_HeaderActive] = ImVec4(0.730f, 0.845f, 0.955f, 1.0f);
        st.Colors[ImGuiCol_Tab] = ImVec4(0.945f, 0.960f, 0.978f, 1.0f);
        st.Colors[ImGuiCol_TabHovered] = ImVec4(0.850f, 0.910f, 0.975f, 1.0f);
        st.Colors[ImGuiCol_TabSelected] = ImVec4(0.820f, 0.895f, 0.978f, 1.0f);
        st.Colors[ImGuiCol_Text] = ImVec4(0.085f, 0.105f, 0.145f, 1.0f);
        st.Colors[ImGuiCol_TextDisabled] = ImVec4(0.420f, 0.470f, 0.540f, 1.0f);
        st.Colors[ImGuiCol_Separator] = ImVec4(0.850f, 0.880f, 0.910f, 1.0f);
        st.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.945f, 0.960f, 0.978f, 1.0f);
        st.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        st.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.0f, 0.0f, 0.0f, 0.022f);
        st.Colors[ImGuiCol_CheckMark] = ImVec4(0.205f, 0.330f, 0.820f, 1.0f);
        st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.205f, 0.330f, 0.820f, 1.0f);
        c_accent = ImVec4(0.205f, 0.330f, 0.820f, 1.0f);
        c_green = ImVec4(0.075f, 0.545f, 0.320f, 1.0f);
        c_amber = ImVec4(0.720f, 0.430f, 0.020f, 1.0f);
        c_red = ImVec4(0.780f, 0.145f, 0.145f, 1.0f);
        c_muted = ImVec4(0.380f, 0.430f, 0.500f, 1.0f);
        c_sidebar = ImVec4(0.962f, 0.972f, 0.984f, 1.0f);
        c_panel = ImVec4(0.975f, 0.982f, 0.992f, 1.0f);
        c_bubble_in = ImVec4(0.930f, 0.942f, 0.965f, 1.0f);
        c_bubble_out = ImVec4(0.205f, 0.330f, 0.820f, 1.0f);
        c_input_bg = ImVec4(0.960f, 0.970f, 0.982f, 1.0f);
    } else {
        ImGui::StyleColorsDark();
        st.Colors[ImGuiCol_WindowBg] = ImVec4(0.050f, 0.058f, 0.075f, 1.0f);
        st.Colors[ImGuiCol_ChildBg] = ImVec4(0.070f, 0.080f, 0.103f, 1.0f);
        st.Colors[ImGuiCol_PopupBg] = ImVec4(0.085f, 0.095f, 0.120f, 0.99f);
        st.Colors[ImGuiCol_Border] = ImVec4(0.165f, 0.195f, 0.250f, 0.90f);
        st.Colors[ImGuiCol_FrameBg] = ImVec4(0.105f, 0.125f, 0.160f, 1.0f);
        st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.140f, 0.170f, 0.220f, 1.0f);
        st.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.160f, 0.200f, 0.260f, 1.0f);
        st.Colors[ImGuiCol_Button] = ImVec4(0.120f, 0.300f, 0.480f, 1.0f);
        st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.160f, 0.420f, 0.650f, 1.0f);
        st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.120f, 0.500f, 0.800f, 1.0f);
        st.Colors[ImGuiCol_Header] = ImVec4(0.125f, 0.255f, 0.390f, 0.80f);
        st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.170f, 0.360f, 0.540f, 0.90f);
        st.Colors[ImGuiCol_HeaderActive] = ImVec4(0.190f, 0.430f, 0.640f, 1.0f);
        st.Colors[ImGuiCol_Tab] = ImVec4(0.080f, 0.100f, 0.130f, 1.0f);
        st.Colors[ImGuiCol_TabHovered] = ImVec4(0.160f, 0.340f, 0.500f, 1.0f);
        st.Colors[ImGuiCol_TabSelected] = ImVec4(0.120f, 0.260f, 0.400f, 1.0f);
        st.Colors[ImGuiCol_Text] = ImVec4(0.905f, 0.925f, 0.955f, 1.0f);
        st.Colors[ImGuiCol_TextDisabled] = ImVec4(0.520f, 0.570f, 0.640f, 1.0f);
        st.Colors[ImGuiCol_Separator] = ImVec4(0.165f, 0.195f, 0.250f, 0.90f);
        st.Colors[ImGuiCol_TableHeaderBg] = ImVec4(0.090f, 0.110f, 0.140f, 1.0f);
        st.Colors[ImGuiCol_TableRowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        st.Colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.024f);
        c_accent = ImVec4(0.300f, 0.720f, 0.980f, 1.0f);
        c_green = ImVec4(0.350f, 0.850f, 0.550f, 1.0f);
        c_amber = ImVec4(0.980f, 0.720f, 0.300f, 1.0f);
        c_red = ImVec4(0.950f, 0.380f, 0.380f, 1.0f);
        c_muted = ImVec4(0.550f, 0.600f, 0.680f, 1.0f);
        c_sidebar = ImVec4(0.058f, 0.068f, 0.088f, 1.0f);
        c_panel = ImVec4(0.075f, 0.086f, 0.110f, 1.0f);
        c_bubble_in = ImVec4(0.115f, 0.130f, 0.165f, 1.0f);
        c_bubble_out = ImVec4(0.080f, 0.360f, 0.610f, 1.0f);
        c_input_bg = ImVec4(0.100f, 0.118f, 0.150f, 1.0f);
    }
}

void App::toggle_maximize() {
    if (fullscreen_) {
        toggle_fullscreen();
        return;
    }
    if (!maximized_) {
        SDL_GetWindowPosition(window_, &saved_x_, &saved_y_);
        SDL_GetWindowSize(window_, &saved_w_, &saved_h_);
        SDL_Rect b{};
        int display = SDL_GetWindowDisplayIndex(window_);
        if (display < 0 || SDL_GetDisplayUsableBounds(display, &b) != 0)
            SDL_GetDisplayBounds(display < 0 ? 0 : display, &b);
        SDL_SetWindowPosition(window_, b.x, b.y);
        SDL_SetWindowSize(window_, b.w, b.h);
        maximized_ = true;
    } else {
        SDL_SetWindowPosition(window_, saved_x_, saved_y_);
        SDL_SetWindowSize(window_, saved_w_, saved_h_);
        maximized_ = false;
    }
}

void App::toggle_fullscreen() {
    if (!fullscreen_) {
        SDL_GetWindowPosition(window_, &saved_x_, &saved_y_);
        SDL_GetWindowSize(window_, &saved_w_, &saved_h_);
        SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
        fullscreen_ = true;
    } else {
        SDL_SetWindowFullscreen(window_, 0);
        SDL_SetWindowPosition(window_, saved_x_, saved_y_);
        SDL_SetWindowSize(window_, saved_w_, saved_h_);
        fullscreen_ = false;
    }
}

void App::shutdown() {
    if (selftest_thread_.joinable()) selftest_thread_.join();
    update_ringtone(false);
    if (voice_.talking()) voice_.stop_talk();
    voice_.shutdown();
    node_.stop();
    if (gl_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();
        SDL_GL_DeleteContext(gl_);
        gl_ = nullptr;
    }
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }
    SDL_Quit();
}

void App::run() {
    while (running_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) running_ = false;
            if (e.type == SDL_DROPFILE && e.drop.file) {
                // Drag & drop: send the dropped file to the open direct chat.
                std::string path = e.drop.file;
                SDL_free(e.drop.file);
                if (selected_kind_ == ConversationKind::Direct) {
                    std::string err;
                    set_status(node_.send_file(selected_id_, path, &err)
                                   ? "File transfer started: " + path
                                   : "File: " + err);
                } else if (selected_kind_ == ConversationKind::Group) {
                    std::string err;
                    int n = node_.send_file_group(selected_id_, path, &err);
                    set_status(n > 0 ? "File sent to " + std::to_string(n) + " member(s)" : "File: " + err);
                } else {
                    set_status("Open a chat, then drop the file again");
                }
            }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.sym == SDLK_F11)
                    toggle_fullscreen();
                else if (e.key.keysym.sym == SDLK_ESCAPE) {
                    // Escape never quits the app; it only exits fullscreen.
                    if (fullscreen_) toggle_fullscreen();
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        refresh();
        demo_tick();
        draw_ui();

        ImGui::Render();
        int w = 0;
        int h = 0;
        SDL_GetWindowSize(window_, &w, &h);
        glViewport(0, 0, w, h);
        if (light_)
            glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
        else
            glClearColor(0.050f, 0.058f, 0.075f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window_);
        SDL_Delay(1);
    }
}

bool App::save_mic_test_metrics() {
    if (mic_test_file_.empty()) return false;
    const auto stats = voice_.mic_test_stats();
    std::ofstream meta(mic_test_file_ + ".json");
    if (!meta) {
        set_status("Mic test: cannot write JSON metrics");
        return false;
    }
    meta << "{\n"
         << "  \"wav\": \""
         << json_escape(std::filesystem::path(mic_test_file_).filename().string())
         << "\",\n"
         << "  \"duration_s\": 10,\n"
         << "  \"sample_rate\": 48000,\n"
         << "  \"channels\": 1,\n"
         << "  \"samples\": " << stats.samples << ",\n"
         << "  \"rms_dbfs\": " << std::fixed << std::setprecision(2) << stats.rms_dbfs
         << ",\n"
         << "  \"peak\": " << stats.peak << ",\n"
         << "  \"clipped_samples\": " << stats.clipped << "\n"
         << "}\n";
    return static_cast<bool>(meta);
}

void App::refresh() {
    uint64_t now = p2p::now_ms();
    if (now - last_refresh_ms_ < 150) return;
    bool first = last_refresh_ms_ == 0;
    last_refresh_ms_ = now;

    contacts_ = node_.contacts();
    contacts_.erase(std::remove_if(contacts_.begin(), contacts_.end(),
                                   [&](const p2p::Contact& c) { return node_.is_infrastructure(c.id); }),
                    contacts_.end());
    peers_ = node_.peers();
    groups_ = node_.groups();
    messages_ = node_.messages();
    stats_ = node_.stats();
    vstats_ = voice_.stats();
    auto next_call = node_.call_info();
    if (call_.state != next_call.state) {
        if (next_call.state == p2p::CallState::ActiveCall)
            set_status(next_call.is_group ? "Group call active" : "Call active");
        else if (next_call.state == p2p::CallState::Idle && call_.state != p2p::CallState::Idle) {
            set_status("Call ended");
            // The call is over (hung up here or by the peer): stop the microphone,
            // drop queued remote audio and forget the voice target.
            if (voice_.talking()) voice_.stop_talk();
            voice_.clear_peers();
            voice_active_by_ui_ = false;
            voice_call_id_ = 0;
            set_voice_target(p2p::NodeId{}, p2p::NodeId{});
        }
    }
    call_ = next_call;
    if (call_.state != p2p::CallState::Idle && mic_preview_ui_) {
        voice_.set_mic_preview(false, false);   // never loop the mic into a call
        mic_preview_ui_ = false;
    }
    nat_ = node_.nat_profile();
    relay_ = node_.relay();
    files_ = node_.file_transfers();

    if (mic_test_active_ && now >= mic_test_until_) {
        voice_.stop_mic_test();
        mic_test_active_ = false;
        if (save_mic_test_metrics())
            set_status("Microphone test saved: " + mic_test_file_);
    }

    if (pending_relay_ && now - pending_relay_at_ > 1400) {
        bool ready = false;
        for (const auto& p : peers_)
            if (p.id == pending_relay_id_ && p.handshake_done) ready = true;
        if (!ready && !relay_.empty()) {
            std::string err;
            if (node_.connect_via_relay(relay_, pending_relay_id_, &err))
                set_status("Direct path blocked; relay handshake sent");
            else
                set_status("Relay: " + err);
        }
        pending_relay_ = false;
    }

    for (const auto& line : node_.take_logs()) {
        log_lines_.push_back(line);
        if (log_lines_.size() > 500)
            log_lines_.erase(log_lines_.begin(), log_lines_.begin() + 80);
    }

    if (first) {
        for (const auto& m : messages_) {
            std::string key = m.peer.hex() + "|" + m.sender.hex() + "|" +
                              std::to_string(m.id) + "|" + (m.is_group ? "1" : "0");
            seen_messages_.insert(key);
        }
    } else {
        size_t before = seen_messages_.size();
        for (const auto& m : messages_) {
            std::string key = m.peer.hex() + "|" + m.sender.hex() + "|" +
                              std::to_string(m.id) + "|" + (m.is_group ? "1" : "0");
            if (seen_messages_.insert(key).second) {
                bool selected = m.peer == selected_id_;
                if (!m.outgoing && !selected) unread_[m.peer]++;
            }
        }
        (void)before;
    }

    if (selected_kind_ == ConversationKind::None) {
        if (!contacts_.empty())
            select_conversation(ConversationKind::Direct, contacts_.front().id);
        else if (!groups_.empty())
            select_conversation(ConversationKind::Group, groups_.front().id);
    }

    // Jump to the ringing conversation ONCE per call.  Doing it on every refresh made the chat
    // switch under the user's hands while they were typing, so a message meant for a group went to
    // a private chat (and the other way round) whenever a call offer arrived.
    update_ringtone(call_.state == p2p::CallState::IncomingRinging);
    if (call_.state != p2p::CallState::IncomingRinging) ringing_selected_call_ = 0;
    if (call_.state == p2p::CallState::IncomingRinging) {
        const p2p::NodeId& ring_from = call_.is_group ? call_.group : call_.peer;
        const uint64_t ring_key = (static_cast<uint64_t>(call_.call_id) << 32) |
                                  (static_cast<uint32_t>(ring_from.b[0]) << 24 |
                                   static_cast<uint32_t>(ring_from.b[1]) << 16 |
                                   static_cast<uint32_t>(ring_from.b[2]) << 8 | ring_from.b[3]);
        if (ring_key != ringing_selected_call_) {
            ringing_selected_call_ = ring_key;
            if (call_.is_group)
                select_conversation(ConversationKind::Group, call_.group);
            else
                select_conversation(ConversationKind::Direct, call_.peer);
        }
    }

    if (voice_.talking()) {
        double loss = 0.0;
        if (vstats_.rx_packets + vstats_.lost > 30)
            loss = 100.0 * static_cast<double>(vstats_.lost) /
                   static_cast<double>(vstats_.rx_packets + vstats_.lost);
        if (std::abs(loss - last_fec_loss_) > 0.4) {
            last_fec_loss_ = loss;
            voice_.set_packet_loss(loss);
        }
    }

    if (call_.state == p2p::CallState::ActiveCall) {
        p2p::NodeId peer = call_.is_group ? p2p::NodeId{} : call_.peer;
        p2p::NodeId group = call_.is_group ? call_.group : p2p::NodeId{};
        if (!voice_active_by_ui_ || voice_call_id_ != call_.call_id) {
            set_voice_target(peer, group);
            voice_call_id_ = call_.call_id;
            if (voice_ok_ && !voice_.talking()) {
                voice_.start_talk();
                voice_active_by_ui_ = true;
            }
        }
        if (call_.is_group) {
            p2p::GroupRoom g;
            if (node_.group(call_.group, g)) {
                bool mesh = g.members.size() <= 4;
                if (mesh) {
                    uint64_t now = p2p::now_ms();
                    for (const auto& member : g.members) {
                        if (member == node_.identity().id) continue;
                        p2p::Peer p;
                        if (!node_.get_peer(member, p) || !p.online(now, 20000)) continue;
                        if (!p.direct) {
                            mesh = false;
                            break;
                        }
                    }
                }
                if (mesh) {
                    voice_.set_topology("Full Mesh");
                } else {
                    p2p::NodeId hub = node_.group_hub(g.id);
                    voice_.set_topology("P2P-SFU via " + hub.short_hex(6));
                }
            }
        } else {
            voice_.set_topology("1-on-1 Direct");
        }
    } else if (call_.state == p2p::CallState::Idle && voice_active_by_ui_) {
        if (voice_.talking()) voice_.stop_talk();
        voice_active_by_ui_ = false;
        voice_call_id_ = 0;
        set_voice_target(p2p::NodeId{}, p2p::NodeId{});
    }

    if (now - status_ms_ > 7000 && status_ != "Ready") {
        status_ = "Ready";
        status_ms_ = now;
    }
}

void App::demo_tick() {
    if (!demo_) return;
    uint64_t t = p2p::now_ms() - demo_t0_;

    if (!demo_select_ && t > 1200 && !contacts_.empty()) {
        select_conversation(ConversationKind::Direct, contacts_.front().id);
        demo_select_ = true;
    }
    if (!demo_inspector_ && t > 4200) {
        inspector_open_ = true;
        inspector_tab_ = 1;
        demo_inspector_ = true;
    }
    if (!demo_msg1_ && t > 2600 && !selected_id_.empty()) {
        send_text("Hi! This is \"" + node_.identity().alias +
                  "\". The session is protected by an ephemeral 4-DH handshake.");
        demo_msg1_ = true;
    }
    if (!demo_msg2_ && t > 6200 && !selected_id_.empty()) {
        p2p::Peer p;
        uint32_t rtt = 0;
        if (node_.get_peer(selected_id_, p)) rtt = p.rtt_ms;
        send_text("RTT " + std::to_string(rtt) +
                  " ms. Opus 32 kbit/s voice and the encrypted vault are ready.");
        demo_msg2_ = true;
    }
}

std::string App::peer_alias(const p2p::NodeId& id) const {
    for (const auto& c : contacts_)
        if (c.id == id) return c.alias.empty() ? id.short_hex(8) : c.alias;
    for (const auto& p : peers_)
        if (p.id == id) return p.alias.empty() ? id.short_hex(8) : p.alias;
    for (const auto& g : groups_)
        if (g.id == id) return g.name;
    return id.short_hex(8);
}

std::string App::conversation_name() const {
    if (selected_kind_ == ConversationKind::None) return "Select a conversation";
    return peer_alias(selected_id_);
}

std::string App::conversation_subtitle() const {
    if (selected_kind_ == ConversationKind::None) return "";
    if (selected_kind_ == ConversationKind::Group) {
        for (const auto& g : groups_) {
            if (g.id == selected_id_)
                return std::to_string(g.members.size()) + " members  ·  group " +
                       g.id.short_hex(8);
        }
        return "Group";
    }
    bool online = is_online(selected_id_);
    p2p::Peer p;
    std::string extra;
    if (node_.get_peer(selected_id_, p)) {
        extra = std::string("  ·  ") + p2p::nat_name(p.nat) + "  ·  " +
                std::to_string(p.rtt_ms) + " ms";
    }
    return std::string(online ? "online" : "offline") + "  ·  " +
           selected_id_.short_hex(8) + extra;
}

std::string App::format_time(uint64_t unix_ms) const {
    std::time_t t = static_cast<std::time_t>(unix_ms / 1000);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    return buf;
}

std::string App::format_duration(uint64_t ms) const {
    uint64_t s = ms / 1000;
    char buf[32];
    if (s >= 3600)
        std::snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
                      static_cast<unsigned long long>(s / 3600),
                      static_cast<unsigned long long>((s / 60) % 60),
                      static_cast<unsigned long long>(s % 60));
    else
        std::snprintf(buf, sizeof(buf), "%02llu:%02llu",
                      static_cast<unsigned long long>(s / 60),
                      static_cast<unsigned long long>(s % 60));
    return buf;
}

std::string App::message_preview(const p2p::NodeId& conversation) const {
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it) {
        if (it->peer != conversation) continue;
        std::string text = it->text;
        if (it->outgoing) text = "You: " + text;
        else if (it->is_group) text = peer_alias(it->sender) + ": " + text;
        return truncate(text, 38);
    }
    return "No messages yet";
}

int App::unread_for(const p2p::NodeId& id) const {
    auto it = unread_.find(id);
    return it == unread_.end() ? 0 : it->second;
}

bool App::is_online(const p2p::NodeId& id) const {
    for (const auto& p : peers_)
        if (p.id == id) return p.online(p2p::now_ms(), 20000) && p.handshake_done;
    return false;
}

bool App::is_group_selected(const p2p::NodeId& id) const {
    for (const auto& g : groups_)
        if (g.id == id) return true;
    return false;
}

void App::set_status(const std::string& text) {
    status_ = text;
    status_ms_ = p2p::now_ms();
}

void App::copy_text(const std::string& text, const std::string& what) {
    ImGui::SetClipboardText(text.c_str());
    set_status(what + " copied to clipboard");
}

void App::select_conversation(ConversationKind kind, const p2p::NodeId& id) {
    // Keep the unsent text per conversation: a draft written for one chat must never be sent to
    // another one just because the open chat changed (click, incoming call, removed contact).
    if (selected_kind_ != ConversationKind::None &&
        (kind != selected_kind_ || id != selected_id_)) {
        auto key = [](ConversationKind k, const p2p::NodeId& i) {
            return std::string(k == ConversationKind::Group ? "g" : "d") + i.hex();
        };
        if (chat_input_[0])
            drafts_[key(selected_kind_, selected_id_)] = chat_input_;
        else
            drafts_.erase(key(selected_kind_, selected_id_));
        auto it = drafts_.find(key(kind, id));
        std::snprintf(chat_input_, sizeof(chat_input_), "%s",
                      it != drafts_.end() ? it->second.c_str() : "");
    }
    selected_kind_ = kind;
    selected_id_ = id;
    unread_[id] = 0;
    auto_scroll_ = true;
}

void App::set_voice_target(const p2p::NodeId& peer, const p2p::NodeId& group) {
    std::lock_guard<std::mutex> lk(voice_target_mtx_);
    voice_peer_ = peer;
    voice_group_ = group;
}

std::pair<p2p::NodeId, p2p::NodeId> App::voice_target() const {
    std::lock_guard<std::mutex> lk(voice_target_mtx_);
    return {voice_peer_, voice_group_};
}

void App::update_ringtone(bool ringing) {
    constexpr int kRate = 48000;
    if (!ringing) {
        if (ring_dev_) {
            SDL_ClearQueuedAudio(ring_dev_);
            SDL_CloseAudioDevice(ring_dev_);
            ring_dev_ = 0;
        }
        return;
    }
    if (!ring_dev_) {
        if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return;
        SDL_AudioSpec want{};
        SDL_AudioSpec have{};
        want.freq = kRate;
        want.format = AUDIO_S16SYS;
        want.channels = 1;
        want.samples = 1024;
        want.callback = nullptr;  // queue mode: we push PCM from the GUI thread
        const std::string out = voice_.playback_name();  // the output chosen in the Audio tab
        if (!out.empty()) ring_dev_ = SDL_OpenAudioDevice(out.c_str(), 0, &want, &have, 0);
        if (!ring_dev_) ring_dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!ring_dev_) return;
        SDL_PauseAudioDevice(ring_dev_, 0);
    }
    if (ring_pcm_.empty()) {
        // Classic double ring: two 0.5 s bursts of 440+480 Hz, 0.2 s apart, then 1.8 s of silence.
        const double two_pi = 6.283185307179586;
        ring_pcm_.assign(static_cast<size_t>(kRate) * 3, 0);
        for (int burst = 0; burst < 2; ++burst) {
            const size_t start = static_cast<size_t>(burst) * static_cast<size_t>(kRate * 7 / 10);
            const size_t len = static_cast<size_t>(kRate / 2);
            for (size_t i = 0; i < len; ++i) {
                const double t = static_cast<double>(i) / kRate;
                double env = 1.0;
                if (i < 480) env = static_cast<double>(i) / 480.0;                   // 10 ms fade in
                if (len - i < 480) env = static_cast<double>(len - i) / 480.0;      // 10 ms fade out
                const double v = 0.5 * (std::sin(two_pi * 440.0 * t) + std::sin(two_pi * 480.0 * t));
                ring_pcm_[start + i] = static_cast<int16_t>(v * env * 9000.0);
            }
        }
    }
    // Keep about one cycle queued while the call is ringing.
    if (SDL_GetQueuedAudioSize(ring_dev_) < static_cast<Uint32>(kRate * 2))
        SDL_QueueAudio(ring_dev_, ring_pcm_.data(),
                       static_cast<Uint32>(ring_pcm_.size() * sizeof(int16_t)));
}

bool App::send_text(const std::string& text) {
    if (text.empty()) return false;
    if (selected_kind_ == ConversationKind::None || selected_id_.empty()) {
        set_status("Select a conversation first");
        return false;
    }
    std::string err;
    bool ok = false;
    if (selected_kind_ == ConversationKind::Group) {
        ok = node_.send_group_chat(selected_id_, text, &err);
    } else {
        ok = node_.send_chat(selected_id_, text, &err);
    }
    if (ok) {
        set_status(err == "queued (offline)" ? "Queued in encrypted offline inbox"
                                             : "Message sent");
        auto_scroll_ = true;
    } else {
        set_status("Error: " + err);
    }
    return ok;
}

void App::start_selected_call() {
    if (selected_kind_ == ConversationKind::None) return;
    std::string err;
    bool ok = selected_kind_ == ConversationKind::Group
                  ? node_.start_group_call(selected_id_, &err)
                  : node_.start_call(selected_id_, &err);
    if (!ok) {
        set_status("Call: " + err);
    } else {
        set_status(selected_kind_ == ConversationKind::Group ? "Group call ringing"
                                                             : "Call ringing");
    }
}

void App::end_call() {
    node_.end_call();
    if (voice_.talking()) voice_.stop_talk();
    voice_.clear_peers();
    voice_active_by_ui_ = false;
    voice_call_id_ = 0;
    set_voice_target(p2p::NodeId{}, p2p::NodeId{});
    call_ = node_.call_info();
    set_status("Call ended");
}

void App::draw_avatar(const p2p::NodeId& id, const std::string& alias, float radius,
                      bool online) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 c(p.x + radius, p.y + radius);
    float hue = (static_cast<float>(id.b[0]) * 360.0f) / 255.0f;
    ImU32 fill = ImGui::GetColorU32(ImColor::HSV(hue / 360.0f, 0.48f, light_ ? 0.74f : 0.80f).Value);
    dl->AddCircleFilled(c, radius, fill, 32);
    std::string ini = initial_of(alias);
    ImVec2 ts = ImGui::CalcTextSize(ini.c_str());
    dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255),
                ini.c_str());
    if (online) {
        dl->AddCircleFilled(ImVec2(c.x + radius * 0.72f, c.y + radius * 0.72f),
                            radius * 0.24f, IM_COL32(40, 190, 95, 255), 12);
        dl->AddCircle(ImVec2(c.x + radius * 0.72f, c.y + radius * 0.72f), radius * 0.24f,
                      ImGui::GetColorU32(ImGuiCol_WindowBg), 12, 1.5f);
    }
    ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f));
}

void App::draw_title_bar() {
    ImGuiIO& io = ImGui::GetIO();
    const float title_top = ImGui::GetCursorScreenPos().y;
    const float title_h = 28.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));

    ImGui::TextColored(c_accent, "P2P");
    ImGui::SameLine(0.0f, 3.0f);
    ImGui::Text("Messenger");
    ImGui::SameLine(0.0f, 10.0f);
    ImGui::TextDisabled("· %s · UDP %u · %s", node_.identity().alias.c_str(), node_.port(),
                        p2p::human_uptime(p2p::now_ms() - started_ms_).c_str());

    const float btn_w = 30.0f;
    const float right = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x -
                        5.0f * btn_w - 4.0f * 6.0f;
    ImGui::SameLine(right);

    auto control = [&](const char* label, const char* tip) -> bool {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 2.0f));
        bool pressed = ImGui::Button(label, ImVec2(btn_w, 22.0f));
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return pressed;
    };

    if (control("_", "Minimize")) SDL_MinimizeWindow(window_);
    ImGui::SameLine();
    if (control(maximized_ ? "[]" : "[]", maximized_ ? "Restore" : "Maximize"))
        toggle_maximize();
    ImGui::SameLine();
    if (control("FS", fullscreen_ ? "Leave fullscreen (F11)" : "Fullscreen (F11)"))
        toggle_fullscreen();
    ImGui::SameLine();
    if (control(light_ ? "L" : "D", light_ ? "Dark theme" : "Light theme"))
        apply_theme(!light_);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, c_red);
    if (control("X", "Close")) running_ = false;
    ImGui::PopStyleColor();

    const bool in_title = io.MousePos.y >= title_top && io.MousePos.y <= title_top + title_h;
    if (in_title && !ImGui::IsAnyItemHovered() && !fullscreen_) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            title_dragging_ = false;
            toggle_maximize();
        } else if (!maximized_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            int wx = 0;
            int wy = 0;
            SDL_GetWindowPosition(window_, &wx, &wy);
            drag_win_x_ = wx;
            drag_win_y_ = wy;
            drag_mouse_x_ = static_cast<float>(wx) + io.MousePos.x;
            drag_mouse_y_ = static_cast<float>(wy) + io.MousePos.y;
            title_dragging_ = true;
        }
    }
    if (title_dragging_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            title_dragging_ = false;
        } else if (!maximized_ && !fullscreen_) {
            int wx = 0;
            int wy = 0;
            SDL_GetWindowPosition(window_, &wx, &wy);
            float cur_x = static_cast<float>(wx) + io.MousePos.x;
            float cur_y = static_cast<float>(wy) + io.MousePos.y;
            SDL_SetWindowPosition(window_,
                                  drag_win_x_ + static_cast<int>(cur_x - drag_mouse_x_),
                                  drag_win_y_ + static_cast<int>(cur_y - drag_mouse_y_));
        }
    }

    ImGui::PopStyleVar();
    ImGui::Separator();
}

void App::draw_sidebar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c_sidebar);
    ImGui::BeginChild("sidebar", ImVec2(0, 0), ImGuiChildFlags_Borders);

    ImGui::PushStyleColor(ImGuiCol_Text, c_muted);
    ImGui::TextDisabled("WORKSPACE");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    draw_avatar(node_.identity().id, node_.identity().alias, 21.0f, true);
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%s", node_.identity().alias.c_str());
    ImGui::TextDisabled("%s", node_.identity().id.short_hex(12).c_str());
    if (ImGui::SmallButton("Edit name")) {
        std::snprintf(profile_input_, sizeof(profile_input_), "%s",
                      node_.identity().alias.c_str());
        show_profile_ = true;
    }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", node_.identity().id.hex().c_str());

    if (ImGui::SmallButton("Copy link")) copy_text(node_.share_uri(), "Contact link");
    ImGui::SameLine();
    if (ImGui::SmallButton("Add contact")) show_add_contact_ = true;

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##search", "Search conversations", search_input_,
                             sizeof(search_input_));
    ImGui::Spacing();

    float list_h = ImGui::GetContentRegionAvail().y - 42.0f;
    if (list_h < 80.0f) list_h = 80.0f;
    ImGui::BeginChild("conversation_list", ImVec2(0, list_h), ImGuiChildFlags_None);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f));
    std::string needle = lower_copy(search_input_);

    ImGui::PushStyleColor(ImGuiCol_Text, c_muted);
    ImGui::Text("DIRECT MESSAGES  %zu", contacts_.size());
    ImGui::PopStyleColor();
    int visible_contacts = 0;
    for (const auto& c : contacts_) {
        std::string name = c.alias.empty() ? c.id.short_hex(8) : c.alias;
        if (!needle.empty() && lower_copy(name).find(needle) == std::string::npos &&
            c.id.hex().find(needle) == std::string::npos)
            continue;
        ++visible_contacts;
        bool sel = selected_kind_ == ConversationKind::Direct && selected_id_ == c.id;
        ImGui::PushID(c.id.hex().c_str());
        ImVec2 row = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("##dmrow", ImVec2(-1.0f, 52.0f));
        bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 bg = sel ? ImGui::GetColorU32(ImGuiCol_Header)
                       : (hovered ? ImGui::GetColorU32(ImGuiCol_HeaderHovered)
                                  : IM_COL32(0, 0, 0, 0));
        if (bg & IM_COL32_A_MASK) dl->AddRectFilled(row, ImVec2(row.x + ImGui::GetContentRegionAvail().x,
                                                               row.y + 52.0f), bg, 10.0f);
        if (sel) dl->AddRectFilled(ImVec2(row.x, row.y + 12.0f), ImVec2(row.x + 3.5f, row.y + 40.0f),
                                   ImGui::GetColorU32(c_accent), 2.0f);
        ImGui::SetCursorScreenPos(ImVec2(row.x + 8.0f, row.y + 8.0f));
        draw_avatar(c.id, name, 17.0f, c.online);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("%s", truncate(name, 19).c_str());
        ImGui::TextDisabled("%s", truncate(message_preview(c.id), 18).c_str());
        ImGui::EndGroup();
        int unread = unread_for(c.id);
        if (unread > 0) {
            char b[16];
            std::snprintf(b, sizeof(b), "%d", unread);
            ImVec2 bs = ImGui::CalcTextSize(b);
            float w = std::max(18.0f, bs.x + 8.0f);
            ImVec2 p2(row.x + ImGui::GetContentRegionAvail().x - w - 8.0f, row.y + 17.0f);
            dl->AddRectFilled(p2, ImVec2(p2.x + w, p2.y + 18.0f),
                              IM_COL32(215, 55, 70, 255), 9.0f);
            dl->AddText(ImVec2(p2.x + (w - bs.x) * 0.5f, p2.y + 2.0f),
                        IM_COL32(255, 255, 255, 255), b);
        }
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + 54.0f));
        if (clicked) select_conversation(ConversationKind::Direct, c.id);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            ImGui::OpenPopup("contact_menu");
        if (ImGui::BeginPopupContextItem("contact_menu")) {
            if (ImGui::MenuItem("Remove contact")) {
                std::string err;
                if (node_.remove_contact(c.id, &err)) {
                    if (selected_kind_ == ConversationKind::Direct && selected_id_ == c.id) {
                        selected_kind_ = ConversationKind::None;
                        selected_id_ = p2p::NodeId{};
                    }
                    set_status("Contact removed");
                } else {
                    set_status("Remove failed: " + err);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (visible_contacts == 0)
        ImGui::TextDisabled("  No contacts. Add an invite URI.");

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, c_muted);
    ImGui::Text("GROUPS  %zu", groups_.size());
    ImGui::PopStyleColor();
    int visible_groups = 0;
    for (const auto& g : groups_) {
        if (!needle.empty() && lower_copy(g.name).find(needle) == std::string::npos)
            continue;
        ++visible_groups;
        bool sel = selected_kind_ == ConversationKind::Group && selected_id_ == g.id;
        ImGui::PushID(g.id.hex().c_str());
        ImVec2 row = ImGui::GetCursorScreenPos();
        bool clicked = ImGui::InvisibleButton("##grouprow", ImVec2(-1.0f, 52.0f));
        bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImU32 bg = sel ? ImGui::GetColorU32(ImGuiCol_Header)
                       : (hovered ? ImGui::GetColorU32(ImGuiCol_HeaderHovered)
                                  : IM_COL32(0, 0, 0, 0));
        if (bg & IM_COL32_A_MASK) dl->AddRectFilled(row, ImVec2(row.x + ImGui::GetContentRegionAvail().x,
                                                               row.y + 52.0f), bg, 10.0f);
        if (sel) dl->AddRectFilled(ImVec2(row.x, row.y + 12.0f), ImVec2(row.x + 3.5f, row.y + 40.0f),
                                   ImGui::GetColorU32(c_accent), 2.0f);
        ImVec2 ap(row.x + 8.0f, row.y + 8.0f);
        dl->AddRectFilled(ap, ImVec2(ap.x + 34.0f, ap.y + 34.0f),
                          ImGui::GetColorU32(ImGuiCol_Button), 9.0f);
        dl->AddText(ImVec2(ap.x + 12.0f, ap.y + 7.0f), ImGui::GetColorU32(ImGuiCol_Text), "#");
        ImGui::SetCursorScreenPos(ImVec2(row.x + 50.0f, row.y + 9.0f));
        ImGui::BeginGroup();
        ImGui::Text("%s", truncate(g.name, 19).c_str());
        ImGui::TextDisabled("%zu members  ·  %s", g.members.size(),
                            truncate(message_preview(g.id), 10).c_str());
        ImGui::EndGroup();
        int unread = unread_for(g.id);
        if (unread > 0) {
            char b[16];
            std::snprintf(b, sizeof(b), "%d", unread);
            ImVec2 bs = ImGui::CalcTextSize(b);
            float w = std::max(18.0f, bs.x + 8.0f);
            ImVec2 p2(row.x + ImGui::GetContentRegionAvail().x - w - 8.0f, row.y + 17.0f);
            dl->AddRectFilled(p2, ImVec2(p2.x + w, p2.y + 18.0f),
                              IM_COL32(215, 55, 70, 255), 9.0f);
            dl->AddText(ImVec2(p2.x + (w - bs.x) * 0.5f, p2.y + 2.0f),
                        IM_COL32(255, 255, 255, 255), b);
        }
        ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + 54.0f));
        if (clicked) select_conversation(ConversationKind::Group, g.id);
        ImGui::PopID();
    }
    if (visible_groups == 0) ImGui::TextDisabled("  No groups yet.");
    ImGui::PopStyleVar();
    ImGui::EndChild();

    if (ImGui::Button("New group", ImVec2(-1.0f, 26.0f))) {
        group_selected_.clear();
        show_new_group_ = true;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::draw_call_panel() {
    if (call_.state == p2p::CallState::Idle) return;

    int pos = 0;
    for (const auto& c : contacts_)
        if (c.id == call_.peer) ++pos;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, c_panel);
    float panel_h = 84.0f;
    if (call_.state == p2p::CallState::IncomingRinging) panel_h = 104.0f;
    else if (call_.state == p2p::CallState::ActiveCall) panel_h = 110.0f;
    ImGui::BeginChild("call_panel", ImVec2(0, panel_h), ImGuiChildFlags_Borders);
    ImGui::PushStyleColor(ImGuiCol_Text, c_accent);
    if (call_.state == p2p::CallState::IncomingRinging) {
        ImGui::Text("Incoming %s call", call_.is_group ? "group" : "secure");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("from %s", call_.is_group ? peer_alias(call_.group).c_str()
                                                       : peer_alias(call_.peer).c_str());
        ImGui::TextDisabled("SAS-protected signalling · Opus media");
        if (ImGui::Button("Accept", ImVec2(100.0f, 26.0f))) {
            std::string err;
            if (!node_.answer_call(true, &err))
                set_status("Accept failed: " + err);
            else
                set_status("Call accepted");
        }
        ImGui::SameLine();
        if (ImGui::Button("Decline", ImVec2(100.0f, 26.0f))) {
            std::string err;
            node_.answer_call(false, &err);
            set_status("Call declined");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Ringtone is visual only");
    } else {
        if (call_.state == p2p::CallState::OutgoingRinging) {
            ImGui::Text("Calling %s...", call_.is_group ? peer_alias(call_.group).c_str()
                                                         : peer_alias(call_.peer).c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90.0f, 24.0f))) end_call();
            ImGui::TextDisabled("Waiting for the peer to accept · encrypted signalling");
        } else {
            uint64_t dur = p2p::now_ms() - call_.started_ms;
            ImGui::Text("In call");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextDisabled("%s  ·  %s", format_duration(dur).c_str(),
                                call_.is_group ? peer_alias(call_.group).c_str()
                                               : peer_alias(call_.peer).c_str());
            ImGui::TextDisabled("%s  ·  %zu participants  ·  %s", vstats_.topology.c_str(),
                                call_.participants.size(), vstats_.fec_enabled ? "FEC on" : "FEC off");
            bool muted = voice_.muted();
            bool deaf = voice_.deafened();
            if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
            if (ImGui::Button(muted ? "Unmute" : "Mute", ImVec2(80.0f, 24.0f))) {
                voice_.set_muted(!muted);
                set_status(muted ? "Microphone unmuted" : "Microphone muted");
            }
            if (muted) ImGui::PopStyleColor();
            ImGui::SameLine();
            if (deaf) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.20f, 0.20f, 1.0f));
            if (ImGui::Button(deaf ? "Undeafen" : "Deafen", ImVec2(86.0f, 24.0f))) {
                voice_.set_deafened(!deaf);
                set_status(deaf ? "Audio restored" : "Output muted");
            }
            if (deaf) ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("End", ImVec2(54.0f, 24.0f))) end_call();

            double loss = 0.0;
            if (vstats_.rx_packets + vstats_.lost > 20)
                loss = 100.0 * static_cast<double>(vstats_.lost) /
                       static_cast<double>(vstats_.rx_packets + vstats_.lost);
            double delay = 40.0 + vstats_.jitter_ms;
            double r = r_factor(delay, loss);
            ImGui::SameLine();
            ImGui::TextColored(r > 70 ? c_green : (r > 50 ? c_amber : c_red),
                               "MOS %.1f · R%.0f · J%.1f · L%.1f%%",
                               mos_from_r(r), r, vstats_.jitter_ms, loss);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("MOS %.2f · R %.1f · jitter %.1f ms · frame loss %.1f%%",
                                  mos_from_r(r), r, vstats_.jitter_ms, loss);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::draw_message(const p2p::ChatMsg& msg) {
    ImGuiIO& io = ImGui::GetIO();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float avail = ImGui::GetContentRegionAvail().x;
    float max_bubble = std::min(540.0f, std::max(220.0f, avail * 0.68f));
    float wrap = max_bubble - 28.0f;
    ImVec2 ts = ImGui::CalcTextSize(msg.text.c_str(), nullptr, false, wrap);
    if (ts.x < 24.0f) ts.x = 24.0f;
    float bubble_w = std::min(max_bubble, ts.x + 28.0f);
    float bubble_h = ts.y + 22.0f;
    bool group_in = msg.is_group && !msg.outgoing;
    float label_h = group_in ? 16.0f : 0.0f;
    float meta_h = 16.0f;
    float total_h = label_h + bubble_h + meta_h + 6.0f;

    ImVec2 start = ImGui::GetCursorScreenPos();
    float x = msg.outgoing ? start.x + avail - bubble_w - 10.0f : start.x + 10.0f;
    float y = start.y + 3.0f;
    if (group_in) {
        dl->AddText(ImVec2(start.x + 14.0f, y), ImGui::GetColorU32(c_accent),
                    peer_alias(msg.sender).c_str());
        y += label_h;
    }
    ImU32 bg = ImGui::GetColorU32(msg.outgoing ? c_bubble_out : c_bubble_in);
    ImU32 fg = msg.outgoing ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + bubble_w, y + bubble_h), bg, 16.0f);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(x + 14.0f, y + 11.0f), fg,
                msg.text.c_str(), nullptr, wrap);

    std::string meta = msg.ts_ms ? format_time(msg.ts_ms) : "--:--";
    if (msg.outgoing) {
        if (msg.queued) meta += "  ·  queued";
        else if (msg.delivered) meta += msg.is_group ? "  ·  broadcast" : "  ·  delivered";
        else meta += "  ·  sent";
    } else if (!msg.is_group) {
        meta += "  ·  E2EE";
    }
    ImU32 meta_col = msg.queued ? ImGui::GetColorU32(c_amber)
                                : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImVec2 ms = ImGui::CalcTextSize(meta.c_str());
    float mx = msg.outgoing ? x + bubble_w - ms.x : x;
    dl->AddText(ImVec2(mx, y + bubble_h + 2.0f), meta_col, meta.c_str());

    ImGui::Dummy(ImVec2(1.0f, total_h));
    (void)io;
}

void App::draw_file_card(const p2p::FileTransferInfo& f, int recipients, int delivered) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float avail = ImGui::GetContentRegionAvail().x;
    float w = std::min(400.0f, std::max(250.0f, avail * 0.62f));
    const float h = 70.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    float x = f.outgoing ? p.x + avail - w - 10.0f : p.x + 10.0f;
    float y = p.y + 4.0f;
    const bool active = !f.complete && !f.failed;
    ImGui::PushID(static_cast<int>(f.id & 0x7fffffff));
    ImGui::SetCursorScreenPos(ImVec2(x, y));
    bool clicked = ImGui::InvisibleButton("##filecard", ImVec2(w, h));
    bool hovered = ImGui::IsItemHovered();
    ImU32 bg = ImGui::GetColorU32(f.outgoing ? c_bubble_out : c_bubble_in);
    ImU32 fg = f.outgoing ? IM_COL32(255, 255, 255, 255) : ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 sub_col = f.outgoing ? IM_COL32(255, 255, 255, 190)
                               : ImGui::GetColorU32(ImGuiCol_TextDisabled);
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), bg, 14.0f);
    if (hovered && f.complete)
        dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), ImGui::GetColorU32(c_accent), 14.0f, 0, 2.0f);
    // icon tile with the file extension
    std::string ext;
    {
        size_t dot = f.name.find_last_of('.');
        if (dot != std::string::npos && f.name.size() - dot <= 6) ext = f.name.substr(dot + 1);
        for (char& c : ext) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (ext.empty()) ext = "FILE";
    }
    ImU32 tile = f.failed ? ImGui::GetColorU32(c_red)
                          : (f.complete ? ImGui::GetColorU32(c_green) : ImGui::GetColorU32(c_amber));
    dl->AddRectFilled(ImVec2(x + 12, y + 12), ImVec2(x + 56, y + 56), tile, 10.0f);
    ImVec2 es = ImGui::CalcTextSize(ext.c_str());
    dl->AddText(ImVec2(x + 34 - es.x * 0.5f, y + 34 - es.y * 0.5f), IM_COL32(255, 255, 255, 255),
                ext.c_str());
    float tx = x + 68.0f, tw = w - 80.0f;
    std::string nm = fit_text(f.name, tw);
    dl->AddText(ImVec2(tx, y + 10.0f), fg, nm.c_str());
    std::string line = human_size(f.size);
    if (f.failed) line += "  \xC2\xB7  " + (f.error.empty() ? std::string("failed") : f.error);
    else if (f.complete && f.outgoing && !f.error.empty()) line += "  \xC2\xB7  " + f.error;
    else if (f.complete) line += f.outgoing ? "  \xC2\xB7  sent \xE2\x9C\x93" : "  \xC2\xB7  click to open";
    else line += "  \xC2\xB7  " + std::to_string(f.size ? static_cast<int>(100 * std::min<uint64_t>(f.transferred, f.size) / f.size) : 0) + "%";
    if (f.outgoing && !f.group.empty()) {
        // one card for the whole group send: "-> 3 members" / "-> 2/3 members" while in progress
        if (recipients > 1)
            line += "  \xC2\xB7  \xE2\x86\x92 " + (delivered == recipients
                                                       ? std::to_string(recipients)
                                                       : std::to_string(delivered) + "/" +
                                                             std::to_string(recipients)) +
                    " members";
        else
            line += "  \xC2\xB7  \xE2\x86\x92 " + peer_alias(f.peer);
    } else if (!f.outgoing && !f.group.empty()) {
        line += "  \xC2\xB7  from " + peer_alias(f.peer);
    }
    dl->AddText(ImVec2(tx, y + 32.0f), sub_col, fit_text(line, tw).c_str());
    if (active) {
        float frac = f.size ? static_cast<float>(std::min<uint64_t>(f.transferred, f.size)) /
                                  static_cast<float>(f.size)
                            : 0.0f;
        dl->AddRectFilled(ImVec2(tx, y + 54.0f), ImVec2(tx + tw, y + 59.0f),
                          IM_COL32(255, 255, 255, f.outgoing ? 60 : 40), 3.0f);
        dl->AddRectFilled(ImVec2(tx, y + 54.0f), ImVec2(tx + tw * frac, y + 59.0f),
                          ImGui::GetColorU32(c_accent), 3.0f);
    }
    if (clicked && f.complete) {
        if (!f.outgoing && risky_file_type(f.name)) {
            reveal_path(f.path);
            set_status("Executable file type: not opened automatically, shown in its folder");
        } else if (!open_path(f.path)) {
            set_status("Cannot open " + f.path);
        }
    }
    if (hovered && f.complete) ImGui::SetTooltip("%s", f.path.c_str());
    if (ImGui::BeginPopupContextItem("##filectx")) {
        if (f.complete && ImGui::MenuItem("Open", nullptr, false, f.outgoing || !risky_file_type(f.name)))
            open_path(f.path);
        if (f.complete && ImGui::MenuItem("Show in folder")) reveal_path(f.path);
        if (ImGui::MenuItem("Copy path")) copy_text(f.path, "Path");
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, y + h + 6.0f));
    ImGui::Dummy(ImVec2(1.0f, 22.0f));
}

void App::draw_conversation() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_WindowBg));
    ImGui::BeginChild("conversation", ImVec2(0, 0), ImGuiChildFlags_Borders);

    if (selected_kind_ == ConversationKind::None) {
        ImGui::Dummy(ImVec2(0.0f, 80.0f));
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 130.0f);
        ImGui::TextColored(c_accent, "P2P Messenger");
        ImGui::SetCursorPosX(ImGui::GetWindowWidth() * 0.5f - 210.0f);
        ImGui::TextWrapped("Select a contact or create a group. Every direct session is "
                           "authenticated with an ephemeral 4-DH handshake, verified by SAS, "
                           "and protected by a symmetric ratchet.");
        ImGui::Dummy(ImVec2(0.0f, 20.0f));
        if (ImGui::Button("Add contact")) show_add_contact_ = true;
        ImGui::SameLine();
        if (ImGui::Button("New group")) {
            group_selected_.clear();
            show_new_group_ = true;
        }
        ImGui::Dummy(ImVec2(0.0f, 30.0f));
        ImGui::TextDisabled("Your invite URI");
        ImGui::PushStyleColor(ImGuiCol_Text, c_accent);
        ImGui::TextWrapped("%s", node_.share_uri().c_str());
        ImGui::PopStyleColor();
        if (ImGui::Button("Copy my link")) copy_text(node_.share_uri(), "Contact link");
        ImGui::EndChild();
        ImGui::PopStyleColor();
        return;
    }

    // Conversation header
    ImGui::BeginChild("conv_header", ImVec2(0, 64.0f), ImGuiChildFlags_None);
    if (selected_kind_ == ConversationKind::Group) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, ImVec2(p.x + 36.0f, p.y + 36.0f),
                          ImGui::GetColorU32(c_accent), 10.0f);
        dl->AddText(ImVec2(p.x + 13.0f, p.y + 8.0f), IM_COL32(255, 255, 255, 255), "#");
        ImGui::Dummy(ImVec2(36.0f, 36.0f));
    } else {
        draw_avatar(selected_id_, conversation_name(), 18.0f, is_online(selected_id_));
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Text("%s", conversation_name().c_str());
    ImGui::TextDisabled("%s", conversation_subtitle().c_str());
    ImGui::EndGroup();

    float btns = 88.0f + 52.0f + 2.0f * ImGui::GetStyle().ItemSpacing.x;
    if (selected_kind_ == ConversationKind::Group) btns += 52.0f + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 120.0f, ImGui::GetWindowWidth() - btns - 18.0f));
    bool in_call = call_.state != p2p::CallState::Idle;
    if (in_call) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.20f, 1.0f));
        if (ImGui::Button("End call", ImVec2(88.0f, 26.0f))) end_call();
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, c_accent);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (ImGui::Button(selected_kind_ == ConversationKind::Group ? "Group call" : "Call",
                          ImVec2(88.0f, 26.0f)))
            start_selected_call();
        ImGui::PopStyleColor(2);
    }
    ImGui::SameLine();
    if (selected_kind_ == ConversationKind::Group) {
        if (ImGui::Button("Sync", ImVec2(52.0f, 26.0f))) {
            if (node_.sync_group(selected_id_)) set_status("Group history sync requested");
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Info", ImVec2(52.0f, 26.0f))) inspector_open_ = !inspector_open_;
    ImGui::EndChild();
    ImGui::Separator();

    draw_call_panel();

    float input_h = 56.0f;
    ImGui::BeginChild("message_scroll", ImVec2(0, -input_h), ImGuiChildFlags_None);
    float scroll = ImGui::GetScrollY();
    float max_scroll = ImGui::GetScrollMaxY();
    bool at_bottom = scroll >= max_scroll - 32.0f;

    struct Entry {
        uint64_t ts;
        const p2p::ChatMsg* m;
        const p2p::FileTransferInfo* f;
        uint64_t batch = 0;  // outgoing group send: one card for all recipients
    };
    // A group send is N pairwise transfers; the chat shows ONE card for it.
    struct Agg {
        p2p::FileTransferInfo info;
        int total = 0, done = 0, failed = 0;
        uint64_t sum = 0;
    };
    std::map<uint64_t, Agg> batches;
    std::vector<Entry> visible;
    for (const auto& m : messages_)
        if (m.peer == selected_id_ && m.is_group == (selected_kind_ == ConversationKind::Group))
            visible.push_back({m.ts_ms, &m, nullptr});
    for (const auto& f : files_) {
        // Files that belong to a group are shown in that group only, never in the private chat
        // with the person who happened to send or receive them.
        bool group_known = false;
        if (!f.group.empty())
            for (const auto& g : groups_)
                if (g.id == f.group) group_known = true;
        bool mine = selected_kind_ == ConversationKind::Group
                        ? (f.group == selected_id_)
                        : (f.peer == selected_id_ && (f.group.empty() || !group_known));
        if (!mine) continue;
        if (f.outgoing && f.group_batch != 0) {
            auto it = batches.find(f.group_batch);
            if (it == batches.end()) {
                Agg a;
                a.info = f;
                it = batches.emplace(f.group_batch, a).first;
                visible.push_back({f.unix_ms, nullptr, nullptr, f.group_batch});
            }
            Agg& a = it->second;
            ++a.total;
            a.sum += std::min<uint64_t>(f.transferred, f.size);
            if (f.complete) ++a.done;
            if (f.failed) ++a.failed;
            continue;
        }
        visible.push_back({f.unix_ms, nullptr, &f});
    }
    for (auto& kv : batches) {
        Agg& a = kv.second;
        a.info.transferred = a.total ? a.sum / static_cast<uint64_t>(a.total) : 0;
        a.info.complete = a.total > 0 && a.done > 0 && a.done + a.failed == a.total;
        a.info.failed = a.total > 0 && a.failed == a.total;
        if (a.info.failed) a.info.error = "nobody received it";
        else if (a.info.complete && a.failed > 0)
            a.info.error = std::to_string(a.failed) + " of " + std::to_string(a.total) + " failed";
    }
    for (auto& e : visible)
        if (e.batch) e.f = &batches[e.batch].info;
    std::stable_sort(visible.begin(), visible.end(),
                     [](const Entry& a, const Entry& b) { return a.ts < b.ts; });
    size_t start_index = visible.size() > 220 ? visible.size() - 220 : 0;
    std::string last_day;
    for (size_t i = start_index; i < visible.size(); ++i) {
        const uint64_t ets = visible[i].ts;
        std::string day;
        if (ets) {
            std::time_t t = static_cast<std::time_t>(ets / 1000);
            std::tm tm{};
#ifdef _WIN32
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char b[24];
            std::strftime(b, sizeof(b), "%d.%m.%Y", &tm);
            day = b;
        }
        if (!day.empty() && day != last_day) {
            ImVec2 p = ImGui::GetCursorScreenPos();
            float w = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPosX(w * 0.5f - 36.0f);
            ImGui::TextDisabled("%s", day.c_str());
            ImGui::SetCursorScreenPos(ImVec2(p.x, ImGui::GetCursorScreenPos().y));
            last_day = day;
        }
        if (visible[i].m) draw_message(*visible[i].m);
        else if (visible[i].batch)
            draw_file_card(*visible[i].f, batches[visible[i].batch].total,
                           batches[visible[i].batch].done);
        else
            draw_file_card(*visible[i].f);
    }

    if (visible.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 36.0f));
        ImGui::SetCursorPosX(20.0f);
        ImGui::TextDisabled("No messages yet. Messages are stored encrypted on this device.");
    }
    if (auto_scroll_ && at_bottom) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();

    ImGui::BeginChild("composer", ImVec2(0, input_h), ImGuiChildFlags_None);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 142.0f);
    if (focus_composer_) {
        ImGui::SetKeyboardFocusHere();  // keep typing after Enter/Send
        focus_composer_ = false;
    }
    std::string composer_hint = "Write an encrypted message...";
    if (selected_kind_ == ConversationKind::Group) {
        for (const auto& g : groups_)
            if (g.id == selected_id_) composer_hint = "Message to group \"" + g.name + "\"...";
    } else if (selected_kind_ == ConversationKind::Direct) {
        composer_hint = "Private message to " + peer_alias(selected_id_) + "...";
    }
    bool enter = ImGui::InputTextWithHint("##composer", composer_hint.c_str(), chat_input_,
                                          sizeof(chat_input_),
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Attach", ImVec2(58.0f, 26.0f))) {
        if (selected_kind_ != ConversationKind::None) {
            file_path_[0] = 0;
            file_target_kind_ = selected_kind_;  // fixed while the dialog is open
            file_target_id_ = selected_id_;
            show_file_ = true;
        }
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, c_accent);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    bool send = ImGui::Button("Send", ImVec2(58.0f, 26.0f));
    ImGui::PopStyleColor(2);
    if (send || enter) {
        // The draft is cleared only when the message was really accepted (a too-long text or an
        // error used to erase what the user had typed).
        if (send_text(chat_input_)) chat_input_[0] = 0;
        focus_composer_ = true;
    }
    ImGui::EndChild();

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::draw_group_manage(const p2p::GroupRoom& g) {
        ImGui::Text("Members (%zu)", g.members.size());
        const bool i_am_admin = !g.members.empty() && g.members.front() == node_.identity().id;
        for (const auto& m : g.members) {
            bool verified = false;
            for (const auto& c : contacts_)
                if (c.id == m) verified = c.verified;
            ImGui::BulletText("%s  %s", peer_alias(m).c_str(),
                              m == node_.identity().id ? "(you)"
                              : m == g.members.front() ? "admin"
                                                       : (verified ? "verified" : "TOFU"));
            if (i_am_admin && m != node_.identity().id) {
                ImGui::SameLine();
                ImGui::PushID(m.hex().c_str());
                if (ImGui::SmallButton("Remove")) {
                    std::string rerr;
                    set_status(node_.exclude_from_group(g.id, m, &rerr)
                                   ? "Member removed, group key rotated"
                                   : "Remove: " + rerr);
                }
                ImGui::PopID();
            }
        }
        if (ImGui::Button("Sync group history")) {
            node_.sync_group(g.id);
            set_status("Group history sync requested");
        }
        if (ImGui::Button("Add member")) ImGui::OpenPopup("add_member_popup");
        ImGui::SameLine();
        if (ImGui::Button("Leave group")) ImGui::OpenPopup("Leave group?");
        if (ImGui::BeginPopup("add_member_popup")) {
            bool any = false;
            for (const auto& c : contacts_) {
                if (std::find(g.members.begin(), g.members.end(), c.id) != g.members.end()) continue;
                any = true;
                const std::string label =
                    (c.alias.empty() ? c.id.short_hex(8) : c.alias) + "##add" + c.id.hex();
                if (ImGui::Selectable(label.c_str())) {
                    set_status(node_.invite_to_group(g.id, c.id)
                                   ? "Invitation sent to " + peer_alias(c.id)
                                   : "Invite failed: no session with " + peer_alias(c.id) +
                                         " yet (it will be delivered when they connect)");
                }
            }
            if (!any) ImGui::TextDisabled("Every contact is already a member");
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopupModal("Leave group?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("Leave \"%s\"? It disappears from your list. The other members keep "
                               "their copy%s.",
                               g.name.c_str(),
                               i_am_admin ? " and get a new group key" : "");
            ImGui::Spacing();
            if (ImGui::Button("Leave", ImVec2(110.0f, 28.0f))) {
                std::string lerr;
                if (node_.leave_group(g.id, &lerr)) {
                    chat_input_[0] = 0;  // the draft was written for the group we just left
                    selected_kind_ = ConversationKind::None;
                    selected_id_ = p2p::NodeId{};
                    set_status("You left the group");
                } else {
                    set_status("Leave: " + lerr);
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(90.0f, 28.0f))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
}

void App::draw_security_tab() {
    if (selected_kind_ == ConversationKind::None) {
        ImGui::TextWrapped("Select a conversation to inspect its security state.");
        draw_identity_backup();
        return;
    }

    if (selected_kind_ == ConversationKind::Group) {
        p2p::GroupRoom g;
        if (!node_.group(selected_id_, g)) return;
        ImGui::TextColored(c_accent, "GROUP END-TO-END KEY");
        ImGui::TextWrapped("A random 256-bit group key is delivered only inside pairwise "
                           "ratcheted sessions. Voice and text are sealed with ChaCha20-Poly1305.");
        ImGui::Spacing();
        ImGui::TextDisabled("Group ID");
        ImGui::TextWrapped("%s", g.id.hex().c_str());
        ImGui::TextDisabled("Key fingerprint");
        ImGui::TextWrapped("%s", p2p::key_fingerprint(g.group_key).c_str());
        ImGui::TextDisabled("Last causal hash");
        ImGui::TextWrapped("%s", g.last_hash.hex().c_str());
        ImGui::Spacing();
        draw_group_manage(g);
        draw_identity_backup();
        return;
    }

    p2p::Peer p;
    node_.get_peer(selected_id_, p);
    p2p::Contact c;
    node_.contact(selected_id_, c);
    std::string code;
    std::string words;
    bool sas_ok = node_.sas(selected_id_, code, words);

    ImGui::TextColored(sas_ok ? c_green : c_amber,
                       sas_ok ? "SESSION AUTHENTICATED" : "WAITING FOR HANDSHAKE");
    ImGui::TextWrapped("Safety number: compare this six-digit code and four words with your "
                       "contact over a separate channel. They come from both identity keys, so "
                       "they stay the same across reconnects. If they differ, someone may be "
                       "impersonating your contact.");
    ImGui::Spacing();
    ImGui::TextDisabled("SAS code");
    ImGui::PushStyleColor(ImGuiCol_Text, sas_ok ? c_green : c_amber);
    ImGui::Text("%s", sas_ok ? code.c_str() : "------");
    ImGui::PopStyleColor();
    ImGui::TextDisabled("Words");
    ImGui::TextWrapped("%s", sas_ok ? words.c_str() : "not available");
    ImGui::Spacing();
    ImGui::TextDisabled("Identity fingerprint");
    ImGui::TextWrapped("%s", node_.fingerprint(selected_id_).c_str());
    ImGui::TextDisabled("Node ID");
    ImGui::TextWrapped("%s", selected_id_.hex().c_str());

    if (sas_ok) {
        ImGui::Spacing();
        ImGui::TextDisabled("Session ratchet");
        ImGui::Text("TX seq %llu  ·  TX epoch %llu  ·  RX epoch %llu",
                    static_cast<unsigned long long>(p.text_ratchet.tx_seq),
                    static_cast<unsigned long long>(p.text_ratchet.tx_epoch),
                    static_cast<unsigned long long>(p.text_ratchet.rx_epoch));
        ImGui::Text("Replay hits %llu  ·  ratchet steps %llu",
                    static_cast<unsigned long long>(p.text_replay.hits + p.voice_replay.hits),
                    static_cast<unsigned long long>(p.text_ratchet.steps + p.voice_ratchet.steps));
        ImGui::TextDisabled("Session key (changes on re-key)");
        ImGui::TextWrapped("%s", p2p::key_fingerprint(p.root_key).c_str());
    }

    ImGui::Spacing();
    bool verified = c.verified;
    if (ImGui::Checkbox("Verified fingerprint", &verified)) {
        node_.mark_verified(selected_id_, verified);
        set_status(verified ? "Contact verified" : "Verification cleared");
    }
    if (ImGui::Button("Copy SAS", ImVec2(90.0f, 24.0f))) copy_text(code + " " + words, "SAS");
    ImGui::Spacing();
    ImGui::TextWrapped("Handshake: Ed25519 signatures + X25519 static/ephemeral DH");
    ImGui::TextWrapped("Transport: ChaCha20-Poly1305 · text epoch 64 · voice epoch 256");

    draw_identity_backup();
}

void App::draw_identity_backup() {
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(c_accent, "IDENTITY BACKUP");
    ImGui::TextWrapped("Your identity (Ed25519 seed) is what makes you you. Export it encrypted "
                       "with a passphrase (Argon2id + XChaCha20-Poly1305) to move it to another "
                       "computer or restore it after reinstalling. Keep the file and the "
                       "passphrase safe: anyone with both can act as you.");
    if (export_path_[0] == 0) {
        std::string home = ".";
#ifdef _WIN32
        if (const wchar_t* w = _wgetenv(L"USERPROFILE")) home = std::filesystem::path(w).u8string();
#else
        if (const char* h = std::getenv("HOME")) home = h;
#endif
        std::snprintf(export_path_, sizeof(export_path_), "%s/MKR-identity.p2pk", home.c_str());
    }
    auto primary = [&](const char* label, bool danger) {
        ImGui::PushStyleColor(ImGuiCol_Button, danger ? c_amber : c_accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, danger ? c_amber : c_accent);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        bool r = ImGui::Button(label, ImVec2(-1.0f, 34.0f));
        ImGui::PopStyleColor(3);
        return r;
    };
    ImGui::Spacing();
    ImGui::BeginChild("backup_export", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(c_accent, "Export");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##export_path", "where to save, e.g. identity.p2pk", export_path_, sizeof(export_path_));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##export_pass", "passphrase (8+ characters)", export_pass_, sizeof(export_pass_), ImGuiInputTextFlags_Password);
    if (primary("Export identity backup", false)) {
        p2p::Identity id = node_.identity();
        std::string err;
        if (std::strlen(export_pass_) < 8) {
            backup_ok_ = false;
            backup_msg_ = "Use a passphrase of at least 8 characters.";
        } else if (p2p::export_key(id, export_pass_, export_path_, err)) {
            backup_ok_ = true;
            backup_msg_ = "Saved to " + std::string(export_path_);
            export_pass_[0] = 0;
        } else {
            backup_ok_ = false;
            backup_msg_ = "Export failed: " + err;
        }
        set_status(backup_msg_);
    }
    ImGui::EndChild();
    ImGui::Spacing();
    ImGui::BeginChild("backup_import", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::TextColored(c_accent, "Import");
    ImGui::SetNextItemWidth(-82.0f);
    ImGui::InputTextWithHint("##import_path", "backup file to restore", import_path_, sizeof(import_path_));
    ImGui::SameLine();
    if (ImGui::Button("Browse", ImVec2(72.0f, 0.0f))) {
        std::string picked = pick_file_dialog();
        if (!picked.empty()) std::snprintf(import_path_, sizeof(import_path_), "%s", picked.c_str());
    }
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##import_pass", "passphrase", import_pass_, sizeof(import_pass_), ImGuiInputTextFlags_Password);
    if (primary("Import and replace my identity", true)) {
        p2p::Identity id;
        std::string err;
        if (import_path_[0] == 0) {
            backup_ok_ = false;
            backup_msg_ = "Choose a backup file first.";
        } else if (p2p::import_key(import_pass_, import_path_, id, err)) {
            if (id.save_key(node_.keyfile(), err)) {
                node_.set_identity(id);
                backup_ok_ = true;
                backup_msg_ = "Identity imported. Restart the app to use it.";
                import_pass_[0] = 0;
            } else {
                backup_ok_ = false;
                backup_msg_ = "Imported but could not save the key file: " + err;
            }
        } else {
            backup_ok_ = false;
            backup_msg_ = "Import failed: " + err;
        }
        set_status(backup_msg_);
    }
    ImGui::EndChild();
    if (!backup_msg_.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, backup_ok_ ? c_green : c_red);
        ImGui::TextWrapped("%s", backup_msg_.c_str());
        ImGui::PopStyleColor();
    }
}

void App::draw_network_tab() {
    ImGui::TextColored(c_accent, "NAT / STUN PROFILE");
    ImGui::Text("Type: %s", p2p::nat_name(nat_.type));
    ImGui::TextWrapped("Mapped: %s", nat_.discovered
                                           ? (nat_.mapped_ip + ":" + std::to_string(nat_.mapped_port)).c_str()
                                           : "not discovered");
    ImGui::TextWrapped("Local UDP: %u  ·  STUN-like probe", node_.port());
    if (nat_.type == p2p::NatType::Symmetric)
        ImGui::TextColored(c_amber, "Symmetric NAT: relay preferred (direct UDP usually blocked)");
    ImGui::Spacing();

    ImGui::TextColored(c_accent, "CANDIDATE RANKING S(n)");
    std::vector<p2p::Candidate> candidates;
    for (const auto& p : peers_) {
        if (!p.online(p2p::now_ms(), 20000)) continue;
        p2p::Candidate c;
        c.id = p.id;
        c.rtt_ms = p.rtt_ms;
        c.nat = p.nat;
        c.direct = p.direct;
        candidates.push_back(c);
    }
    candidates = p2p::rank_candidates(node_.identity().id, candidates);
    if (candidates.empty()) {
        ImGui::TextDisabled("No online candidates");
    } else if (ImGui::BeginTable("rank", 4,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Peer");
        ImGui::TableSetupColumn("S(n)");
        ImGui::TableSetupColumn("RTT");
        ImGui::TableSetupColumn("NAT");
        ImGui::TableHeadersRow();
        for (const auto& c : candidates) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", peer_alias(c.id).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", c.score);
            ImGui::TableNextColumn();
            ImGui::Text("%u ms", c.rtt_ms);
            ImGui::TableNextColumn();
            ImGui::Text("%s", p2p::nat_name(c.nat));
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextColored(c_accent, "RELAY");
    std::vector<p2p::NodeId> relay_options{p2p::NodeId{}};
    std::vector<std::string> relay_names{"Automatic / none"};
    int selected_relay = 0;
    for (const auto& p : peers_) {
        if (!p.handshake_done) continue;
        relay_options.push_back(p.id);
        relay_names.push_back(peer_alias(p.id) + "  " + p.id.short_hex(6));
        if (p.id == relay_) selected_relay = static_cast<int>(relay_options.size()) - 1;
    }
    std::vector<const char*> relay_cstr;
    for (const auto& n : relay_names) relay_cstr.push_back(n.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("##relay", &selected_relay, relay_cstr.data(),
                     static_cast<int>(relay_cstr.size()))) {
        node_.set_relay(relay_options[selected_relay]);
        set_status(selected_relay == 0 ? "Relay disabled" : "Relay selected");
    }
    if (!relay_.empty()) {
        ImGui::TextDisabled("Active relay: %s", peer_alias(relay_).c_str());
        if (ImGui::SmallButton("Clear relay")) node_.set_relay(p2p::NodeId{});
    }

    ImGui::Spacing();
    ImGui::TextColored(c_accent, "PEERS");
    if (peers_.empty()) {
        ImGui::TextDisabled("No peers discovered");
    } else if (ImGui::BeginTable("peers", 4,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Peer", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("RTT", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("NAT", ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableHeadersRow();
        uint64_t now = p2p::now_ms();
        for (const auto& p : peers_) {
            bool on = p.online(now, 20000);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(on ? c_green : c_muted, "%s", peer_alias(p.id).c_str());
            ImGui::TableNextColumn();
            ImGui::Text("%u", p.rtt_ms);
            ImGui::TableNextColumn();
            ImGui::Text("%s", p.direct ? "direct" : (p.relayed ? "relay" : "indir"));
            ImGui::TableNextColumn();
            const char* nat = p2p::nat_name(p.nat);
            if (std::strcmp(nat, "FullCone") == 0) nat = "full";
            else if (std::strcmp(nat, "Restricted") == 0) nat = "restr";
            else if (std::strcmp(nat, "Symmetric") == 0) nat = "sym";
            ImGui::Text("%s", nat);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextColored(c_accent, "DHT / SESSION COUNTERS");
    ImGui::Text("handshakes %llu  ·  MITM %llu",
                static_cast<unsigned long long>(stats_.handshake_ok),
                static_cast<unsigned long long>(stats_.mitm_rejected));
    ImGui::Text("relay fwd %llu  ·  replay %llu",
                static_cast<unsigned long long>(stats_.relay_forwarded),
                static_cast<unsigned long long>(stats_.replay_hits));
    ImGui::Text("queued %llu  ·  inbox %llu/%llu",
                static_cast<unsigned long long>(stats_.queued),
                static_cast<unsigned long long>(stats_.envelopes_stored),
                static_cast<unsigned long long>(stats_.envelopes_fetched));
    ImGui::Text("TX %llu  ·  RX %llu  ·  drop %llu",
                static_cast<unsigned long long>(stats_.tx_packets),
                static_cast<unsigned long long>(stats_.rx_packets),
                static_cast<unsigned long long>(stats_.dropped));

    ImGui::Spacing();
    ImGui::TextColored(c_accent, "INVITE / CONNECT");
    ImGui::TextWrapped("%s", node_.share_uri().c_str());
    if (ImGui::Button("Copy my link")) copy_text(node_.share_uri(), "Contact link");
    ImGui::SameLine();
    if (ImGui::Button("Add contact")) show_add_contact_ = true;

    ImGui::Spacing();
    ImGui::TextColored(c_accent, "SELF-TEST");
    int st = selftest_state_.load();
    if (st == 1) {
        ImGui::TextColored(c_amber, "Running security/network/voice suite (~1-2 min)...");
    } else if (ImGui::Button("Run self-test")) {
        start_gui_selftest();
    }
    if (st == 2) {
        std::lock_guard<std::mutex> lk(selftest_mtx_);
        ImGui::SameLine();
        ImGui::TextColored(selftest_rc_ == 0 ? c_accent : c_amber,
                           selftest_rc_ == 0 ? "PASS" : "FAIL (rc=%d)", selftest_rc_);
        if (ImGui::CollapsingHeader("Self-test report")) {
            for (const auto& l : selftest_lines_) ImGui::TextWrapped("%s", l.c_str());
            if (ImGui::Button("Copy report")) {
                std::string all;
                for (const auto& l : selftest_lines_) all += l + "\n";
                copy_text(all, "Self-test report");
            }
        }
    }

    if (ImGui::CollapsingHeader("Recent diagnostics")) {
        size_t start = log_lines_.size() > 9 ? log_lines_.size() - 9 : 0;
        for (size_t i = start; i < log_lines_.size(); ++i)
            ImGui::TextWrapped("%s", log_lines_[i].c_str());
    }
}

void App::start_gui_selftest() {
    if (selftest_state_.load() == 1) return;
    if (selftest_thread_.joinable()) selftest_thread_.join();
    selftest_state_ = 1;
    std::string exe = self_exe_;
    std::string out = (testkit_dir_.empty() ? std::string("testkit") : testkit_dir_) +
                      "/selftest_report.txt";
    selftest_thread_ = std::thread([this, exe, out] {
        std::vector<std::string> lines;
        int rc = -1;
        if (exe.empty()) {
            lines.push_back("executable path unknown; run p2pnode --selftest from a terminal");
        } else {
#ifdef _WIN32
            // cmd.exe strips the outer pair of quotes.
            std::string cmd = "\"set P2P_LOG_FILE=& set P2P_METRICS=& \"" + exe + "\" --selftest > \"" + out + "\" 2>&1\"";
#else
            // The child must not inherit --stats mirrors (it would pollute the bundle).
            std::string cmd = "env -u P2P_LOG_FILE -u P2P_METRICS '" + exe + "' --selftest > '" + out + "' 2>&1";
#endif
            rc = std::system(cmd.c_str());
            std::ifstream in(out);
            for (std::string l; std::getline(in, l);) lines.push_back(l);
            lines.push_back("report saved to " + out);
        }
        {
            std::lock_guard<std::mutex> lk(selftest_mtx_);
            selftest_rc_ = rc;
            selftest_lines_ = std::move(lines);
        }
        selftest_state_ = 2;
    });
}

void App::draw_inspector() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c_panel);
    ImGui::BeginChild("inspector", ImVec2(0, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImGui::TextColored(c_accent, "INSPECTOR");
    ImGui::SameLine(ImGui::GetWindowWidth() - 42.0f);
    if (ImGui::SmallButton("X")) inspector_open_ = false;
    ImGui::Separator();

    const char* tab_labels[3] = {"Security", "Network", "Audio"};
    for (int i = 0; i < 3; ++i) {
        bool active = inspector_tab_ == i;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? c_accent
                                                       : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              active ? ImVec4(1, 1, 1, 1)
                                     : ImGui::GetStyleColorVec4(ImGuiCol_Text));
        if (ImGui::Button(tab_labels[i], ImVec2(96.0f, 26.0f))) inspector_tab_ = i;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Separator();

    if (inspector_tab_ == 0)
        draw_security_tab();
    else if (inspector_tab_ == 1)
        draw_network_tab();
    else
        draw_audio_tab();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(c_accent, "PAIRING PIN");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##pin", "shared PIN / label", pair_code_, sizeof(pair_code_));
    if (ImGui::Button("Set + publish")) {
        node_.set_pairing_code(pair_code_);
        set_status(pair_code_[0] ? "Pairing PIN published to DHT" : "Pairing PIN cleared");
    }
    ImGui::SameLine();
    if (ImGui::Button("Find by PIN")) {
        std::string err;
        if (node_.search_pairing_code(pair_code_, err))
            set_status("Pairing PIN found; HELLO sent");
        else
            set_status("PIN lookup: " + err);
    }
    ImGui::TextWrapped("Both peers must set the same PIN. HMAC-like MAC is checked in HELLO.");
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

bool App::save_test_report() {
    const std::string dir = testkit_dir_.empty() ? "testkit" : testkit_dir_;
    const std::string path = dir + "/test_report.txt";
    p2p::MicReport mic;
    if (!mic_test_file_.empty() && !mic_test_active_) {
        const auto st = voice_.mic_test_stats();
        mic.present = true;
        mic.file = std::filesystem::path(mic_test_file_).filename().string();
        mic.duration_s = 10;
        mic.samples = st.samples;
        mic.rms_dbfs = st.rms_dbfs;
        mic.peak = st.peak;
        mic.clipped = st.clipped;
    }
    if (!p2p::write_text_report(path, node_, &mic)) {
        set_status("Test report: cannot write " + path);
        return false;
    }
    set_status("Test report saved: " + path);
    return true;
}

void App::refresh_audio_devices() {
    in_devs_ = p2p::VoiceEngine::list_devices(true);
    out_devs_ = p2p::VoiceEngine::list_devices(false);
    devs_loaded_ = true;
    in_sel_ = 0;
    out_sel_ = 0;
    std::string cn = voice_.capture_name(), pn = voice_.playback_name();
    for (size_t i = 0; i < in_devs_.size(); ++i)
        if (in_devs_[i] == cn) in_sel_ = static_cast<int>(i) + 1;
    for (size_t i = 0; i < out_devs_.size(); ++i)
        if (out_devs_[i] == pn) out_sel_ = static_cast<int>(i) + 1;
}

void App::apply_audio_devices() {
    std::string in = in_sel_ > 0 && in_sel_ <= static_cast<int>(in_devs_.size()) ? in_devs_[in_sel_ - 1] : "";
    std::string out = out_sel_ > 0 && out_sel_ <= static_cast<int>(out_devs_.size()) ? out_devs_[out_sel_ - 1] : "";
    std::string err;
    if (voice_.set_devices(in, out, err))
        set_status(err.empty() ? "Audio devices switched" : "Audio: " + err);
    else
        set_status("Audio: " + (err.empty() ? std::string("no device could be opened") : err));
    save_audio_prefs();
}

void App::save_audio_prefs() {
    std::ofstream f(std::filesystem::u8path(app_data_dir() + "/audio.cfg"), std::ios::trunc);
    if (!f) return;
    f << "in=" << voice_.capture_name() << "\n"
      << "out=" << voice_.playback_name() << "\n"
      << "filter=" << (voice_.filter_enabled() ? 1 : 0) << "\n"
      << "agc=" << (voice_.agc() ? 1 : 0) << "\n"
      << "gain=" << voice_.input_gain_db() << "\n";
}

void App::load_audio_prefs() {
    std::ifstream f(std::filesystem::u8path(app_data_dir() + "/audio.cfg"));
    if (!f) return;
    std::string line, in, out;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "in") in = v;
        else if (k == "out") out = v;
        else if (k == "filter") voice_.set_filter_enabled(v == "1");
        else if (k == "agc") voice_.set_agc(v == "1");
        else if (k == "gain") voice_.set_input_gain_db(std::atoi(v.c_str()));
    }
    if (!in.empty() || !out.empty()) {
        std::string err;
        voice_.set_devices(in, out, err);
    }
}

void App::draw_audio_tab() {
    if (!devs_loaded_) refresh_audio_devices();
    const float full = ImGui::GetContentRegionAvail().x;
    auto combo = [&](const char* id, const char* caption, std::vector<std::string>& list, int& sel) {
        ImGui::TextDisabled("%s", caption);
        ImGui::SetNextItemWidth(-1.0f);
        std::string cur = sel == 0 || sel > static_cast<int>(list.size()) ? "System default" : list[sel - 1];
        bool changed = false;
        if (ImGui::BeginCombo(id, fit_text(cur, full - 48.0f).c_str())) {
            if (ImGui::Selectable("System default", sel == 0)) { sel = 0; changed = true; }
            for (size_t i = 0; i < list.size(); ++i)
                if (ImGui::Selectable(fit_text(list[i], full - 40.0f).c_str(), sel == static_cast<int>(i) + 1)) {
                    sel = static_cast<int>(i) + 1;
                    changed = true;
                }
            ImGui::EndCombo();
        }
        return changed;
    };

    ImGui::TextColored(c_accent, "AUDIO DEVICES");
    bool ch = combo("##in_dev", "Microphone (input)", in_devs_, in_sel_);
    ch |= combo("##out_dev", "Speakers / headphones (output)", out_devs_, out_sel_);
    if (ch) apply_audio_devices();
    if (ImGui::SmallButton("Refresh device list")) refresh_audio_devices();
    ImGui::TextDisabled("in: %s", vstats_.capture_device.c_str());
    ImGui::TextDisabled("out: %s", vstats_.playback_device.c_str());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(c_accent, "VOICE FILTER");
    ImGui::TextWrapped("For old, weak or noisy microphones: removes rumble and DC offset, "
                       "adapts the voice gate to the room noise, and levels quiet voices.");
    bool filt = voice_.filter_enabled();
    if (ImGui::Checkbox("Noise gate + high-pass filter", &filt)) {
        voice_.set_filter_enabled(filt);
        save_audio_prefs();
    }
    bool agc = voice_.agc();
    if (ImGui::Checkbox("Automatic gain (levels quiet voices)", &agc)) {
        voice_.set_agc(agc);
        save_audio_prefs();
    }
    int gain = voice_.input_gain_db();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##gain", &gain, -12, 30, "input gain %+d dB")) {
        voice_.set_input_gain_db(gain);
        save_audio_prefs();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(c_accent, "MICROPHONE CHECK");
    float level = std::clamp(static_cast<float>((voice_.mic_level_db() + 60.0) / 60.0), 0.0f, 1.0f);
    char lv[48];
    std::snprintf(lv, sizeof(lv), "%.0f dBFS", voice_.mic_level_db());
    ImGui::ProgressBar(mic_preview_ui_ ? level : 0.0f, ImVec2(-1.0f, 16.0f),
                       mic_preview_ui_ ? lv : "off");
    bool busy = call_.state != p2p::CallState::Idle;
    if (busy && mic_preview_ui_) {
        voice_.set_mic_preview(false, false);
        mic_preview_ui_ = false;
    }
    ImGui::PushStyleColor(ImGuiCol_Button, mic_preview_ui_ ? c_red : c_accent);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    if (ImGui::Button(mic_preview_ui_ ? "Stop check" : "Start mic check", ImVec2(150.0f, 30.0f))) {
        if (busy) {
            set_status("Finish the call before a microphone check");
        } else {
            mic_preview_ui_ = !mic_preview_ui_;
            if (mic_preview_ui_) hear_myself_ui_ = true;  // a check you cannot hear is useless
            voice_.set_mic_preview(mic_preview_ui_, mic_preview_ui_ && hear_myself_ui_);
        }
    }
    ImGui::PopStyleColor(2);
    ImGui::SameLine();
    if (ImGui::Checkbox("Hear myself", &hear_myself_ui_) && mic_preview_ui_)
        voice_.set_mic_preview(true, hear_myself_ui_);
    if (hear_myself_ui_) ImGui::TextColored(c_amber, "Use headphones, speakers will cause feedback.");
    if (mic_preview_ui_)
        ImGui::TextDisabled("noise floor %.0f dBFS  ·  speak: the bar should reach about -25 dBFS",
                            voice_.noise_floor_db());
    ImGui::Spacing();
    ImGui::TextDisabled("Save a 10 s WAV recording (capture only):");
    if (mic_test_active_) {
        if (ImGui::Button("Stop and save", ImVec2(140.0f, 28.0f))) {
            voice_.stop_mic_test();
            mic_test_active_ = false;
            if (save_mic_test_metrics())
                set_status("Microphone test saved: " + mic_test_file_);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("recording...");
    } else {
        if (ImGui::Button("Record 10 s", ImVec2(120.0f, 28.0f))) {
            if (call_.state != p2p::CallState::Idle) {
                set_status("Finish the call before a microphone test");
            } else {
                if (mic_preview_ui_) {
                    voice_.set_mic_preview(false, false);
                    mic_preview_ui_ = false;
                }
                mic_test_file_ = testkit_dir_ + "/mic_test.wav";
                std::string err;
                if (voice_.start_mic_test(mic_test_file_, 10, err)) {
                    mic_test_active_ = true;
                    mic_test_until_ = p2p::now_ms() + 11000;
                    set_status("Microphone test started (capture only)");
                } else {
                    set_status("Mic test: " + err);
                }
            }
        }
    }
    ImGui::Spacing();
    ImGui::TextColored(c_accent, "CODEC SELF-CHECK");
    ImGui::TextDisabled("Encodes and decodes a test tone in memory (no sound).");
    if (ImGui::Button("Run codec check", ImVec2(140.0f, 28.0f))) {
        double snr = 0.0;
        double kbps = 0.0;
        std::string err;
        if (voice_.loopback_test(snr, kbps, err))
            set_status("Opus loopback " + fmt1(snr) + " dB SNR, " + fmt1(kbps) + " kbit/s");
        else
            set_status("Loopback: " + err);
    }
    ImGui::TextDisabled("last SNR %.2f dB · jitter %.1f ms · loss %.2f%%",
                        vstats_.last_snr_db, vstats_.jitter_ms, vstats_.packet_loss_pct);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(c_accent, "TRANSFERS");
    if (files_.empty()) ImGui::TextDisabled("No file transfers yet.");
    int shown = 0;
    for (const auto& f : files_) {
        if (shown++ >= 4) break;
        float frac = f.size ? static_cast<float>(std::min<uint64_t>(f.transferred, f.size)) /
                                  static_cast<float>(f.size)
                            : 0.0f;
        std::string state = f.complete ? "all bytes received"
                                      : (f.failed ? "failed: " + f.error : "transferring");
        ImGui::TextDisabled("%s %s", f.outgoing ? "\xE2\x86\x91" : "\xE2\x86\x93", f.name.c_str());
        ImGui::ProgressBar(frac, ImVec2(-1.0f, 14.0f), state.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(c_accent, "TEST REPORT");
    ImGui::TextWrapped("Writes node, peer, relay, group, file-transfer and microphone "
                       "metrics to testkit/test_report.txt. Message contents and keys "
                       "are not included.");
    if (ImGui::Button("Save test report", ImVec2(160.0f, 28.0f)))
        save_test_report();
}

void App::draw_audio_bar() {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, c_panel);
    ImGui::BeginChild("audio_bar", ImVec2(0, 44.0f), ImGuiChildFlags_Borders);
    bool talking = vstats_.talking;
    ImGui::TextColored(talking ? c_green : c_muted, "%s", talking ? "LIVE" : "IDLE");
    ImGui::SameLine();
    {
        // device names are long; keep them clear of the controls on the right
        float room = std::max(60.0f, ImGui::GetWindowWidth() - 700.0f - ImGui::GetCursorPosX());
        std::string devs = vstats_.capture_device + "  \xE2\x86\x92  " + vstats_.playback_device;
        ImGui::TextDisabled("%s", fit_text(devs, room).c_str());
    }

    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, ImGui::GetWindowWidth() - 690.0f));
    const char* bitrates[] = {"16k", "24k", "32k", "48k"};
    const int values[] = {16000, 24000, 32000, 48000};
    ImGui::SetNextItemWidth(72.0f);
    if (ImGui::Combo("##bitrate", &bitrate_index_, bitrates, 4))
        voice_.set_bitrate(values[bitrate_index_]);
    ImGui::SameLine();
    bool muted = voice_.muted();
    if (muted) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.58f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button(muted ? "Mute*" : "Mute", ImVec2(58.0f, 24.0f))) {
        voice_.set_muted(!muted);
        set_status(muted ? "Microphone unmuted" : "Microphone muted");
    }
    if (muted) ImGui::PopStyleColor();
    ImGui::SameLine();
    bool deaf = voice_.deafened();
    if (deaf) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.58f, 0.20f, 0.20f, 1.0f));
    if (ImGui::Button(deaf ? "Deaf*" : "Deaf", ImVec2(58.0f, 24.0f))) {
        voice_.set_deafened(!deaf);
        set_status(deaf ? "Audio restored" : "Output muted");
    }
    if (deaf) ImGui::PopStyleColor();
    ImGui::SameLine();
    bool vad = voice_.vad_enabled();
    if (ImGui::Checkbox("VAD", &vad)) {
        voice_.set_vad(vad);
        if (vad) ptt_mode_ = false;
    }
    ImGui::SameLine();
    bool ptt = voice_.ptt_mode();
    if (ImGui::Checkbox("PTT", &ptt)) {
        voice_.set_ptt(ptt);
        ptt_mode_ = ptt;
        if (!ptt) voice_.set_ptt_pressed(false);
    }
    if (ptt_mode_) {
        ImGui::SameLine();
        if (ptt_button_active_) {
            ImGui::PushStyleColor(ImGuiCol_Button, c_green);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        }
        ImGui::Button(ptt_button_active_ ? "LIVE##ptt" : "Hold##ptt", ImVec2(52.0f, 24.0f));
        if (ptt_button_active_) ImGui::PopStyleColor(2);
        ImVec2 hold_min = ImGui::GetItemRectMin();
        ImVec2 hold_max = ImGui::GetItemRectMax();
        ImGuiIO& io = ImGui::GetIO();
        const bool raw_hold = ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
                              io.MousePos.x >= hold_min.x && io.MousePos.x <= hold_max.x &&
                              io.MousePos.y >= hold_min.y && io.MousePos.y <= hold_max.y;
        ptt_button_active_ = ImGui::IsItemActive() || raw_hold;
    } else {
        ptt_button_active_ = false;
    }

    ImGui::SameLine();
    ImGui::TextDisabled("%s", vstats_.fec_enabled ? "FEC on" : "FEC off");

    ImGui::SameLine(ImGui::GetWindowWidth() - (mic_preview_ui_ ? 250.0f : 126.0f));
    if (mic_preview_ui_) {
        float level = std::clamp(static_cast<float>((voice_.mic_level_db() + 60.0) / 60.0), 0.0f, 1.0f);
        ImGui::ProgressBar(level, ImVec2(104.0f, 18.0f), "");
        ImGui::SameLine();
    }
    // One click = you hear your own microphone live (use headphones).  This used to be a
    // silent in-memory codec check, which is why "loopback" never made a sound.
    const bool mic_test_styled = mic_preview_ui_;  // the click below may flip the state
    if (mic_test_styled) ImGui::PushStyleColor(ImGuiCol_Button, c_red);
    if (ImGui::SmallButton(mic_test_styled ? "Stop mic test" : "Mic test")) {
        if (call_.state != p2p::CallState::Idle && !mic_preview_ui_) {
            set_status("Finish the call before a microphone test");
        } else {
            mic_preview_ui_ = !mic_preview_ui_;
            hear_myself_ui_ = true;
            voice_.set_mic_preview(mic_preview_ui_, mic_preview_ui_);
            set_status(mic_preview_ui_ ? "Speak now: you should hear yourself (use headphones)"
                                       : "Microphone test stopped");
        }
    }
    if (mic_test_styled) ImGui::PopStyleColor();
    if (!voice_error_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(c_red, "audio: %s", voice_error_.c_str());
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::draw_profile_modal() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(420.0f, 0.0f), ImVec2(420.0f, 2000.0f));
    if (!ImGui::BeginPopupModal("Edit profile", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextColored(c_accent, "PROFILE NAME");
    ImGui::TextWrapped("The name is stored encrypted in the local vault and is sent in HELLO "
                       "to peers. It does not change your NodeID or keys.");
    ImGui::SetNextItemWidth(-1.0f);
    bool enter = ImGui::InputText("##profile_name", profile_input_, sizeof(profile_input_),
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::Spacing();
    if (ImGui::Button("Save", ImVec2(100.0f, 28.0f)) || enter) {
        std::string err;
        if (node_.set_alias(profile_input_, &err)) {
            set_status("Profile name updated");
            ImGui::CloseCurrentPopup();
        } else {
            set_status("Profile: " + err);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90.0f, 28.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::draw_file_modal() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 0.0f), ImVec2(560.0f, 2000.0f));
    if (!ImGui::BeginPopupModal("Send file", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    {
        std::string file_to = "ENCRYPTED FILE TRANSFER  >  ";
        if (file_target_kind_ == ConversationKind::Group) {
            for (const auto& g : groups_)
                if (g.id == file_target_id_) file_to += g.name;
        } else {
            file_to += peer_alias(file_target_id_);
        }
        ImGui::TextColored(c_accent, "%s", file_to.c_str());
    }
    ImGui::TextWrapped("The file is sent in chunked UDP records inside the pairwise ratchet. "
                       "The receiver verifies SHA-256 and reports 'all bytes received'; there is "
                       "no artificial size limit (disk and link speed are the only limits).");
    ImGui::TextDisabled("File path on this computer");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputTextWithHint("##file_path", "e.g. testkit/file_1mb.bin", file_path_,
                             sizeof(file_path_));
    ImGui::SameLine();
    if (ImGui::SmallButton("Browse...")) {
        std::string picked = pick_file_dialog();
        if (!picked.empty()) std::snprintf(file_path_, sizeof(file_path_), "%s", picked.c_str());
    }
    ImGui::TextDisabled("You can also drag a file onto the window.");
    if (ImGui::SmallButton("Use 1 MiB test file")) {
        std::string p = testkit_dir_ + "/file_1mb.bin";
        std::snprintf(file_path_, sizeof(file_path_), "%s", p.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Use 10 MiB test file")) {
        std::string p = testkit_dir_ + "/file_10mb.bin";
        std::snprintf(file_path_, sizeof(file_path_), "%s", p.c_str());
    }
    ImGui::TextDisabled("Tip: the test kit is generated next to the executable.");
    ImGui::Spacing();
    if (ImGui::Button("Send", ImVec2(110.0f, 28.0f))) {
        if (file_target_kind_ == ConversationKind::None) {
            set_status("Select a conversation first");
        } else if (clean_path(file_path_).empty()) {
            set_status("File path is empty");
        } else {
            std::string err;
            if (file_target_kind_ == ConversationKind::Group) {
                int n = node_.send_file_group(file_target_id_, clean_path(file_path_), &err);
                if (n > 0) {
                    set_status("File sent to " + std::to_string(n) + " member(s)");
                    ImGui::CloseCurrentPopup();
                } else {
                    set_status("File: " + err);
                }
            } else if (node_.send_file(file_target_id_, clean_path(file_path_), &err)) {
                set_status("File transfer started");
                ImGui::CloseCurrentPopup();
            } else {
                set_status("File: " + err);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90.0f, 28.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::draw_add_contact_modal() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(560.0f, 0.0f), ImVec2(560.0f, 2000.0f));
    if (!ImGui::BeginPopupModal("Add contact", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextColored(c_accent, "PAIR WITH A NODE");
    ImGui::TextWrapped("Invite URIs are self-certifying: the 160-bit NodeID is BLAKE2b-160 of "
                       "the Ed25519 public key, and HELLO is signed by that key.");
    if (ImGui::RadioButton("Invite URI", !add_by_pin_)) add_by_pin_ = false;
    ImGui::SameLine();
    if (ImGui::RadioButton("Pairing PIN", add_by_pin_)) add_by_pin_ = true;
    ImGui::Spacing();

    if (!add_by_pin_) {
        ImGui::TextDisabled("Contact link");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##invite", "username:p2p://<node-id>@host:port", invite_input_,
                                 sizeof(invite_input_));
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("The name is taken from the link and is updated automatically when "
                           "your contact changes their profile name.");
        ImGui::PopStyleColor();
        if (ImGui::Button("Connect", ImVec2(120.0f, 28.0f))) {
            std::string host;
            uint16_t port = 0;
            p2p::NodeId id;
            if (!p2p::Node::parse_invite(invite_input_, id, host, port)) {
                set_status("Invalid invite URI");
                add_contact_error_ = "Invalid link. Expected username:p2p://<40 hex>@host:port";
            } else {
                add_contact_error_.clear();
                std::string alias = p2p::Node::parse_invite_name(invite_input_);
                if (alias.empty()) {
                    alias = id.short_hex(8);
                    // re-pasting a bare link must not replace a name we already know
                    for (const auto& ct : contacts_)
                        if (ct.id == id && !ct.alias.empty()) alias = ct.alias;
                }
                if (id == node_.identity().id) {
                    add_contact_error_ = "This is your own link. Send it to your contact instead.";
                    set_status("That is your own link");
                } else {
                node_.add_contact(id, alias);
                std::string err;
                if (node_.connect_uri(invite_input_, err)) {
                    select_conversation(ConversationKind::Direct, id);
                    pending_relay_id_ = id;
                    pending_relay_at_ = p2p::now_ms();
                    pending_relay_ = true;
                    set_status("HELLO sent to " + alias);
                    ImGui::CloseCurrentPopup();
                } else {
                    set_status("Connect: " + err);
                    add_contact_error_ = "Connect failed: " + err;
                }
                }  // not own link
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Copy my link")) copy_text(node_.share_uri(), "Contact link");
        if (!add_contact_error_.empty())
            ImGui::TextColored(ImVec4(0.85f, 0.2f, 0.2f, 1.0f), "%s", add_contact_error_.c_str());
        if (!relay_.empty()) {
            ImGui::Spacing();
            if (ImGui::Button("Connect via selected relay", ImVec2(210.0f, 28.0f))) {
                std::string host;
                uint16_t port = 0;
                p2p::NodeId id;
                if (p2p::Node::parse_invite(invite_input_, id, host, port)) {
                    std::string alias = p2p::Node::parse_invite_name(invite_input_);
                    if (alias.empty()) {
                        alias = id.short_hex(8);
                        for (const auto& ct : contacts_)
                            if (ct.id == id && !ct.alias.empty()) alias = ct.alias;
                    }
                    if (id == node_.identity().id) {
                        set_status("That is your own link");
                        add_contact_error_ = "This is your own link. Send it to your contact instead.";
                    } else {
                    node_.add_contact(id, alias);
                    std::string err;
                    if (node_.connect_via_relay(relay_, id, &err)) {
                        select_conversation(ConversationKind::Direct, id);
                        set_status("Relay HELLO sent to " + alias);
                        ImGui::CloseCurrentPopup();
                    } else {
                        set_status("Relay: " + err);
                    }
                    }  // not own link
                } else {
                    set_status("Invalid invite URI");
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled("via %s", peer_alias(relay_).c_str());
        }
    } else {
        ImGui::TextDisabled("Shared PIN / label (both peers must set it)");
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##pinadd", "e.g. lab-demo-42", pin_input_,
                                 sizeof(pin_input_));
        if (ImGui::Button("Store PIN locally", ImVec2(150.0f, 28.0f))) {
            node_.set_pairing_code(pin_input_);
            std::snprintf(pair_code_, sizeof(pair_code_), "%s", pin_input_);
            set_status("Pairing PIN set and published");
        }
        ImGui::SameLine();
        if (ImGui::Button("Discover in DHT", ImVec2(150.0f, 28.0f))) {
            node_.set_pairing_code(pin_input_);
            std::string err;
            if (node_.search_pairing_code(pin_input_, err)) {
                set_status("Pairing PIN resolved; HELLO sent");
                ImGui::CloseCurrentPopup();
            } else {
                set_status("Discovery: " + err);
            }
        }
        ImGui::TextWrapped("Requires an existing bootstrap peer. The PIN is used only to publish "
                           "and find an invite record; the HELLO MAC verifies both sides.");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("My NodeID: %s", node_.identity().id.hex().c_str());
    if (ImGui::Button("Close", ImVec2(90.0f, 28.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::draw_new_group_modal() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(540.0f, 0.0f), ImVec2(540.0f, 2000.0f));
    if (!ImGui::BeginPopupModal("New group", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextColored(c_accent, "CREATE AN ENCRYPTED GROUP");
    ImGui::TextWrapped("Members receive a random group key inside their pairwise ratcheted "
                       "session. Group voice uses Full Mesh for up to 4 peers and a ranked "
                       "P2P-SFU hub above 4.");
    ImGui::TextDisabled("Group name");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##gname", "e.g. Thesis demo", group_name_, sizeof(group_name_));
    ImGui::Spacing();
    ImGui::Text("Select members");
    ImGui::BeginChild("group_members", ImVec2(0, 210.0f), ImGuiChildFlags_Borders);
    for (const auto& c : contacts_) {
        bool checked = group_selected_.find(c.id) != group_selected_.end();
        std::string label = c.alias.empty() ? c.id.short_hex(8) : c.alias;
        if (ImGui::Checkbox((label + "  (" + c.id.short_hex(6) + ")").c_str(), &checked)) {
            if (checked)
                group_selected_.insert(c.id);
            else
                group_selected_.erase(c.id);
        }
    }
    if (contacts_.empty()) ImGui::TextDisabled("No contacts yet");
    ImGui::EndChild();
    if (ImGui::Button("Create", ImVec2(110.0f, 28.0f))) {
        if (group_name_[0] == 0) {
            // No name typed: use a readable default instead of silently doing nothing.
            std::snprintf(group_name_, sizeof(group_name_), "Group %zu", node_.groups().size() + 1);
        }
        if (group_selected_.empty()) {
            set_status("Select at least one member");
        } else {
            std::vector<p2p::NodeId> members(group_selected_.begin(), group_selected_.end());
            p2p::NodeId id;
            std::string err;
            if (node_.create_group(group_name_, members, id, &err)) {
                select_conversation(ConversationKind::Group, id);
                set_status("Group created and invites sent");
                ImGui::CloseCurrentPopup();
            } else {
                set_status("Group: " + err);
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90.0f, 28.0f))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void App::draw_ui() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    if (!fullscreen_) {
        ImGui::GetForegroundDrawList()->AddRect(
            vp->Pos, ImVec2(vp->Pos.x + vp->Size.x - 1.0f, vp->Pos.y + vp->Size.y - 1.0f),
            ImGui::GetColorU32(ImGuiCol_Border), 0.0f, ImDrawFlags_None, 1.0f);
    }

    if (next_tab_ >= 0) {
        if (next_tab_ == 0) {
            inspector_open_ = true;
            inspector_tab_ = 1;
        } else if (next_tab_ == 2) {
            inspector_open_ = true;
            inspector_tab_ = 0;
        } else if (next_tab_ == 3) {
            inspector_open_ = true;
            inspector_tab_ = 1;
        }
        next_tab_ = -1;
    }

    draw_title_bar();

    float footer = 78.0f;
    float body_h = ImGui::GetContentRegionAvail().y - footer;
    if (body_h < 120.0f) body_h = 120.0f;
    ImGui::BeginChild("body", ImVec2(0, body_h), ImGuiChildFlags_None);
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = ImGui::GetStyle().ItemSpacing.x;
    float sidebar_w = 258.0f;
    float inspector_w = inspector_open_ ? 334.0f : 0.0f;
    if (avail < 900.0f) {
        sidebar_w = 226.0f;
        inspector_w = 0.0f;
    }
    float center_w = avail - sidebar_w - inspector_w - (inspector_open_ ? 2.0f * gap : gap);
    if (center_w < 280.0f) center_w = 280.0f;

    ImGui::BeginChild("sidebar_host", ImVec2(sidebar_w, 0), ImGuiChildFlags_None);
    draw_sidebar();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("center_host", ImVec2(center_w, 0), ImGuiChildFlags_None);
    draw_conversation();
    ImGui::EndChild();

    if (inspector_open_ && inspector_w > 0.0f) {
        ImGui::SameLine();
        ImGui::BeginChild("inspector_host", ImVec2(inspector_w, 0), ImGuiChildFlags_None);
        draw_inspector();
        ImGui::EndChild();
    }
    ImGui::EndChild();

    draw_audio_bar();
    ImGui::SameLine();
    if (voice_.ptt_mode() && !ImGui::GetIO().WantTextInput)
        voice_.set_ptt_pressed(ImGui::IsKeyDown(ImGuiKey_Space) || ptt_button_active_);
    if (ImGui::GetIO().WantTextInput || !voice_.ptt_mode())
        voice_.set_ptt_pressed(false);

    ImGui::Separator();
    ImGui::TextDisabled("%s", status_.c_str());
    ImGui::SameLine(ImGui::GetWindowWidth() - 390.0f);
    ImGui::TextDisabled("%zu contacts  ·  %zu peers  ·  %zu groups  ·  %llu TX / %llu RX",
                        contacts_.size(), peers_.size(), groups_.size(),
                        static_cast<unsigned long long>(stats_.tx_packets),
                        static_cast<unsigned long long>(stats_.rx_packets));

    if (show_add_contact_) {
        ImGui::OpenPopup("Add contact");
        show_add_contact_ = false;
    }
    if (show_new_group_) {
        ImGui::OpenPopup("New group");
        show_new_group_ = false;
    }
    if (show_profile_) {
        ImGui::OpenPopup("Edit profile");
        show_profile_ = false;
    }
    if (show_file_) {
        ImGui::OpenPopup("Send file");
        show_file_ = false;
    }
    draw_add_contact_modal();
    draw_new_group_modal();
    draw_profile_modal();
    draw_file_modal();

    ImGui::End();
}

