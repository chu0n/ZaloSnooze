#include "core.h"
#include "startup.h"
#include "version.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <fstream>
#include <shlobj.h>
#include <sddl.h>
#include <array>

using namespace snooze;
static int assertions = 0;
static void startup_network_tests(const fs::path& root);
static DWORD WINAPI internet_without_com(void*) {
    return static_cast<DWORD>(internet_state());
}
static void check(bool ok, const char* name) {
    ++assertions;
    if (!ok)
        throw std::runtime_error(std::string("FAIL: ") + name);
}
template <class F> static void rejects(F action, const char* name) {
    bool rejected = false;
    try {
        action();
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, name);
}
static DWORD lock_probe(const std::wstring& arguments = L"--startup-lock-probe") {
    auto exe = executable_path();
    auto command = quote_arg(exe.wstring()) + L" " + arguments;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                         nullptr, nullptr, &startup, &process),
          "start another startup-lock process");
    CloseHandle(process.hThread);
    Handle child(process.hProcess);
    if (WaitForSingleObject(child.value, 5000) != WAIT_OBJECT_0) {
        TerminateProcess(child.value, 1);
        WaitForSingleObject(child.value, 2000);
        check(false, "startup-lock probe exits without waiting for the owner");
    }
    DWORD code = 0;
    GetExitCodeProcess(child.value, &code);
    return code;
}
static std::string bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    check(file.good(), "read shortcut test bytes");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
static std::wstring read_marker(const fs::path& path) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        try {
            auto value = read_text(path);
            if (!value.empty())
                return value;
        } catch (const std::exception&) {
        }
        Sleep(10);
    }
    return read_text(path);
}
static std::wstring hex_bytes(const void* data, size_t size) {
    static const wchar_t* digits = L"0123456789abcdef";
    std::wstring result;
    auto* value = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        result += digits[value[i] >> 4];
        result += digits[value[i] & 15];
    }
    return result;
}
static std::wstring hex_text(const std::wstring& value) {
    auto encoded = utf8(value);
    return hex_bytes(encoded.data(), encoded.size());
}
static std::wstring registry_hex(const std::wstring& value) {
    return hex_bytes(value.c_str(), (value.size() + 1) * sizeof(wchar_t));
}
static std::wstring dacl_text(const fs::path& path) {
    DWORD needed = 0;
    GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
    check(needed > 0, "read test file DACL size");
    std::vector<BYTE> data(needed);
    check(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, data.data(), needed, &needed),
          "read test file DACL");
    LPWSTR text = nullptr;
    check(ConvertSecurityDescriptorToStringSecurityDescriptorW(
              data.data(), SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr),
          "format test file DACL");
    std::wstring result(text);
    LocalFree(text);
    return result;
}
static void set_owner_only_dacl(const fs::path& path) {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)", SDDL_REVISION_1,
                                                               &descriptor, nullptr),
          "build owner-only DACL");
    auto applied = SetFileSecurityW(
        path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor);
    LocalFree(descriptor);
    check(applied, "apply owner-only DACL");
}
static void shortcut(const fs::path& file, const fs::path& exe, const std::wstring& arguments,
                     const fs::path& directory) {
    IShellLinkW* link = nullptr;
    check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                     IID_IShellLinkW, reinterpret_cast<void**>(&link))),
          "create test shortcut");
    IPersistFile* persist = nullptr;
    try {
        check(SUCCEEDED(link->SetPath(exe.c_str())) &&
                  SUCCEEDED(link->SetArguments(arguments.c_str())) &&
                  SUCCEEDED(link->SetWorkingDirectory(directory.c_str())),
              "set test shortcut fields");
        check(SUCCEEDED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persist))),
              "persist test shortcut");
        check(SUCCEEDED(persist->Save(file.c_str(), TRUE)), "save test shortcut");
        persist->Release();
        link->Release();
    } catch (...) {
        if (persist)
            persist->Release();
        link->Release();
        throw;
    }
}
static void legacy_shortcut_tests(const fs::path& root) {
    auto directory = root / L"legacy-startup";
    fs::create_directories(directory);
    auto paths = Paths::for_executable(root / L"legacy" / L"ZaloSnooze.exe");
    paths.recovery = root / L"legacy-state" / L"recovery";
    auto file = directory / L"ZaloSnooze.lnk";
    auto backup = paths.recovery.parent_path() / L"legacy-startup-shortcut.lnk";
    shortcut(file, root / L"foreign" / L"ZaloSnooze.exe", L"", root);
    auto foreign = bytes(file);
    remove_legacy_shortcut(paths, directory);
    check(bytes(file) == foreign && !fs::exists(backup),
          "legacy cleanup leaves another copy's shortcut untouched");
    shortcut(file, paths.exe, L"--settings", root);
    auto custom = bytes(file);
    remove_legacy_shortcut(paths, directory);
    check(bytes(file) == custom && !fs::exists(backup),
          "legacy cleanup preserves custom shortcut arguments");
    shortcut(file, paths.exe, L"", root);
    auto original = bytes(file);
    remove_legacy_shortcut(paths, directory);
    check(!fs::exists(file) && bytes(backup) == original,
          "legacy cleanup saves the exact owned shortcut before deletion");
    fs::copy_file(backup, file);
    remove_legacy_shortcut(paths, directory);
    check(!fs::exists(file) && bytes(backup) == original,
          "legacy cleanup accepts an identical existing backup");
    shortcut(file, paths.exe, L"--startup", root);
    auto changed = bytes(file);
    rejects([&] { remove_legacy_shortcut(paths, directory); },
            "legacy cleanup refuses to discard a changed shortcut when an older backup exists");
    check(bytes(file) == changed && bytes(backup) == original,
          "legacy conflicts retain both exact copies");
}
static void tests(const fs::path& root) {
    check(sizeof(void*) == 4, "x86 executable");
    {
        Handle thread(CreateThread(nullptr, 0, internet_without_com, nullptr, 0, nullptr));
        check(thread.value && WaitForSingleObject(thread.value, 5000) == WAIT_OBJECT_0,
              "Internet-status probe returns promptly without an initialized COM apartment");
        DWORD result = 0;
        check(GetExitCodeThread(thread.value, &result) &&
                  result == static_cast<DWORD>(InternetState::Unknown),
              "unavailable Windows Internet status is explicit, not mistaken for offline");
    }
    {
        Handle token;
        check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value),
              "read test process privileges");
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        check(GetTokenInformation(token.value, TokenElevation, &elevation, size, &size),
              "query test process elevation");
        if (elevation.TokenIsElevated)
            rejects([] { require_standard_user(); },
                    "elevated entry points refuse per-user startup recovery");
        else {
            require_standard_user();
            check(true, "normal entry points allow per-user startup changes");
        }
    }
    check(wide(APP_VERSION) == APP_VERSION_W, "consistent generated version constants");
    check(wide(utf8(L"Tài khoản / 日本語")) == L"Tài khoản / 日本語", "UTF-8 Unicode round trip");
    check(trim(L" \r\n text \t") == L"text", "trim whitespace");
    check(equal(L"ZALO.exe", L"zalo.EXE"), "case insensitive Windows comparison");
    check(quote_arg(L"C:\\Folder name\\") == L"\"C:\\Folder name\\\\\"",
          "command quote trailing slash");
    check(quote_arg(L"a\"b") == L"\"a\\\"b\"", "command quote embedded quote");
    check(task_restore_signature(L"<Task>\n <Settings><Enabled>true</Enabled></Settings></Task>") ==
              task_restore_signature(L"<Task><Settings><Enabled>false</Enabled></Settings></Task>"),
          "task restoration ignores only our enabled-state change");
    check(
        task_restore_signature(
            L"<Task><Triggers><Enabled>true</Enabled></Triggers><Settings></Settings></Task>") !=
            task_restore_signature(
                L"<Task><Triggers><Enabled>false</Enabled></Triggers><Settings></Settings></Task>"),
        "task restoration detects newer trigger changes");
    check(split_list(L"\"D:\\Account,A\\Zalo.exe\", \"D:\\Account B\\Zalo.exe\"").size() == 2,
          "quoted commas");
    check(split_list(L"Zalo,zalo,Zalo2").size() == 2, "list case-insensitive deduplication");
    check(split_list(L"\"a\"\"b\"")[0] == L"a\"b", "escaped quote");
    rejects([] { split_list(L"\"broken"); }, "reject unmatched quote");
    check(split_list(join_list({L"one", L"two,three"})).size() == 2, "list round trip");
    check(same_installation(L"D:\\Zalo\\Zalo.exe", L"D:\\Zalo\\Zalo-26.10.20\\Zalo.exe"),
          "versioned Zalo stub match");
    check(!same_installation(L"D:\\Zalo\\Zalo.exe", L"D:\\ZaloOther\\Zalo.exe"),
          "reject neighboring installation");
    check(!same_installation(L"D:\\Zalo\\Zalo.exe", L"D:\\Zalo\\unrelated\\Zalo.exe"),
          "reject unrelated subdirectory");
    check(!same_installation(L"D:\\Zalo\\Zalo.exe", L"D:\\Zalo\\Zalo-Work\\Zalo.exe"),
          "named work installation is not mistaken for a numeric version folder");
    for (auto folder : {L"Zalo-2-work", L"Zalo-26.10-beta", L"Zalo-26..10", L"Zalo-26."})
        check(
            !same_installation(L"D:\\Zalo\\Zalo.exe", fs::path(L"D:\\Zalo") / folder / L"Zalo.exe"),
            "reject a non-version folder even when its suffix starts with a digit");
    check(main_window(L"Zalo", false) && main_window(L"Zalo - Đăng nhập Zalo", false),
          "eligible main and login titles");
    check(!main_window(L"Zalo Settings", false) && !main_window(L"Zalo", true),
          "exclude dialogs and owned windows");
    check(equal(process_path(GetCurrentProcessId()), executable_path().wstring()),
          "query own process path");
    const auto startup_entries = find_zalo_startup();
    check(std::all_of(startup_entries.begin(), startup_entries.end(),
                      [](const auto& entry) {
                          return zalo_executable(entry.executable) &&
                                 (entry.kind == StartupEntry::Kind::Task
                                      ? !entry.command.empty()
                                      : zalo_executable(entry.kind == StartupEntry::Kind::Run
                                                            ? extract_exe(entry.command)
                                                            : entry.command));
                      }),
          "read-only startup discovery returns only verified Zalo targets");
    if (const wchar_t* cross_pid = _wgetenv(L"SNOOZE_TEST_CROSS_PID")) {
        check(!process_path(static_cast<DWORD>(std::stoul(cross_pid))).empty(),
              "x86 helper reads 64-bit parent process path");
    }
    auto portable = root / L"portable";
    fs::create_directories(portable);
    auto paths = Paths::for_executable(portable / L"ZaloSnooze.exe");
    check(!paths.installed && paths.config == portable / L"ZaloSnooze.ini",
          "portable storage stays beside exe");
    write_atomic(portable / L"installed.ini", L"ZaloSnooze.InstallMode=Installed\r\n");
    auto installed = Paths::for_executable(portable / L"ZaloSnooze.exe");
    check(installed.installed && installed.config == local_data() / L"ZaloSnooze.ini",
          "installed per-user AppData storage");
    fs::remove(portable / L"installed.ini");
    Config defaults;
    check(!defaults.launch_at_startup && !defaults.minimize_to_taskbar &&
              !defaults.experimental_opacity,
          "safe opt-in defaults");
    check(defaults.grace == 45 && defaults.maximum == 120, "existing timing defaults preserved");
    auto ini = portable / L"ZaloSnooze.ini";
    write_atomic(ini, L"; "
                      L"retained\r\nUnknown=keep\r\ngrACeseconds=2\r\nGraceSeconds=8\r\nLanguage="
                      L"en\r\nZaloExe=\"Tài khoản\\Zalo.exe\"\r\nProcessName=\r\n");
    Config config;
    config.load(ini);
    check(config.grace == 8 && config.language == L"en", "last key wins");
    check(config.executables.size() == 1 && config.executables[0] == L"Tài khoản\\Zalo.exe",
          "Unicode ini path");
    check(config.processes.empty(), "empty process list retains path verification");
    config.grace = 9;
    config.save(ini);
    auto saved = read_text(ini);
    check(saved.find(L"; retained") != std::wstring::npos &&
              saved.find(L"Unknown=keep") != std::wstring::npos,
          "preserve comments and unknown keys");
    check(saved.find(L"grACeseconds") == std::wstring::npos &&
              saved.find(L"GraceSeconds=9") != std::wstring::npos,
          "remove duplicate keys when saving");
    auto resolved = config.resolved(paths);
    check(resolved.executables[0] == (portable / L"Tài khoản" / L"Zalo.exe").wstring(),
          "relative exe paths resolve beside app");
    Config duplicates;
    duplicates.executables = {L"Work\\Zalo-1\\Zalo.exe", L"Work\\Zalo-2\\Zalo.exe",
                              L"Work\\Zalo.exe", L"WORK\\ZALO.EXE", L"Personal\\Zalo.exe"};
    auto unique = duplicates.resolved(paths).executables;
    check(unique.size() == 2, "coalesce one installation without merging independent Zalo folders");
    check(equal(unique.front(), (portable / L"Work" / L"Zalo.exe").wstring()),
          "prefer stable Zalo launcher over duplicate versioned executables");
    duplicates.executables = {L"Work\\Zalo.exe", L"Work\\Zalo-2-work\\Zalo.exe"};
    check(duplicates.resolved(paths).executables.size() == 2,
          "retain independently selected numbered work folders");
    config.log_file = L"ZaloSnooze.log";
    check(config.resolved(paths).log_file == (portable / L"ZaloSnooze.log").wstring(),
          "portable logs beside settings");
    check(config.resolved(installed).log_file == (local_data() / L"ZaloSnooze.log").wstring(),
          "installed logs in AppData");
    Config bad_log;
    for (const auto& reserved : {paths.config, paths.exe, portable / L"installed.ini",
                                 paths.recovery / L"backup.ini", paths.recovery / L"debug.log"}) {
        bad_log.log_file = reserved.wstring();
        rejects([&] { bad_log.resolved(paths); },
                "reject log collisions with settings, binaries, markers, and recovery files");
    }
    bad_log.log_file = (portable / L".." / L"portable" / L"ZaloSnooze.ini").wstring();
    rejects([&] { bad_log.resolved(paths); }, "reject normalized log aliases to preferences");
    auto alias = portable / L"linked.log";
    fs::create_hard_link(ini, alias);
    bad_log.log_file = alias.wstring();
    rejects([&] { bad_log.resolved(paths); }, "reject hard-linked log aliases to preferences");
    fs::remove(alias);
    auto recovery_paths = paths;
    recovery_paths.recovery = root / L"log-recovery";
    auto recovery_file = recovery_paths.recovery / L"backup.ini";
    write_atomic(recovery_file, L"Recovery must survive.");
    fs::create_hard_link(recovery_file, alias);
    rejects([&] { bad_log.resolved(recovery_paths); },
            "reject hard-linked log aliases to recovery files");
    fs::remove(alias);
    auto legacy_backup = recovery_paths.recovery.parent_path() / L"legacy-startup-shortcut.lnk";
    write_atomic(legacy_backup, L"legacy shortcut fixture");
    bad_log.log_file = legacy_backup.wstring();
    rejects([&] { bad_log.resolved(recovery_paths); },
            "reject logging directly into a legacy startup shortcut backup");
    alias = legacy_backup.parent_path() / L"legacy-linked.log";
    fs::create_hard_link(legacy_backup, alias);
    bad_log.log_file = alias.wstring();
    rejects([&] { bad_log.resolved(recovery_paths); },
            "reject hard-linked log aliases to a legacy startup shortcut backup");
    fs::remove(alias);
    bad_log.executables = {(root / L"Custom" / L"Zalo.exe").wstring()};
    bad_log.log_file = bad_log.executables.front();
    rejects([&] { bad_log.resolved(paths); }, "reject logging to a configured Zalo executable");
    config.executables = {L"notes.txt"};
    rejects([&] { config.validate(); }, "reject non-executable paths");
    config.executables = {};
    config.maximum = 0;
    rejects([&] { config.save(ini); }, "reject invalid time");
    Config malformed;
    write_atomic(root / L"malformed.ini",
                 L"GraceSeconds=no\nMinWidth=-1\nLanguage=bad\nZaloExe=\"unterminated\n");
    malformed.load(root / L"malformed.ini");
    check(malformed.grace == 45 && malformed.min_width == 350 && malformed.language == L"vi" &&
              malformed.executables.empty(),
          "malformed values retain defaults");
    {
        Handle locked(
            CreateFileW(ini.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        rejects(
            [&] {
                Config x;
                x.load(ini);
            },
            "unreadable INI fails closed");
        rejects([&] { defaults.save(ini); }, "locked INI prevents save");
    }
    check(read_text(ini) == saved, "failed save preserves bytes");
    for (const auto& f : fs::directory_iterator(portable))
        check(f.path().extension() != L".tmp", "failed save removes temporary file");
    {
        auto minimize_ini = root / L"minimize.ini";
        Config minimize_config;
        minimize_config.minimize_to_taskbar = true;
        minimize_config.save(minimize_ini);
        Config loaded_minimize;
        loaded_minimize.load(minimize_ini);
        check(loaded_minimize.minimize_to_taskbar,
              "minimize-to-taskbar preference survives save and load");
    }
    {
        auto restricted = root / L"permissions.ini";
        write_atomic(restricted, L"before");
        set_owner_only_dacl(restricted);
        auto before_dacl = dacl_text(restricted);
        auto entries = [](const std::wstring& value) {
            auto first = value.find(L'(');
            return first == std::wstring::npos ? value : value.substr(first);
        };
        write_atomic(restricted, L"after");
        check(read_text(restricted) == L"after" &&
                  entries(dacl_text(restricted)) == entries(before_dacl),
              "atomic replacement preserves existing restricted DACL entries");
    }
    Config absent;
    absent.load(root / L"absent.ini");
    check(absent.grace == 45, "missing INI uses defaults");
    WindowState state(1);
    check(state.observe(true, 0), "first visible requests close");
    state.request_result(true, 0);
    check(!state.observe(false, 250) && !state.stable, "hide not immediately success");
    state.observe(false, 1250);
    check(state.stable && state.hides == 1, "confirmed stable hide");
    check(state.observe(true, 2000), "reopening requests close");
    state.request_result(true, 2000);
    state.observe(false, 2250);
    state.observe(false, 3250);
    check(state.hides == 2, "second confirmed close");
    state.observe(true, 4000);
    state.request_result(true, 4000);
    state.observe(false, 4250);
    state.observe(false, 5250);
    check(!state.observe(true, 6000) && state.hides == 3, "three-hide cap");
    WindowState delayed(2);
    delayed.observe(false, 0);
    delayed.observe(false, 60000);
    check(!delayed.stable, "never shown startup cannot succeed");
    WindowState hidden(3, true);
    hidden.observe(false, 0);
    hidden.observe(false, 1000);
    check(hidden.stable && hidden.hides == 0, "pre-existing hidden window can settle");
    WindowState bounce(4);
    for (int i = 0; i < 6; ++i) {
        check(bounce.observe(true, i * 500), "startup bounce still requests close");
        bounce.request_result(true, i * 500);
        bounce.observe(false, i * 500 + 200);
        check(bounce.hides == 0, "short bounce not counted");
    }
    bounce.observe(false, 4000);
    check(bounce.stable && bounce.hides == 1, "bounces eventually settle");
    {
        StartupLock first;
        StartupLock nested;
        check(lock_probe() == 3,
              "another process cannot change startup while a transaction owns the user lock");
    }
    check(lock_probe() == 0, "startup transaction releases its cross-process lock");
    auto config_probe = L"--config-lock-probe " + quote_arg(paths.config.wstring());
    auto before_lock = read_text(paths.config);
    {
        ConfigLock editor(paths.config);
        ConfigLock nested(paths.config);
        check(lock_probe(config_probe) == 3,
              "an open editor blocks another process's startup configuration");
        check(read_text(paths.config) == before_lock,
              "blocked startup setup does not change editor preferences");
    }
    check(lock_probe(config_probe) == 0,
          "startup configuration succeeds after the editor releases its lock");
    legacy_shortcut_tests(root);
    auto original_link = root / L"original.lnk", restored_link = root / L"restored.lnk";
    shortcut(original_link, paths.exe, L"--old", root);
    auto original_link_bytes = bytes(original_link);
    restore_startup_shortcut(restored_link, original_link);
    check(bytes(restored_link) == original_link_bytes && fs::exists(original_link),
          "restore exact shortcut bytes without deleting its recovery source");
    restore_startup_shortcut(restored_link, original_link);
    shortcut(restored_link, paths.exe, L"--new", root);
    auto newer_link_bytes = bytes(restored_link);
    rejects([&] { restore_startup_shortcut(restored_link, original_link); },
            "retain backup when shortcut arguments changed but target did not");
    check(bytes(restored_link) == newer_link_bytes && bytes(original_link) == original_link_bytes,
          "shortcut conflict preserves both newer and saved contents");
    shortcut(restored_link, paths.exe, L"--old", portable);
    rejects([&] { restore_startup_shortcut(restored_link, original_link); },
            "retain backup when only shortcut working directory changed");
    std::wstring sandbox =
        L"Software\\ZaloSnooze.NativeTests." + std::to_wstring(GetCurrentProcessId());
    paths.recovery = root / L"startup-recovery";
    HKEY sandbox_key = nullptr;
    check(RegCreateKeyExW(HKEY_CURRENT_USER, sandbox.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                          nullptr, &sandbox_key, nullptr) == ERROR_SUCCESS,
          "create isolated registry sandbox");
    check(RegOverridePredefKey(HKEY_CURRENT_USER, sandbox_key) == ERROR_SUCCESS,
          "redirect test registry writes");
    try {
        check(!startup_enabled(paths), "startup defaults off in empty registry");
        set_startup(paths, true);
        check(startup_enabled(paths), "startup enable round trip");
        check(startup_command() == quote_arg(paths.exe.wstring()) + L" --startup",
              "startup command explicitly selects background mode");
        auto other = Paths::for_executable(root / L"other" / L"ZaloSnooze.exe");
        other.recovery = paths.recovery;
        check(!startup_enabled(other), "another portable copy does not inherit startup identity");
        rejects([&] { set_startup(other, true); },
                "reject silent takeover of another copy startup");
        set_startup(paths, false);
        check(startup_command().empty(), "startup disable removes owned registration");
        set_startup(paths, false);
        check(!startup_enabled(paths), "startup disable is idempotent");
        const auto foreign = quote_arg(other.exe.wstring()) + L" --startup";
        HKEY helper_run = nullptr;
        check(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                            0, KEY_ALL_ACCESS, &helper_run) == ERROR_SUCCESS,
              "open isolated helper registration");
        auto set_helper = [&](DWORD type, const void* data, DWORD size) {
            check(RegSetValueExW(helper_run, L"ZaloSnooze", 0, type, static_cast<const BYTE*>(data),
                                 size) == ERROR_SUCCESS,
                  "seed isolated helper registration");
        };
        set_helper(REG_EXPAND_SZ, foreign.c_str(),
                   static_cast<DWORD>((foreign.size() + 1) * sizeof(wchar_t)));
        rejects([&] { set_startup(paths, true); },
                "never replace a foreign expandable startup command");
        rejects([&] { set_startup(paths, false); },
                "never remove a foreign expandable startup command");
        check(startup_command() == foreign,
              "foreign expandable startup data survives rejected operations");
        DWORD opaque = 42;
        set_helper(REG_BINARY, &opaque, sizeof(opaque));
        rejects([&] { set_startup(paths, true); },
                "unsupported startup data is not treated as a missing value");
        check(startup_state(paths) == StartupState::Unknown,
              "unknown startup data produces an explicit unknown state");
        wchar_t unterminated = L'X';
        set_helper(REG_SZ, &unterminated, sizeof(unterminated));
        rejects([&] { set_startup(paths, true); }, "malformed startup strings remain untouched");
        SetEnvironmentVariableW(L"SNOOZE_TEST_EXE", paths.exe.c_str());
        std::wstring expandable = L"\"%SNOOZE_TEST_EXE%\" --startup";
        set_helper(REG_EXPAND_SZ, expandable.c_str(),
                   static_cast<DWORD>((expandable.size() + 1) * sizeof(wchar_t)));
        check(startup_registered(paths),
              "recognize this copy in a correctly expanded startup command");
        set_startup(paths, false);
        SetEnvironmentVariableW(L"SNOOZE_TEST_EXE", nullptr);
        RegCloseKey(helper_run);
        set_startup(paths, true);
        HKEY approved = nullptr;
        check(RegCreateKeyExW(
                  HKEY_CURRENT_USER,
                  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
                  0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &approved,
                  nullptr) == ERROR_SUCCESS,
              "create isolated Windows approval state");
        std::array<BYTE, 12> approval{};
        auto approval_state = [&](BYTE state) {
            approval[0] = state;
            check(RegSetValueExW(approved, L"ZaloSnooze", 0, REG_BINARY, approval.data(),
                                 approval.size()) == ERROR_SUCCESS,
                  "seed Windows startup approval");
        };
        approval_state(3);
        check(startup_registered(paths) && !startup_enabled(paths) &&
                  startup_state(paths) == StartupState::DisabledByWindows,
              "Windows-disabled registration is not reported as On");
        set_startup(paths, true);
        check(startup_state(paths) == StartupState::DisabledByWindows,
              "registering startup never overrides a Windows disable choice");
        approval_state(7);
        check(startup_state(paths) == StartupState::DisabledByWindows,
              "recognize the alternate Windows disabled state");
        approval_state(2);
        check(startup_enabled(paths), "recognize Windows-enabled startup");
        approval_state(6);
        check(startup_enabled(paths), "recognize the alternate Windows enabled state");
        HKEY approved32 = nullptr;
        check(RegCreateKeyExW(
                  HKEY_CURRENT_USER,
                  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run32",
                  0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &approved32,
                  nullptr) == ERROR_SUCCESS,
              "create alternate Windows approval bucket");
        std::array<BYTE, 12> disabled32{};
        disabled32[0] = 3;
        check(RegSetValueExW(approved32, L"ZaloSnooze", 0, REG_BINARY, disabled32.data(),
                             disabled32.size()) == ERROR_SUCCESS &&
                  startup_state(paths) == StartupState::DisabledByWindows,
              "a Windows disable choice in Run32 is respected too");
        RegDeleteValueW(approved32, L"ZaloSnooze");
        RegCloseKey(approved32);
        check(RegSetValueExW(approved, L"ZaloSnooze", 0, REG_BINARY, approval.data(), 4) ==
                      ERROR_SUCCESS &&
                  startup_state(paths) == StartupState::Unknown,
              "truncated Windows approval data is never reported as enabled");
        approval_state(42);
        check(startup_state(paths) == StartupState::Unknown && !startup_enabled(paths),
              "unknown approval formats fail closed");
        RegDeleteValueW(approved, L"ZaloSnooze");
        set_startup(paths, false);
        fs::copy_file(executable_path().parent_path() / L"tray_fixture.exe", root / L"Zalo.exe");
        const std::wstring original = quote_arg((root / L"Zalo.exe").wstring()) + L" --sample";
        StartupEntry entry{StartupEntry::Kind::Run,
                           L"Zalo test launch",
                           original,
                           KEY_WOW64_32KEY,
                           REG_EXPAND_SZ,
                           {},
                           (root / L"Zalo.exe").wstring()};
        entry.data.resize((original.size() + 1) * sizeof(wchar_t));
        memcpy(entry.data.data(), original.c_str(), entry.data.size());
        HKEY run = nullptr;
        check(RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                            0, KEY_ALL_ACCESS | entry.view, &run) == ERROR_SUCCESS,
              "open isolated Zalo startup key");
        auto write_entry = [&](const std::wstring& value) {
            check(RegSetValueExW(run, entry.name.c_str(), 0, entry.type,
                                 reinterpret_cast<const BYTE*>(value.c_str()),
                                 static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
                      ERROR_SUCCESS,
                  "seed isolated startup entry");
        };
        write_entry(original);
        disable_zalo_startup(paths, {entry});
        check(startup_backup_exists(paths), "startup backup persists before registry removal");
        auto same_copy = paths;
        same_copy.exe = lower(paths.exe.wstring());
        check(startup_backup_exists(same_copy), "backup ownership respects Windows path casing");
        check(RegQueryValueExW(run, entry.name.c_str(), nullptr, nullptr, nullptr, nullptr) ==
                  ERROR_FILE_NOT_FOUND,
              "verified startup entry removed");
        check(!startup_backup_exists(other), "other portable copy does not own recovery records");
        rejects([&] { restore_zalo_startup(other, startup_folder()); },
                "reject recovery by another copy");
        write_entry(L"newer user change");
        rejects([&] { restore_zalo_startup(paths, startup_folder()); },
                "never overwrite newer startup entries");
        check(startup_backup_exists(paths), "conflict retains recovery record");
        RegDeleteValueW(run, entry.name.c_str());
        set_startup(paths, true);
        Config restore_config;
        restore_config.launch_at_startup = true;
        apply_startup(paths, restore_config, false);
        check(!startup_enabled(paths) && !restore_config.launch_at_startup,
              "restore action disables owned helper startup and updates config");
        Config saved_restore;
        saved_restore.load(paths.config);
        check(!saved_restore.launch_at_startup, "restore action saves disabled startup preference");
        DWORD byte_count = 0, type = 0;
        check(RegQueryValueExW(run, entry.name.c_str(), nullptr, &type, nullptr, &byte_count) ==
                  ERROR_SUCCESS,
              "restore original startup entry");
        std::vector<BYTE> restored(byte_count);
        RegQueryValueExW(run, entry.name.c_str(), nullptr, nullptr, restored.data(), &byte_count);
        check(type == entry.type && restored == entry.data,
              "restore exact registry type and bytes");
        check(!startup_backup_exists(paths), "successful restore removes backup record");
        StartupEntry repeated = entry;
        repeated.name = L"Zalo repeated launch";
        auto write_repeated = [&](const std::wstring& value) {
            repeated.command = value;
            repeated.data.resize((value.size() + 1) * sizeof(wchar_t));
            memcpy(repeated.data.data(), value.c_str(), repeated.data.size());
            check(RegSetValueExW(run, repeated.name.c_str(), 0, repeated.type, repeated.data.data(),
                                 static_cast<DWORD>(repeated.data.size())) == ERROR_SUCCESS,
                  "seed repeated Zalo startup entry");
        };
        auto older = quote_arg((root / L"old" / L"Zalo.exe").wstring()) + L" --old";
        auto newer = quote_arg((root / L"new" / L"Zalo.exe").wstring()) + L" --new";
        write_repeated(older);
        disable_zalo_startup(paths, {repeated});
        write_repeated(newer);
        disable_zalo_startup(paths, {repeated});
        check(startup_backup_exists(paths), "repeated handoff keeps one recovery generation");
        restore_zalo_startup(paths, startup_folder());
        check(!startup_backup_exists(paths), "repeated handoff restores without a stale conflict");
        DWORD repeated_type = 0, repeated_size = 0;
        check(RegQueryValueExW(run, repeated.name.c_str(), nullptr, &repeated_type, nullptr,
                               &repeated_size) == ERROR_SUCCESS,
              "read newer repeated startup entry");
        std::vector<BYTE> repeated_value(repeated_size);
        RegQueryValueExW(run, repeated.name.c_str(), nullptr, nullptr, repeated_value.data(),
                         &repeated_size);
        check(repeated_type == repeated.type && repeated_value == repeated.data,
              "repeated handoff restores the newest command, not the stale backup");
        RegDeleteValueW(run, repeated.name.c_str());
        std::wstring legacy_name = L"Zalo legacy duplicate";
        auto legacy_old = quote_arg((root / L"legacy-old" / L"Zalo.exe").wstring());
        auto legacy_new = quote_arg((root / L"legacy-new" / L"Zalo.exe").wstring());
        auto legacy_record = [&](const std::wstring& value) {
            return L"R|" + std::to_wstring(entry.view) + L"|" + hex_text(legacy_name) + L"|" +
                   std::to_wstring(entry.type) + L"|" + registry_hex(value) + L"\r\n";
        };
        fs::create_directories(paths.recovery);
        write_atomic(paths.recovery / L"backup.ini", L"Owner=" + hex_text(paths.exe.wstring()) +
                                                         L"\r\n" + legacy_record(legacy_old) +
                                                         legacy_record(legacy_new));
        restore_zalo_startup(paths, startup_folder());
        DWORD legacy_type = 0, legacy_size = 0;
        check(RegQueryValueExW(run, legacy_name.c_str(), nullptr, &legacy_type, nullptr,
                               &legacy_size) == ERROR_SUCCESS,
              "restore legacy duplicate startup entry");
        std::vector<BYTE> legacy_value(legacy_size);
        RegQueryValueExW(run, legacy_name.c_str(), nullptr, nullptr, legacy_value.data(),
                         &legacy_size);
        std::vector<BYTE> expected_legacy((legacy_new.size() + 1) * sizeof(wchar_t));
        memcpy(expected_legacy.data(), legacy_new.c_str(), expected_legacy.size());
        check(legacy_type == entry.type && legacy_value == expected_legacy &&
                  !startup_backup_exists(paths),
              "existing duplicate records restore the newest generation");
        RegDeleteValueW(run, legacy_name.c_str());
        restore_zalo_startup(paths, startup_folder());
        check(!startup_backup_exists(paths), "startup recovery is idempotent");
        Config automatic;
        automatic.save(paths.config);
        approval_state(3);
        Config plain;
        plain.load(paths.config);
        apply_startup(paths, plain, true);
        check(startup_registered(paths) && plain.launch_at_startup &&
                  startup_state(paths) == StartupState::On,
              "enable re-approves a Windows-disabled entry so the startup status can be trusted");
        approval_state(3);
        apply_startup(paths, automatic, true, {entry});
        check(startup_state(paths) == StartupState::On &&
                  RegQueryValueExW(run, entry.name.c_str(), nullptr, nullptr, nullptr, nullptr) ==
                          ERROR_FILE_NOT_FOUND &&
                  startup_backup_exists(paths),
              "enable re-approves a Windows-disabled entry before the Zalo handoff");
        write_entry(original);
        approval_state(2);
        RegCloseKey(approved);
        apply_startup(paths, automatic, true, {entry});
        auto custom = (root / L"Zalo.exe").wstring();
        check(std::find(automatic.executables.begin(), automatic.executables.end(), custom) !=
                  automatic.executables.end(),
              "handoff pins an auto-detected custom path before removing its only discovery entry");
        Config after_handoff;
        after_handoff.load(paths.config);
        auto after_targets = after_handoff.resolved(paths).executables;
        check(std::find(after_targets.begin(), after_targets.end(), custom) != after_targets.end(),
              "custom Zalo remains discoverable from saved preferences after handoff");
        check(read_text(paths.config).find(L"Unknown=keep") != std::wstring::npos,
              "handoff preserves unknown preference keys");
        apply_startup(paths, automatic, false);
        Config no_handoff;
        no_handoff.save(paths.config);
        apply_startup(paths, no_handoff, true);
        check(no_handoff.executables.empty(),
              "automatic detection remains automatic without a handoff");
        apply_startup(paths, no_handoff, false);
        auto old_local = expand(L"%LOCALAPPDATA%");
        auto home = root / L"standard-home";
        auto standard_exe = home / L"Programs" / L"Zalo" / L"Zalo.exe";
        fs::create_directories(standard_exe.parent_path());
        fs::copy_file(root / L"Zalo.exe", standard_exe);
        check(SetEnvironmentVariableW(L"LOCALAPPDATA", home.c_str()),
              "use a process-local standard Zalo fixture");
        try {
            auto standard_command = quote_arg(standard_exe.wstring());
            auto standard_entry = entry;
            standard_entry.command = standard_command;
            standard_entry.executable = standard_exe.wstring();
            standard_entry.data.resize((standard_command.size() + 1) * sizeof(wchar_t));
            memcpy(standard_entry.data.data(), standard_command.c_str(),
                   standard_entry.data.size());
            write_entry(standard_command);
            Config normal;
            normal.save(paths.config);
            apply_startup(paths, normal, true, {standard_entry});
            check(normal.executables.empty(),
                  "standard installations keep automatic detection after handoff");
            apply_startup(paths, normal, false);
            write_entry(original);
        } catch (...) {
            SetEnvironmentVariableW(L"LOCALAPPDATA", old_local.c_str());
            throw;
        }
        SetEnvironmentVariableW(L"LOCALAPPDATA", old_local.c_str());
        RegDeleteValueW(run, entry.name.c_str());
        auto shortcut_directory = root / L"startup-shortcut-fixtures";
        fs::create_directories(shortcut_directory);
        auto shortcut_file = shortcut_directory / L"Custom Zalo.lnk";
        shortcut(shortcut_file, custom, L"--sample", root);
        auto shortcut_bytes = bytes(shortcut_file);
        StartupEntry shortcut_entry{
            StartupEntry::Kind::Shortcut, shortcut_file.wstring(), custom, 0, 0, {}, custom};
        Config shortcut_config;
        shortcut_config.save(paths.config);
        apply_startup(paths, shortcut_config, true, {shortcut_entry});
        Config saved_shortcut_config;
        saved_shortcut_config.load(paths.config);
        auto shortcut_targets = saved_shortcut_config.resolved(paths).executables;
        check(!fs::exists(shortcut_file) &&
                  std::find(shortcut_targets.begin(), shortcut_targets.end(), custom) !=
                      shortcut_targets.end(),
              "shortcut-only custom installation is pinned before its discovery entry is removed");
        set_startup(paths, false);
        auto recovery_record = paths.recovery / L"backup.ini";
        auto find_saved_shortcut = [&] {
            for (const auto& file : fs::directory_iterator(paths.recovery))
                if (file.path().extension() == L".lnk")
                    return file.path();
            throw std::runtime_error("Saved shortcut missing");
        };
        auto saved_shortcut = find_saved_shortcut();
        auto before_restore = read_text(recovery_record);
        {
            Handle locked(CreateFileW(recovery_record.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING, 0, nullptr));
            check(locked.value != INVALID_HANDLE_VALUE,
                  "lock shortcut recovery record against deletion");
            rejects([&] { restore_zalo_startup(paths, shortcut_directory); },
                    "record deletion failure keeps shortcut recovery retryable");
            check(read_text(recovery_record) == before_restore &&
                      bytes(saved_shortcut) == shortcut_bytes &&
                      bytes(shortcut_file) == shortcut_bytes,
                  "failed recovery commit preserves record, saved source, and restored shortcut");
        }
        restore_zalo_startup(paths, shortcut_directory);
        check(!fs::exists(recovery_record) && !fs::exists(saved_shortcut),
              "successful recovery commits before deleting the saved shortcut");
        disable_zalo_startup(paths, {shortcut_entry});
        auto first_shortcut_backup = find_saved_shortcut();
        shortcut(shortcut_file, custom, L"--second", root);
        auto second_shortcut_bytes = bytes(shortcut_file);
        disable_zalo_startup(paths, {shortcut_entry});
        check(!fs::exists(first_shortcut_backup),
              "repeated shortcut handoff removes the superseded backup file");
        restore_zalo_startup(paths, shortcut_directory);
        check(fs::exists(shortcut_file) && bytes(shortcut_file) == second_shortcut_bytes &&
                  !fs::exists(recovery_record),
              "repeated shortcut handoff restores the newest bytes");
        shortcut_bytes = second_shortcut_bytes;
        disable_zalo_startup(paths, {shortcut_entry});
        write_entry(original);
        disable_zalo_startup(paths, {entry});
        write_entry(L"newer user change");
        saved_shortcut = find_saved_shortcut();
        before_restore = read_text(recovery_record);
        {
            Handle locked(CreateFileW(recovery_record.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING, 0, nullptr));
            check(locked.value != INVALID_HANDLE_VALUE,
                  "lock partial recovery record against replacement");
            rejects([&] { restore_zalo_startup(paths, shortcut_directory); },
                    "partial-record save failure keeps shortcut sources");
            check(read_text(recovery_record) == before_restore &&
                      bytes(saved_shortcut) == shortcut_bytes,
                  "atomic record replacement failure never removes a saved shortcut");
        }
        RegDeleteValueW(run, entry.name.c_str());
        restore_zalo_startup(paths, shortcut_directory);
        check(!fs::exists(recovery_record) && !fs::exists(saved_shortcut),
              "partial recovery can retry and remove committed sources safely");
        RegDeleteValueW(run, entry.name.c_str());
        Config task_config;
        task_config.save(paths.config);
        StartupEntry missing_task{StartupEntry::Kind::Task,
                                  L"\\ZaloSnooze.TestMissing." +
                                      std::to_wstring(GetCurrentProcessId()) + L"." +
                                      std::to_wstring(GetTickCount64()),
                                  L"missing task signature",
                                  0,
                                  0,
                                  {},
                                  custom};
        rejects([&] { apply_startup(paths, task_config, true, {missing_task}); },
                "a missing task reports partial handoff failure");
        check(std::find(task_config.executables.begin(), task_config.executables.end(), custom) !=
                  task_config.executables.end(),
              "task-discovered custom target is pinned before attempting task mutation");
        set_startup(paths, false);
        restore_zalo_startup(paths, shortcut_directory);
        startup_network_tests(root);
        RegCloseKey(run);
    } catch (...) {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        RegCloseKey(sandbox_key);
        RegDeleteTreeW(HKEY_CURRENT_USER, sandbox.c_str());
        throw;
    }
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(sandbox_key);
    RegDeleteTreeW(HKEY_CURRENT_USER, sandbox.c_str());
}
static void multi_installation_test(const fs::path& root) {
    std::vector<HANDLE> processes;
    std::vector<HWND> windows;
    Config config;
    config.window_class = L"SnoozeFixture";
    config.grace = 1;
    config.maximum = 8;
    auto stop = [&] {
        for (auto window : windows)
            PostMessageW(window, WM_APP + 2, 0, 0);
        for (auto process : processes) {
            if (WaitForSingleObject(process, 2000) == WAIT_TIMEOUT) {
                TerminateProcess(process, 1);
                WaitForSingleObject(process, 2000);
            }
            CloseHandle(process);
        }
    };
    try {
        for (const auto& folder : {L"Work, account A", L"Personal account B"}) {
            auto directory = root / folder;
            fs::create_directories(directory);
            auto exe = directory / L"Zalo.exe", marker = directory / L"ready";
            fs::copy_file(executable_path().parent_path() / L"tray_fixture.exe", exe);
            config.executables.push_back(exe.wstring());
            auto command =
                quote_arg(exe.wstring()) + L" " + quote_arg(marker.wstring()) + L" visible";
            STARTUPINFOW startup{};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process{};
            check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                 nullptr, &startup, &process),
                  "start independent custom-path Zalo fixture");
            CloseHandle(process.hThread);
            processes.push_back(process.hProcess);
            auto started = GetTickCount64();
            while (!fs::exists(marker) && GetTickCount64() - started < 5000)
                Sleep(20);
            check(fs::exists(marker), "custom-path fixture is ready");
            windows.push_back(
                reinterpret_cast<HWND>(static_cast<UINT_PTR>(std::stoull(read_marker(marker)))));
        }
        check(run_watcher(config, Paths::for_executable(executable_path())) == 0,
              "one watcher settles multiple independent custom installations");
        for (size_t i = 0; i < windows.size(); ++i)
            check(!IsWindowVisible(windows[i]) &&
                      WaitForSingleObject(processes[i], 0) == WAIT_TIMEOUT,
                  "each configured installation stays running in tray");
        stop();
    } catch (...) {
        stop();
        throw;
    }
}
static void mixed_precedence_test(const fs::path& root) {
    auto directory = root / L"mixed-precedence";
    fs::create_directories(directory);
    auto first = directory / L"SnoozeMixedA.exe", second = directory / L"SnoozeMixedB.exe";
    auto fixture = executable_path().parent_path() / L"tray_fixture.exe";
    fs::copy_file(fixture, first);
    fs::copy_file(fixture, second);
    auto first_marker = directory / L"first.ready", second_marker = directory / L"second.ready";
    auto start = [&](const fs::path& exe, const fs::path& marker, const wchar_t* mode) {
        auto command = quote_arg(exe.wstring()) + L" " + quote_arg(marker.wstring()) + L" " + mode;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        check(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             nullptr, &startup, &process),
              "start mixed-precedence fixture");
        CloseHandle(process.hThread);
        return process.hProcess;
    };
    Handle first_process(start(first, first_marker, L"visible"));
    Handle second_process(start(second, second_marker, L"headless"));
    HWND window = nullptr;
    try {
        auto clock = GetTickCount64();
        while ((!fs::exists(first_marker) || !fs::exists(second_marker)) &&
               GetTickCount64() - clock < 5000)
            Sleep(20);
        check(fs::exists(first_marker) && fs::exists(second_marker),
              "mixed-precedence fixtures are ready");
        window =
            reinterpret_cast<HWND>(static_cast<UINT_PTR>(std::stoull(read_marker(first_marker))));
        Config config;
        config.executables = {first.wstring()};
        config.processes = {L"SnoozeMixedA", L"SnoozeMixedB"};
        config.window_class = L"SnoozeFixture";
        config.grace = 1;
        config.maximum = 4;
        check(run_watcher(config, Paths::for_executable(executable_path())) != 0,
              "process overrides require every matched target to settle");
        PostMessageW(window, WM_APP + 2, 0, 0);
        window = nullptr;
        if (WaitForSingleObject(first_process.value, 2000) == WAIT_TIMEOUT)
            TerminateProcess(first_process.value, 1);
        TerminateProcess(second_process.value, 0);
    } catch (...) {
        if (window)
            PostMessageW(window, WM_APP + 2, 0, 0);
        TerminateProcess(first_process.value, 1);
        TerminateProcess(second_process.value, 1);
        throw;
    }
}
static HWND opacity_target = nullptr;
static bool opacity_zero_seen = false;
static ULONGLONG storm_until = 0;
static unsigned storm_messages = 0;
static LRESULT CALLBACK message_storm(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_APP + 50) {
        ++storm_messages;
        if (GetTickCount64() < storm_until)
            PostMessageW(window, message, 0, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
static void CALLBACK observe_opacity(HWND, UINT, UINT_PTR, DWORD) {
    COLORREF color = 0;
    BYTE alpha = 255;
    DWORD flags = 0;
    if (opacity_target && GetLayeredWindowAttributes(opacity_target, &color, &alpha, &flags) &&
        alpha == 0 && (flags & LWA_ALPHA))
        opacity_zero_seen = true;
}
static void fixture_test(const fs::path& root, const std::wstring& mode, bool expected) {
    auto fixture = executable_path().parent_path() / L"tray_fixture.exe";
    auto marker = root / (mode + L".ready");
    auto command = quote_arg(fixture.wstring()) + L" " + quote_arg(marker.wstring()) + L" " + mode;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    check(CreateProcessW(fixture.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                         nullptr, &startup, &process),
          "start isolated fixture");
    CloseHandle(process.hThread);
    Handle handle(process.hProcess);
    Handle helper;
    Handle unrelated;
    HWND unrelated_window = nullptr;
    auto unrelated_marker = root / L"unrelated.ready";
    HWND window = nullptr;
    UINT_PTR opacity_timer = 0;
    HWND storm_window = nullptr;
    try {
        auto start = GetTickCount64();
        while (!fs::exists(marker) && GetTickCount64() - start < 5000)
            Sleep(20);
        check(fs::exists(marker), "fixture ready");
        if (mode == L"visible") {
            auto helper_command = quote_arg(fixture.wstring()) + L" " +
                                  quote_arg((root / L"helper.ready").wstring()) + L" headless";
            PROCESS_INFORMATION child{};
            check(CreateProcessW(fixture.c_str(), helper_command.data(), nullptr, nullptr, FALSE, 0,
                                 nullptr, nullptr, &startup, &child),
                  "start windowless renderer-like fixture");
            CloseHandle(child.hThread);
            helper.value = child.hProcess;
            auto other_exe = root / L"Unrelated.exe";
            fs::copy_file(fixture, other_exe);
            auto other_command = quote_arg(other_exe.wstring()) + L" " +
                                 quote_arg(unrelated_marker.wstring()) + L" unrelated";
            PROCESS_INFORMATION other{};
            check(CreateProcessW(other_exe.c_str(), other_command.data(), nullptr, nullptr, FALSE,
                                 0, nullptr, nullptr, &startup, &other),
                  "start twenty unrelated windows sharing one PID");
            CloseHandle(other.hThread);
            unrelated.value = other.hProcess;
            auto ready = GetTickCount64();
            while (!fs::exists(unrelated_marker) && GetTickCount64() - ready < 5000)
                Sleep(20);
            check(fs::exists(unrelated_marker), "unrelated-window scan fixture is ready");
            unrelated_window = reinterpret_cast<HWND>(
                static_cast<UINT_PTR>(std::stoull(read_marker(unrelated_marker))));
        }
        window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(std::stoull(read_marker(marker))));
        Config config;
        config.executables = {fixture.wstring()};
        config.window_class = L"SnoozeFixture";
        config.grace = 1;
        config.minimize_to_taskbar = mode.find(L"minimize") == 0;
        config.maximum = mode == L"busy" || mode == L"opacity-busy" || mode == L"queue" ? 1 : 8;
        if (mode == L"minimize-busy" || mode == L"minimize-hidden")
            config.maximum = 2;
        config.experimental_opacity = mode.find(L"opacity") == 0;
        config.log_file = (root / (mode + L".log")).wstring();
        fs::path rotated = config.log_file + L".old";
        if (mode == L"visible") {
            write_atomic(config.log_file, std::wstring(1024 * 1024, L'x'));
            write_atomic(rotated, L"unrelated file");
        }
        auto paths = Paths::for_executable(executable_path());
        if (config.experimental_opacity) {
            opacity_target = window;
            opacity_zero_seen = false;
            opacity_timer = SetTimer(nullptr, 0, 10, observe_opacity);
            check(opacity_timer != 0,
                  "observe real opacity suppression during the watcher message loop");
        }
        auto clock = GetTickCount64();
        if (mode == L"queue") {
            WNDCLASSW cls{};
            cls.lpfnWndProc = message_storm;
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.lpszClassName = L"SnoozeMessageStorm";
            check(RegisterClassW(&cls) != 0, "register nonempty message-queue fixture");
            storm_window = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                         nullptr, cls.hInstance, nullptr);
            check(storm_window != nullptr, "create message-queue fixture");
            storm_messages = 0;
            storm_until = GetTickCount64() + 3000;
            PostMessageW(storm_window, WM_APP + 50, 0, 0);
        }
        int code = run_watcher(config, paths);
        if (mode == L"visible")
            check(read_text(rotated) == L"unrelated file", "log rotation preserves existing files");
        if (mode == L"minimize-busy") {
            const auto log = read_text(config.log_file);
            const std::wstring request = L"Minimize-to-taskbar request queued.";
            const auto first = log.find(request);
            const auto second =
                first == std::wstring::npos ? first : log.find(request, first + request.size());
            check(first != std::wstring::npos &&
                      (second == std::wstring::npos ||
                       log.find(request, second + request.size()) == std::wstring::npos),
                  "busy taskbar window gets at most two spaced minimize requests in two seconds");
        }
        if (storm_window) {
            check(storm_messages > 0 && GetTickCount64() - clock < 2000,
                  "a continuously nonempty message queue cannot bypass the one-second watch limit");
            MSG queued;
            while (PeekMessageW(&queued, storm_window, 0, 0, PM_REMOVE)) {
            }
            DestroyWindow(storm_window);
            storm_window = nullptr;
        }
        check((code == 0) == expected, "watcher result matches fixture behavior");
        if (opacity_timer) {
            KillTimer(nullptr, opacity_timer);
            opacity_timer = 0;
            opacity_target = nullptr;
        }
        if (config.experimental_opacity) {
            COLORREF color = 0;
            BYTE alpha = 0;
            DWORD flags = 0;
            check(opacity_zero_seen, "experimental watcher actually made the fixture transparent");
            check(GetLayeredWindowAttributes(window, &color, &alpha, &flags) &&
                      color == RGB(11, 22, 33) && alpha == 173 &&
                      flags == (LWA_ALPHA | LWA_COLORKEY),
                  "normal and timeout exits restore the original alpha, color key, and flags");
        }
        if (mode == L"busy" || mode == L"opacity-busy")
            check(GetTickCount64() - clock < 3000, "busy UI cannot block watch limit");
        if (expected) {
            if (config.minimize_to_taskbar) {
                check(IsIconic(window) && IsWindowVisible(window),
                      "minimize mode leaves a visible taskbar window minimized");
                check(!fs::exists(marker.wstring() + L".closed"),
                      "minimize mode does not send Close-to-tray");
            } else
                check(!IsWindowVisible(window), "fixture window hidden");
            check(WaitForSingleObject(handle.value, 0) == WAIT_TIMEOUT,
                  "fixture process still running");
            PostMessageW(window, WM_APP + 1, 0, 0);
            Sleep(250);
            check(IsWindowVisible(window) && !IsIconic(window),
                  "fixture can reopen after watcher exits");
        }
        if (unrelated.value) {
            check(
                WaitForSingleObject(unrelated.value, 0) == WAIT_TIMEOUT &&
                    !fs::exists(unrelated_marker.wstring() + L".closed"),
                "per-scan PID rejection leaves all unrelated windows and their process untouched");
            PostMessageW(unrelated_window, WM_APP + 2, 0, 0);
            if (WaitForSingleObject(unrelated.value, 2000) == WAIT_TIMEOUT) {
                TerminateProcess(unrelated.value, 1);
                WaitForSingleObject(unrelated.value, 2000);
            }
        }
        PostMessageW(window, WM_APP + 2, 0, 0);
        if (WaitForSingleObject(handle.value, 8000) == WAIT_TIMEOUT)
            TerminateProcess(handle.value, 1);
        if (helper.value) {
            TerminateProcess(helper.value, 0);
            WaitForSingleObject(helper.value, 2000);
        }
    } catch (...) {
        if (storm_window)
            DestroyWindow(storm_window);
        if (opacity_timer) {
            KillTimer(nullptr, opacity_timer);
            opacity_target = nullptr;
        }
        if (unrelated.value) {
            TerminateProcess(unrelated.value, 1);
            WaitForSingleObject(unrelated.value, 2000);
        }
        if (helper.value) {
            TerminateProcess(helper.value, 1);
            WaitForSingleObject(helper.value, 2000);
        }
        TerminateProcess(handle.value, 1);
        WaitForSingleObject(handle.value, 2000);
        throw;
    }
}
static void startup_network_tests(const fs::path& root) {
    auto directory = root / L"network-startup";
    auto first = directory / L"first" / L"Zalo.exe";
    auto second = directory / L"second" / L"Zalo.exe";
    for (const auto& file : {first, second}) {
        fs::create_directories(file.parent_path());
        fs::copy_file(executable_path().parent_path() / L"tray_fixture.exe", file);
    }
    auto paths = Paths::for_executable(directory / L"ZaloSnooze.exe");
    paths.recovery = directory / L"recovery";
    Config config;
    config.language = L"en";
    config.window_class = L"SnoozeFixture";
    config.grace = 1;
    config.maximum = 3;
    config.launch_at_startup = true;
    const wchar_t* previous = _wgetenv(L"SNOOZE_FIXTURE_MARKER");
    bool had_marker = previous != nullptr;
    std::wstring previous_marker = previous ? previous : L"";
    previous = _wgetenv(L"SNOOZE_FIXTURE_MODE");
    bool had_mode = previous != nullptr;
    std::wstring previous_mode = previous ? previous : L"";
    auto restore_environment = [&] {
        SetEnvironmentVariableW(L"SNOOZE_FIXTURE_MARKER",
                                had_marker ? previous_marker.c_str() : nullptr);
        SetEnvironmentVariableW(L"SNOOZE_FIXTURE_MODE", had_mode ? previous_mode.c_str() : nullptr);
    };
    fs::path marker;
    HWND window = nullptr;
    auto stop = [&] {
        if (!window && !marker.empty() && fs::exists(marker))
            window =
                reinterpret_cast<HWND>(static_cast<UINT_PTR>(std::stoull(read_marker(marker))));
        DWORD pid = 0;
        if (window)
            GetWindowThreadProcessId(window, &pid);
        if (pid && (equal(process_path(pid), first.wstring()) ||
                    equal(process_path(pid), second.wstring()))) {
            Handle process(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid));
            PostMessageW(window, WM_APP + 2, 0, 0);
            if (process.value && WaitForSingleObject(process.value, 2000) == WAIT_TIMEOUT) {
                TerminateProcess(process.value, 1);
                WaitForSingleObject(process.value, 2000);
            }
        }
        window = nullptr;
    };
    auto prepare = [&](const wchar_t* name) {
        stop();
        marker = directory / (std::wstring(name) + L".ready");
        config.executables = {first.wstring()};
        config.log_file = (directory / (std::wstring(name) + L".log")).wstring();
        config.save(paths.config);
        set_startup(paths, true);
        check(SetEnvironmentVariableW(L"SNOOZE_FIXTURE_MARKER", marker.c_str()) &&
                  SetEnvironmentVariableW(L"SNOOZE_FIXTURE_MODE", L"deferred"),
              "configure only the test process's auto-launch fixture environment");
    };
    auto verify_hidden_and_reopen = [&](const fs::path& expected) {
        check(fs::exists(marker), "automatic launch started the isolated Zalo fixture");
        window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(std::stoull(read_marker(marker))));
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        check(pid && equal(process_path(pid), expected.wstring()),
              "launch uses the current configured installation");
        Handle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
        check(process.value && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT,
              "network-startup fixture remains running after observation");
        check(!IsWindowVisible(window) && fs::exists(marker.wstring() + L".closed"),
              "the first online window received Close-to-tray, not only an external hide");
        PostMessageW(window, WM_APP + 1, 0, 0);
        Sleep(150);
        check(IsWindowVisible(window), "manual reopening works after startup observation exits");
        stop();
    };
    try {
        prepare(L"late-internet");
        unsigned probes = 0;
        std::wstring saved_preferences;
        auto clock = GetTickCount64();
        int result = run_watcher(config, paths, true, [&] {
            ++probes;
            check(!fs::exists(marker),
                  "Zalo is not launched while initial Internet access is pending");
            if (probes == 3) {
                Config updated = config;
                updated.executables = {second.wstring()};
                updated.maximum = 8;
                ConfigLock editor(paths.config);
                updated.save(paths.config);
                saved_preferences = read_text(paths.config);
            }
            return probes < 5 ? InternetState::Offline : InternetState::Online;
        });
        if (result != 0)
            std::cerr << utf8(read_text(config.log_file)) << "Fixture ready: " << fs::exists(marker)
                      << ", close received: " << fs::exists(marker.wstring() + L".closed") << '\n';
        check(result == 0, "late first Internet connection still reaches a confirmed tray state");
        check(probes == 5 && GetTickCount64() - clock >= 5500,
              "offline wait can exceed MaxSeconds without consuming the later observation budget");
        check(read_text(paths.config) == saved_preferences,
              "preferences edited during the offline wait are reloaded without being rewritten");
        auto log = read_text(config.log_file);
        check(log.find(L"is waiting for Windows") != std::wstring::npos &&
                  log.find(L"Internet access is available") != std::wstring::npos,
              "offline wait and first Internet readiness are recorded once");
        verify_hidden_and_reopen(second);

        for (auto state : {InternetState::Online, InternetState::Unknown}) {
            prepare(state == InternetState::Online ? L"already-online" : L"network-unavailable");
            probes = 0;
            check(run_watcher(config, paths, true,
                              [&] {
                                  ++probes;
                                  return state;
                              }) == 0,
                  "online or unavailable Internet status preserves normal bounded startup");
            check(probes == 1, "online and unknown status do not enter an offline polling loop");
            log = read_text(config.log_file);
            check((log.find(L"Internet state is unavailable") != std::wstring::npos) ==
                      (state == InternetState::Unknown),
                  "unavailable Internet status has an explicit fallback diagnostic");
            verify_hidden_and_reopen(first);
        }

        prepare(L"existing-offline");
        auto command = quote_arg(first.wstring()) + L" " + quote_arg(marker.wstring()) + L" hidden";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION child{};
        check(CreateProcessW(first.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             nullptr, &startup, &child),
              "start an already-running offline Zalo fixture");
        CloseHandle(child.hThread);
        Handle existing(child.hProcess);
        clock = GetTickCount64();
        while (!fs::exists(marker) && GetTickCount64() - clock < 5000)
            Sleep(20);
        check(fs::exists(marker), "existing offline fixture is ready");
        window = reinterpret_cast<HWND>(static_cast<UINT_PTR>(std::stoull(read_marker(marker))));
        probes = 0;
        check(
            run_watcher(
                config, paths, true,
                [&] {
                    ++probes;
                    check(
                        !IsWindowVisible(window) && !fs::exists(marker.wstring() + L".closed"),
                        "the pending startup does not manipulate an already-hidden offline window");
                    if (probes < 3)
                        return InternetState::Offline;
                    DWORD_PTR result = 0;
                    check(SendMessageTimeoutW(window, WM_APP + 1, 0, 0, SMTO_ABORTIFHUNG, 1000,
                                              &result) &&
                              IsWindowVisible(window),
                          "simulate Zalo revealing its existing window as the first connection "
                          "returns");
                    return InternetState::Online;
                }) == 0 &&
                probes == 3,
            "the first online reveal of an existing Zalo process is closed to tray");
        verify_hidden_and_reopen(first);

        prepare(L"manual-offline");
        probes = 0;
        check(run_watcher(config, paths, false,
                          [&] {
                              ++probes;
                              return InternetState::Offline;
                          }) == 0 &&
                  probes == 0,
              "manual runs neither require Internet nor query automatic-startup readiness");
        verify_hidden_and_reopen(first);

        prepare(L"cancel-offline");
        probes = 0;
        check(run_watcher(config, paths, true,
                          [&] {
                              if (++probes == 2) {
                                  set_startup(paths, false);
                                  return InternetState::Online;
                              }
                              return InternetState::Offline;
                          }) == 0 &&
                  probes == 2 && !fs::exists(marker),
              "removing registration while offline cancels before launch even as Internet returns");
        check(read_text(config.log_file).find(L"cancelled before launch") != std::wstring::npos,
              "startup cancellation is not reported as a completed Zalo launch");

        prepare(L"approval-offline");
        HKEY approval = nullptr;
        check(RegCreateKeyExW(
                  HKEY_CURRENT_USER,
                  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
                  0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &approval,
                  nullptr) == ERROR_SUCCESS,
              "create isolated approval change during network wait");
        try {
            probes = 0;
            check(
                run_watcher(
                    config, paths, true,
                    [&] {
                        if (++probes == 2) {
                            std::array<BYTE, 12> disabled{};
                            disabled[0] = 3;
                            check(
                                RegSetValueExW(approval, L"ZaloSnooze", 0, REG_BINARY,
                                               disabled.data(), disabled.size()) == ERROR_SUCCESS,
                                "simulate Windows disabling startup only in the registry sandbox");
                            return InternetState::Online;
                        }
                        return InternetState::Offline;
                    }) == 0 &&
                    startup_registered(paths) && !startup_enabled(paths) && !fs::exists(marker),
                "Windows-disabled approval cancels the offline worker without deleting its "
                "registration");
        } catch (...) {
            RegCloseKey(approval);
            throw;
        }
        RegDeleteValueW(approval, L"ZaloSnooze");
        RegCloseKey(approval);
        set_startup(paths, false);
        stop();
        restore_environment();
    } catch (...) {
        try {
            stop();
        } catch (...) {
        }
        restore_environment();
        throw;
    }
}
static int startup_ui_fixture(const fs::path& root, const std::wstring& mode) {
    fs::create_directories(root);
    auto paths = Paths::for_executable(executable_path());
    paths.installed = false;
    paths.config = root / L"ZaloSnooze.ini";
    paths.recovery = root / L"recovery";
    fs::copy_file(executable_path().parent_path() / L"tray_fixture.exe", root / L"Zalo.exe");
    Config config;
    config.language = L"en";
    config.executables = {(root / L"Zalo.exe").wstring()};
    config.launch_at_startup = true;
    config.save(paths.config);
    auto name = L"Software\\ZaloSnooze.UiTests." + std::to_wstring(GetCurrentProcessId());
    HKEY sandbox = nullptr, approved = nullptr;
    bool redirected = false;
    int result = 0;
    try {
        check(RegCreateKeyExW(HKEY_CURRENT_USER, name.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                              nullptr, &sandbox, nullptr) == ERROR_SUCCESS,
              "create GUI registry sandbox");
        check(RegOverridePredefKey(HKEY_CURRENT_USER, sandbox) == ERROR_SUCCESS,
              "redirect GUI startup writes");
        redirected = true;
        set_startup(paths, true);
        check(RegCreateKeyExW(
                  HKEY_CURRENT_USER,
                  L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
                  0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, nullptr, &approved,
                  nullptr) == ERROR_SUCCESS,
              "create GUI approval sandbox");
        std::array<BYTE, 12> data{};
        data[0] = mode == L"disabled" ? 3 : 42;
        check(RegSetValueExW(approved, L"ZaloSnooze", 0, REG_BINARY, data.data(), data.size()) ==
                  ERROR_SUCCESS,
              "set GUI approval fixture");
        show_gui(paths);
        std::array<BYTE, 12> after{};
        DWORD type = 0, size = after.size();
        auto queried = RegQueryValueExW(approved, L"ZaloSnooze", nullptr, &type, after.data(), &size);
        DWORD seeded_state = 0, after_state = 0;
        memcpy(&seeded_state, data.data(), sizeof(seeded_state));
        memcpy(&after_state, after.data(), sizeof(after_state));
        check(queried == ERROR_SUCCESS && type == REG_BINARY && size == data.size() &&
                  after_state == seeded_state,
              "GUI only changes Windows approval state through explicit startup actions");
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        result = 2;
    }
    if (approved)
        RegCloseKey(approved);
    if (redirected)
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (sandbox) {
        RegCloseKey(sandbox);
        RegDeleteTreeW(HKEY_CURRENT_USER, name.c_str());
    }
    return result;
}
int wmain(int count, wchar_t** arguments) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if ((count == 2 && std::wstring(arguments[1]) == L"--startup-lock-probe") ||
        (count == 3 && std::wstring(arguments[1]) == L"--config-lock-probe")) {
        int result = 0;
        auto paths = Paths::for_executable(executable_path());
        if (count == 3) {
            paths.config = arguments[2];
            paths.recovery = paths.config.parent_path() / L"lock-probe-recovery";
        }
        auto name = L"Software\\ZaloSnooze.LockProbe." + std::to_wstring(GetCurrentProcessId());
        HKEY sandbox = nullptr;
        bool redirected = false;
        try {
            check(RegCreateKeyExW(HKEY_CURRENT_USER, name.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                                  nullptr, &sandbox, nullptr) == ERROR_SUCCESS,
                  "create lock probe sandbox");
            check(RegOverridePredefKey(HKEY_CURRENT_USER, sandbox) == ERROR_SUCCESS,
                  "redirect lock probe writes");
            redirected = true;
            if (count == 3) {
                Config config;
                apply_startup(paths, config, false);
            } else {
                set_startup(paths, true);
                set_startup(paths, false);
            }
        } catch (...) {
            result = 3;
        }
        if (redirected)
            RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (sandbox) {
            RegCloseKey(sandbox);
            RegDeleteTreeW(HKEY_CURRENT_USER, name.c_str());
        }
        CoUninitialize();
        return result;
    }
    if (count == 4 && std::wstring(arguments[1]) == L"--startup-ui-fixture") {
        int result = 2;
        try {
            result = startup_ui_fixture(arguments[2], arguments[3]);
        } catch (const std::exception& error) {
            std::cerr << error.what() << "\n";
        }
        CoUninitialize();
        return result;
    }
    if (count != 1) {
        CoUninitialize();
        return 2;
    }
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    fs::path root =
        fs::path(temp) / (L"ZaloSnooze-native-tests-" + std::to_wstring(GetCurrentProcessId()));
    try {
        fs::create_directories(root);
        // Shell links can return a long path even when TEMP uses a short Windows alias.
        std::wstring long_root(32768, L'\0');
        DWORD length =
            GetLongPathNameW(root.c_str(), long_root.data(), static_cast<DWORD>(long_root.size()));
        check(length != 0 && length < long_root.size(), "expand native fixture directory");
        long_root.resize(length);
        root = long_root;
        tests(root);
        multi_installation_test(root);
        mixed_precedence_test(root);
        fixture_test(root, L"visible", true);
        fixture_test(root, L"minimize", true);
        fixture_test(root, L"minimize-hidden", false);
        fixture_test(root, L"minimize-busy", false);
        fixture_test(root, L"hidden", true);
        fixture_test(root, L"bounce", true);
        fixture_test(root, L"busy", false);
        fixture_test(root, L"exit", false);
        fixture_test(root, L"opacity", true);
        fixture_test(root, L"opacity-busy", false);
        fixture_test(root, L"queue", false);
        std::cout << "PASS: " << assertions
                  << " native assertions, including isolated Close-to-tray fixtures.\n";
        fs::remove_all(root);
        CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\nEvidence retained: " << utf8(root.wstring()) << "\n";
        CoUninitialize();
        return 1;
    }
}
