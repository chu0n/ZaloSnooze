#include <windows.h>
#include <shellapi.h>
#include <string>

static std::wstring marker, mode;
static int closes = 0, bounces = 0;
static void write(const std::wstring& file, const std::wstring& value) {
    auto temporary = file + L".tmp";
    HANDLE h = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        ExitProcess(5);
    DWORD n;
    const BYTE bom[] = {0xff, 0xfe};
    auto size = static_cast<DWORD>(value.size() * sizeof(wchar_t));
    bool written = WriteFile(h, bom, sizeof(bom), &n, nullptr) && n == sizeof(bom) &&
                   WriteFile(h, value.data(), size, &n, nullptr) && n == size;
    CloseHandle(h);
    if (!written || !MoveFileExW(temporary.c_str(), file.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        ExitProcess(5);
    }
}
static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_CLOSE) {
        ++closes;
        ShowWindow(w, SW_HIDE);
        write(marker + L".closed", std::to_wstring(closes));
        if (mode == L"exit") {
            DestroyWindow(w);
            return 0;
        }
        if (mode == L"bounce" && bounces++ < 5)
            SetTimer(w, 1, 150, nullptr);
        return 0;
    }
    if (m == WM_TIMER && wp == 1) {
        KillTimer(w, 1);
        ShowWindow(w, SW_SHOWNORMAL);
        return 0;
    }
    if (m == WM_TIMER && wp == 2) {
        KillTimer(w, 2);
        ShowWindow(w, SW_SHOWNORMAL);
        write(marker + L".online", L"1");
        return 0;
    }
    if (m == WM_APP + 1) {
        ShowWindow(w, SW_SHOWNORMAL);
        return 0;
    }
    if (m == WM_APP + 2) {
        DestroyWindow(w);
        return 0;
    }
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    if (m == WM_APP + 3) {
        Sleep(6000);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int count = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv)
        return 2;
    if (count == 1) {
        wchar_t ready[32768]{}, behavior[64]{};
        auto ready_size = GetEnvironmentVariableW(L"SNOOZE_FIXTURE_MARKER", ready, 32768);
        auto mode_size = GetEnvironmentVariableW(L"SNOOZE_FIXTURE_MODE", behavior, 64);
        if (!ready_size || ready_size >= 32768 || !mode_size || mode_size >= 64) {
            LocalFree(argv);
            return 2;
        }
        marker = ready;
        mode = behavior;
    } else if (count < 3) {
        LocalFree(argv);
        return 2;
    } else {
        marker = argv[1];
        mode = argv[2];
    }
    LocalFree(argv);
    if (mode == L"headless") {
        write(marker, L"0");
        Sleep(15000);
        return 0;
    }
    WNDCLASSW cls{};
    cls.lpfnWndProc = proc;
    cls.hInstance = instance;
    cls.lpszClassName = L"SnoozeFixture";
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&cls);
    if (mode == L"unrelated") {
        HWND first = nullptr;
        for (int i = 0; i < 20; ++i) {
            HWND other = CreateWindowW(cls.lpszClassName, L"Other product", WS_OVERLAPPEDWINDOW, 0,
                                       0, 450, 350, nullptr, nullptr, instance, nullptr);
            if (!other)
                return 3;
            if (!first)
                first = other;
        }
        write(marker, std::to_wstring(reinterpret_cast<UINT_PTR>(first)));
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return 0;
    }
    bool opacity = mode.find(L"opacity") == 0;
    HWND window =
        CreateWindowExW(opacity ? WS_EX_LAYERED : 0, cls.lpszClassName, L"Zalo - Test fixture",
                        WS_OVERLAPPEDWINDOW, 60, 60, 650, 450, nullptr, nullptr, instance, nullptr);
    if (!window)
        return 3;
    if (opacity &&
        !SetLayeredWindowAttributes(window, RGB(11, 22, 33), 173, LWA_ALPHA | LWA_COLORKEY))
        return 4;
    NOTIFYICONDATAW tray{};
    tray.cbSize = sizeof(tray);
    tray.hWnd = window;
    tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_TIP;
    tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(tray.szTip, L"ZaloSnooze isolated test");
    Shell_NotifyIconW(NIM_ADD, &tray);
    if (mode != L"hidden" && mode != L"minimize-hidden" && mode != L"deferred")
        ShowWindow(window, SW_SHOWNORMAL);
    if (mode == L"deferred")
        SetTimer(window, 2, 500, nullptr);
    write(marker, std::to_wstring(reinterpret_cast<UINT_PTR>(window)));
    if (mode == L"busy" || mode == L"opacity-busy" || mode == L"minimize-busy" || mode == L"queue")
        PostMessageW(window, WM_APP + 3, 0, 0);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    Shell_NotifyIconW(NIM_DELETE, &tray);
    return 0;
}
