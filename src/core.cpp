#include "core.h"
#include <shlobj.h>
#include <shellapi.h>
#include <netlistmgr.h>
#include <algorithm>
#include <cwctype>
#include <sstream>
#include <functional>
#include <iterator>

namespace snooze {
std::wstring wide(const std::string& text, UINT page) {
    if (text.empty())
        return {};
    int n = MultiByteToWideChar(page, page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, text.data(),
                                static_cast<int>(text.size()), nullptr, 0);
    if (!n)
        fail(L"Invalid text encoding.");
    std::wstring result(n, L'\0');
    MultiByteToWideChar(page, page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, text.data(),
                        static_cast<int>(text.size()), result.data(), n);
    return result;
}
std::string utf8(const std::wstring& text) {
    if (text.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!n)
        throw std::runtime_error("Invalid Unicode text.");
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), n,
                        nullptr, nullptr);
    return result;
}
[[noreturn]] void fail(const std::wstring& message) {
    throw std::runtime_error(utf8(message));
}
std::wstring error_text(const std::wstring& action, DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring result = action + L": " + (message ? trim(message) : std::to_wstring(code));
    if (message)
        LocalFree(message);
    return result;
}
std::wstring trim(std::wstring value) {
    auto first = value.find_first_not_of(L" \t\r\n\ufeff");
    if (first == std::wstring::npos)
        return {};
    return value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
}
std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    return value;
}
bool equal(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
std::wstring expand(const std::wstring& value) {
    DWORD size = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (!size)
        fail(error_text(L"Expand path"));
    std::wstring result(size, L'\0');
    if (!ExpandEnvironmentStringsW(value.c_str(), result.data(), size))
        fail(error_text(L"Expand path"));
    result.resize(size - 1);
    return result;
}
fs::path executable_path() {
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n == path.size())
        fail(error_text(L"Find executable"));
    path.resize(n);
    return path;
}
fs::path local_data() {
    PWSTR path = nullptr;
    HRESULT hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path);
    if (FAILED(hr))
        fail(L"Cannot locate your Local AppData folder.");
    fs::path result(path);
    CoTaskMemFree(path);
    return result / L"ZaloSnooze";
}
std::wstring quote_arg(const std::wstring& value) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"')
            result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
