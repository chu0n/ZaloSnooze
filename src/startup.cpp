#include "startup.h"
#include <shlobj.h>
#include <taskschd.h>
#include <sddl.h>
#include <algorithm>
#include <cwctype>
#include <sstream>
#include <fstream>
#include <array>

namespace snooze {
std::wstring task_restore_signature(std::wstring xml) {
    auto settings = xml.find(L"<Settings>");
    auto end = xml.find(L"</Settings>", settings);
    auto enabled = xml.find(L"<Enabled>", settings);
    if (enabled != std::wstring::npos && enabled < end) {
        auto close = xml.find(L"</Enabled>", enabled);
        if (close == std::wstring::npos || close > end)
            fail(L"Invalid scheduled-task settings.");
        xml.erase(enabled, close + 10 - enabled);
    }
    std::wstring result;
    for (size_t i = 0; i < xml.size();) {
        if (!result.empty() && result.back() == L'>' && iswspace(xml[i])) {
            auto next = i;
            while (next < xml.size() && iswspace(xml[next]))
                ++next;
            if (next < xml.size() && xml[next] == L'<') {
                i = next;
                continue;
            }
        }
        result += xml[i++];
    }
    return result;
}
namespace {
constexpr auto run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
template <class T> struct Com {
    T* p = nullptr;
    ~Com() {
        if (p)
            p->Release();
    }
    T** address() {
        return &p;
    }
    T* operator->() const {
        return p;
    }
    explicit operator bool() const {
        return p != nullptr;
    }
};
struct Bstr {
    BSTR p = nullptr;
    Bstr() = default;
    explicit Bstr(const std::wstring& s)
        : p(SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {}
    ~Bstr() {
        SysFreeString(p);
    }
    std::wstring text() const {
        return p ? std::wstring(p, SysStringLen(p)) : L"";
    }
};
void check(HRESULT hr, const std::wstring& action) {
    if (FAILED(hr))
        fail(error_text(action, static_cast<DWORD>(hr)));
}
std::wstring hex(const std::string& data) {
    const wchar_t* digits = L"0123456789abcdef";
    std::wstring out;
    for (unsigned char c : data) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}
std::string unhex(const std::wstring& data) {
    if (data.size() % 2)
        fail(L"Invalid startup backup.");
    auto digit = [](wchar_t c) {
        if (c >= L'0' && c <= L'9')
            return c - L'0';
        if (c >= L'a' && c <= L'f')
            return c - L'a' + 10;
        fail(L"Invalid startup backup.");
    };
    std::string out;
    for (size_t i = 0; i < data.size(); i += 2)
        out += static_cast<char>((digit(data[i]) << 4) | digit(data[i + 1]));
    return out;
}
std::wstring encode(const std::wstring& s) {
    return hex(utf8(s));
}
std::wstring decode(const std::wstring& s) {
    return wide(unhex(s));
}
fs::path backup_path(const Paths& paths) {
    return paths.recovery / L"backup.ini";
}
std::wstring backup_header(const Paths& paths) {
    return L"Owner=" + encode(paths.exe.wstring()) + L"\r\n";
}
bool backup_owned_by(const Paths& paths, const std::wstring& data) {
    if (data.find(L"Owner=") != 0)
        fail(L"Invalid startup recovery header. Backup retained.");
    auto end = data.find_first_of(L"\r\n");
    return equal(decode(data.substr(6, end == std::wstring::npos ? end : end - 6)),
                 paths.exe.wstring());
}
std::wstring owned_backup(const Paths& paths) {
    auto data = read_text(backup_path(paths));
    if (!data.empty() && !backup_owned_by(paths, data))
        fail(L"Another ZaloSnooze copy owns the startup backup. Disable startup in that copy "
             L"first.");
    return data;
}
std::wstring shortcut_target(const fs::path& path, std::wstring* arguments = nullptr) {
    Com<IShellLinkW> link;
    check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                           reinterpret_cast<void**>(link.address())),
          L"Read shortcut");
    Com<IPersistFile> persist;
    check(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(persist.address())),
          L"Read shortcut");
    if (FAILED(persist->Load(path.c_str(), STGM_READ)))
        return {};
    wchar_t target[32768]{};
    if (FAILED(link->GetPath(target, 32768, nullptr, SLGP_RAWPATH)))
        return {};
    if (arguments) {
        wchar_t value[32768]{};
        check(link->GetArguments(value, 32768), L"Read shortcut arguments");
        *arguments = value;
    }
    return expand(target);
}
bool current_user(const std::wstring& account) {
    if (account.empty())
        return false;
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
        return false;
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> data(size);
    if (!GetTokenInformation(token.value, TokenUser, data.data(), size, &size))
        return false;
    auto* user = reinterpret_cast<TOKEN_USER*>(data.data());
    PSID sid = nullptr;
    if (ConvertStringSidToSidW(account.c_str(), &sid)) {
        bool match = EqualSid(user->User.Sid, sid);
        LocalFree(sid);
        return match;
    }
    DWORD sid_size = 0, domain_size = 0;
    SID_NAME_USE use;
    LookupAccountNameW(nullptr, account.c_str(), nullptr, &sid_size, nullptr, &domain_size, &use);
    if (!sid_size)
        return false;
    std::vector<BYTE> other(sid_size);
    std::wstring domain(domain_size, L'\0');
    return LookupAccountNameW(nullptr, account.c_str(), other.data(), &sid_size, domain.data(),
                              &domain_size, &use) &&
           EqualSid(user->User.Sid, other.data());
}
bool task_details(IRegisteredTask* task, std::wstring& signature, std::wstring& exe) {
    Com<ITaskDefinition> definition;
    if (FAILED(task->get_Definition(definition.address())))
        return false;
    Com<IPrincipal> principal;
    if (FAILED(definition->get_Principal(principal.address())))
        return false;
    Bstr user;
    if (FAILED(principal->get_UserId(&user.p)) || !current_user(user.text()))
        return false;
    Com<IActionCollection> actions;
    if (FAILED(definition->get_Actions(actions.address())))
        return false;
    LONG count = 0;
    actions->get_Count(&count);
    if (count != 1)
        return false;
    Com<IAction> action;
    if (FAILED(actions->get_Item(1, action.address())))
        return false;
    Com<IExecAction> exec;
    if (FAILED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.address()))))
        return false;
    Bstr path, arguments, directory;
    if (FAILED(exec->get_Path(&path.p)) || FAILED(exec->get_Arguments(&arguments.p)) ||
        FAILED(exec->get_WorkingDirectory(&directory.p)))
        return false;
    exe = expand(path.text());
    if (!zalo_executable(exe))
        return false;
    Bstr xml;
    if (FAILED(definition->get_XmlText(&xml.p)) ||
        xml.text().find(L"<Settings>") == std::wstring::npos)
        return false;
    signature = user.text() + L"\n" + path.text() + L"\n" + arguments.text() + L"\n" +
                directory.text() + L"\n" + task_restore_signature(xml.text());
    return true;
}
void connect_tasks(Com<ITaskService>& service) {
    check(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                           reinterpret_cast<void**>(service.address())),
          L"Open Task Scheduler");
    VARIANT empty;
    VariantInit(&empty);
    check(service->Connect(empty, empty, empty, empty), L"Connect Task Scheduler");
}
void enumerate_tasks(ITaskFolder* folder, std::vector<StartupEntry>& entries, int depth) {
    if (depth > 16)
        return;
    Com<IRegisteredTaskCollection> tasks;
    if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, tasks.address()))) {
        LONG count = 0;
        tasks->get_Count(&count);
        for (LONG i = 1; i <= count; ++i) {
            VARIANT index;
            VariantInit(&index);
            index.vt = VT_I4;
            index.lVal = i;
            Com<IRegisteredTask> task;
            if (FAILED(tasks->get_Item(index, task.address())))
                continue;
            VARIANT_BOOL enabled = VARIANT_FALSE;
            if (FAILED(task->get_Enabled(&enabled)) || enabled != VARIANT_TRUE)
                continue;
            std::wstring signature, exe;
            if (!task_details(task.p, signature, exe))
                continue;
            Com<ITaskDefinition> definition;
            task->get_Definition(definition.address());
            Com<ITriggerCollection> triggers;
            if (!definition || FAILED(definition->get_Triggers(triggers.address())))
                continue;
            LONG n = 0;
            triggers->get_Count(&n);
            bool startup = false;
            for (LONG t = 1; t <= n; ++t) {
                Com<ITrigger> trigger;
                if (SUCCEEDED(triggers->get_Item(t, trigger.address()))) {
                    TASK_TRIGGER_TYPE2 type;
                    if (SUCCEEDED(trigger->get_Type(&type)) && type == TASK_TRIGGER_LOGON)
                        startup = true;
                }
            }
            if (!startup)
                continue;
            Bstr path;
            if (SUCCEEDED(task->get_Path(&path.p)))
                entries.push_back(
                    {StartupEntry::Kind::Task, path.text(), signature, 0, 0, {}, exe});
        }
    }
    Com<ITaskFolderCollection> folders;
    if (FAILED(folder->GetFolders(0, folders.address())))
        return;
    LONG count = 0;
    folders->get_Count(&count);
    for (LONG i = 1; i <= count; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        Com<ITaskFolder> child;
        if (SUCCEEDED(folders->get_Item(index, child.address())))
            enumerate_tasks(child.p, entries, depth + 1);
    }
}
void set_task(const std::wstring& name, const std::wstring& expected, bool enabled) {
    Com<ITaskService> service;
    connect_tasks(service);
    Com<ITaskFolder> root;
    Bstr slash(L"\\");
    check(service->GetFolder(slash.p, root.address()), L"Open task folder");
    Com<IRegisteredTask> task;
    Bstr path(name);
    HRESULT hr = root->GetTask(path.p, task.address());
    if (FAILED(hr)) {
        if (enabled && (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
                        hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)))
            return;
        check(hr, L"Find scheduled task");
    }
    std::wstring signature, exe;
    if (!task_details(task.p, signature, exe) || signature != expected)
        fail(L"Scheduled task changed; leaving it untouched: " + name);
    check(task->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE),
          L"Change scheduled task " + name);
}
std::vector<std::wstring> fields(const std::wstring& line) {
    std::vector<std::wstring> out;
    std::wistringstream in(line);
    std::wstring field;
    while (std::getline(in, field, L'|'))
        out.push_back(field);
    return out;
}
std::wstring record_key(const std::wstring& line) {
    auto f = fields(trim(line));
    if (f.size() == 5 && f[0] == L"R")
        return L"R|" + f[1] + L"|" + f[2];
    if (f.size() == 4 && f[0] == L"S")
        return L"S|" + f[1];
    if (f.size() == 3 && f[0] == L"T")
        return L"T|" + f[1];
    return {};
}
std::wstring entry_key(const StartupEntry& entry) {
    if (entry.kind == StartupEntry::Kind::Run)
        return L"R|" + std::to_wstring(entry.view) + L"|" + encode(entry.name);
    if (entry.kind == StartupEntry::Kind::Shortcut)
        return L"S|" + encode(entry.name);
    return L"T|" + encode(entry.name);
}
std::vector<fs::path> remove_older_records(std::wstring& record, const StartupEntry& entry,
                                           const Paths& paths) {
    auto key = entry_key(entry);
    std::wstring result, line;
    std::wistringstream input(record);
    std::vector<fs::path> superseded;
    bool header = true;
    while (std::getline(input, line)) {
        auto clean = trim(line);
        if (clean.empty())
            continue;
        if (header) {
            result = clean + L"\r\n";
            header = false;
            continue;
        }
        if (record_key(clean) == key) {
            auto f = fields(clean);
            if (entry.kind == StartupEntry::Kind::Shortcut && f.size() == 4) {
                try {
                    fs::path stored(decode(f[2]));
                    if (!stored.has_parent_path() && equal(stored.extension().wstring(), L".lnk"))
                        superseded.push_back(paths.recovery / stored);
                } catch (const std::exception&) {
                }
            }
            continue;
        }
        result += clean + L"\r\n";
    }
    record = result.empty() ? backup_header(paths) : result;
    return superseded;
}
bool same_contents(const fs::path& left, const fs::path& right) {
    std::ifstream a(left, std::ios::binary), b(right, std::ios::binary);
    if (!a || !b)
        fail(L"Cannot read startup shortcut; backup retained.");
    std::array<char, 4096> first{}, second{};
    do {
        a.read(first.data(), first.size());
        b.read(second.data(), second.size());
        if (a.bad() || b.bad())
            fail(L"Cannot compare startup shortcuts; backup retained.");
        if (a.gcount() != b.gcount() ||
            !std::equal(first.begin(), first.begin() + a.gcount(), second.begin()))
            return false;
    } while (a.gcount());
    return true;
}
void verify_startup_owner(const Paths& paths) {
    auto current = startup_command();
    if (!current.empty() && !equal(current, quote_arg(paths.exe.wstring()) + L" --startup"))
        fail(L"Another ZaloSnooze copy owns startup. Disable it there before enabling this copy.");
}
StartupState approval(const wchar_t* bucket) {
    auto path =
        std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\") +
        bucket;
    HKEY key = nullptr;
    auto status =
        RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
        return StartupState::On;
    if (status != ERROR_SUCCESS)
        return StartupState::Unknown;
    DWORD type = 0, size = 12;
    std::array<BYTE, 12> data{};
    status = RegQueryValueExW(key, L"ZaloSnooze", nullptr, &type, data.data(), &size);
    RegCloseKey(key);
    if (status == ERROR_FILE_NOT_FOUND)
        return StartupState::On;
    if (status != ERROR_SUCCESS || type != REG_BINARY || size != data.size())
        return StartupState::Unknown;
    DWORD state = 0;
    memcpy(&state, data.data(), sizeof(state));
    if (state == 3 || state == 7)
        return StartupState::DisabledByWindows;
    if (state == 2 || state == 6)
        return StartupState::On;
    return StartupState::Unknown;
}
void set_approval(bool enabled) {
    DWORD state = enabled ? 2 : 3;
    std::array<BYTE, 12> data{};
    memcpy(data.data(), &state, sizeof(state));
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    memcpy(data.data() + sizeof(state), &now, sizeof(now));
    for (const wchar_t* bucket : {L"Run", L"Run32"}) {
        auto path =
            std::wstring(L"Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Explorer\\\\StartupApproved\\\\") +
            bucket;
        HKEY key = nullptr;
        auto status = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                                      KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
        if (status != ERROR_SUCCESS)
            fail(error_text(L"Open Windows startup approval", status));
        status = RegSetValueExW(key, L"ZaloSnooze", 0, REG_BINARY, data.data(),
                                static_cast<DWORD>(data.size()));
        RegCloseKey(key);
        if (status != ERROR_SUCCESS)
            fail(error_text(L"Change Windows startup approval", status));
    }
}
} // namespace
fs::path startup_folder() {
    PWSTR p = nullptr;
    check(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &p), L"Find Startup folder");
    fs::path result(p);
    CoTaskMemFree(p);
    return result;
}
void require_standard_user() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
        fail(error_text(L"Read startup privileges"));
    TOKEN_ELEVATION elevation{};
    DWORD elevation_size = sizeof(elevation);
    if (!GetTokenInformation(token.value, TokenElevation, &elevation, elevation_size,
                             &elevation_size))
        fail(error_text(L"Read startup privileges"));
    if (elevation.TokenIsElevated)
        fail(L"Startup changes and recovery must run without administrator rights. Reopen "
             L"ZaloSnooze normally.");
}
StartupLock::StartupLock() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
        fail(error_text(L"Read startup lock identity"));
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> data(size);
    if (!GetTokenInformation(token.value, TokenUser, data.data(), size, &size))
        fail(error_text(L"Read startup lock user"));
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &sid))
        fail(error_text(L"Name startup lock"));
    auto name = std::wstring(L"Global\\ZaloSnooze.Startup.") + sid;
    LocalFree(sid);
    mutex.value = CreateMutexW(nullptr, FALSE, name.c_str());
    if (!mutex.value)
        fail(error_text(L"Create startup lock"));
    DWORD wait = WaitForSingleObject(mutex.value, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        fail(L"Another startup change is in progress. Try again shortly.");
}
StartupLock::~StartupLock() {
    ReleaseMutex(mutex.value);
}
std::wstring startup_command() {
    HKEY key = nullptr;
    auto status = RegOpenKeyExW(HKEY_CURRENT_USER, run_key, 0, KEY_QUERY_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
        return {};
    if (status != ERROR_SUCCESS)
        fail(error_text(L"Read startup registration", status));
    DWORD type = 0, size = 0;
    status = RegQueryValueExW(key, L"ZaloSnooze", nullptr, &type, nullptr, &size);
    if (status == ERROR_FILE_NOT_FOUND) {
        RegCloseKey(key);
        return {};
    }
    if (status != ERROR_SUCCESS) {
        RegCloseKey(key);
        fail(error_text(L"Read startup value", status));
    }
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t) || size > 65536 ||
        size % sizeof(wchar_t)) {
        RegCloseKey(key);
        fail(L"Startup registration has an unsupported format; leaving it untouched.");
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    DWORD read_type = 0, read_size = size;
    status = RegQueryValueExW(key, L"ZaloSnooze", nullptr, &read_type,
                              reinterpret_cast<BYTE*>(value.data()), &read_size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS)
        fail(error_text(L"Read startup command", status));
    if (read_type != type || read_size != size || value.back() != L'\0' ||
        value.find(L'\0') != value.size() - 1 || value.size() == 1)
        fail(L"Startup registration is invalid or changed; leaving it untouched.");
    value.pop_back();
    return type == REG_EXPAND_SZ ? expand(value) : value;
}
bool startup_registered(const Paths& paths) {
    return equal(startup_command(), quote_arg(paths.exe.wstring()) + L" --startup");
}
StartupState startup_state(const Paths& paths) {
    try {
        if (!startup_registered(paths))
            return StartupState::Off;
        auto run = approval(L"Run"), run32 = approval(L"Run32");
        if (run == StartupState::DisabledByWindows || run32 == StartupState::DisabledByWindows)
            return StartupState::DisabledByWindows;
        return run == StartupState::Unknown || run32 == StartupState::Unknown
                   ? StartupState::Unknown
                   : StartupState::On;
    } catch (const std::exception&) {
        return StartupState::Unknown;
    }
}
bool startup_enabled(const Paths& paths) {
    return startup_state(paths) == StartupState::On;
}
void set_startup(const Paths& paths, bool enabled) {
    StartupLock lock;
    verify_startup_owner(paths);
    auto expected = quote_arg(paths.exe.wstring()) + L" --startup";
    HKEY key = nullptr;
    auto status = RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                                  &key, nullptr);
    if (status != ERROR_SUCCESS)
        fail(error_text(L"Open startup settings", status));
    if (enabled)
        status = RegSetValueExW(key, L"ZaloSnooze", 0, REG_SZ,
                                reinterpret_cast<const BYTE*>(expected.c_str()),
                                static_cast<DWORD>((expected.size() + 1) * sizeof(wchar_t)));
    else {
        status = RegDeleteValueW(key, L"ZaloSnooze");
        if (status == ERROR_FILE_NOT_FOUND)
            status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS)
        fail(error_text(L"Change startup", status));
}
std::vector<StartupEntry> find_zalo_startup() {
    std::vector<StartupEntry> entries;
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, run_key, 0, KEY_READ | view, &key) != ERROR_SUCCESS)
            continue;
        for (DWORD i = 0;; ++i) {
            wchar_t name[16384];
            BYTE data[32768];
            DWORD n = 16384, size = sizeof(data), type = 0;
            auto status = RegEnumValueW(key, i, name, &n, nullptr, &type, data, &size);
            if (status == ERROR_NO_MORE_ITEMS)
                break;
            if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || size < 2 ||
                size % 2)
                continue;
            std::wstring command(reinterpret_cast<wchar_t*>(data), size / 2);
            auto zero = command.find(L'\0');
            if (zero != std::wstring::npos)
                command.resize(zero);
            auto executable = extract_exe(command);
            if (!zalo_executable(executable))
                continue;
            if (std::any_of(entries.begin(), entries.end(), [&](const auto& e) {
                    return e.kind == StartupEntry::Kind::Run && equal(e.name, name) &&
                           e.data == std::vector<BYTE>(data, data + size);
                }))
                continue;
            entries.push_back({StartupEntry::Kind::Run, name, command, view, type,
                               std::vector<BYTE>(data, data + size), executable});
        }
        RegCloseKey(key);
    }
    std::error_code error;
    for (const auto& file : fs::directory_iterator(startup_folder(), error)) {
        if (!equal(file.path().extension().wstring(), L".lnk"))
            continue;
        auto target = shortcut_target(file.path());
        if (zalo_executable(target))
            entries.push_back(
                {StartupEntry::Kind::Shortcut, file.path().wstring(), target, 0, 0, {}, target});
    }
    Com<ITaskService> service;
    connect_tasks(service);
    Com<ITaskFolder> root;
    Bstr slash(L"\\");
    check(service->GetFolder(slash.p, root.address()), L"Open task root");
    enumerate_tasks(root.p, entries, 0);
    return entries;
}
bool startup_backup_exists(const Paths& paths) {
    auto data = read_text(backup_path(paths));
    return !data.empty() && backup_owned_by(paths, data);
}
void disable_zalo_startup(const Paths& paths, const std::vector<StartupEntry>& entries) {
    StartupLock lock;
    auto record = owned_backup(paths);
    if (record.empty())
        record = backup_header(paths);
    auto commit = [&](const std::wstring& line, const std::vector<fs::path>& superseded) {
        auto next = record + line + L"\r\n";
        write_atomic(backup_path(paths), next);
        for (const auto& file : superseded)
            DeleteFileW(file.c_str());
        record = next;
    };
    for (const auto& entry : entries) {
        auto superseded = remove_older_records(record, entry, paths);
        std::wstring line;
        if (entry.kind == StartupEntry::Kind::Run) {
            line = L"R|" + std::to_wstring(entry.view) + L"|" + encode(entry.name) + L"|" +
                   std::to_wstring(entry.type) + L"|" +
                   hex(std::string(entry.data.begin(), entry.data.end()));
            HKEY key = nullptr;
            auto status = RegOpenKeyExW(HKEY_CURRENT_USER, run_key, 0,
                                        KEY_QUERY_VALUE | KEY_SET_VALUE | entry.view, &key);
            if (status != ERROR_SUCCESS)
                fail(error_text(L"Open Zalo startup entry", status));
            DWORD type = 0, size = 0;
            status = RegQueryValueExW(key, entry.name.c_str(), nullptr, &type, nullptr, &size);
            std::vector<BYTE> current(size);
            if (status == ERROR_SUCCESS)
                status = RegQueryValueExW(key, entry.name.c_str(), nullptr, &type, current.data(),
                                          &size);
            if (status != ERROR_SUCCESS || type != entry.type || current != entry.data) {
                RegCloseKey(key);
                fail(L"Zalo startup entry changed. Refresh settings before trying again.");
            }
            commit(line, superseded);
            type = 0;
            size = static_cast<DWORD>(current.size());
            status =
                RegQueryValueExW(key, entry.name.c_str(), nullptr, &type, current.data(), &size);
            if (status != ERROR_SUCCESS || type != entry.type || size != current.size() ||
                current != entry.data) {
                RegCloseKey(key);
                fail(L"Zalo startup entry changed after backup; leaving it untouched.");
            }
            status = RegDeleteValueW(key, entry.name.c_str());
            RegCloseKey(key);
            if (status != ERROR_SUCCESS)
                fail(error_text(L"Disable Zalo startup", status));
        } else if (entry.kind == StartupEntry::Kind::Shortcut) {
            if (!equal(shortcut_target(entry.name), entry.command))
                fail(L"Startup shortcut changed; leaving it untouched.");
            auto stored = L"shortcut-" + std::to_wstring(GetTickCount64()) + L"-" +
                          std::to_wstring(record.size()) + L".lnk";
            auto destination = paths.recovery / stored;
            fs::create_directories(destination.parent_path());
            if (!CopyFileW(entry.name.c_str(), destination.c_str(), TRUE))
                fail(error_text(L"Back up startup shortcut"));
            line =
                L"S|" + encode(entry.name) + L"|" + encode(stored) + L"|" + encode(entry.command);
            try {
                commit(line, superseded);
            } catch (...) {
                DeleteFileW(destination.c_str());
                throw;
            }
            if (!same_contents(entry.name, destination))
                fail(L"Startup shortcut changed; backup retained.");
            if (!DeleteFileW(entry.name.c_str()))
                fail(error_text(L"Disable startup shortcut"));
        } else {
            line = L"T|" + encode(entry.name) + L"|" + encode(entry.command);
            commit(line, superseded);
            set_task(entry.name, entry.command, false);
        }
    }
}
void restore_zalo_startup(const Paths& paths, const fs::path& shortcut_directory) {
    StartupLock lock;
    auto data = owned_backup(paths);
    if (data.empty())
        return;
    std::wistringstream input(data);
    std::wstring line;
    std::getline(input, line);
    std::wstring remaining = backup_header(paths), errors;
    std::vector<std::wstring> records;
    while (std::getline(input, line)) {
        auto clean = trim(line);
        if (!clean.empty())
            records.push_back(clean);
    }
    std::map<std::wstring, size_t> newest;
    for (size_t i = 0; i < records.size(); ++i) {
        auto key = record_key(records[i]);
        if (!key.empty())
            newest[key] = i;
    }
    std::vector<fs::path> restored_shortcuts, superseded_shortcuts;
    for (size_t index = 0; index < records.size(); ++index) {
        const auto& record = records[index];
        auto key = record_key(record);
        if (!key.empty() && newest[key] != index) {
            auto f = fields(record);
            if (f.size() == 4 && f[0] == L"S") {
                try {
                    fs::path stored(decode(f[2]));
                    if (!stored.has_parent_path() && equal(stored.extension().wstring(), L".lnk"))
                        superseded_shortcuts.push_back(paths.recovery / stored);
                } catch (const std::exception&) {
                }
            }
            continue;
        }
        try {
            auto f = fields(record);
            if (f.size() == 5 && f[0] == L"R") {
                DWORD view = std::stoul(f[1]), type = std::stoul(f[3]);
                if ((view != KEY_WOW64_32KEY && view != KEY_WOW64_64KEY) ||
                    (type != REG_SZ && type != REG_EXPAND_SZ))
                    fail(L"Invalid startup backup type.");
                auto name = decode(f[2]);
                auto bytes = unhex(f[4]);
                HKEY key = nullptr;
                auto status =
                    RegCreateKeyExW(HKEY_CURRENT_USER, run_key, 0, nullptr, 0,
                                    KEY_QUERY_VALUE | KEY_SET_VALUE | view, nullptr, &key, nullptr);
                if (status != ERROR_SUCCESS)
                    fail(error_text(L"Restore startup entry", status));
                DWORD present_type = 0, size = 0;
                status =
                    RegQueryValueExW(key, name.c_str(), nullptr, &present_type, nullptr, &size);
                if (status == ERROR_FILE_NOT_FOUND)
                    status =
                        RegQueryValueExW(key, name.c_str(), nullptr, &present_type, nullptr, &size);
                if (status == ERROR_FILE_NOT_FOUND)
                    status = RegSetValueExW(key, name.c_str(), 0, type,
                                            reinterpret_cast<const BYTE*>(bytes.data()),
                                            static_cast<DWORD>(bytes.size()));
                else if (status == ERROR_SUCCESS) {
                    std::string current(size, '\0');
                    status = RegQueryValueExW(key, name.c_str(), nullptr, nullptr,
                                              reinterpret_cast<BYTE*>(current.data()), &size);
                    if (status == ERROR_SUCCESS && (present_type != type || current != bytes)) {
                        RegCloseKey(key);
                        fail(L"A newer startup entry exists; backup retained: " + name);
                    }
                }
                RegCloseKey(key);
                if (status != ERROR_SUCCESS)
                    fail(error_text(L"Restore startup entry", status));
            } else if (f.size() == 4 && f[0] == L"S") {
                fs::path target(decode(f[1])), stored(decode(f[2]));
                if (!equal(target.parent_path().wstring(), shortcut_directory.wstring()) ||
                    !equal(target.extension().wstring(), L".lnk") || stored.has_parent_path() ||
                    !equal(stored.extension().wstring(), L".lnk"))
                    fail(L"Invalid shortcut backup path.");
                auto backup = paths.recovery / stored;
                restore_startup_shortcut(target, backup);
                restored_shortcuts.push_back(backup);
            } else if (f.size() == 3 && f[0] == L"T")
                set_task(decode(f[1]), decode(f[2]), true);
            else
                fail(L"Invalid startup backup record.");
        } catch (const std::exception& e) {
            remaining += record + L"\r\n";
            errors += wide(e.what()) + L"\n";
        }
    }
    if (remaining == backup_header(paths)) {
        if (!DeleteFileW(backup_path(paths).c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
            fail(error_text(L"Remove restored startup backup"));
    } else
        write_atomic(backup_path(paths), remaining);
    for (const auto& file : restored_shortcuts)
        DeleteFileW(file.c_str());
    for (const auto& file : superseded_shortcuts)
        DeleteFileW(file.c_str());
    if (!errors.empty())
        fail(errors);
}
void restore_startup_shortcut(const fs::path& target, const fs::path& backup) {
    if (fs::exists(target)) {
        if (!same_contents(target, backup))
            fail(L"A newer startup shortcut exists; backup retained.");
    } else if (!CopyFileW(backup.c_str(), target.c_str(), TRUE))
        fail(error_text(L"Restore startup shortcut"));
}
void apply_startup(const Paths& paths, Config& config, bool enabled,
                   const std::vector<StartupEntry>& handoff) {
    ConfigLock settings(paths.config);
    StartupLock lock;
    verify_startup_owner(paths);
    Config updated;
    updated.load(paths.config);
    if (enabled) {
        bool automatic = updated.executables.empty() && updated.processes.empty();
        Config discovered = updated;
        if (automatic && !handoff.empty()) {
            discovered.executables = detect_zalo();
            for (const auto& entry : handoff) {
                if (!zalo_executable(entry.executable))
                    fail(L"A Zalo startup target is unavailable. Refresh settings before trying "
                         L"again.");
                discovered.executables.push_back(entry.executable);
            }
        }
        auto resolved = discovered.resolved(paths);
        if (resolved.executables.empty() && resolved.processes.empty())
            fail(L"Zalo was not found. Choose its location first.");
        if (!handoff.empty() && automatic) {
            auto standard = standard_zalo_paths();
            bool custom = std::any_of(
                resolved.executables.begin(), resolved.executables.end(), [&](const auto& path) {
                    return std::none_of(standard.begin(), standard.end(), [&](const auto& known) {
                        return same_installation(known, path);
                    });
                });
            if (custom)
                updated.executables = resolved.executables;
        }
    }
    updated.launch_at_startup = enabled;
    updated.save(paths.config);
    try {
        set_startup(paths, enabled);
        set_approval(enabled);
        if (enabled && !startup_enabled(paths))
            fail(handoff.empty()
                     ? L"Windows has not enabled ZaloSnooze startup. Check Windows Settings > Apps > "
                       L"Startup."
                     : L"Windows has not enabled ZaloSnooze startup. Zalo's startup entries were "
                       L"left unchanged. Check Windows Settings > Apps > Startup.");
        if (enabled && !handoff.empty())
            disable_zalo_startup(paths, handoff);
        if (!enabled && startup_backup_exists(paths))
            restore_zalo_startup(paths, startup_folder());
    } catch (...) {
        updated.launch_at_startup = startup_registered(paths);
        updated.save(paths.config);
        config = updated;
        throw;
    }
    config = updated;
}
void remove_legacy_shortcut(const Paths& paths, const fs::path& shortcut_directory) {
    StartupLock lock;
    auto path = shortcut_directory / L"ZaloSnooze.lnk";
    if (!fs::exists(path))
        return;
    std::wstring arguments;
    auto target = shortcut_target(path, &arguments);
    if (equal(target, paths.exe.wstring()) &&
        (trim(arguments).empty() || trim(arguments) == L"--startup")) {
        auto backup = paths.recovery.parent_path() / L"legacy-startup-shortcut.lnk";
        fs::create_directories(backup.parent_path());
        restore_startup_shortcut(backup, path);
        if (!same_contents(path, backup))
            fail(L"Legacy startup shortcut changed; both copies retained.");
        if (!DeleteFileW(path.c_str()))
            fail(error_text(L"Remove old startup shortcut"));
    }
}
} // namespace snooze
