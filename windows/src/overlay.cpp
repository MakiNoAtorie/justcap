#include "overlay.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>

ComPtr<IWICImagingFactory> wic;
static ComPtr<ID2D1Factory> d2d;
static ComPtr<IDWriteFactory> dw;
static const wchar_t* windowClass = L"justcap.overlay";

void gfxInit() {
    if (d2d) return;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Overlay::proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = windowClass;
    RegisterClassW(&wc);
}

static D2D1_RECT_F dx(const Rect& r) {
    return D2D1::RectF(float(r.x), float(r.y), float(r.right()), float(r.bottom()));
}

Overlay::Overlay(ComPtr<IWICBitmap> img, RECT m, double scale, bool window) : image(img), k(scale) {
    int pw = m.right - m.left, ph = m.bottom - m.top;
    bounds = {0, 0, pw / k, ph / k};
    if (!window) return;
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, windowClass, L"justcap", WS_POPUP, m.left, m.top, pw, ph,
                           NULL, NULL, GetModuleHandleW(NULL), this);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
}

Overlay::~Overlay() {
    if (hwnd) DestroyWindow(hwnd);
}

LRESULT CALLBACK Overlay::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto o = static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        o->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(o));
    }
    auto o = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return o ? o->handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Overlay::handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_CLOSE:  // Alt+F4 closes all overlays, not one.
        cancel();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) cancel();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Overlay::cancel() { onDone(nullptr, false); }

void Overlay::createTarget() {
    RECT c;
    GetClientRect(hwnd, &c);
    float dpi = float(96 * k);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi, dpi);
    // Partial redraw needs the back buffer kept between frames.
    auto hprops = D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(c.right, c.bottom), D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS);
    d2d->CreateHwndRenderTarget(props, hprops, &rt);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    rt->CreateBitmapFromWicBitmap(image.Get(), &bmp);
}

void Overlay::paint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    if (!rt) createTarget();
    Rect dirty = {ps.rcPaint.left / k, ps.rcPaint.top / k, (ps.rcPaint.right - ps.rcPaint.left) / k, (ps.rcPaint.bottom - ps.rcPaint.top) / k};
    rt->BeginDraw();
    rt->PushAxisAlignedClip(dx(dirty), D2D1_ANTIALIAS_MODE_ALIASED);
    rt->DrawBitmap(bmp.Get(), dx(bounds), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    rt->PopAxisAlignedClip();
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        bmp = nullptr;
        rt = nullptr;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    EndPaint(hwnd, &ps);
}
