#include "core.h"
#include "startup.h"
#include "version.h"
#include <shellapi.h>
#include <commctrl.h>
#include <iostream>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace snooze;
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com))
        return 1;
    struct EndCom {
        ~EndCom() {
            CoUninitialize();
        }
    } end_com;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LINK_CLASS};
    InitCommonControlsEx(&controls);
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    struct FreeArgs {
        LPWSTR* value;
        ~FreeArgs() {
            LocalFree(value);
        }
    } free_args{arguments};
    bool quiet = false;
    try {
        if (!arguments)
            fail(L"Cannot read command line.");
        Paths paths = Paths::for_executable(executable_path());
        if (count == 1 || (count == 2 && std::wstring(arguments[1]) == L"--settings"))
            return show_gui(paths);
        std::wstring command = arguments[1];
        quiet = command == L"--startup" || command == L"--uninstall-user";
        if (count != 2)
            fail(L"Unknown command. Use --settings, --run, --startup, --enable-startup, or "
                 L"--uninstall-user.");
        if (command == L"--uninstall-user") {
            require_standard_user();
            StartupLock lock;
            if (startup_registered(paths))
                set_startup(paths, false);
            if (startup_backup_exists(paths))
                restore_zalo_startup(paths, startup_folder());
            return 0;
        }
        Config config;
        if (command == L"--enable-startup") {
            configure_startup(nullptr, paths, config, true);
            return 0;
        }
        config.load(paths.config);
        if (command == L"--run")
            return run_watcher(config, paths);
        if (command == L"--startup") {
            if (!startup_enabled(paths))
                return 0;
            return run_watcher(config, paths, true);
        }
        fail(L"Unknown command. Open ZaloSnooze without arguments for settings.");
    } catch (const std::exception& error) {
        if (!quiet)
            MessageBoxW(nullptr, wide(error.what()).c_str(), L"ZaloSnooze", MB_OK | MB_ICONERROR);
        else
            OutputDebugStringW((L"ZaloSnooze: " + wide(error.what()) + L"\n").c_str());
        return 2;
    }
}
