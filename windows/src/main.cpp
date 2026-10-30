#include "overlay.h"
#include <shellapi.h>
#include <windowsx.h>
#include <objbase.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <mmsystem.h>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

#define STR2(x) L#x
#define STR(x) STR2(x)

// Global hotkey: PrtScn (the Lightshot default on Windows). Change it with Set Hotkey in the tray menu or
// `reg add HKCU\Software\justcap /v hotkeyVK /t REG_DWORD /d 0x33 /f` and `hotkeyMods`.
static const UINT defaultVK = VK_SNAPSHOT, defaultMods = 0;
static const wchar_t* runKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const UINT WM_TRAY = WM_APP + 1, WM_FINISH = WM_APP + 2;
enum { ID_CAPTURE = 1, ID_HOTKEY, ID_LOGIN, ID_QUIT };
enum { TIMER_CAPTURE = 1 };

static HWND app;
static HICON trayIcon;
static UINT taskbarCreated;
static bool custom;
static UINT hotkeyVK, hotkeyMods;  // The registered hotkey.
static HWND hotkeyWnd;
static bool modal;  // A save dialog or an alert is open: no capture over it.
static std::vector<Overlay*> windows;
static HWND previous;  // Gets the focus back after the overlay.
static ComPtr<IWICBitmap> result;
static bool resultSave;

static void alert(const wchar_t* text) {
    modal = true;
    MessageBoxW(NULL, text, L"justcap", MB_ICONWARNING);
    modal = false;
}

static BOOL CALLBACK addMonitor(HMONITOR h, HDC, LPRECT, LPARAM list) {
    reinterpret_cast<std::vector<HMONITOR>*>(list)->push_back(h);
    return TRUE;
}

/// The monitor pixels as a WIC bitmap that owns its memory.
static ComPtr<IWICBitmap> grab(HDC screen, const RECT& r) {
    int w = r.right - r.left, h = r.bottom - r.top;
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, w, h);
    HGDIOBJ old = SelectObject(mem, bitmap);
    BOOL ok = BitBlt(mem, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    SelectObject(mem, old);
    DeleteDC(mem);
    ComPtr<IWICBitmap> raw, out;
    ComPtr<IWICFormatConverter> converter;
    if (ok) wic->CreateBitmapFromHBITMAP(bitmap, NULL, WICBitmapIgnoreAlpha, &raw);
    DeleteObject(bitmap);
    if (raw && SUCCEEDED(wic->CreateFormatConverter(&converter))
        && SUCCEEDED(converter->Initialize(raw.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)))
        wic->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnLoad, &out);
    return out;
}

static void capture() {
    if (!windows.empty() || modal) return;
    gfxInit();
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(NULL, NULL, addMonitor, reinterpret_cast<LPARAM>(&monitors));
    std::vector<ComPtr<IWICBitmap>> images;
    std::vector<RECT> rects;
    HDC screen = GetDC(NULL);
    for (HMONITOR h : monitors) {
        MONITORINFO info = {sizeof info};
        GetMonitorInfoW(h, &info);
        rects.push_back(info.rcMonitor);
        images.push_back(grab(screen, info.rcMonitor));
    }
    ReleaseDC(NULL, screen);
    for (auto& image : images) {
        if (image) continue;
        alert(L"justcap cannot capture the screen.");
        return;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground != app) previous = foreground;
    for (size_t i = 0; i < monitors.size(); i++) {
        UINT dpiX = 96, dpiY = 96;
        GetDpiForMonitor(monitors[i], MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        auto o = new Overlay(images[i], rects[i], dpiX / 96.0);
        o->onDone = [](ComPtr<IWICBitmap> image, bool save) {
            result = image;
            resultSave = save;
            PostMessageW(app, WM_FINISH, 0, 0);
        };
        // A new selection on one monitor clears the selection on the others.
        o->onSelectionStart = [o] {
            for (Overlay* other : windows)
                if (other != o) other->clearSelection();
        };
        windows.push_back(o);
    }
    POINT cursor;
    GetCursorPos(&cursor);
    HMONITOR under = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    HWND focus = windows[0]->hwnd;
    for (size_t i = 0; i < monitors.size(); i++)
        if (monitors[i] == under) focus = windows[i]->hwnd;
    SetForegroundWindow(focus);
    SetFocus(focus);
}

static bool encodePng(IWICBitmap* image, IStream* stream) {
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    UINT w, h;
    image->GetSize(&w, &h);
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    return SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, NULL, &encoder))
        && SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache))
        && SUCCEEDED(encoder->CreateNewFrame(&frame, NULL))
        && SUCCEEDED(frame->Initialize(NULL))
        && SUCCEEDED(frame->SetSize(w, h))
        && SUCCEEDED(frame->SetPixelFormat(&format))
        && SUCCEEDED(frame->WriteSource(image, NULL))
        && SUCCEEDED(frame->Commit())
        && SUCCEEDED(encoder->Commit());
}

