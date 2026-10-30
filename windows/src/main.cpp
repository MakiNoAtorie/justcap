#include "overlay.h"
#include <shellapi.h>
#include <windowsx.h>
#include <objbase.h>
#include <cstdint>
#include <cwchar>

#define STR2(x) L#x
#define STR(x) STR2(x)

// Global hotkey: PrtScn (the Lightshot default on Windows). Change it with
// `reg add HKCU\Software\justcap /v hotkeyVK /t REG_DWORD /d 0x33 /f` and `hotkeyMods`.
static const UINT defaultVK = VK_SNAPSHOT, defaultMods = 0;
static const UINT WM_TRAY = WM_APP + 1;
enum { ID_CAPTURE = 1, ID_LOGIN, ID_QUIT };
enum { TIMER_CAPTURE = 1 };

static HWND app;
static HICON trayIcon;
static UINT taskbarCreated;
static bool custom;
static bool modal;  // A save dialog or an alert is open: no capture over it.

static void alert(const wchar_t* text) {
    modal = true;
    MessageBoxW(NULL, text, L"justcap", MB_ICONWARNING);
    modal = false;
}

static void capture() {
    if (modal) return;
    alert(L"capture");
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

static void showMenu(int x, int y) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_CAPTURE, custom ? L"Capture" : L"Capture  PrtScn");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_QUIT, L"Quit justcap");
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

static int selftest() { return 0; }

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
    custom = vk != defaultVK || mods != defaultMods;
    if (!RegisterHotKey(app, 1, mods | MOD_NOREPEAT, vk)) {
        alert(L"The justcap hotkey is not available. Another app uses it. On Windows 11 the Snipping Tool uses PrtScn: "
              L"turn off Settings > Accessibility > Keyboard > \"Use the Print screen key to open screen capture\", or set another hotkey:\n\n"
              L"reg add HKCU\\Software\\justcap /v hotkeyVK /t REG_DWORD /d 0x33 /f\n"
              L"reg add HKCU\\Software\\justcap /v hotkeyMods /t REG_DWORD /d 6 /f\n\n"
              L"(0x33 = the 3 key, 6 = Ctrl+Shift.) Then start justcap again. Capture in the tray menu still works.");
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
