// SPDX-License-Identifier: GPL-3.0-only
#include "display_settings.hpp"
#include <windows.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <fstream>
#pragma comment(                                                                                   \
    linker,                                                                                        \
    "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
using namespace dust;
namespace {
enum {
    Mode = 100,
    Resolution,
    Scaling,
    Volume,
    Captions,
    Intro,
    Root,
    Browse,
    Defaults,
    Play,
    Smooth,
    RestorePreset,
    Temporal
};
struct Size {
    int width, height;
};
constexpr std::array sizes{Size{512, 384},   Size{1024, 768},  Size{1536, 1152}, Size{2048, 1536},
                           Size{2560, 1920}, Size{1920, 1080}, Size{2560, 1440}, Size{3840, 2160}};
struct Control {
    HWND window;
    int x, y, width, height;
    bool heading;
};
struct Launcher {
    HWND window{}, mode{}, resolution{}, scaling{}, volume{}, volumeText{}, captions{}, intro{},
        root{}, preview{}, smooth{}, restorePreset{}, temporal{};
    HFONT normalFont{}, titleFont{};
    std::vector<Control> controls;
    DisplaySettings settings;
    std::filesystem::path file, executable;
    bool smoke = false, smokeFit = false, smokeGpu = false, launched = false;
    HANDLE child = nullptr;
    ~Launcher() {
        if (normalFont)
            DeleteObject(normalFont);
        if (titleFont)
            DeleteObject(titleFont);
        if (child)
            CloseHandle(child);
    }
    HWND control(const wchar_t *kind, const wchar_t *text, DWORD style, int id, int x, int y,
                 int width, int height, bool heading = false) {
        auto item = CreateWindowExW(kind == std::wstring(L"EDIT") ? WS_EX_CLIENTEDGE : 0, kind,
                                    text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window,
                                    reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr),
                                    nullptr);
        if (!item)
            throw std::runtime_error("cannot create launcher control");
        controls.push_back({item, x, y, width, height, heading});
        return item;
    }
    void label(const wchar_t *text, int x, int y, int width, bool heading = false) {
        control(L"STATIC", text, 0, 0, x, y, width, heading ? 36 : 22, heading);
    }
    void layout() {
        auto dpi = GetDpiForWindow(window);
        RECT client{};
        GetClientRect(window, &client);
        dpi = std::min(dpi, UINT(std::min(client.right * 96 / 600, client.bottom * 96 / 724)));
        auto px = [dpi](int value) { return MulDiv(value, dpi, 96); };
        if (normalFont)
            DeleteObject(normalFont);
        if (titleFont)
            DeleteObject(titleFont);
        normalFont = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        titleFont = CreateFontW(-px(28), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0,
                                0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        for (const auto &c : controls) {
            SetWindowPos(c.window, nullptr, px(c.x), px(c.y), px(c.width), px(c.height),
                         SWP_NOZORDER);
            SendMessageW(c.window, WM_SETFONT,
                         reinterpret_cast<WPARAM>(c.heading ? titleFont : normalFont), TRUE);
        }
    }
    void create() {
        label(L"DUST", 24, 18, 550, true);
        label(L"A Tale of the Wired West  ·  Native edition", 26, 58, 550);
        control(L"BUTTON", L"Display", BS_GROUPBOX, 0, 24, 94, 552, 280);
        label(L"Window mode", 40, 123, 155);
        mode = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Mode, 218, 119, 340, 160);
        SendMessageW(mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Windowed"));
        SendMessageW(mode, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Borderless fullscreen (desktop)"));
        label(L"Windowed resolution", 40, 163, 175);
        resolution = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Resolution, 218, 159,
                             340, 260);
        for (auto size : sizes) {
            auto text = std::to_wstring(size.width) + L" × " + std::to_wstring(size.height);
            if (size.width == 3840)
                text += L"  (4K)";
            SendMessageW(resolution, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
        label(L"Scaling", 40, 203, 155);
        scaling =
            control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, Scaling, 218, 199, 340, 150);
        SendMessageW(scaling, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Integer — whole-number enlargement"));
        SendMessageW(scaling, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Fit — full height, original proportions"));
        preview = control(L"STATIC", L"", 0, 0, 40, 240, 516, 78);
        smooth = control(L"BUTTON", L"Clean original character frames",
                         BS_AUTOCHECKBOX | WS_TABSTOP, Smooth, 40, 322, 320, 22);
        restorePreset = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, RestorePreset, 370,
                                319, 188, 130);
        for (auto text : {L"Gentle", L"Balanced", L"Strong", L"Custom (INI)"})
            SendMessageW(restorePreset, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
        temporal = control(L"BUTTON", L"Temporal colour cleanup (experimental)",
                           BS_AUTOCHECKBOX | WS_TABSTOP, Temporal, 40, 348, 510, 22);
        control(L"BUTTON", L"Audio & dialogue", BS_GROUPBOX, 0, 24, 384, 552, 156);
        label(L"Master volume", 40, 416, 140);
        volume = control(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, Volume, 180,
                         408, 300, 35);
        SendMessageW(volume, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        volumeText = control(L"STATIC", L"", SS_RIGHT, 0, 492, 415, 65, 22);
        captions = control(L"BUTTON", L"Always show dialogue captions",
                           BS_AUTOCHECKBOX | WS_TABSTOP, Captions, 40, 456, 510, 26);
        intro = control(L"BUTTON", L"Skip startup films", BS_AUTOCHECKBOX | WS_TABSTOP, Intro, 40,
                        495, 510, 26);
        label(L"Original game folder", 26, 550, 500);
        root = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, Root, 26, 576, 434, 28);
        control(L"BUTTON", L"Browse…", WS_TABSTOP, Browse, 470, 575, 106, 30);
        label(L"Settings are saved when you press Play.", 26, 625, 550);
        control(L"BUTTON", L"Defaults", WS_TABSTOP, Defaults, 26, 662, 112, 36);
        control(L"BUTTON", L"Close", WS_TABSTOP, IDCANCEL, 318, 662, 112, 36);
        control(L"BUTTON", L"Play", BS_DEFPUSHBUTTON | WS_TABSTOP, Play, 444, 662, 132, 36);
        populate();
        layout();
    }
    void populate() {
        SendMessageW(mode, CB_SETCURSEL, settings.fullscreen ? 1 : 0, 0);
        int selected = 1;
        for (size_t i = 0; i < sizes.size(); ++i)
            if (sizes[i].width == settings.width && sizes[i].height == settings.height)
                selected = int(i);
        SendMessageW(resolution, CB_SETCURSEL, selected, 0);
        SendMessageW(scaling, CB_SETCURSEL, settings.integerScaling ? 0 : 1, 0);
        SendMessageW(smooth, BM_SETCHECK, settings.smoothScaling ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(temporal, BM_SETCHECK,
                     settings.restoration.temporal ? BST_CHECKED : BST_UNCHECKED, 0);
        int preset = 3;
        const std::array<RestorationSettings, 3> presets{
            {{true, 25, 30, 8}, {true, 40, 45, 15}, {true, 65, 65, 25}}};
        for (int i = 0; i < 3; ++i)
            if (settings.restoration.deblock == presets[i].deblock &&
                settings.restoration.chroma == presets[i].chroma &&
                settings.restoration.detail == presets[i].detail)
                preset = i;
        SendMessageW(restorePreset, CB_SETCURSEL, preset, 0);
        SendMessageW(volume, TBM_SETPOS, TRUE, settings.volume);
        SendMessageW(captions, BM_SETCHECK, settings.captions ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(intro, BM_SETCHECK, settings.skipIntro ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(root, settings.gameRoot.c_str());
        refresh();
    }
    void refresh() {
        const bool full = SendMessageW(mode, CB_GETCURSEL, 0, 0) == 1;
        EnableWindow(resolution, !full);
        EnableWindow(temporal, SendMessageW(smooth, BM_GETCHECK, 0, 0) == BST_CHECKED);
        EnableWindow(restorePreset, SendMessageW(smooth, BM_GETCHECK, 0, 0) == BST_CHECKED);
        const int index = int(SendMessageW(resolution, CB_GETCURSEL, 0, 0));
        if (index < 0 || size_t(index) >= sizes.size())
            return;
        auto size = sizes[size_t(index)];
        if (full) {
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            size = {monitor.rcMonitor.right - monitor.rcMonitor.left,
                    monitor.rcMonitor.bottom - monitor.rcMonitor.top};
        }
        bool integer = SendMessageW(scaling, CB_GETCURSEL, 0, 0) == 0;
        auto v = scaledViewport(size.width, size.height, 512, 384, integer);
        auto text = L"Gameplay: " + std::to_wstring(v.width) + L" × " + std::to_wstring(v.height);
        text += v.integerScale ? L"  (" + std::to_wstring(v.integerScale) + L"× integer)"
                               : L"  (aspect fit)";
        if (integer) {
            text += SendMessageW(smooth, BM_GETCHECK, 0, 0) == BST_CHECKED
                        ? L"\r\nWhole-number layout with cached character restoration."
                        : L"\r\nExact pixels; unused space stays black.";
            text += L"\r\n4K integer: 5× with 120px top/bottom borders.";
        } else
            text += L"\r\nOriginal proportions, using the full available area.\r\n4K full "
                    L"height: 2880 × 2160, side borders only.";
        SetWindowTextW(preview, text.c_str());
        auto percent = std::to_wstring(SendMessageW(volume, TBM_GETPOS, 0, 0)) + L"%";
        SetWindowTextW(volumeText, percent.c_str());
    }
    void browse() {
        IFileOpenDialog *dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&dialog))))
            throw std::runtime_error("cannot open folder picker");
        dialog->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Select the original Dust installation folder");
        if (SUCCEEDED(dialog->Show(window))) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    SetWindowTextW(root, path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    void capturePreview() {
        wchar_t text[1024]{};
        GetWindowTextW(preview, text, int(std::size(text)));
        RECT available{};
        GetClientRect(preview, &available);
        auto dc = GetDC(preview);
        auto font = SelectObject(dc, normalFont);
        auto required = available;
        DrawTextW(dc, text, -1, &required, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, font);
        ReleaseDC(preview, dc);
        if (required.bottom > available.bottom)
            throw std::runtime_error("launcher scaling preview text is clipped");
        RECT r{};
        GetClientRect(window, &r);
        auto screen = GetDC(window);
        auto memory = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = r.right;
        info.bmiHeader.biHeight = -r.bottom;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        uint32_t *pixels = nullptr;
        auto bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS,
                                       reinterpret_cast<void **>(&pixels), nullptr, 0);
        if (!bitmap || !memory)
            throw std::runtime_error("cannot capture launcher preview");
        auto old = SelectObject(memory, bitmap);
        SendMessageW(window, WM_PRINT, reinterpret_cast<WPARAM>(memory),
                     PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        GdiFlush();
        std::filesystem::create_directories(file.parent_path());
        std::ofstream out(file.parent_path() / L"launcher-preview.bmp", std::ios::binary);
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + r.right * r.bottom * 4;
        out.write(reinterpret_cast<const char *>(&header), sizeof(header));
        out.write(reinterpret_cast<const char *>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
        out.write(reinterpret_cast<const char *>(pixels), size_t(r.right) * r.bottom * 4);
        SelectObject(memory, old);
        DeleteObject(bitmap);
        DeleteDC(memory);
        ReleaseDC(window, screen);
    }
    void play() {
        wchar_t folder[32768]{};
        GetWindowTextW(root, folder, int(std::size(folder)));
        settings.gameRoot = folder;
        if (!std::filesystem::is_directory(settings.gameRoot) ||
            !std::filesystem::is_directory(std::filesystem::path(folder) / L"MOVIES"))
            throw std::runtime_error(
                "Choose the original game installation folder containing MOVIES and DATA.");
        auto index = SendMessageW(resolution, CB_GETCURSEL, 0, 0);
        if (index < 0 || size_t(index) >= sizes.size())
            throw std::runtime_error("Select a resolution.");
        settings.width = sizes[size_t(index)].width;
        settings.height = sizes[size_t(index)].height;
        settings.fullscreen = SendMessageW(mode, CB_GETCURSEL, 0, 0) == 1;
        settings.integerScaling = SendMessageW(scaling, CB_GETCURSEL, 0, 0) == 0;
        settings.smoothScaling = SendMessageW(smooth, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const auto preset = SendMessageW(restorePreset, CB_GETCURSEL, 0, 0);
        const std::array<RestorationSettings, 3> presets{
            {{true, 25, 30, 8}, {true, 40, 45, 15}, {true, 65, 65, 25}}};
        if (preset >= 0 && preset < 3) {
            settings.restoration.deblock = presets[size_t(preset)].deblock;
            settings.restoration.chroma = presets[size_t(preset)].chroma;
            settings.restoration.detail = presets[size_t(preset)].detail;
        }
        settings.restoration.temporal = SendMessageW(temporal, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings.restoration.enabled = settings.smoothScaling;
        settings.volume = int(SendMessageW(volume, TBM_GETPOS, 0, 0));
        settings.captions = SendMessageW(captions, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings.skipIntro = SendMessageW(intro, BM_GETCHECK, 0, 0) == BST_CHECKED;
        saveSettings(file, settings);
        auto log = file.parent_path() / L"game-window.log";
        auto quoted = [](const std::wstring &s) {
            if (s.find(L'"') != s.npos)
                throw std::runtime_error("invalid path");
            size_t trailing = 0;
            for (size_t i = s.size(); i && s[i - 1] == L'\\'; --i)
                ++trailing;
            return L"\"" + s + std::wstring(trailing, L'\\') + L"\"";
        };
        std::wstring command = quoted(executable.wstring()) + L" " + quoted(settings.gameRoot) +
                               L" " + quoted(log.wstring());
        if (smoke)
            command += L" --smoke";
        command += L" --settings " + quoted(file.wstring());
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr,
                            executable.parent_path().c_str(), &startup, &process))
            throw std::runtime_error(
                "Cannot start dust_game.exe. Build the Release executables first.");
        CloseHandle(process.hThread);
        child = process.hProcess;
        launched = true;
        DestroyWindow(window);
    }
};
Launcher *app;
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM w, LPARAM l) {
    try {
        switch (message) {
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            SetBkColor(reinterpret_cast<HDC>(w), RGB(255, 255, 255));
            SetTextColor(reinterpret_cast<HDC>(w), RGB(40, 34, 27));
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        case WM_CREATE:
            app->window = window;
            app->create();
            return 0;
        case WM_COMMAND:
            switch (LOWORD(w)) {
            case IDOK:
            case Play:
                app->play();
                break;
            case Browse:
                app->browse();
                break;
            case Defaults:
                app->settings = DisplaySettings{};
                app->populate();
                break;
            case IDCANCEL:
                DestroyWindow(window);
                break;
            default:
                app->refresh();
                break;
            }
            return 0;
        case WM_HSCROLL:
            app->refresh();
            return 0;
        case WM_DPICHANGED: {
            auto r = reinterpret_cast<RECT *>(l);
            SetWindowPos(window, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER);
            app->layout();
            app->refresh();
            return 0;
        }
        case WM_TIMER:
            KillTimer(window, 1);
            if (app->smoke) {
                SendMessageW(app->mode, CB_SETCURSEL, 1, 0);
                SendMessageW(app->resolution, CB_SETCURSEL, 7, 0);
                SendMessageW(app->scaling, CB_SETCURSEL, app->smokeFit ? 1 : 0, 0);
                SendMessageW(app->smooth, BM_SETCHECK, app->smokeGpu ? BST_CHECKED : BST_UNCHECKED,
                             0);
                SendMessageW(app->volume, TBM_SETPOS, TRUE, 37);
                SendMessageW(app->captions, BM_SETCHECK, BST_CHECKED, 0);
                SendMessageW(app->intro, BM_SETCHECK, BST_CHECKED, 0);
                app->refresh();
                app->capturePreview();
                SendMessageW(window, WM_COMMAND, Play, 0);
            }
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception &error) {
        auto text = std::string(error.what());
        if (app->smoke) {
            std::filesystem::create_directories(app->file.parent_path());
            std::ofstream(app->file.parent_path() / L"launcher-error.log") << text;
            DestroyWindow(window);
        } else
            MessageBoxA(window, text.c_str(), "Dust launcher", MB_OK | MB_ICONERROR);
        return message == WM_CREATE ? -1 : 0;
    }
    return DefWindowProcW(window, message, w, l);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES};
        InitCommonControlsEx(&common);
        Launcher state;
        app = &state;
        int count = 0;
        auto args = CommandLineToArgvW(GetCommandLineW(), &count);
        state.smokeGpu = count > 2 && std::wstring(args[1]) == L"--smoke-gpu-test";
        state.smokeFit =
            state.smokeGpu || (count > 2 && std::wstring(args[1]) == L"--smoke-fit-test");
        state.smoke = state.smokeFit || (count > 2 && std::wstring(args[1]) == L"--smoke-test");
        state.file = state.smoke ? std::filesystem::absolute(args[2]) : defaultSettingsPath();
        state.settings = loadSettings(state.file);
        if (state.smoke && count > 3 && args[3][0])
            state.settings.gameRoot = args[3];
        if (!state.smoke && count > 1 && args[1][0])
            state.settings.gameRoot = args[1];
        LocalFree(args);
        wchar_t module[32768]{};
        GetModuleFileNameW(nullptr, module, DWORD(std::size(module)));
        state.executable = std::filesystem::path(module).parent_path() / L"dust_game.exe";
        WNDCLASSW wc{};
        wc.lpfnWndProc = procedure;
        wc.hInstance = instance;
        wc.lpszClassName = L"DustNativeLauncher";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        auto dpi = GetDpiForSystem();
        RECT r{0, 0, MulDiv(600, dpi, 96), MulDiv(724, dpi, 96)};
        AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        const int width =
            std::min(int(r.right - r.left), int(monitor.rcWork.right - monitor.rcWork.left));
        const int height =
            std::min(int(r.bottom - r.top), int(monitor.rcWork.bottom - monitor.rcWork.top));
        const int x =
            monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2;
        const int y =
            monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2;
        auto window = CreateWindowW(wc.lpszClassName, L"Dust — Launch settings", style, x, y, width,
                                    height, nullptr, nullptr, instance, nullptr);
        if (!window)
            throw std::runtime_error("cannot create launcher");
        ShowWindow(window, SW_SHOW);
        UpdateWindow(window);
        if (state.smoke)
            SetTimer(window, 1, 350, nullptr);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
            if (!IsDialogMessageW(window, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        int result = 0;
        if (state.smoke) {
            if (!state.launched)
                result = 1;
            else {
                WaitForSingleObject(state.child, 120000);
                DWORD code = 1;
                GetExitCodeProcess(state.child, &code);
                result = int(code);
            }
        }
        CoUninitialize();
        return result;
    } catch (const std::exception &error) {
        MessageBoxA(nullptr, error.what(), "Dust launcher", MB_OK | MB_ICONERROR);
        return 1;
    }
}