/// The image as PNG bytes in a moveable global block, exact size: the stream's own block can be larger.
static HGLOBAL pngGlobal(IWICBitmap* image) {
    ComPtr<IStream> stream;
    HGLOBAL source;
    STATSTG stat;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &stream)) || !encodePng(image, stream.Get())
        || FAILED(GetHGlobalFromStream(stream.Get(), &source)) || FAILED(stream->Stat(&stat, STATFLAG_NONAME)))
        return NULL;
    SIZE_T size = SIZE_T(stat.cbSize.QuadPart);
    HGLOBAL out = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!out) return NULL;
    memcpy(GlobalLock(out), GlobalLock(source), size);
    GlobalUnlock(source);
    GlobalUnlock(out);
    return out;
}

/// CF_DIB: 32 bpp, bottom-up rows.
static HGLOBAL dibGlobal(IWICBitmap* image) {
    UINT w, h, stride, size;
    image->GetSize(&w, &h);
    WICRect all = {0, 0, INT(w), INT(h)};
    ComPtr<IWICBitmapLock> lock;
    BYTE* src;
    if (FAILED(image->Lock(&all, WICBitmapLockRead, &lock)) || FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &src))) return NULL;
    HGLOBAL out = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + SIZE_T(w) * 4 * h);
    if (!out) return NULL;
    auto header = static_cast<BITMAPINFOHEADER*>(GlobalLock(out));
    *header = {sizeof(BITMAPINFOHEADER), LONG(w), LONG(h), 1, 32, BI_RGB, w * 4 * h};
    BYTE* dst = reinterpret_cast<BYTE*>(header + 1);
    for (UINT y = 0; y < h; y++) memcpy(dst + (h - 1 - y) * w * 4, src + y * stride, w * 4);
    GlobalUnlock(out);
    return out;
}

static bool copyImage(IWICBitmap* image) {
    HGLOBAL png = pngGlobal(image), dib = dibGlobal(image);
    bool open = false;
    // Another app can hold the clipboard for a moment.
    for (int i = 0; i < 10 && !(open = OpenClipboard(app)); i++) Sleep(20);
    if (!open || !png || !dib) {
        if (open) CloseClipboard();
        if (png) GlobalFree(png);
        if (dib) GlobalFree(dib);
        return false;
    }
    EmptyClipboard();
    if (!SetClipboardData(RegisterClipboardFormatW(L"PNG"), png)) GlobalFree(png);
    if (!SetClipboardData(CF_DIB, dib)) GlobalFree(dib);
    CloseClipboard();
    return true;
}

/// Where Win+PrtScn saves, else the Desktop.
static ComPtr<IShellItem> saveFolder() {
    ComPtr<IShellItem> item;
    for (auto id : {FOLDERID_Screenshots, FOLDERID_Desktop}) {
        PWSTR path = NULL;
        bool found = SUCCEEDED(SHGetKnownFolderPath(id, 0, NULL, &path)) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES
            && SUCCEEDED(SHCreateItemFromParsingName(path, NULL, IID_PPV_ARGS(&item)));
        CoTaskMemFree(path);
        if (found) break;
    }
    return item;
}

