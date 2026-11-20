#include "core.h"
#include "startup.h"
#include "version.h"
#include "../resources/resource.h"
#include <commdlg.h>
#include <shellapi.h>
#include <commctrl.h>
#include <algorithm>
#include <sstream>

namespace snooze {
namespace {
std::wstring text(HWND window, int id) {
    HWND control = GetDlgItem(window, id);
    int n = GetWindowTextLengthW(control);
    std::wstring value(n + 1, L'\0');
    GetWindowTextW(control, value.data(), n + 1);
    value.resize(n);
    return value;
}
void set(HWND window, int id, const std::wstring& value) {
    SetDlgItemTextW(window, id, value.c_str());
}
void show_error(HWND owner, const std::exception& error) {
    MessageBoxW(owner, wide(error.what()).c_str(), L"ZaloSnooze", MB_OK | MB_ICONERROR);
}
void inherit_dialog_icons(HWND w) {
    auto owner = GetWindow(w, GW_OWNER);
    for (int kind : {ICON_SMALL, ICON_BIG})
        SendMessageW(w, WM_SETICON, kind, SendMessageW(owner, WM_GETICON, kind, 0));
}
void populate_paths(HWND list, const std::vector<std::wstring>& paths) {
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    HDC dc = GetDC(list);
    auto old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(list, WM_GETFONT, 0, 0)));
    int extent = 0;
    for (const auto& path : paths) {
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(path.c_str()));
        SIZE size{};
        GetTextExtentPoint32W(dc, path.c_str(), static_cast<int>(path.size()), &size);
        extent = std::max(extent, static_cast<int>(size.cx) + 12);
    }
    SelectObject(dc, old);
    ReleaseDC(list, dc);
    SendMessageW(list, LB_SETHORIZONTALEXTENT, extent, 0);
}
bool english(const Config& c) {
    return c.language == L"en";
}
std::wstring tr(const Config& c, const wchar_t* vi, const wchar_t* en) {
    return english(c) ? en : vi;
}
UINT window_dpi(HWND w) {
    using GetDpi = UINT(WINAPI*)(HWND);
    auto address = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    GetDpi get = nullptr;
    memcpy(&get, &address, sizeof(get));
    return get ? get(w) : 96;
}
int icon_metric(int metric, UINT dpi) {
    using GetMetric = int(WINAPI*)(int, UINT);
    auto address = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    GetMetric get = nullptr;
    memcpy(&get, &address, sizeof(get));
    return get ? get(metric, dpi) : GetSystemMetrics(metric);
}
HICON action_icon(wchar_t glyph, int size) {
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = size;
    bitmap.bmiHeader.biHeight = -size;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    DWORD* pixels = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &bitmap, DIB_RGB_COLORS,
                                     reinterpret_cast<void**>(&pixels), nullptr, 0);
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             DEFAULT_PITCH, L"Segoe MDL2 Assets");
    if (!color || !dc || !font) {
        if (color)
            DeleteObject(color);
        if (dc)
            DeleteDC(dc);
        if (font)
            DeleteObject(font);
        fail(L"Cannot create a Windows action icon.");
    }
    std::fill_n(pixels, size * size, 0);
    auto old_bitmap = SelectObject(dc, color), old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT bounds{0, 0, size, size};
    DrawTextW(dc, &glyph, 1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    COLORREF ink = GetSysColor(COLOR_BTNTEXT);
    for (int i = 0; i < size * size; ++i) {
        DWORD alpha = pixels[i] & 255;
        pixels[i] = (alpha << 24) | ((GetRValue(ink) * alpha / 255) << 16) |
                    ((GetGValue(ink) * alpha / 255) << 8) | (GetBValue(ink) * alpha / 255);
    }
    SelectObject(dc, old_font);
    SelectObject(dc, old_bitmap);
    DeleteObject(font);
    DeleteDC(dc);
    std::vector<BYTE> mask_bits(((size + 15) / 16) * 2 * size, 0);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, mask_bits.data());
    ICONINFO info{TRUE, 0, 0, mask, color};
    HICON icon = mask ? CreateIconIndirect(&info) : nullptr;
    if (mask)
        DeleteObject(mask);
    DeleteObject(color);
    if (!icon)
        fail(L"Cannot finish a Windows action icon.");
    return icon;
}
void fit_main_dialog(HWND w, int height) {
    RECT rect{0, 0, 320, height};
    MapDialogRect(w, &rect);
    using Adjust = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    auto address = GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    Adjust adjust = nullptr;
    memcpy(&adjust, &address, sizeof(adjust));
    DWORD style = GetWindowLongW(w, GWL_STYLE), extended = GetWindowLongW(w, GWL_EXSTYLE);
    if (adjust)
        adjust(&rect, style, TRUE, extended, window_dpi(w));
    else
        AdjustWindowRectEx(&rect, style, TRUE, extended);
    SetWindowPos(w, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
void layout_main(HWND w) {
    RECT units{0, 0, 100, 10};
    MapDialogRect(w, &units);
    auto measure = [&](int id, int width) {
        HWND control = GetDlgItem(w, id);
        HDC dc = GetDC(control);
        auto old =
            SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
        RECT rect{0, 0, width * units.right / 100, 0};
        auto value = text(w, id);
        DrawTextW(dc, value.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        TEXTMETRICW metrics{};
        GetTextMetricsW(dc, &metrics);
        SelectObject(dc, old);
        ReleaseDC(control, dc);
        return 11 *
               std::max(1L, (rect.bottom + metrics.tmHeight - 1) / std::max(1L, metrics.tmHeight));
    };
    auto place = [&](int id, int x, int y, int width, int height) {
        RECT r{x, y, x + width, y + height};
        MapDialogRect(w, &r);
        SetWindowPos(GetDlgItem(w, id), nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER);
    };
    int banner_height =
        (MulDiv(320 * units.right / 100, 180, 1600) * 10 + units.bottom - 1) / units.bottom;
    place(IDC_BANNER, 0, 0, 320, banner_height);
    place(IDC_META, 12, banner_height + 8, 296, 12);
    int y = banner_height + 26;
    place(IDC_STARTUP, 12, y, 296, 24);
    y += 31;
    place(IDC_MINIMIZE, 12, y, 296, 16);
    y += 22;
    int intro_height = measure(IDC_INTRO, 296);
    place(IDC_INTRO, 12, y, 296, intro_height);
    y += intro_height + 10;
    place(IDC_PATHLABEL, 12, y, 296, 12);
    y += 15;
    int count = static_cast<int>(SendDlgItemMessageW(w, IDC_PATHS, LB_GETCOUNT, 0, 0));
    int path_height = 10 * std::clamp(count, 1, 3) + 10;
    place(IDC_PATHS, 12, y, 248, path_height);
    place(IDC_SETTINGS, 264, y, 20, 20);
    place(IDC_OPEN_ZALO, 288, y, 20, 20);
    y += path_height;
    bool has_status = !text(w, IDC_STATUS).empty();
    ShowWindow(GetDlgItem(w, IDC_STATUS), has_status ? SW_SHOW : SW_HIDE);
    if (has_status) {
        y += 6;
        int status_height = measure(IDC_STATUS, 296);
        place(IDC_STATUS, 12, y, 296, status_height);
        y += status_height;
    }
    fit_main_dialog(w, y + 12);
}
struct About {
    std::wstring title, body;
};
INT_PTR CALLBACK about_proc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_INITDIALOG) {
        const auto& value = *reinterpret_cast<const About*>(lp);
        SetWindowTextW(w, value.title.c_str());
        inherit_dialog_icons(w);
        SendDlgItemMessageW(w, IDC_ABOUT_ICON, STM_SETIMAGE, IMAGE_ICON,
                            SendMessageW(GetWindow(w, GW_OWNER), WM_GETICON, ICON_BIG, 0));
        set(w, IDC_ABOUT_TEXT, value.body);
        return TRUE;
    }
    if (message == WM_CLOSE ||
        (message == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL))) {
        EndDialog(w, 0);
        return TRUE;
    }
    return FALSE;
}
struct Advanced {
    Config config;
    const Paths* paths;
    void localize(HWND w) {
        SetWindowTextW(w, tr(config, L"Thử nghiệm", L"Experimental").c_str());
        set(w, IDC_ADVNOTE,
            tr(config, L"Mặc định phù hợp sử dụng bình thường. Tùy chỉnh có thể làm sai nhận diện.",
               L"Defaults work for normal use. Overrides may prevent detection."));
        set(w, 1201, tr(config, L"Lớp cửa sổ", L"Window class"));
        set(w, 1202,
            tr(config, L"Tên tiến trình (cách bằng dấu phẩy)", L"Process names (comma-separated)"));
        set(w, 1203, tr(config, L"Thời gian ổn định (giây)", L"Quiet period (seconds)"));
        set(w, 1204, tr(config, L"Giới hạn theo dõi (giây)", L"Watch limit (seconds)"));
        set(w, 1205, tr(config, L"Rộng / cao tối thiểu", L"Minimum width / height"));
        set(w, IDC_LOG, tr(config, L"Bật log chẩn đoán", L"Enable diagnostic log"));
        set(w, IDC_OPACITY,
            tr(config, L"Thử nghiệm giảm nháy (độ trong suốt)",
               L"Experimental flicker suppression (opacity)"));
        set(w, IDOK, tr(config, L"Lưu", L"Save"));
        set(w, IDCANCEL, tr(config, L"Hủy", L"Cancel"));
    }
    static INT_PTR CALLBACK proc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Advanced*>(GetWindowLongPtrW(w, DWLP_USER));
        if (message == WM_INITDIALOG) {
            self = reinterpret_cast<Advanced*>(lp);
            SetWindowLongPtrW(w, DWLP_USER, lp);
            self->localize(w);
            inherit_dialog_icons(w);
            set(w, IDC_CLASS, self->config.window_class);
            set(w, IDC_PROCESSES, join_list(self->config.processes));
            set(w, IDC_GRACE, std::to_wstring(self->config.grace));
            set(w, IDC_MAXIMUM, std::to_wstring(self->config.maximum));
            set(w, IDC_WIDTH, std::to_wstring(self->config.min_width));
            set(w, IDC_HEIGHT, std::to_wstring(self->config.min_height));
            CheckDlgButton(w, IDC_LOG, self->config.log_file.empty() ? BST_UNCHECKED : BST_CHECKED);
            CheckDlgButton(w, IDC_OPACITY,
                           self->config.experimental_opacity ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        }
        if (!self)
            return FALSE;
        try {
            if (message == WM_COMMAND) {
                switch (LOWORD(wp)) {
                case IDOK: {
                    Config updated = self->config;
                    updated.window_class = trim(text(w, IDC_CLASS));
                    updated.processes = split_list(text(w, IDC_PROCESSES));
                    auto number = [&](int id) {
                        auto s = trim(text(w, id));
                        size_t end = 0;
                        int n = std::stoi(s, &end);
                        if (end != s.size())
                            fail(L"Enter a valid positive integer.");
                        return n;
                    };
                    updated.grace = number(IDC_GRACE);
                    updated.maximum = number(IDC_MAXIMUM);
                    updated.min_width = number(IDC_WIDTH);
                    updated.min_height = number(IDC_HEIGHT);
                    if (IsDlgButtonChecked(w, IDC_LOG) == BST_CHECKED) {
                        if (updated.log_file.empty())
                            updated.log_file = L"ZaloSnooze.log";
                    } else
                        updated.log_file.clear();
                    updated.experimental_opacity =
                        IsDlgButtonChecked(w, IDC_OPACITY) == BST_CHECKED;
                    if (updated.experimental_opacity && !self->config.experimental_opacity &&
                        MessageBoxW(w,
                                    tr(updated,
                                       L"Tính năng này có thể để cửa sổ trong suốt nếu app bị buộc "
                                       L"tắt. Bật thử nghiệm?",
                                       L"Force-closing the helper can leave Zalo transparent. "
                                       L"Enable experimental opacity suppression?")
                                        .c_str(),
                                    L"ZaloSnooze",
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                        return TRUE;
                    updated.resolved(*self->paths);
                    updated.save(self->paths->config);
                    self->config = updated;
                    EndDialog(w, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(w, IDCANCEL);
                    return TRUE;
                }
            }
            if (message == WM_CLOSE) {
                EndDialog(w, IDCANCEL);
                return TRUE;
            }
        } catch (const std::exception& error) {
            show_error(w, error);
            return TRUE;
        }
        return FALSE;
    }
};
struct Settings {
    Config config;
    const Paths* paths;
    std::vector<std::wstring> installations;
    bool automatic = true;
    Settings(Config value, const Paths* location) : config(std::move(value)), paths(location) {}

    void refresh(HWND w) {
        auto list = GetDlgItem(w, IDC_PATHS);
        populate_paths(list, installations);
        CheckDlgButton(w, IDC_AUTO, automatic ? BST_CHECKED : BST_UNCHECKED);
        set(w, IDC_PATHLABEL,
            automatic
                ? tr(config, L"Tự tìm các bản Zalo đã cài. Thêm hoặc xóa đường dẫn để tự chọn.",
                     L"Installed copies are found automatically. Add or remove paths to choose "
                     L"your own.")
                : tr(config, L"Chỉ dùng các bản Zalo bên dưới. Có thể thêm nhiều đường dẫn.",
                     L"Use only the installations below. You can add more than one."));
        EnableWindow(GetDlgItem(w, IDC_REMOVE), FALSE);
    }

    static INT_PTR CALLBACK proc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Settings*>(GetWindowLongPtrW(w, DWLP_USER));
        if (message == WM_INITDIALOG) {
            self = reinterpret_cast<Settings*>(lp);
            SetWindowLongPtrW(w, DWLP_USER, lp);
        }
        if (!self)
            return FALSE;
        try {
            if (message == WM_INITDIALOG) {
                inherit_dialog_icons(w);
                SetWindowTextW(w, tr(self->config, L"Vị trí Zalo", L"Zalo locations").c_str());
                set(w, IDC_AUTO, tr(self->config, L"Tự tìm Zalo", L"Find Zalo automatically"));
                set(w, IDC_BROWSE, tr(self->config, L"Thêm...", L"Add..."));
                set(w, IDC_REMOVE, tr(self->config, L"Xóa", L"Remove"));
                set(w, IDOK, tr(self->config, L"Lưu", L"Save"));
                set(w, IDCANCEL, tr(self->config, L"Hủy", L"Cancel"));
                self->automatic =
                    self->config.executables.empty() && self->config.processes.empty();
                self->installations = self->automatic ? detect_zalo() : self->config.executables;
                self->refresh(w);
                return TRUE;
            }
            if (message == WM_COMMAND) {
                switch (LOWORD(wp)) {
                case IDC_AUTO:
                    self->automatic = IsDlgButtonChecked(w, IDC_AUTO) == BST_CHECKED;
                    if (self->automatic)
                        self->installations = detect_zalo();
                    self->refresh(w);
                    return TRUE;
                case IDC_PATHS:
                    if (HIWORD(wp) == LBN_SELCHANGE)
                        EnableWindow(GetDlgItem(w, IDC_REMOVE),
                                     SendDlgItemMessageW(w, IDC_PATHS, LB_GETSELCOUNT, 0, 0) > 0);
                    return TRUE;
                case IDC_BROWSE: {
                    wchar_t filename[32768]{};
                    OPENFILENAMEW dialog{};
                    dialog.lStructSize = sizeof(dialog);
                    dialog.hwndOwner = w;
                    dialog.lpstrFile = filename;
                    dialog.nMaxFile = 32768;
                    dialog.lpstrFilter = L"Zalo (Zalo.exe)\0Zalo.exe\0Executables (*.exe)\0*.exe\0";
                    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                    if (GetOpenFileNameW(&dialog)) {
                        if (std::none_of(self->installations.begin(), self->installations.end(),
                                         [&](const auto& path) { return equal(path, filename); }))
                            self->installations.emplace_back(filename);
                        self->automatic = false;
                        self->refresh(w);
                    }
                    return TRUE;
                }
                case IDC_REMOVE: {
                    auto list = GetDlgItem(w, IDC_PATHS);
                    for (int i = static_cast<int>(self->installations.size()) - 1; i >= 0; --i)
                        if (SendMessageW(list, LB_GETSEL, i, 0) > 0)
                            self->installations.erase(self->installations.begin() + i);
                    self->automatic = false;
                    self->refresh(w);
                    return TRUE;
                }
                case IDOK: {
                    Config updated = self->config;
                    updated.executables =
                        self->automatic ? std::vector<std::wstring>{} : self->installations;
                    if (self->automatic && !updated.processes.empty())
                        fail(tr(updated,
                                L"Xóa tên tiến trình trong Thử nghiệm trước khi bật tự tìm.",
                                L"Clear process overrides in Experimental before using automatic "
                                L"detection."));
                    if (!self->automatic && updated.executables.empty() &&
                        updated.processes.empty())
                        fail(tr(
                            updated, L"Thêm ít nhất một bản Zalo hoặc bật tự tìm.",
                            L"Add at least one Zalo installation or enable automatic detection."));
                    updated.save(self->paths->config);
                    self->config = updated;
                    EndDialog(w, IDOK);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(w, IDCANCEL);
                    return TRUE;
                }
            }
            if (message == WM_CLOSE) {
                EndDialog(w, IDCANCEL);
                return TRUE;
            }
        } catch (const std::exception& error) {
            show_error(w, error);
            return TRUE;
        }
        return FALSE;
    }
};
struct Main {
    Paths paths;
    Config config;
    HICON icon = nullptr, small_icon = nullptr;
    HICON manage_icon = nullptr, folder_icon = nullptr;
    HFONT startup_font = nullptr;
    HWND tooltips = nullptr;
    HBITMAP banner_en = nullptr, banner_vi = nullptr;
    std::wstring result, tooltip_text;
    std::vector<std::wstring> installations;
    bool initialized = false;
    StartupState displayed_startup = StartupState::Off;
    explicit Main(Paths p) : paths(std::move(p)) {
        config.load(paths.config);
        auto load = [](int id) {
            return static_cast<HBITMAP>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(id),
                                                   IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
        };
        banner_en = load(IDB_BANNER_EN);
        banner_vi = load(IDB_BANNER_VI);
        if (!banner_en || !banner_vi) {
            if (banner_en)
                DeleteObject(banner_en);
            if (banner_vi)
                DeleteObject(banner_vi);
            fail(L"Cannot load the embedded application banner.");
        }
    }
    ~Main() {
        if (icon)
            DestroyIcon(icon);
        if (small_icon)
            DestroyIcon(small_icon);
        if (manage_icon)
            DestroyIcon(manage_icon);
        if (folder_icon)
            DestroyIcon(folder_icon);
        if (startup_font)
            DeleteObject(startup_font);
        if (IsWindow(tooltips))
            DestroyWindow(tooltips);
        DeleteObject(banner_en);
        DeleteObject(banner_vi);
    }
    void draw_banner(const DRAWITEMSTRUCT& item) const {
        auto image = english(config) ? banner_en : banner_vi;
        BITMAP bitmap{};
        GetObjectW(image, sizeof(bitmap), &bitmap);
        HDC memory = CreateCompatibleDC(item.hDC);
        auto old = SelectObject(memory, image);
        int width = item.rcItem.right - item.rcItem.left,
            height = item.rcItem.bottom - item.rcItem.top;
        int draw_width = width, draw_height = MulDiv(width, bitmap.bmHeight, bitmap.bmWidth);
        if (draw_height > height) {
            draw_height = height;
            draw_width = MulDiv(height, bitmap.bmWidth, bitmap.bmHeight);
        }
        auto background = CreateSolidBrush(RGB(0, 104, 228));
        FillRect(item.hDC, &item.rcItem, background);
        DeleteObject(background);
        int mode = SetStretchBltMode(item.hDC, HALFTONE);
        POINT origin{};
        SetBrushOrgEx(item.hDC, 0, 0, &origin);
        StretchBlt(item.hDC, item.rcItem.left + (width - draw_width) / 2,
                   item.rcItem.top + (height - draw_height) / 2, draw_width, draw_height, memory, 0,
                   0, bitmap.bmWidth, bitmap.bmHeight, SRCCOPY);
        SetBrushOrgEx(item.hDC, origin.x, origin.y, nullptr);
        SetStretchBltMode(item.hDC, mode);
        SelectObject(memory, old);
        DeleteDC(memory);
    }
    void update_icons(HWND w) {
        UINT dpi = window_dpi(w);
        auto load = [](int width, int height) {
            return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                                 MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, width,
                                                 height, LR_DEFAULTCOLOR));
        };
        auto large = load(icon_metric(SM_CXICON, dpi), icon_metric(SM_CYICON, dpi));
        auto small = load(icon_metric(SM_CXSMICON, dpi), icon_metric(SM_CYSMICON, dpi));
        SendMessageW(w, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(large));
        SendMessageW(w, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));
        if (icon)
            DestroyIcon(icon);
        if (small_icon)
            DestroyIcon(small_icon);
        icon = large;
        small_icon = small;
        int size = icon_metric(SM_CXSMICON, dpi);
        auto manage = action_icon(L'\uE713', size);
        HICON folder = nullptr;
        try {
            SHSTOCKICONINFO info{};
            info.cbSize = sizeof(info);
            if (FAILED(SHGetStockIconInfo(SIID_FOLDEROPEN, SHGSI_ICON | SHGSI_LARGEICON, &info)))
                fail(L"Cannot load the Windows folder icon.");
            folder = static_cast<HICON>(CopyImage(info.hIcon, IMAGE_ICON, size, size, 0));
            DestroyIcon(info.hIcon);
            if (!folder)
                fail(error_text(L"Size folder icon"));
        } catch (...) {
            DestroyIcon(manage);
            throw;
        }
        SendDlgItemMessageW(w, IDC_SETTINGS, BM_SETIMAGE, IMAGE_ICON,
                            reinterpret_cast<LPARAM>(manage));
        SendDlgItemMessageW(w, IDC_OPEN_ZALO, BM_SETIMAGE, IMAGE_ICON,
                            reinterpret_cast<LPARAM>(folder));
        if (manage_icon)
            DestroyIcon(manage_icon);
        if (folder_icon)
            DestroyIcon(folder_icon);
        manage_icon = manage;
        folder_icon = folder;
        LOGFONTW font{};
        GetObjectW(reinterpret_cast<HFONT>(SendDlgItemMessageW(w, IDC_INTRO, WM_GETFONT, 0, 0)),
                   sizeof(font), &font);
        font.lfWeight = FW_SEMIBOLD;
        auto bold = CreateFontIndirectW(&font);
        SendDlgItemMessageW(w, IDC_STARTUP, WM_SETFONT, reinterpret_cast<WPARAM>(bold), TRUE);
        if (startup_font)
            DeleteObject(startup_font);
        startup_font = bold;
        InvalidateRect(GetDlgItem(w, IDC_BANNER), nullptr, FALSE);
    }
    void create_tooltips(HWND w) {
        tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                   WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                                   CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, w, nullptr,
                                   GetModuleHandleW(nullptr), nullptr);
        if (!tooltips)
            fail(error_text(L"Create action tooltips"));
        SendMessageW(tooltips, TTM_SETMAXTIPWIDTH, 0, MulDiv(420, window_dpi(w), 96));
        for (int id : {IDC_SETTINGS, IDC_OPEN_ZALO, IDC_PATHS}) {
            TOOLINFOW tool{};
            tool.cbSize = sizeof(tool);
            tool.hwnd = w;
            tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            tool.uId = reinterpret_cast<UINT_PTR>(GetDlgItem(w, id));
            tool.lpszText = LPSTR_TEXTCALLBACKW;
            if (!SendMessageW(tooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool)))
                fail(L"Cannot add an action tooltip.");
        }
    }
    void select_path(HWND w) const {
        int index = static_cast<int>(SendDlgItemMessageW(w, IDC_PATHS, LB_GETCURSEL, 0, 0));
        EnableWindow(GetDlgItem(w, IDC_OPEN_ZALO),
                     index >= 0 && static_cast<size_t>(index) < installations.size());
    }
    void update_startup_state(HWND w) {
        auto state = startup_state(paths);
        displayed_startup = state;
        set(w, IDC_STARTUP,
            state == StartupState::On
                ? tr(config, L"Tắt tự khởi động", L"Disable automatic startup")
            : state == StartupState::DisabledByWindows
                ? tr(config, L"Xóa đăng ký tự khởi động", L"Remove startup registration")
            : state == StartupState::Unknown
                ? tr(config, L"Mở cài đặt startup Windows", L"Open Windows startup settings")
                : tr(config, L"Bật tự khởi động", L"Enable automatic startup"));
        set(w, IDC_META,
            (paths.installed ? tr(config, L"Đã cài đặt", L"Installed") : L"Portable") +
                tr(config, L" · Tự khởi động: ", L" · Automatic startup: ") +
                (state == StartupState::On ? tr(config, L"Bật", L"On")
                 : state == StartupState::DisabledByWindows
                     ? tr(config, L"Windows đã tắt", L"Disabled by Windows")
                 : state == StartupState::Unknown ? tr(config, L"Chưa xác định", L"Unknown")
                                                  : tr(config, L"Tắt", L"Off")));
    }
    void refresh(HWND w) {
        SetWindowTextW(w, L"ZaloSnooze v" APP_VERSION_W);
        update_startup_state(w);
        set(w, IDC_MINIMIZE,
            tr(config, L"Thu nhỏ Zalo xuống thanh tác vụ thay vì tray",
               L"Minimize Zalo to the taskbar instead of the tray"));
        CheckDlgButton(w, IDC_MINIMIZE, config.minimize_to_taskbar ? BST_CHECKED : BST_UNCHECKED);
        set(w, IDC_INTRO,
            config.minimize_to_taskbar
                ? tr(config,
                     L"Bật một lần để Zalo thu nhỏ xuống thanh tác vụ mỗi khi đăng nhập Windows. "
                     L"Sau đó, đóng cửa sổ này.",
                     L"Enable once to start Zalo minimized on the taskbar at every Windows "
                     L"sign-in. Then close this window.")
                : tr(config,
                     L"Bật một lần để Zalo vào tray mỗi khi đăng nhập Windows. Sau đó, đóng cửa "
                     L"sổ này.",
                     L"Enable once to start Zalo in the tray at every Windows sign-in. Then close "
                     L"this window."));
        Config resolved;
        std::wstring configuration_error;
        bool backup = false;
        try {
            resolved = config.resolved(paths);
            backup = startup_backup_exists(paths);
        } catch (const std::exception& error) {
            configuration_error = wide(error.what());
        }
        const auto& targets = resolved.executables;
        installations = targets;
        populate_paths(GetDlgItem(w, IDC_PATHS), installations);
        if (!installations.empty())
            SendDlgItemMessageW(w, IDC_PATHS, LB_SETCURSEL, 0, 0);
        select_path(w);
        set(w, IDC_PATHLABEL,
            config.executables.empty() && config.processes.empty()
                ? tr(config, L"Vị trí Zalo · Tự tìm", L"Zalo locations · Automatic")
                : tr(config, L"Vị trí Zalo · Tùy chỉnh", L"Zalo locations · Custom"));
        size_t available = 0;
        for (const auto& path : targets) {
            std::error_code error;
            if (fs::is_regular_file(path, error))
                ++available;
        }
        std::wstring ready;
        if (!configuration_error.empty())
            ready = configuration_error;
        else if (displayed_startup == StartupState::DisabledByWindows)
            ready = tr(config, L"Windows đã tắt startup. Mở Công cụ > Startup Windows để bật lại.",
                       L"Windows disabled startup. Open Tools > Windows startup settings to "
                       L"re-enable it.");
        else if (displayed_startup == StartupState::Unknown)
            ready =
                tr(config, L"Không đọc được trạng thái startup. Kiểm tra cài đặt startup Windows.",
                   L"Startup state is unavailable. Check Windows startup settings.");
        else if (available != targets.size())
            ready = tr(config, L"Có đường dẫn Zalo không còn tồn tại. Kiểm tra vị trí cài.",
                       L"A selected Zalo path is unavailable. Check Zalo locations.");
        else if (targets.empty())
            ready = resolved.processes.empty()
                        ? tr(config, L"Chưa tìm thấy Zalo. Chọn vị trí cài đặt.",
                             L"Zalo not found. Choose its location.")
                        : tr(config, L"Đang dùng nhận diện tiến trình tùy chỉnh.",
                             L"Custom process matching is configured.");
        set(w, IDC_STATUS, result.empty() || !configuration_error.empty() ? ready : result);
        set(w, IDC_SETTINGS, tr(config, L"Quản lý vị trí Zalo", L"Manage Zalo locations"));
        set(w, IDC_OPEN_ZALO, tr(config, L"Mở thư mục Zalo đã chọn", L"Open selected Zalo folder"));
        HMENU menu = CreateMenu(), file = CreatePopupMenu(), tools = CreatePopupMenu(),
              language = CreatePopupMenu(), help = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, IDM_FOLDER,
                    tr(config, L"Mở thư mục cấu hình", L"Open settings folder").c_str());
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, IDM_EXIT, tr(config, L"Thoát", L"Exit").c_str());
        AppendMenuW(tools, MF_STRING, IDM_SETTINGS,
                    tr(config, L"Vị trí Zalo...", L"Zalo locations...").c_str());
        AppendMenuW(tools, MF_STRING | (backup ? MF_ENABLED : MF_GRAYED), IDM_RESTORE,
                    tr(config, L"Khôi phục startup Zalo...", L"Restore Zalo startup...").c_str());
        AppendMenuW(tools, MF_STRING, IDC_ADVANCED,
                    tr(config, L"Thử nghiệm...", L"Experimental...").c_str());
        AppendMenuW(tools, MF_STRING, IDM_WINDOWS_STARTUP,
                    tr(config, L"Startup Windows...", L"Windows startup settings...").c_str());
        AppendMenuW(language, MF_STRING | (english(config) ? 0 : MF_CHECKED), IDM_VI,
                    L"Tiếng Việt");
        AppendMenuW(language, MF_STRING | (english(config) ? MF_CHECKED : 0), IDM_EN, L"English");
        AppendMenuW(help, MF_STRING, IDM_ABOUT, tr(config, L"Giới thiệu...", L"About...").c_str());
        AppendMenuW(help, MF_STRING, IDM_UPDATES,
                    tr(config, L"Kiểm tra cập nhật...", L"Check for updates...").c_str());
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file),
                    tr(config, L"Tệp", L"File").c_str());
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tools),
                    tr(config, L"Công cụ", L"Tools").c_str());
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(language),
                    tr(config, L"Ngôn ngữ", L"Language").c_str());
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help),
                    tr(config, L"Trợ giúp", L"Help").c_str());
        auto old = GetMenu(w);
        SetMenu(w, menu);
        if (old)
            DestroyMenu(old);
        DrawMenuBar(w);
        layout_main(w);
        InvalidateRect(GetDlgItem(w, IDC_BANNER), nullptr, FALSE);
    }
    static INT_PTR CALLBACK proc(HWND w, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Main*>(GetWindowLongPtrW(w, DWLP_USER));
        if (message == WM_INITDIALOG) {
            self = reinterpret_cast<Main*>(lp);
            SetWindowLongPtrW(w, DWLP_USER, lp);
        }
        if (!self)
            return FALSE;
        try {
            if (message == WM_INITDIALOG) {
                self->refresh(w);
                self->update_icons(w);
                self->create_tooltips(w);
                self->initialized = true;
                return TRUE;
            }
            if (message == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE && self->initialized) {
                PostMessageW(w, WM_APP + 11, 0, 0);
                return FALSE;
            }
            if (message == WM_APP + 11) {
                if (startup_state(self->paths) != self->displayed_startup) {
                    self->result.clear();
                    self->refresh(w);
                }
                return TRUE;
            }
            if (message == WM_DPICHANGED) {
                PostMessageW(w, WM_APP + 10, 0, 0);
                return FALSE;
            }
            if (message == WM_APP + 10) {
                layout_main(w);
                self->update_icons(w);
                return TRUE;
            }
            if (message == WM_DRAWITEM && wp == IDC_BANNER) {
                self->draw_banner(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));
                return TRUE;
            }
            if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(lp)->code == TTN_GETDISPINFOW) {
                auto* info = reinterpret_cast<NMTTDISPINFOW*>(lp);
                int id = GetDlgCtrlID(reinterpret_cast<HWND>(info->hdr.idFrom));
                int index = static_cast<int>(SendDlgItemMessageW(w, IDC_PATHS, LB_GETCURSEL, 0, 0));
                self->tooltip_text = id == IDC_PATHS && index >= 0 &&
                                             static_cast<size_t>(index) < self->installations.size()
                                         ? self->installations[index]
                                         : text(w, id);
                info->lpszText = self->tooltip_text.data();
                return TRUE;
            }
            if (message == WM_COMMAND) {
                switch (LOWORD(wp)) {
                case IDC_STARTUP: {
                    auto state = startup_state(self->paths);
                    if (state != self->displayed_startup) {
                        self->refresh(w);
                        return TRUE;
                    }
                    if (state == StartupState::Unknown) {
                        open_target(w, L"ms-settings:startupapps");
                        return TRUE;
                    }
                    configure_startup(w, self->paths, self->config, state == StartupState::Off);
                    self->result.clear();
                    self->refresh(w);
                    return TRUE;
                }
                case IDM_WINDOWS_STARTUP:
                    open_target(w, L"ms-settings:startupapps");
                    return TRUE;
                case IDC_MINIMIZE: {
                    auto updated = self->config;
                    updated.minimize_to_taskbar =
                        IsDlgButtonChecked(w, IDC_MINIMIZE) == BST_CHECKED;
                    CheckDlgButton(w, IDC_MINIMIZE,
                                   self->config.minimize_to_taskbar ? BST_CHECKED : BST_UNCHECKED);
                    updated.save(self->paths.config);
                    self->config = updated;
                    self->result.clear();
                    self->refresh(w);
                    return TRUE;
                }
                case IDC_PATHS:
                    if (HIWORD(wp) == LBN_SELCHANGE)
                        self->select_path(w);
                    return TRUE;
                case IDC_OPEN_ZALO: {
                    int index =
                        static_cast<int>(SendDlgItemMessageW(w, IDC_PATHS, LB_GETCURSEL, 0, 0));
                    if (index < 0 || static_cast<size_t>(index) >= self->installations.size())
                        return TRUE;
                    auto folder = fs::path(self->installations[index]).parent_path();
                    if (!fs::is_directory(folder))
                        fail(tr(self->config, L"Thư mục Zalo không còn tồn tại.",
                                L"The Zalo folder is unavailable."));
                    open_target(w, folder.wstring());
                    return TRUE;
                }
                case IDC_SETTINGS:
                case IDM_SETTINGS: {
                    Settings settings{self->config, &self->paths};
                    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_SETTINGS),
                                        w, Settings::proc,
                                        reinterpret_cast<LPARAM>(&settings)) == IDOK) {
                        self->config = settings.config;
                        self->result.clear();
                        self->refresh(w);
                    }
                    return TRUE;
                }
                case IDC_ADVANCED: {
                    Advanced advanced{self->config, &self->paths};
                    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ADVANCED),
                                        w, Advanced::proc,
                                        reinterpret_cast<LPARAM>(&advanced)) == IDOK) {
                        self->config = advanced.config;
                        self->result.clear();
                        self->refresh(w);
                    }
                    return TRUE;
                }
                case IDM_FOLDER:
                    fs::create_directories(self->paths.config.parent_path());
                    open_target(w, self->paths.config.parent_path().wstring());
                    return TRUE;
                case IDM_VI:
                case IDM_EN: {
                    auto updated = self->config;
                    updated.language = LOWORD(wp) == IDM_EN ? L"en" : L"vi";
                    updated.save(self->paths.config);
                    self->config = updated;
                    self->result.clear();
                    self->refresh(w);
                    return TRUE;
                }
                case IDM_ABOUT: {
                    About about{tr(self->config, L"Giới thiệu ZaloSnooze", L"About ZaloSnooze"),
                                L"ZaloSnooze " APP_VERSION_W L"\n" +
                                    (self->paths.installed
                                         ? tr(self->config, L"Bản cài đặt", L"Installed version")
                                         : tr(self->config, L"Bản portable", L"Portable version")) +
                                    L"\n\n" +
                                    (self->config.minimize_to_taskbar
                                         ? tr(self->config,
                                              L"Thu nhỏ Zalo xuống thanh tác vụ, rồi tự thoát.",
                                              L"Starts Zalo minimized on the taskbar, then exits.")
                                         : tr(self->config, L"Đưa Zalo xuống tray, rồi tự thoát.",
                                              L"Starts Zalo in the tray, then exits.")) +
                                    tr(self->config, L"\nKhông service, không runtime cài thêm.",
                                       L"\nNo service or extra runtime installation.") +
                                    L"\n\nMIT License · © 2026 ZaloSnooze contributors"};
                    DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_ABOUT), w,
                                    about_proc, reinterpret_cast<LPARAM>(&about));
                    return TRUE;
                }
                case IDM_UPDATES: {
                    std::wstring url = PROJECT_URL_W;
                    if (url.empty()) {
                        MessageBoxW(w,
                                    tr(self->config,
                                       L"Bản build này chưa được cấu hình địa chỉ dự án. Không có "
                                       L"kiểm tra cập nhật tự động.",
                                       L"This build has no project URL configured. No automatic "
                                       L"update check is performed.")
                                        .c_str(),
                                    L"ZaloSnooze", MB_OK | MB_ICONINFORMATION);
                    } else
                        open_target(w, url + L"/releases");
                    return TRUE;
                }
                case IDM_RESTORE:
                    if (!startup_backup_exists(self->paths))
                        return TRUE;
                    if (MessageBoxW(w,
                                    tr(self->config,
                                       L"Tắt tự khởi động của ZaloSnooze và khôi phục startup gốc "
                                       L"của Zalo?",
                                       L"Turn off ZaloSnooze automatic startup and restore Zalo's "
                                       L"original startup entries?")
                                        .c_str(),
                                    L"ZaloSnooze",
                                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
                        configure_startup(w, self->paths, self->config, false);
                        self->result =
                            tr(self->config,
                               L"Đã khôi phục startup Zalo. Tự khởi động của công cụ đã tắt.",
                               L"Zalo startup restored. This helper's automatic startup is off.");
                        self->refresh(w);
                    }
                    return TRUE;
                case IDM_EXIT:
                case IDCANCEL:
                    EndDialog(w, 0);
                    return TRUE;
                }
            }
            if (message == WM_CLOSE) {
                EndDialog(w, 0);
                return TRUE;
            }
        } catch (const std::exception& error) {
            show_error(w, error);
            self->update_startup_state(w);
            return TRUE;
        }
        return FALSE;
    }
};
} // namespace
void configure_startup(HWND owner, const Paths& paths, Config& config, bool enabled) {
    require_standard_user();
    ConfigLock settings(paths.config);
    config = Config{};
    config.load(paths.config);
    std::vector<StartupEntry> handoff;
    if (enabled) {
        auto entries = find_zalo_startup();
        if (!entries.empty()) {
            std::wstring prompt = tr(config, L"Zalo đã có các mục startup sau:\n",
                                     L"Zalo already has these startup entries:\n");
            for (const auto& entry : entries)
                prompt += L"\n• " + entry.name;
            prompt += tr(config,
                         L"\n\nCho phép ZaloSnooze tắt các mục này và quản lý startup? Chọn Không "
                         L"để giữ nguyên; Hủy để không bật.",
                         L"\n\nLet ZaloSnooze disable these entries and handle startup? No leaves "
                         L"them unchanged; Cancel does not enable startup.");
            int answer = MessageBoxW(owner, prompt.c_str(), L"ZaloSnooze",
                                     MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
            if (answer == IDCANCEL)
                return;
            if (answer == IDYES)
                handoff = entries;
        }
    }
    StartupLock lock;
    apply_startup(paths, config, enabled, handoff);
    if (enabled)
        remove_legacy_shortcut(paths, startup_folder());
    if (enabled && !owner && startup_state(paths) != StartupState::On)
        MessageBoxW(
            nullptr,
            tr(config,
               L"Windows chưa cho phép startup. Mở Windows Settings > Apps > Startup để kiểm tra.",
               L"Windows has not enabled startup. Check Windows Settings > Apps > Startup.")
                .c_str(),
            L"ZaloSnooze", MB_OK | MB_ICONINFORMATION);
}
int show_gui(const Paths& paths) {
    ConfigLock lock(paths.config);
    Main main(paths);
    auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_MAIN), nullptr,
                                  Main::proc, reinterpret_cast<LPARAM>(&main));
    if (result == -1)
        fail(error_text(L"Open settings window"));
    return static_cast<int>(result);
}
} // namespace snooze
