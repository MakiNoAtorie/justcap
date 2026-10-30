#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <functional>

using Microsoft::WRL::ComPtr;

extern ComPtr<IWICImagingFactory> wic;
void gfxInit();

inline DWORD regGet(const wchar_t* name, DWORD fallback) {
    DWORD v, size = sizeof v;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\justcap", name, RRF_RT_REG_DWORD, NULL, &v, &size) == ERROR_SUCCESS ? v : fallback;
}

inline void regSet(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\justcap", name, REG_DWORD, &v, sizeof v);
}

struct Pt {
    double x = 0, y = 0;
    bool operator==(const Pt& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Pt& o) const { return !(*this == o); }
};

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    double right() const { return x + w; }
    double bottom() const { return y + h; }
    bool contains(Pt p) const { return p.x >= x && p.x < right() && p.y >= y && p.y < bottom(); }
    Rect inset(double dx, double dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
};

/// One window per monitor. Shows the frozen screen image, the selection and the annotations.
/// All coordinates are DIPs, origin top-left. A window of false is the self-test mode: no window, no toolbar.
class Overlay {
public:
    Overlay(ComPtr<IWICBitmap> image, RECT monitor, double k, bool window = true);
    ~Overlay();

    HWND hwnd = NULL;
    /// Called with the result image (null = cancel) and the save flag. The owner must not destroy this overlay inside the call.
    std::function<void(ComPtr<IWICBitmap>, bool)> onDone;
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);

private:
    LRESULT handle(UINT, WPARAM, LPARAM);
    void createTarget();
    void paint();
    void cancel();

    ComPtr<IWICBitmap> image;
    double k;
    Rect bounds;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1Bitmap> bmp;
};