static void saveImage(IWICBitmap* image) {
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return alert(L"justcap cannot open the save dialog.");
    COMDLG_FILTERSPEC types[] = {{L"PNG image", L"*.png"}};
    dialog->SetFileTypes(1, types);
    dialog->SetDefaultExtension(L"png");
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"Screenshot %04d-%02d-%02d %02d.%02d.%02d.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    dialog->SetFileName(name);
    if (auto folder = saveFolder()) dialog->SetFolder(folder.Get());
    modal = true;
    HRESULT shown = dialog->Show(NULL);
    modal = false;
    ComPtr<IShellItem> item;
    PWSTR path = NULL;
    if (FAILED(shown) || FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    ComPtr<IStream> stream;
    bool ok = SUCCEEDED(SHCreateStreamOnFileEx(path, STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, NULL, &stream)) && encodePng(image, stream.Get());
    CoTaskMemFree(path);
    if (!ok) alert(L"justcap cannot save the file.");
}

static void finish() {
    // Release the windows: they hold the full-screen images.
    for (Overlay* o : windows) delete o;
    windows.clear();
    ComPtr<IWICBitmap> image = result;
    result = nullptr;
    if (image) {
        if (resultSave) {
            saveImage(image.Get());
        } else if (copyImage(image.Get())) {
            PlaySoundW(L"SystemAsterisk", NULL, SND_ALIAS | SND_ASYNC);
        } else {
            alert(L"justcap cannot copy the image to the clipboard.");
        }
        image = nullptr;
    }
    // After the save dialog: it needs the foreground first.
    if (previous) SetForegroundWindow(previous);
    previous = NULL;
    SetProcessWorkingSetSize(GetCurrentProcess(), SIZE_T(-1), SIZE_T(-1));  // Graphics drivers stay loaded: back to the idle footprint.
}

/// The camera glyph as an icon. Text coverage becomes alpha, in the color of the taskbar theme.
static HICON makeIcon() {
    DWORD light = 0, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), 32, -32, 1, 32, BI_RGB};
    void* bits;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HFONT font = CreateFontW(-28, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe MDL2 Assets");
    HGDIOBJ oldBitmap = SelectObject(dc, color), oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT r = {0, 0, 32, 32};
    wchar_t glyph = 0xE722;
    DrawTextW(dc, &glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldBitmap);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);
    uint32_t ink = light ? 0 : 0xFFFFFF;
    uint32_t* px = static_cast<uint32_t*>(bits);
    for (int i = 0; i < 32 * 32; i++) px[i] = (px[i] & 0xFF) << 24 | ink;
    HBITMAP mask = CreateBitmap(32, 32, 1, 1, NULL);
    ICONINFO info = {TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

static void addTray() {
    NOTIFYICONDATAW n = {sizeof n};
    n.hWnd = app;
    n.uID = 1;
    n.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = trayIcon;
    wcscpy_s(n.szTip, L"justcap 0.1.0 (" STR(BUILD_NUMBER) L")");
    Shell_NotifyIconW(NIM_ADD, &n);
    n.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &n);
}

static void removeTray() {
    NOTIFYICONDATAW n = {sizeof n};
    n.hWnd = app;
    n.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &n);
}

static bool loginEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, runKey, L"justcap", RRF_RT_REG_SZ, NULL, NULL, NULL) == ERROR_SUCCESS;
}

static void toggleLogin() {
    LSTATUS status;
    if (loginEnabled()) {
        status = RegDeleteKeyValueW(HKEY_CURRENT_USER, runKey, L"justcap");
    } else {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
        status = RegSetKeyValueW(HKEY_CURRENT_USER, runKey, L"justcap", REG_SZ, quoted.c_str(), DWORD((quoted.size() + 1) * sizeof(wchar_t)));
    }
    if (status != ERROR_SUCCESS) alert(L"justcap cannot change the Launch at Login setting.");
}

static bool applyHotkey(UINT vk, UINT mods) {
    UnregisterHotKey(app, 1);
    if (!RegisterHotKey(app, 1, mods | MOD_NOREPEAT, vk)) {
        RegisterHotKey(app, 1, hotkeyMods | MOD_NOREPEAT, hotkeyVK);
        return false;
    }
    regSet(L"hotkeyVK", vk);
    regSet(L"hotkeyMods", mods);
    hotkeyVK = vk;
    hotkeyMods = mods;
    custom = vk != defaultVK || mods != defaultMods;
    return true;
}

static LRESULT CALLBACK hotkeyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        NONCLIENTMETRICSW metrics = {sizeof metrics};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof metrics, &metrics, 0);
        HFONT font = CreateFontIndirectW(&metrics.lfMessageFont);
        HGDIOBJ old = SelectObject(dc, font);
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        RECT r;
        GetClientRect(hwnd, &r);
        r.top = (r.bottom - 2 * tm.tmHeight) / 2;
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"Press the new hotkey.\nEsc keeps the current one.", -1, &r, DT_CENTER | DT_NOPREFIX);
        SelectObject(dc, old);
        DeleteObject(font);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        UINT vk = UINT(wp);
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return 0;
        if (vk == VK_ESCAPE) return DestroyWindow(hwnd), 0;
        UINT mods = (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) | (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0)
            | (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0) | (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0 ? MOD_WIN : 0);
        if (applyHotkey(vk, mods)) DestroyWindow(hwnd);
        else alert(L"That hotkey is not available. Another app uses it. Try another one.");
        return 0;  // Alt combinations must not reach the system menu.
    }
    case WM_SYSCHAR: return 0;  // No beep for Alt combinations.
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_NCDESTROY: hotkeyWnd = NULL; break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/// A small window that takes the next key combination as the hotkey.
static void setHotkey() {
    if (hotkeyWnd) return (void)SetForegroundWindow(hotkeyWnd);
    HINSTANCE inst = GetModuleHandleW(NULL);
    static bool registered;
    if (!registered) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = hotkeyProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = HBRUSH(COLOR_WINDOW + 1);
        wc.lpszClassName = L"justcap.hotkey";
        registered = RegisterClassW(&wc) != 0;
    }
    double scale = GetDpiForSystem() / 96.0;
    int w = int(360 * scale), h = int(110 * scale);
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    hotkeyWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME, L"justcap.hotkey", L"justcap", WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, app, NULL, inst, NULL);
    if (!hotkeyWnd) return;
    ShowWindow(hotkeyWnd, SW_SHOW);
    SetForegroundWindow(hotkeyWnd);
    SetFocus(hotkeyWnd);
}

