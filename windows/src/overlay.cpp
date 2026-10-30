#include "overlay.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

ComPtr<IWICImagingFactory> wic;
static ComPtr<ID2D1Factory> d2d;
static ComPtr<IDWriteFactory> dw;
static ComPtr<IDWriteTextFormat> labelFormat;
static const wchar_t* windowClass = L"justcap.overlay";

void gfxInit() {
    if (d2d) return;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
    dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11, L"", &labelFormat);
    labelFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = Overlay::proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = windowClass;
    RegisterClassW(&wc);
}

static D2D1_RECT_F dx(const Rect& r) {
    return D2D1::RectF(float(r.x), float(r.y), float(r.right()), float(r.bottom()));
}

static D2D1_POINT_2F dx(Pt p) { return D2D1::Point2F(float(p.x), float(p.y)); }

static ComPtr<ID2D1SolidColorBrush> brush(ID2D1RenderTarget* t, D2D1_COLOR_F c) {
    ComPtr<ID2D1SolidColorBrush> b;
    t->CreateSolidColorBrush(c, &b);
    return b;
}

static ComPtr<IDWriteTextLayout> layout(const std::wstring& s, IDWriteTextFormat* f) {
    ComPtr<IDWriteTextLayout> l;
    dw->CreateTextLayout(s.c_str(), UINT32(s.size()), f, 100000, 100000, &l);
    return l;
}

static Pt extent(IDWriteTextLayout* l) {
    DWRITE_TEXT_METRICS m;
    l->GetMetrics(&m);
    return {m.widthIncludingTrailingWhitespace, m.height};
}

