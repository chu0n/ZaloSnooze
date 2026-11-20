#include "core.h"
#include "startup.h"
#include "version.h"
#include <tlhelp32.h>
#include <algorithm>
#include <set>

namespace snooze {
namespace {
struct Opacity {
    DWORD pid = 0, color = 0, flags = 0;
    BYTE alpha = 255;
    bool active = false;
    bool same(HWND window) const {
        DWORD current = 0;
        GetWindowThreadProcessId(window, &current);
        return current == pid;
    }
    void suppress(HWND window) {
        if (!same(window) || !(GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_LAYERED))
            return;
        COLORREF current_color;
        BYTE current_alpha;
        DWORD current_flags;
        if (!GetLayeredWindowAttributes(window, &current_color, &current_alpha, &current_flags))
            return;
        if (active && current_alpha == 0 && current_color == color &&
            current_flags == (flags | LWA_ALPHA))
            return;
        color = current_color;
        alpha = current_alpha;
        flags = current_flags;
        active = SetLayeredWindowAttributes(window, color, 0, flags | LWA_ALPHA);
    }
    bool restore(HWND window) {
        if (!active || !same(window) || !(GetWindowLongW(window, GWL_EXSTYLE) & WS_EX_LAYERED)) {
            active = false;
            return true;
        }
        COLORREF current_color;
        BYTE current_alpha;
        DWORD current_flags;
        if (!GetLayeredWindowAttributes(window, &current_color, &current_alpha, &current_flags))
            return false;
        if (current_alpha == 0 && current_color == color && current_flags == (flags | LWA_ALPHA))
            if (!SetLayeredWindowAttributes(window, color, alpha, flags))
                return false;
        active = false;
        return true;
    }
};
struct Tracked {
    WindowState state;
    Opacity opacity;
};
struct Watcher {
    Config config;
    DWORD session = 0;
    ULONGLONG start = 0;
    std::map<DWORD, std::wstring> processes;
    std::map<HWND, Tracked> windows;
    std::set<HWND> seen;
    std::set<DWORD> rejected;
    HWINEVENTHOOK hook = nullptr, names = nullptr;
    bool initial = true, finishing = false, saw_window = false, acted = false;
    explicit Watcher(Config c) : config(std::move(c)) {
        ProcessIdToSessionId(GetCurrentProcessId(), &session);
    }
    ~Watcher() {
        if (hook)
            UnhookWinEvent(hook);
        if (names)
            UnhookWinEvent(names);
        for (auto& entry : windows)
            entry.second.opacity.restore(entry.first);
    }
    void log(const std::wstring& text) const {
        if (config.log_file.empty())
            return;
        try {
            fs::path path(config.log_file);
            fs::create_directories(path.parent_path());
            if (fs::exists(path) && fs::file_size(path) > 1024 * 1024) {
                fs::path old = path.wstring() + L".old";
                if (!MoveFileExW(path.c_str(), old.c_str(), 0))
                    return;
            }
            Handle file(CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                                    OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file.value == INVALID_HANDLE_VALUE)
                return;
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t time[64];
            swprintf(time, 64, L"%04u-%02u-%02u %02u:%02u:%02u.%03u ", t.wYear, t.wMonth, t.wDay,
                     t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
            std::string data = utf8(std::wstring(time) + text + L"\r\n");
            DWORD written;
            WriteFile(file.value, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        } catch (...) { /* Diagnostics must not stop window cleanup. */
        }
    }
    bool target_matches(const std::wstring& target, const std::wstring& path) const {
        return config.processes.empty()
                   ? same_installation(target, path)
                   : equal(fs::path(target).stem().wstring(), fs::path(path).stem().wstring());
    }
    bool matches(DWORD pid, const std::wstring& path) const {
        DWORD other_session = 0;
        if (!ProcessIdToSessionId(pid, &other_session) || other_session != session || path.empty())
            return false;
        const auto& targets = config.processes.empty() ? config.executables : config.processes;
        return std::any_of(targets.begin(), targets.end(),
                           [&](const auto& target) { return target_matches(target, path); });
    }
    void gather() {
        processes.clear();
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (snapshot.value == INVALID_HANDLE_VALUE)
            fail(error_text(L"List processes"));
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Process32FirstW(snapshot.value, &entry))
            return;
        do {
            bool candidate = std::any_of(
                config.executables.begin(), config.executables.end(), [&](const auto& exe) {
                    return equal(fs::path(exe).filename().wstring(), entry.szExeFile);
                });
            if (!config.processes.empty())
                candidate = std::any_of(
                    config.processes.begin(), config.processes.end(), [&](const auto& name) {
                        return equal(fs::path(name).stem().wstring(),
                                     fs::path(entry.szExeFile).stem().wstring());
                    });
            if (!candidate)
                continue;
            auto path = process_path(entry.th32ProcessID);
            if (matches(entry.th32ProcessID, path))
                processes[entry.th32ProcessID] = path;
        } while (Process32NextW(snapshot.value, &entry));
    }
    bool launch_missing() {
        bool success = true;
        for (const auto& exe : config.executables) {
            if (std::any_of(processes.begin(), processes.end(),
                            [&](const auto& p) { return same_installation(exe, p.second); }))
                continue;
            if (!fs::is_regular_file(exe)) {
                log(L"Missing executable: " + exe);
                success = false;
                continue;
            }
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
            info.lpFile = exe.c_str();
            auto directory = fs::path(exe).parent_path().wstring();
            info.lpDirectory = directory.c_str();
            info.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&info)) {
                log(error_text(L"Launch " + exe));
                success = false;
            } else {
                if (info.hProcess)
                    CloseHandle(info.hProcess);
                log(L"Launching: " + exe);
            }
        }
        return success;
    }
    void handle(HWND window, bool enumeration) {
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        if (!processes.count(pid)) {
            if (enumeration && rejected.count(pid))
                return;
            auto path = process_path(pid);
            if (!matches(pid, path)) {
                if (enumeration)
                    rejected.insert(pid);
                return;
            }
            processes[pid] = path;
        }
        wchar_t title[512]{}, cls[256]{};
        GetWindowTextW(window, title, 512);
        GetClassNameW(window, cls, 256);
        auto old = windows.find(window);
        bool known = old != windows.end() && old->second.state.pid == pid;
        if (GetWindow(window, GW_OWNER) || (!known && !main_window(title, false)))
            return;
        if (!config.window_class.empty() && !equal(config.window_class, L"none") &&
            !equal(config.window_class, cls))
            return;
        RECT rect{};
        if (!GetWindowRect(window, &rect))
            return;
        if (IsIconic(window)) {
            WINDOWPLACEMENT p{};
            p.length = sizeof(p);
            if (!GetWindowPlacement(window, &p))
                return;
            rect = p.rcNormalPosition;
        }
        if (rect.right - rect.left < config.min_width || rect.bottom - rect.top < config.min_height)
            return;
        bool iconic = IsIconic(window);
        bool shown = IsWindowVisible(window);
        bool visible = shown && !(config.minimize_to_taskbar && iconic);
        if (!known) {
            if (old != windows.end()) {
                old->second.opacity.restore(window);
                windows.erase(old);
            }
            Tracked tracked;
            tracked.state = WindowState(pid, initial && enumeration && !visible &&
                                                 (!config.minimize_to_taskbar || shown));
            tracked.opacity.pid = pid;
            windows.emplace(window, tracked);
        }
        auto& tracked = windows.at(window);
        auto& state = tracked.state;
        if (enumeration)
            seen.insert(window);
        if (!finishing && state.hides < 3 && config.experimental_opacity &&
            !config.minimize_to_taskbar)
            tracked.opacity.suppress(window);
        else
            tracked.opacity.restore(window);
        saw_window = true;
        if (config.minimize_to_taskbar && !shown) {
            state.stable = false;
            state.hidden_seen = false;
            return;
        }
        int before = state.hides;
        bool close = state.observe(visible, GetTickCount64() - start);
        if (before != state.hides) {
            acted = true;
            log((config.minimize_to_taskbar ? L"Confirmed minimize to taskbar: PID="
                                            : L"Confirmed Close-to-tray: PID=") +
                std::to_wstring(pid));
        }
        if (config.minimize_to_taskbar) {
            if (visible && close && !finishing) {
                bool accepted = ShowWindowAsync(window, SW_SHOWMINNOACTIVE) != FALSE;
                state.request_result(accepted, GetTickCount64() - start);
                acted = true;
                log(accepted ? L"Minimize-to-taskbar request queued."
                             : error_text(L"Minimize to taskbar"));
            }
        } else {
            if (visible && state.hides < 3 && !finishing)
                ShowWindowAsync(window, SW_HIDE);
            if (close && !finishing) {
                bool accepted = PostMessageW(window, WM_CLOSE, 0, 0);
                state.request_result(accepted, GetTickCount64() - start);
                log(accepted ? L"Close request queued." : error_text(L"Close request failed"));
            }
        }
    }
    static Watcher* active;
    static BOOL CALLBACK enumerate(HWND window, LPARAM data) {
        auto* self = reinterpret_cast<Watcher*>(data);
        try {
            self->handle(window, true);
        } catch (const std::exception& e) {
            self->log(wide(e.what()));
        }
        return TRUE;
    }
    static void CALLBACK event(HWINEVENTHOOK, DWORD type, HWND window, LONG object, LONG child,
                               DWORD, DWORD) {
        if (!active || !window || object != OBJID_WINDOW || child != CHILDID_SELF)
            return;
        try {
            if (type == EVENT_OBJECT_DESTROY) {
                active->windows.erase(window);
                return;
            }
            active->handle(window, false);
        } catch (const std::exception& e) {
            active->log(wide(e.what()));
        }
    }
    void scan() {
        seen.clear();
        rejected.clear();
        if (!EnumWindows(enumerate, reinterpret_cast<LPARAM>(this)))
            fail(error_text(L"Inspect windows"));
        for (auto i = windows.begin(); i != windows.end();) {
            if (!seen.count(i->first)) {
                i->second.opacity.restore(i->first);
                i = windows.erase(i);
            } else
                ++i;
        }
    }
    bool settled() const {
        if (processes.empty())
            return false;
        const auto& targets = config.processes.empty() ? config.executables : config.processes;
        for (const auto& target : targets) {
            bool found = false;
            for (const auto& window : windows) {
                auto process = processes.find(window.second.state.pid);
                if (process == processes.end())
                    continue;
                if (!target_matches(target, process->second))
                    continue;
                if (!window.second.state.stable)
                    return false;
                found = true;
            }
            if (!found)
                return false;
        }
        return !targets.empty();
    }
    int run() {
        start = GetTickCount64();
        active = this;
        struct Reset {
            ~Reset() {
                Watcher::active = nullptr;
            }
        } reset;
        hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr, event, 0, 0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        names = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, nullptr, event, 0,
                                0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        log(L"ZaloSnooze " APP_VERSION_W L" native x86 watcher started.");
        gather();
        scan();
        initial = false;
        bool launched = launch_missing();
        ULONGLONG next_process = 1000, next_launch = 10000, next_scan = 0, quiet_since = 0;
        bool quiet = false;
        while (GetTickCount64() - start < static_cast<ULONGLONG>(config.maximum) * 1000) {
            auto now = GetTickCount64() - start;
            if (now >= next_process) {
                gather();
                next_process = now + 500;
            }
            if (now >= next_launch) {
                launched = launch_missing();
                next_launch = now + 10000;
            }
            if (now >= next_scan) {
                scan();
                next_scan = now + (hook ? 250 : 25);
            }
            if (saw_window && launched && settled()) {
                if (!quiet || acted) {
                    quiet_since = now;
                    quiet = true;
                }
                if (now - quiet_since >= static_cast<ULONGLONG>(config.grace) * 1000)
                    break;
            } else
                quiet = false;
            acted = false;
            MSG message;
            while (GetTickCount64() - start < static_cast<ULONGLONG>(config.maximum) * 1000 &&
                   PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 25, QS_ALLINPUT);
        }
        finishing = true;
        gather();
        scan();
        bool restored = true;
        for (auto& entry : windows)
            if (!entry.second.opacity.restore(entry.first))
                restored = false;
        int result = launched && restored && settled() ? 0 : 1;
        log(result == 0
                ? (config.minimize_to_taskbar
                       ? L"Success: matching Zalo processes remain running with minimized taskbar "
                         L"windows."
                       : L"Success: matching Zalo processes remain running with settled hidden "
                         L"windows.")
                : L"Incomplete: target windows did not settle or a launch/cleanup failed.");
        return result;
    }
};
Watcher* Watcher::active = nullptr;
} // namespace
int run_watcher(const Config& config, const Paths& paths, bool automatic_startup,
                const std::function<InternetState()>& internet) {
    Handle mutex(CreateMutexW(nullptr, FALSE, L"Local\\ZaloSnooze.Watcher"));
    if (!mutex.value)
        fail(error_text(L"Create watcher lock"));
    DWORD wait = WaitForSingleObject(mutex.value, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        return 3;
    struct Release {
        HANDLE mutex;
        ~Release() {
            ReleaseMutex(mutex);
        }
    } release{mutex.value};
    auto resolved = config.resolved(paths);
    if (resolved.executables.empty() && resolved.processes.empty())
        fail(L"Zalo was not found. Open Settings and choose Zalo.exe.");
    Watcher watcher(std::move(resolved));
    if (automatic_startup) {
        bool waited = false;
        auto state = InternetState::Unknown;
        while (startup_enabled(paths)) {
            state = internet ? internet() : internet_state();
            if (state != InternetState::Offline)
                break;
            if (!waited) {
                watcher.log(L"Automatic startup is waiting for Windows to report Internet access.");
                waited = true;
            }
            Sleep(1000);
        }
        if (!startup_enabled(paths)) {
            watcher.log(
                L"Automatic startup cancelled before launch: registration or approval changed.");
            return 0;
        }
        if (state == InternetState::Unknown)
            watcher.log(
                L"Windows Internet state is unavailable; using bounded window observation.");
        else if (waited)
            watcher.log(L"Internet access is available; starting bounded window observation.");
        Config current;
        current.load(paths.config);
        watcher.config = current.resolved(paths);
        if (watcher.config.executables.empty() && watcher.config.processes.empty())
            fail(L"Zalo was not found. Open Settings and choose Zalo.exe.");
    }
    return watcher.run();
}
} // namespace snooze