static void showMenu(int x, int y) {
    if (modal) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_CAPTURE, custom ? L"Capture" : L"Capture  PrtScn");
    AppendMenuW(menu, MF_STRING, ID_HOTKEY, L"Set Hotkey…");
    AppendMenuW(menu, MF_STRING | (loginEnabled() ? MF_CHECKED : 0), ID_LOGIN, L"Launch at Login");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit justcap");
    previous = GetForegroundWindow();
    SetForegroundWindow(app);  // Or the menu does not close on an outside click.
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, x, y, 0, app, NULL);
    PostMessageW(app, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

static LRESULT CALLBACK appProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreated && msg != 0) {
        addTray();
        return 0;
    }
    switch (msg) {
    case WM_HOTKEY:
        capture();
        return 0;
    case WM_FINISH:
        finish();
        return 0;
    case WM_TRAY:
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
            capture();
            break;
        case WM_CONTEXTMENU:
            showMenu(GET_X_LPARAM(wp), GET_Y_LPARAM(wp));
            break;
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        // Lets the tray menu fade out first, so it is not in the capture.
        case ID_CAPTURE: SetTimer(hwnd, TIMER_CAPTURE, 150, NULL); break;
        case ID_HOTKEY: setHotkey(); break;
        case ID_LOGIN: toggleLogin(); break;
        case ID_QUIT: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_TIMER:
        KillTimer(hwnd, wp);
        capture();
        return 0;
    case WM_DESTROY:
        removeTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/// Windows 11 22H2 and later: the Snipping Tool gets PrtScn first, and RegisterHotKey still succeeds.
static bool snippingOwnsPrintScreen() {
    DWORD enabled, size = sizeof enabled;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", L"PrintScreenKeyForSnippingEnabled", RRF_RT_REG_DWORD, NULL, &enabled, &size) == ERROR_SUCCESS)
        return enabled != 0;
    wchar_t build[16] = L"";
    DWORD bytes = sizeof build;
    RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuildNumber", RRF_RT_REG_SZ, NULL, build, &bytes);
    return _wtoi(build) >= 22621;
}

static void say(const char* text) {
    DWORD written;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);  // A redirected stdout comes first: AttachConsole replaces it.
    if ((!out || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) out = GetStdHandle(STD_OUTPUT_HANDLE);
    WriteFile(out, text, DWORD(strlen(text)), &written, NULL);
}

/// `justcap --selftest`: renders a selection with annotations and checks the output.
static int selftest() {
    gfxInit();
    // Source: 200 x 100 px, left half red, right half blue.
    std::vector<uint32_t> source(200 * 100);
    for (size_t i = 0; i < source.size(); i++) source[i] = i % 200 < 100 ? 0xFFFF0000 : 0xFF0000FF;
    ComPtr<IWICBitmap> image;
    wic->CreateBitmapFromMemory(200, 100, GUID_WICPixelFormat32bppPBGRA, 800, UINT(source.size() * 4), reinterpret_cast<BYTE*>(source.data()), &image);
    Overlay view(image, RECT{0, 0, 200, 100}, 2, false);  // scale 2
    // The y values are mirrored from main.swift: this view has its origin at the top.
    view.sel = Rect{20, 20, 40, 20};
    view.annotations = {
        {Tool::line, {0, 1, 0, 1}, 4, {{36, 25}, {60, 25}}},
        // One 10 DIP block across the red/blue boundary at x = 50.
        {Tool::pixelate, {0, 0, 1, 1}, 4, {{45, 28}, {55, 38}}},
        {Tool::text, {1, 1, 1, 1}, 3, {{22, 20}}, L"H"},
    };
    ComPtr<IWICBitmap> out = view.render();
    UINT w = 0, h = 0, stride, size;
    if (out) out->GetSize(&w, &h);
    if (w != 80 || h != 40) return say("selftest failed: size\n"), 1;
    WICRect all = {0, 0, 80, 40};
    ComPtr<IWICBitmapLock> lock;
    BYTE* px;
    out->Lock(&all, WICBitmapLockRead, &lock);
    lock->GetStride(&stride);
    lock->GetDataPointer(&size, &px);
    struct Color { double r, g, b; };
    auto at = [&](int x, int y) {
        BYTE* p = px + y * stride + x * 4;
        return Color{p[2] / 255.0, p[1] / 255.0, p[0] / 255.0};
    };
    Color line = at(40, 10), red = at(2, 2), blue = at(78, 2), blocks = at(60, 26);
    if (!(line.g > 0.8 && line.r < 0.2)) return say("selftest failed: annotation\n"), 1;
    if (!(red.r > 0.9 && red.b < 0.1)) return say("selftest failed: source image, red half\n"), 1;
    if (!(blue.b > 0.9 && blue.r < 0.1)) return say("selftest failed: source image, blue half\n"), 1;
    if (!(blocks.r > 0.2 && blocks.r < 0.8 && blocks.b > 0.2 && blocks.b < 0.8)) return say("selftest failed: pixelate\n"), 1;
    bool glyph = false;
    for (int x = 4; x < 28; x++) {
        for (int y = 0; y < 36; y++) {
            Color c = at(x, y);
            glyph = glyph || (c.r > 0.9 && c.g > 0.9 && c.b > 0.9);
        }
    }
    if (!glyph) return say("selftest failed: text\n"), 1;

    // 125 %: the output must match the source pixel by pixel.
    for (size_t i = 0; i < source.size(); i++) source[i] = 0xFF000000 | (uint32_t(i) * 2654435761u >> 8);
    ComPtr<IWICBitmap> noise, fractional;
    wic->CreateBitmapFromMemory(200, 100, GUID_WICPixelFormat32bppPBGRA, 800, UINT(source.size() * 4), reinterpret_cast<BYTE*>(source.data()), &noise);
    Overlay scaled(noise, RECT{0, 0, 200, 100}, 1.25, false);
    scaled.sel = Rect{17 / 1.25, 11 / 1.25, 101 / 1.25, 53 / 1.25};
    fractional = scaled.render();
    if (fractional) fractional->GetSize(&w, &h);
    if (!fractional || w != 101 || h != 53) return say("selftest failed: size at 125 %\n"), 1;
    WICRect part = {0, 0, 101, 53};
    ComPtr<IWICBitmapLock> partLock;
    fractional->Lock(&part, WICBitmapLockRead, &partLock);
    partLock->GetStride(&stride);
    partLock->GetDataPointer(&size, &px);
    for (UINT y = 0; y < h; y++)
        if (memcmp(px + y * stride, &source[(y + 11) * 200 + 17], 101 * 4) != 0) return say("selftest failed: pixels at 125 %\n"), 1;
    say("selftest ok\n");
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (wcsstr(cmd, L"--selftest")) return selftest();
    CreateMutexW(NULL, FALSE, L"justcap");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = appProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"justcap.app";
    RegisterClassW(&wc);
    taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    // Hidden top-level window, not HWND_MESSAGE: a message-only window gets no TaskbarCreated broadcast and cannot be foreground.
    app = CreateWindowExW(0, wc.lpszClassName, L"justcap", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    trayIcon = makeIcon();
    addTray();

    UINT vk = regGet(L"hotkeyVK", defaultVK), mods = regGet(L"hotkeyMods", defaultMods);
    hotkeyVK = vk;
    hotkeyMods = mods;
    custom = vk != defaultVK || mods != defaultMods;
    if (!RegisterHotKey(app, 1, mods | MOD_NOREPEAT, vk) || (vk == VK_SNAPSHOT && mods == 0 && snippingOwnsPrintScreen())) {
        alert(L"The justcap hotkey is not available. Another app uses it. On Windows 11 the Snipping Tool uses PrtScn: "
              L"turn off Settings > Accessibility > Keyboard > \"Use the Print screen key to open screen capture\", or set another hotkey:\n\n"
              L"reg add HKCU\\Software\\justcap /v hotkeyVK /t REG_DWORD /d 0x33 /f\n"
              L"reg add HKCU\\Software\\justcap /v hotkeyMods /t REG_DWORD /d 6 /f\n\n"
              L"(0x33 = the 3 key, 6 = Ctrl+Shift.) Then start justcap again, or use Set Hotkey in the tray menu. Capture in the tray menu still works.");
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