Overlay::Overlay(ComPtr<IWICBitmap> img, RECT m, double scale, bool window) : image(img), k(scale) {
    int pw = m.right - m.left, ph = m.bottom - m.top;
    bounds = {0, 0, pw / k, ph / k};
    width = std::clamp(double(regGet(L"width", 3)), 1.0, 20.0);
    cursor = LoadCursorW(NULL, IDC_CROSS);
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
    case WM_SETCURSOR:
        if (LOWORD(lp) != HTCLIENT) break;
        SetCursor(cursor);
        return TRUE;
    case WM_CLOSE:  // Alt+F4 closes all overlays, not one.
        cancel();
        return 0;
    case WM_KEYDOWN: {
        bool shift = GetKeyState(VK_SHIFT) & 0x8000, ctrl = GetKeyState(VK_CONTROL) & 0x8000;
        if (ctrl && (wp == 'C' || wp == 'S')) finish(wp == 'S');
        switch (wp) {
        case VK_ESCAPE: cancel(); break;
        case VK_RETURN: finish(false); break;
        case VK_LEFT: nudge(-1, 0, shift); break;
        case VK_RIGHT: nudge(1, 0, shift); break;
        case VK_UP: nudge(0, -1, shift); break;
        case VK_DOWN: nudge(0, 1, shift); break;
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (wp & MK_LBUTTON) mouseDragged(mouse(lp), wp & MK_SHIFT);
        else mouseMoved(mouse(lp));
        return 0;
    case WM_LBUTTONDOWN: {
        // The keyboard follows the mouse.
        if (GetForegroundWindow() != hwnd) SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        Pt p = mouse(lp);
        if (!barHidden && barRect.contains(p)) return 0;
        SetCapture(hwnd);
        mouseDown(p);
        return 0;
    }
    case WM_LBUTTONUP:
        if (GetCapture() != hwnd) return 0;
        mouseUp();
        ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if (drag.kind != Drag::none) mouseUp();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Overlay::cancel() { onDone(nullptr, false); }

void Overlay::finish(bool save) {
    commitText();
    if (auto out = render()) onDone(out, save);
}

Pt Overlay::mouse(LPARAM lp) const { return {GET_X_LPARAM(lp) / k, GET_Y_LPARAM(lp) / k}; }

/// Selection edges stay on the pixel grid: k can be 1.25 or 1.5.
double Overlay::snap(double v) const { return std::round(v * k) / k; }

void Overlay::invalidate(const Rect& r) {
    if (!hwnd) return;
    RECT px = {LONG(std::floor(r.x * k)) - 1, LONG(std::floor(r.y * k)) - 1, LONG(std::ceil(r.right() * k)) + 1, LONG(std::ceil(r.bottom() * k)) + 1};
    InvalidateRect(hwnd, &px, FALSE);
}

void Overlay::setSel(std::optional<Rect> s) {
    auto old = sel;
    sel = s;
    if (old) invalidate(chrome(*old));
    if (sel) invalidate(chrome(*sel));
}

void Overlay::annotationsChanged() {
    if (sel) invalidate(*sel);
    if (hoverText && *hoverText >= int(annotations.size())) setHover(std::nullopt);
}

void Overlay::setHover(std::optional<int> h) {
    auto old = hoverText;
    hoverText = h;
    for (auto i : {old, hoverText})
        if (i && *i < int(annotations.size())) invalidate(textRect(annotations[*i]).inset(-6, -6));
}

void Overlay::setCursor(LPCWSTR id) {
    cursor = LoadCursorW(NULL, id);
    SetCursor(cursor);
}

void Overlay::mouseMoved(Pt p) {
    bool onBar = !barHidden && barRect.contains(p);
    auto hover = tool == Tool::text && !onBar ? textIndex(p) : std::nullopt;
    if (hover != hoverText) setHover(hover);
    if (onBar) return setCursor(IDC_ARROW);
    if (!sel) return setCursor(IDC_CROSS);
    if (auto h = handleHit(p, *sel)) {
        setCursor(h->hy == 0 ? IDC_SIZEWE : h->hx == 0 ? IDC_SIZENS : h->hx * h->hy > 0 ? IDC_SIZENWSE : IDC_SIZENESW);
    } else if (hoverText) {
        setCursor(IDC_HAND);
    } else {
        setCursor(!tool && sel->contains(p) ? IDC_SIZEALL : IDC_CROSS);
    }
}

// MARK: Toolbar

static const double barWidth = 362, barHeight = 32, buttonWidth = 26, buttonHeight = 24;

/// Buttons: seven tools, color, undo, copy, save, close. 2 DIP apart, 10 after the tools and after undo.
Rect Overlay::button(int i) const {
    return {barRect.x + 6 + i * (buttonWidth + 2) + (i >= 7 ? 8 : 0) + (i >= 9 ? 8 : 0), barRect.y + 4, buttonWidth, buttonHeight};
}

int Overlay::buttonAt(Pt p) const {
    for (int i = 0; i < 12; i++)
        if (button(i).contains(p)) return i;
    return -1;
}

void Overlay::layoutBar() {
    if (!barHidden) invalidate(barRect);
    if (!sel) {
        barHidden = true;
        return;
    }
    const Rect& s = *sel;
    double x = std::max(4.0, std::min(s.right() - barWidth, bounds.right() - barWidth - 4));
    double y = s.bottom() + 6;                                       // below the selection
    if (y + barHeight > bounds.bottom() - 4) y = s.y - barHeight - 6;  // above
    if (y < 4) y = s.bottom() - barHeight - 6;                       // inside
    barRect = {x, y, barWidth, barHeight};
    barHidden = hwnd == NULL;
    if (!barHidden) invalidate(barRect);
}

void Overlay::hideBar() {
    if (!barHidden) invalidate(barRect);
    barHidden = true;
}

/// Arrow keys move the selection by 1 DIP, with Shift by 10 DIP.
void Overlay::nudge(double dx, double dy, bool shift) {
    if (!sel) return;
    Rect r = *sel;
    double step = shift ? 10 : 1;
    r.x = snap(std::max(0.0, std::min(r.x + dx * step, bounds.w - r.w)));
    r.y = snap(std::max(0.0, std::min(r.y + dy * step, bounds.h - r.h)));
    setSel(r);
    layoutBar();
}

// MARK: Mouse

void Overlay::mouseDown(Pt p) {
    commitText();
    setHover(std::nullopt);
    if (sel) {
        const Rect s = *sel;
        if (auto h = handleHit(p, s)) {
            drag = {};
            drag.kind = Drag::resize;
            drag.anchor = {h->hx < 0 ? s.right() : s.x, h->hy < 0 ? s.bottom() : s.y};
            drag.hx = h->hx != 0;
            drag.hy = h->hy != 0;
            drag.orig = s;
            hideBar();
            return;
        }
        if (s.contains(p)) {
            if (tool == Tool::text) {
                beginText(p);
            } else if (tool) {
                current = Annotation{*tool, D2D1::ColorF(D2D1::ColorF::Red), width, {p, p}};
                drag = {};
                drag.kind = Drag::draw;
            } else {
                drag = {};
                drag.kind = Drag::move;
                drag.dx = p.x - s.x;
                drag.dy = p.y - s.y;
                hideBar();
            }
            return;
        }
        // A click outside must not delete annotations.
        if (!annotations.empty()) return;
    }
    if (onSelectionStart) onSelectionStart();
    drag = {};
    drag.kind = Drag::resize;
    drag.anchor = p;
    drag.hx = drag.hy = true;
    setSel(Rect{p.x, p.y, 0, 0});
    hideBar();
}

void Overlay::mouseDragged(Pt p, bool shift) {
    p.x = std::max(0.0, std::min(p.x, bounds.w));
    p.y = std::max(0.0, std::min(p.y, bounds.h));
    switch (drag.kind) {
    case Drag::none:
        break;
    case Drag::resize: {
        const Pt a = drag.anchor;
        const Rect o = drag.orig;
        double x0 = drag.hx ? std::min(a.x, p.x) : o.x, x1 = drag.hx ? std::max(a.x, p.x) : o.right();
        double y0 = drag.hy ? std::min(a.y, p.y) : o.y, y1 = drag.hy ? std::max(a.y, p.y) : o.bottom();
        setSel(Rect{snap(x0), snap(y0), snap(x1 - x0), snap(y1 - y0)});
        break;
    }
    case Drag::move: {
        if (!sel) break;
        Rect r = *sel;
        r.x = snap(std::max(0.0, std::min(p.x - drag.dx, bounds.w - r.w)));
        r.y = snap(std::max(0.0, std::min(p.y - drag.dy, bounds.h - r.h)));
        setSel(r);
        break;
    }
    case Drag::text:
        break;
    case Drag::draw: {
        if (!current) break;
        Annotation& a = *current;
        std::vector<Pt> old = a.points;
        bool freehand = a.tool == Tool::pen || a.tool == Tool::marker;
        if (freehand) a.points.push_back(p);
        else a.points[1] = shift ? constrain(a.points[0], p, a.tool != Tool::line && a.tool != Tool::arrow) : p;
        std::vector<Pt> pts = old;
        if (freehand) pts = {a.points[a.points.size() - 2], a.points.back()};
        else pts.insert(pts.end(), a.points.begin(), a.points.end());
        double margin = a.width * 5 + 10;  // marker width, arrow head
        invalidate(bbox(pts).inset(-margin, -margin));
        break;
    }
    }
}

void Overlay::mouseUp() {
    if (current) {
        Annotation a = *current;
        bool freehand = a.tool == Tool::pen || a.tool == Tool::marker;
        current = std::nullopt;
        if (freehand || a.points[0] != a.points[1]) {
            annotations.push_back(a);
            annotationsChanged();
        } else if (sel) {
            invalidate(*sel);
        }
    }
    if (sel && (sel->w < 2 || sel->h < 2)) {
        annotations.clear();
        annotationsChanged();
        setSel(std::nullopt);
    }
    drag = {};
    layoutBar();
}

void Overlay::clearSelection() {
    commitText();
    annotations.clear();
    annotationsChanged();
    setSel(std::nullopt);
    hideBar();
}

std::vector<Overlay::Handle> Overlay::handlePoints(const Rect& s) const {
    std::vector<Handle> out;
    for (int hx = -1; hx <= 1; hx++)
        for (int hy = -1; hy <= 1; hy++)
            if (hx != 0 || hy != 0)
                out.push_back({{hx < 0 ? s.x : hx > 0 ? s.right() : s.x + s.w / 2, hy < 0 ? s.y : hy > 0 ? s.bottom() : s.y + s.h / 2}, hx, hy});
    return out;
}

std::optional<Overlay::Handle> Overlay::handleHit(Pt p, const Rect& s) const {
    for (const Handle& h : handlePoints(s))
        if (std::abs(h.p.x - p.x) <= 9 && std::abs(h.p.y - p.y) <= 9) return h;
    return std::nullopt;
}

Pt Overlay::constrain(Pt a, Pt p, bool square) const {
    double ddx = p.x - a.x, ddy = p.y - a.y;
    if (square) {
        double side = std::max(std::abs(ddx), std::abs(ddy));
        return {a.x + (ddx < 0 ? -side : side), a.y + (ddy < 0 ? -side : side)};
    }
    const double pi4 = std::atan(1.0);
    double ang = std::round(std::atan2(ddy, ddx) / pi4) * pi4, len = std::hypot(ddx, ddy);
    return {a.x + len * std::cos(ang), a.y + len * std::sin(ang)};
}

// MARK: Text

std::optional<int> Overlay::textIndex(Pt) const { return std::nullopt; }
Rect Overlay::textRect(const Annotation&) const { return {}; }
void Overlay::commitText() {}
void Overlay::beginText(Pt) {}

// MARK: Drawing

/// The area drawn for a selection: border, handles and size label.
Rect Overlay::chrome(const Rect& s) const { return {s.x - 40, s.y - 40, std::max(s.w, 200.0) + 80, s.h + 80}; }

Rect Overlay::bbox(const std::vector<Pt>& pts) const {
    double x0 = pts[0].x, x1 = x0, y0 = pts[0].y, y1 = y0;
    for (Pt p : pts) {
        x0 = std::min(x0, p.x);
        x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y);
        y1 = std::max(y1, p.y);
    }
    return {x0, y0, x1 - x0, y1 - y0};
}

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
    draw();
    rt->PopAxisAlignedClip();
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        bmp = nullptr;
        rt = nullptr;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    EndPaint(hwnd, &ps);
}

void Overlay::draw() {
    ID2D1RenderTarget* t = rt.Get();
    const auto nearest = D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
    t->DrawBitmap(bmp.Get(), dx(bounds), 1, nearest);
    t->FillRectangle(dx(bounds), brush(t, D2D1::ColorF(0, 0.5f)).Get());
    if (!sel) return;
    const Rect s = *sel;
    t->PushAxisAlignedClip(dx(s), D2D1_ANTIALIAS_MODE_ALIASED);
    t->DrawBitmap(bmp.Get(), dx(bounds), 1, nearest);
    t->PopAxisAlignedClip();

    auto white = brush(t, D2D1::ColorF(D2D1::ColorF::White)), black = brush(t, D2D1::ColorF(D2D1::ColorF::Black));
    t->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // Chrome stays sharp at 125 %.
    t->DrawRectangle(dx(s.inset(-0.5, -0.5)), white.Get());
    for (const Handle& h : handlePoints(s)) {
        Rect r = {h.p.x - 3, h.p.y - 3, 6, 6};
        t->FillRectangle(dx(r), white.Get());
        t->DrawRectangle(dx(r), black.Get());
    }
    t->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    wchar_t text[64];
    swprintf(text, 64, L" %ld × %ld   %d pt ", std::lround(s.w * k), std::lround(s.h * k), int(width));
    auto l = layout(text, labelFormat.Get());
    Pt size = extent(l.Get());
    Pt at = {s.x, s.y - size.y - 4 < 0 ? s.y + 4 : s.y - size.y - 4};
    t->FillRectangle(dx(Rect{at.x, at.y, size.x, size.y}), brush(t, D2D1::ColorF(0, 0.7f)).Get());
    t->DrawTextLayout(dx(at), l.Get(), white.Get());

    if (!barHidden) drawBar();
}

ComPtr<IWICBitmap> Overlay::render() {
    if (!sel) return nullptr;
    const Rect s = *sel;
    ComPtr<IWICBitmap> out;
    if (FAILED(wic->CreateBitmap(UINT(std::lround(s.w * k)), UINT(std::lround(s.h * k)), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &out)))
        return nullptr;
    float dpi = float(96 * k);
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                              D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
    ComPtr<ID2D1RenderTarget> t;
    ComPtr<ID2D1Bitmap> src;
    if (FAILED(d2d->CreateWicBitmapRenderTarget(out.Get(), props, &t)) || FAILED(t->CreateBitmapFromWicBitmap(image.Get(), &src))) return nullptr;
    t->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    t->BeginDraw();
    t->SetTransform(D2D1::Matrix3x2F::Translation(float(-s.x), float(-s.y)));
    t->DrawBitmap(src.Get(), dx(bounds), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    return SUCCEEDED(t->EndDraw()) ? out : nullptr;
}

void Overlay::drawBar() {
    ID2D1RenderTarget* t = rt.Get();
    auto r = D2D1::RoundedRect(dx(barRect), 7, 7);
    t->FillRoundedRectangle(r, brush(t, D2D1::ColorF(0.25f, 0.25f, 0.25f, 0.95f)).Get());
}