std::wstring read_text(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return {};
        fail(error_text(L"Read " + path.wstring(), error));
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size))
        fail(error_text(L"Read file size"));
    if (size.QuadPart > 2 * 1024 * 1024)
        fail(L"Configuration file exceeds 2 MB.");
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD count = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) ||
        count != bytes.size())
        fail(error_text(L"Read configuration"));
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
        static_cast<unsigned char>(bytes[1]) == 0xfe) {
        if ((bytes.size() - 2) % 2)
            fail(L"Invalid UTF-16 configuration.");
        std::wstring result((bytes.size() - 2) / 2, L'\0');
        memcpy(result.data(), bytes.data() + 2, bytes.size() - 2);
        return result;
    }
    if (bytes.compare(0, 3, "\xef\xbb\xbf") == 0)
        bytes.erase(0, 3);
    try {
        return wide(bytes);
    } catch (const std::runtime_error&) {
        return wide(bytes, CP_ACP);
    }
}
void write_atomic(const fs::path& path, const std::wstring& text) {
    fs::create_directories(path.parent_path());
    fs::path temp = path.wstring() + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(GetTickCount64()) + L".tmp";
    try {
        Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.value == INVALID_HANDLE_VALUE)
            fail(error_text(L"Save settings. Move portable app to a writable folder if necessary"));
        std::string bytes = "\xef\xbb\xbf" + utf8(text);
        DWORD count = 0;
        if (!WriteFile(file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &count,
                       nullptr) ||
            count != bytes.size() || !FlushFileBuffers(file.value))
            fail(error_text(L"Write settings"));
        CloseHandle(file.value);
        file.value = nullptr;
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                fail(error_text(L"Read settings", error));
            if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH))
                fail(error_text(L"Replace settings"));
        } else if (!ReplaceFileW(path.c_str(), temp.c_str(), nullptr,
                                 REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            auto error = GetLastError();
            if (error != ERROR_NOT_SUPPORTED && error != ERROR_INVALID_FUNCTION)
                fail(error_text(L"Replace settings", error));
            if (!MoveFileExW(temp.c_str(), path.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                fail(error_text(L"Replace settings"));
        }
    } catch (...) {
        DeleteFileW(temp.c_str());
        throw;
    }
}
std::vector<std::wstring> split_list(const std::wstring& value) {
    std::vector<std::wstring> result;
    std::wstring item;
    bool quoted = false;
    auto add = [&] {
        auto v = trim(item);
        if (!v.empty() &&
            std::none_of(result.begin(), result.end(), [&](const auto& x) { return equal(x, v); }))
            result.push_back(v);
        item.clear();
    };
    for (size_t i = 0; i < value.size(); ++i) {
        wchar_t c = value[i];
        if (c == L'"') {
            if (quoted && i + 1 < value.size() && value[i + 1] == L'"') {
                item += c;
                ++i;
            } else
                quoted = !quoted;
        } else if (c == L',' && !quoted)
            add();
        else
            item += c;
    }
    if (quoted)
        fail(L"Unclosed quote in configuration list.");
    add();
    return result;
}
std::wstring join_list(const std::vector<std::wstring>& values) {
    std::wstring out;
    for (const auto& value : values) {
        if (!out.empty())
            out += L',';
        out += L'"';
        for (auto c : value) {
            if (c == L'"')
                out += c;
            out += c;
        }
        out += L'"';
    }
    return out;
}
static fs::path normalized(const fs::path& path) {
    return fs::absolute(path).lexically_normal();
}
bool same_installation(const fs::path& configured, const fs::path& running) {
    auto a = normalized(configured), b = normalized(running);
    if (equal(a.wstring(), b.wstring()))
        return true;
    auto folder = lower(b.parent_path().filename().wstring());
    auto prefix = lower(a.stem().wstring()) + L"-";
    if (!equal(a.filename().wstring(), b.filename().wstring()) ||
        !equal(a.parent_path().wstring(), b.parent_path().parent_path().wstring()) ||
        folder.find(prefix) != 0 || folder.size() <= prefix.size())
        return false;
    bool digit = false;
    for (auto c : folder.substr(prefix.size())) {
        if (c >= L'0' && c <= L'9')
            digit = true;
        else if (c == L'.' && digit)
            digit = false;
        else
            return false;
    }
    return digit;
}
std::wstring extract_exe(const std::wstring& command) {
    auto value = trim(expand(command));
    if (value.empty())
        return {};
    if (value.front() == L'"') {
        size_t end = value.find(L'"', 1);
        return end == std::wstring::npos ? L"" : value.substr(1, end - 1);
    }
    auto folded = lower(value);
    size_t end = folded.find(L".exe");
    std::wstring first;
    for (; end != std::wstring::npos; end = folded.find(L".exe", end + 4)) {
        auto candidate = value.substr(0, end + 4);
        if (first.empty())
            first = candidate;
        if (fs::is_regular_file(candidate))
            return candidate;
    }
    return first;
}
std::wstring process_path(DWORD pid) {
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process.value)
        return {};
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &size))
        return {};
    path.resize(size);
    return path;
}
InternetState internet_state() {
    INetworkListManager* manager = nullptr;
    if (FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL,
                                IID_INetworkListManager, reinterpret_cast<void**>(&manager))))
        return InternetState::Unknown;
    NLM_CONNECTIVITY connectivity = NLM_CONNECTIVITY_DISCONNECTED;
    auto result = manager->GetConnectivity(&connectivity);
    manager->Release();
    if (FAILED(result))
        return InternetState::Unknown;
    return connectivity & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)
               ? InternetState::Online
               : InternetState::Offline;
}
static void add_installation(std::vector<std::wstring>& paths, const std::wstring& candidate) {
    auto match = std::find_if(paths.begin(), paths.end(), [&](const auto& path) {
        return same_installation(path, candidate) || same_installation(candidate, path);
    });
    if (match == paths.end()) {
        paths.push_back(candidate);
        return;
    }
    // Prefer the stable launcher over a versioned child, regardless of discovery order.
    const auto chosen = same_installation(*match, candidate) ? *match : candidate;
    *match = chosen;
    paths.erase(std::remove_if(std::next(match), paths.end(),
                               [&](const auto& path) { return same_installation(chosen, path); }),
                paths.end());
}
bool zalo_executable(const std::wstring& path) {
    return !path.empty() && equal(fs::path(path).filename().wstring(), L"Zalo.exe") &&
           fs::is_regular_file(path);
}
std::vector<std::wstring> standard_zalo_paths() {
    std::vector<std::wstring> found;
    for (const auto& relative :
         {L"%LOCALAPPDATA%\\Programs\\Zalo\\Zalo.exe", L"%ProgramW6432%\\Zalo\\Zalo.exe",
          L"%ProgramFiles%\\Zalo\\Zalo.exe", L"%ProgramFiles(x86)%\\Zalo\\Zalo.exe"}) {
        auto path = expand(relative);
        if (zalo_executable(path))
            add_installation(found, path);
    }
    return found;
}
std::vector<std::wstring> detect_zalo() {
    std::vector<std::wstring> found;
    auto add = [&](const std::wstring& path) {
        if (zalo_executable(path))
            add_installation(found, path);
    };
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                          0, KEY_READ | view, &key) != ERROR_SUCCESS)
            continue;
        for (DWORD i = 0;; ++i) {
            wchar_t name[16384];
            BYTE data[32768];
            DWORD n = 16384, bytes = sizeof(data), type = 0;
            LSTATUS status = RegEnumValueW(key, i, name, &n, nullptr, &type, data, &bytes);
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            if (status == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) &&
                bytes >= 2) {
                std::wstring command(reinterpret_cast<wchar_t*>(data), bytes / 2);
                auto zero = command.find(L'\0');
                if (zero != std::wstring::npos)
                    command.resize(zero);
                add(extract_exe(command));
            }
        }
        RegCloseKey(key);
    }
    for (const auto& path : standard_zalo_paths())
        add(path);
    return found;
}
void open_target(HWND owner, const std::wstring& target) {
    auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(owner, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32)
        fail(L"Windows could not open: " + target);
}
Paths Paths::for_executable(const fs::path& exe) {
    Paths result;
    result.exe = normalized(exe);
    result.directory = result.exe.parent_path();
    result.installed =
        trim(read_text(result.directory / L"installed.ini")) == L"ZaloSnooze.InstallMode=Installed";
    auto data = local_data();
    result.config = (result.installed ? data : result.directory) / L"ZaloSnooze.ini";
    result.recovery = data / L"startup-state";
    return result;
}
ConfigLock::ConfigLock(const fs::path& path) {
    auto name =
        L"Global\\ZaloSnooze.Settings." +
        std::to_wstring(std::hash<std::wstring>{}(lower(fs::weakly_canonical(path).wstring())));
    mutex.value = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!mutex.value)
        fail(error_text(L"Lock settings"));
    auto wait = WaitForSingleObject(mutex.value, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        fail(L"Settings are already open for this configuration. Close the editor or finish setup, "
             L"then try again.");
}
ConfigLock::~ConfigLock() {
    ReleaseMutex(mutex.value);
}
static int positive(const std::wstring& value, int maximum = 86400) {
    size_t end = 0;
    int n = std::stoi(value, &end);
    if (end != value.size() || n <= 0 || n > maximum)
        fail(L"Value must be a positive integer within its documented limit.");
    return n;
}
static std::wstring unquote(std::wstring value) {
    if (value.size() >= 2 && (value.front() == L'"' || value.front() == L'\'') &&
        value.back() == value.front())
        return value.substr(1, value.size() - 2);
    return value;
}
void Config::load(const fs::path& path) {
    std::wistringstream input(read_text(path));
    std::wstring line;
    while (std::getline(input, line)) {
        line = trim(line);
        auto eq = line.find(L'=');
        if (line.empty() || line[0] == L';' || line[0] == L'#' || eq == std::wstring::npos)
            continue;
        auto key = lower(trim(line.substr(0, eq))), raw = trim(line.substr(eq + 1)),
             value = unquote(raw);
        try {
            if (key == L"zaloexe")
                executables = split_list(raw);
            else if (key == L"processname")
                processes = split_list(raw);
            else if (key == L"language" && (value == L"vi" || value == L"en" || value == L"both"))
                language = value;
            else if (key == L"windowclass")
                window_class = value;
            else if (key == L"logfile")
                log_file = value;
            else if (key == L"graceseconds")
                grace = positive(value);
            else if (key == L"maxseconds")
                maximum = positive(value);
            else if (key == L"minwidth")
                min_width = positive(value, 10000);
            else if (key == L"minheight")
                min_height = positive(value, 10000);
            else if (key == L"launchatstartup")
                launch_at_startup = equal(value, L"true");
            else if (key == L"minimizetotaskbar")
                minimize_to_taskbar = equal(value, L"true");
            else if (key == L"experimentalopacity")
                experimental_opacity = equal(value, L"true");
        } catch (
            const std::exception&) { /* Malformed values keep defaults; unreadable files do not. */
        }
    }
}
void Config::validate() const {
    for (const auto& path : executables)
        if (!equal(fs::path(expand(path)).extension().wstring(), L".exe") ||
            path.find_first_of(L"\r\n\"") != std::wstring::npos)
            fail(L"Each Zalo path must name an .exe file without quotes or newlines.");
    for (const auto& name : processes)
        if (name.find_first_of(L"\\/:\r\n\"") != std::wstring::npos)
            fail(L"Process overrides must be file names, not paths.");
    if (window_class.find_first_of(L"\r\n\"") != std::wstring::npos ||
        log_file.find_first_of(L"\r\n\"") != std::wstring::npos)
        fail(L"Settings cannot contain quotes or newlines.");
    positive(std::to_wstring(grace));
    positive(std::to_wstring(maximum));
    positive(std::to_wstring(min_width), 10000);
    positive(std::to_wstring(min_height), 10000);
    if (language != L"vi" && language != L"en" && language != L"both")
        fail(L"Unsupported language.");
}
void Config::save(const fs::path& path) const {
    validate();
    std::map<std::wstring, std::wstring> values = {
        {L"ZaloExe", join_list(executables)},
        {L"ProcessName", join_list(processes)},
        {L"Language", language},
        {L"WindowClass", L"\"" + window_class + L"\""},
        {L"LogFile", L"\"" + log_file + L"\""},
        {L"GraceSeconds", std::to_wstring(grace)},
        {L"MaxSeconds", std::to_wstring(maximum)},
        {L"MinWidth", std::to_wstring(min_width)},
        {L"MinHeight", std::to_wstring(min_height)},
        {L"LaunchAtStartup", launch_at_startup ? L"true" : L"false"},
        {L"MinimizeToTaskbar", minimize_to_taskbar ? L"true" : L"false"},
        {L"ExperimentalOpacity", experimental_opacity ? L"true" : L"false"}};
    std::wstring output, line;
    std::wistringstream input(read_text(path));
    while (std::getline(input, line)) {
        auto clean = trim(line);
        auto eq = clean.find(L'=');
        bool replace = false;
        if (!clean.empty() && clean[0] != L';' && clean[0] != L'#' && eq != std::wstring::npos)
            for (const auto& item : values)
                if (equal(trim(clean.substr(0, eq)), item.first)) {
                    replace = true;
                    break;
                }
        if (!replace) {
            if (!line.empty() && line.back() == L'\r')
                line.pop_back();
            output += line + L"\r\n";
        }
    }
    for (const auto& item : values)
        output += item.first + L"=" + item.second + L"\r\n";
    write_atomic(path, output);
}
Config Config::resolved(const Paths& paths) const {
    validate();
    Config result = *this;
    if (result.executables.empty() && result.processes.empty())
        result.executables = detect_zalo();
    std::vector<std::wstring> installations;
    for (const auto& p : result.executables) {
        fs::path expanded(expand(p));
        add_installation(
            installations,
            normalized(expanded.is_absolute() ? expanded : paths.directory / expanded).wstring());
    }
    result.executables = std::move(installations);
    if (!result.log_file.empty()) {
        fs::path log(expand(result.log_file));
        result.log_file =
            normalized(log.is_absolute() ? log : paths.config.parent_path() / log).wstring();
        auto log_path = fs::weakly_canonical(result.log_file);
        auto recovery = fs::weakly_canonical(paths.recovery);
        auto inside = log_path;
        while (!inside.empty()) {
            if (equal(inside.wstring(), recovery.wstring()))
                fail(L"Diagnostic logs cannot use the startup recovery folder.");
            auto parent = inside.parent_path();
            if (parent == inside)
                break;
            inside = parent;
        }
        auto protected_file = [&](const fs::path& file) {
            std::error_code error;
            if (equal(log_path.wstring(), fs::weakly_canonical(file).wstring()) ||
                fs::equivalent(log_path, file, error))
                fail(L"Diagnostic logs cannot overwrite settings or application files.");
        };
        protected_file(paths.config);
        protected_file(paths.exe);
        protected_file(paths.directory / L"installed.ini");
        protected_file(paths.recovery.parent_path() / L"legacy-startup-shortcut.lnk");
        for (const auto& exe : result.executables)
            protected_file(exe);
        if (fs::exists(log_path) && fs::exists(recovery))
            for (const auto& file : fs::recursive_directory_iterator(recovery))
                if (file.is_regular_file())
                    protected_file(file.path());
    }
    return result;
}
bool WindowState::observe(bool is_visible, ULONGLONG now) {
    if (is_visible)
        hidden_seen = false;
    else if (!hidden_seen) {
        hidden_since = now;
        hidden_seen = true;
    }
    if (pending && is_visible && (!visible || now - last_request >= 1000))
        retry = true;
    stable = !is_visible && (initially_hidden || pending || hides > 0) && (pending || !retry) &&
             now - hidden_since >= 1000;
    if (pending && stable) {
        ++hides;
        pending = false;
        retry = false;
    }
    bool close = (is_visible || (!pending && retry)) && (!pending || retry) && hides < 3 &&
                 (!observed || !visible || retry) && (!requested || now - last_request >= 250);
    observed = true;
    visible = is_visible;
    return close;
}
void WindowState::request_result(bool accepted, ULONGLONG now) {
    requested = true;
    last_request = now;
    pending = accepted;
    retry = !accepted;
}
bool main_window(const std::wstring& title, bool owned) {
    return !owned && (title == L"Zalo" || title.find(L"Zalo - ") == 0);
}
} // namespace snooze
