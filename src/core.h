#pragma once
#include <windows.h>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <functional>

namespace snooze {
namespace fs = std::filesystem;
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(HANDLE h = nullptr) : value(h) {}
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
std::wstring error_text(const std::wstring& action, DWORD code = GetLastError());
[[noreturn]] void fail(const std::wstring& message);
std::wstring wide(const std::string& text, UINT page = CP_UTF8);
std::string utf8(const std::wstring& text);
std::wstring trim(std::wstring value);
std::wstring lower(std::wstring value);
bool equal(const std::wstring& a, const std::wstring& b);
std::wstring expand(const std::wstring& value);
fs::path executable_path();
fs::path local_data();
std::wstring quote_arg(const std::wstring& value);
std::wstring read_text(const fs::path& path);
void write_atomic(const fs::path& path, const std::wstring& text);
std::vector<std::wstring> split_list(const std::wstring& value);
std::wstring join_list(const std::vector<std::wstring>& values);
bool same_installation(const fs::path& configured, const fs::path& running);
std::wstring extract_exe(const std::wstring& command);
std::wstring process_path(DWORD pid);
enum class InternetState { Offline, Online, Unknown };
InternetState internet_state();
std::vector<std::wstring> detect_zalo();
std::vector<std::wstring> standard_zalo_paths();
bool zalo_executable(const std::wstring& path);
void open_target(HWND owner, const std::wstring& target);

struct Paths {
    fs::path exe;
    fs::path directory;
    fs::path config;
    fs::path recovery;
    bool installed = false;
    static Paths for_executable(const fs::path& exe);
};
struct ConfigLock {
    Handle mutex;
    explicit ConfigLock(const fs::path& path);
    ~ConfigLock();
};
struct Config {
    std::vector<std::wstring> executables;
    std::vector<std::wstring> processes;
    std::wstring language = L"vi";
    std::wstring window_class = L"Chrome_WidgetWin_1";
    std::wstring log_file;
    int grace = 45, maximum = 120, min_width = 350, min_height = 300;
    bool launch_at_startup = false;
    bool minimize_to_taskbar = false;
    bool experimental_opacity = false;
    void load(const fs::path& path);
    void save(const fs::path& path) const;
    void validate() const;
    Config resolved(const Paths& paths) const;
};
struct WindowState {
    DWORD pid;
    int hides = 0;
    bool visible = false, stable = false;
    bool initially_hidden, observed = false, pending = false, retry = false, requested = false;
    ULONGLONG last_request = 0, hidden_since = 0;
    bool hidden_seen = false;
    explicit WindowState(DWORD p = 0, bool hidden = false) : pid(p), initially_hidden(hidden) {}
    bool observe(bool is_visible, ULONGLONG now);
    void request_result(bool accepted, ULONGLONG now);
};
bool main_window(const std::wstring& title, bool owned);
int run_watcher(const Config& config, const Paths& paths, bool automatic_startup = false,
                const std::function<InternetState()>& internet = {});
int show_gui(const Paths& paths);
} // namespace snooze
