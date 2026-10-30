#include "overlay.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

ComPtr<IWICImagingFactory> wic;
static ComPtr<ID2D1Factory> d2d;
static ComPtr<IDWriteFactory> dw;
static ComPtr<IDWriteTextFormat> labelFormat, iconFormat;
static ComPtr<ID2D1StrokeStyle> roundStroke, miterStroke, dashStroke[2];
static const wchar_t* windowClass = L"justcap.overlay";

static const struct {
    const wchar_t* name;
    D2D1_COLOR_F color;
} palette[] = {
    {L"Red", {1, 0.15f, 0.15f, 1}},
    {L"Orange", {1, 0.55f, 0, 1}},
    {L"Yellow", {1, 0.9f, 0, 1}},
    {L"Green", {0.1f, 0.8f, 0.2f, 1}},
    {L"Blue", {0.1f, 0.5f, 1, 1}},
    {L"White", {1, 1, 1, 1}},
    {L"Black", {0, 0, 0, 1}},
};
static const int paletteCount = int(sizeof palette / sizeof palette[0]);

void gfxInit() {
    if (d2d) return;
    CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
    dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11, L"", &labelFormat);
    labelFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    dw->CreateTextFormat(L"Segoe MDL2 Assets", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 16, L"", &iconFormat);
    iconFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    iconFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    auto stroke = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    d2d->CreateStrokeStyle(stroke, NULL, 0, &roundStroke);
    stroke = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER);
    d2d->CreateStrokeStyle(stroke, NULL, 0, &miterStroke);
    const float dashes[] = {3, 3};
    for (int i = 0; i < 2; i++) {
        stroke = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10,
                                             D2D1_DASH_STYLE_CUSTOM, i * 3.0f);
        d2d->CreateStrokeStyle(stroke, dashes, 2, &dashStroke[i]);
    }
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

static IDWriteTextFormat* font(double width) {
    static ComPtr<IDWriteTextFormat> formats[21];
    int i = std::clamp(int(width), 1, 20);
    if (!formats[i]) {
        dw->CreateTextFormat(L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, float(10 + i * 2), L"", &formats[i]);
        formats[i]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return formats[i].Get();
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

static ComPtr<ID2D1PathGeometry> path(const std::vector<Pt>& pts, bool filled) {
    ComPtr<ID2D1PathGeometry> g;
    ComPtr<ID2D1GeometrySink> sink;
    d2d->CreatePathGeometry(&g);
    g->Open(&sink);
    sink->BeginFigure(dx(pts[0]), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t i = 1; i < pts.size(); i++) sink->AddLine(dx(pts[i]));
    sink->EndFigure(filled ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    return g;
}

static Rect intersect(const Rect& a, const Rect& b) {
    double x = std::max(a.x, b.x), y = std::max(a.y, b.y);
    return {x, y, std::min(a.right(), b.right()) - x, std::min(a.bottom(), b.bottom()) - y};
}

Overlay::Overlay(ComPtr<IWICBitmap> img, RECT m, double scale, bool window) : image(img), k(scale) {
    int pw = m.right - m.left, ph = m.bottom - m.top;
    bounds = {0, 0, pw / k, ph / k};
    width = std::clamp(double(regGet(L"width", 3)), 1.0, 20.0);
    colorIndex = std::clamp(int(regGet(L"color", 0)), 0, paletteCount - 1);
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
        // While a text is typed, the keys belong to the text.
        if (editing) {
            keyEdit(wp, ctrl);
            return 0;
        }
        if (ctrl && (wp == 'C' || wp == 'S')) finish(wp == 'S');
        if (ctrl && wp == 'Z') undo();
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
    case WM_CHAR:
        if (editing && wp == 0x08) eraseChar(false);
        else if (editing && wp >= 0x20 && wp != 0x7F) insertText(std::wstring(1, wchar_t(wp)));
        return 0;
    case WM_MOUSEMOVE:
        if (wp & MK_LBUTTON) mouseDragged(mouse(lp), wp & MK_SHIFT);
        else mouseMoved(mouse(lp));
        return 0;
    case WM_LBUTTONDOWN: {
        // The keyboard follows the mouse.
        if (GetForegroundWindow() != hwnd) SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        Pt p = mouse(lp);
        if (!barHidden && barRect.contains(p)) {
            if (int i = buttonAt(p); i >= 0) press(i);
            return 0;
        }
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
    case WM_MOUSEWHEEL: {
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};  // Screen coordinates.
        ScreenToClient(hwnd, &p);
        scrollWheel({p.x / k, p.y / k}, GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    }
    case WM_TIMER:
        KillTimer(hwnd, wp);
        setPreview(std::nullopt);
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

void Overlay::setTool(std::optional<Tool> t) {
    tool = t;
    if (!barHidden) invalidate(barRect);
    setHover(std::nullopt);
}

void Overlay::setColor(int i) {
    colorIndex = i;
    regSet(L"color", i);
    if (!barHidden) invalidate(barRect);
    if (editing) {
        fieldColor = palette[i].color;
        fieldChanged();
    }
}

void Overlay::setWidth(double w) {
    width = w;
    regSet(L"width", DWORD(w));
}

void Overlay::setPreview(std::optional<Pt> p) {
    auto old = preview;
    preview = p;
    for (auto q : {old, preview})
        if (q) invalidate({q->x - 60, q->y - 60, 120, 120});
}

void Overlay::mouseMoved(Pt p) {
    bool onBar = !barHidden && barRect.contains(p);
    auto hover = tool == Tool::text && !editing && !onBar ? textIndex(p) : std::nullopt;
    if (hover != hoverText) setHover(hover);
    if (onBar) return setCursor(IDC_ARROW);
    if (!sel) return setCursor(IDC_CROSS);
    if (editing && fieldFrame().contains(p)) return setCursor(IDC_IBEAM);
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

void Overlay::press(int i) {
    if (i < 7) {
        commitText();
        setTool(tool && int(*tool) == i ? std::nullopt : std::optional<Tool>(Tool(i)));
        return;
    }
    switch (i) {
    case 7: pickColor(); break;
    case 8: undo(); break;
    case 9: finish(false); break;
    case 10: finish(true); break;
    case 11: cancel(); break;
    }
}

void Overlay::pickColor() {
    int size = GetSystemMetrics(SM_CXMENUCHECK);
    HMENU menu = CreatePopupMenu();
    HDC screen = GetDC(hwnd), mem = CreateCompatibleDC(screen);
    HBITMAP swatches[paletteCount];
    for (int i = 0; i < paletteCount; i++) {
        const D2D1_COLOR_F c = palette[i].color;
        swatches[i] = CreateCompatibleBitmap(screen, size, size);
        HGDIOBJ oldBitmap = SelectObject(mem, swatches[i]);
        RECT r = {0, 0, size, size};
        FillRect(mem, &r, GetSysColorBrush(COLOR_MENU));
        HBRUSH fill = CreateSolidBrush(RGB(c.r * 255, c.g * 255, c.b * 255));
        HGDIOBJ oldBrush = SelectObject(mem, fill), oldPen = SelectObject(mem, GetStockObject(NULL_PEN));
        Ellipse(mem, 1, 1, size, size);
        SelectObject(mem, oldBrush);
        SelectObject(mem, oldPen);
        SelectObject(mem, oldBitmap);
        DeleteObject(fill);
        std::wstring label = (i == colorIndex ? L"✓ " : L"") + std::wstring(palette[i].name);
        AppendMenuW(menu, MF_STRING, i + 1, label.c_str());
        SetMenuItemBitmaps(menu, i, MF_BYPOSITION, swatches[i], swatches[i]);
    }
    DeleteDC(mem);
    ReleaseDC(hwnd, screen);
    Rect b = button(7);
    POINT at = {std::lround(b.x * k), std::lround((b.bottom() + 4) * k)};
    ClientToScreen(hwnd, &at);
    int picked = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, at.x, at.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    for (HBITMAP swatch : swatches) DeleteObject(swatch);
    if (picked) setColor(picked - 1);
}

void Overlay::undo() {
    commitText();
    if (annotations.empty()) return;
    annotations.pop_back();
    annotationsChanged();
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
    // A click in the field places the caret, as in a text control.
    if (editing && fieldFrame().contains(p)) {
        BOOL trailing, inside;
        DWRITE_HIT_TEST_METRICS m;
        layout(text, font(width))->HitTestPoint(float(p.x - fieldOrigin.x), float(p.y - fieldOrigin.y), &trailing, &inside, &m);
        caret = m.textPosition + (trailing ? m.length : 0);
        allSelected = false;
        fieldChanged();
        return;
    }
    bool wasEditing = editing;
    commitText();
    setPreview(std::nullopt);
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
                // Drag moves an existing text. A click without movement edits it (see mouseUp).
                // A click away from a text in edit only completes that text.
                if (auto i = textIndex(p)) {
                    drag = {};
                    drag.kind = Drag::text;
                    drag.index = *i;
                    drag.start = drag.last = p;
                } else if (!wasEditing) {
                    beginText(p);
                }
            } else if (tool) {
                current = Annotation{*tool, palette[colorIndex].color, width, {p, p}};
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
    case Drag::text: {
        if (!sel) break;
        // Keep the text inside the selection, or it can no longer be reached.
        const Rect s = *sel;
        Annotation& a = annotations[drag.index];
        Rect r = textRect(a);
        a.points[0] = {std::max(s.x, std::min(r.x + p.x - drag.last.x, s.right() - r.w)),
                       std::max(s.y, std::min(r.y + p.y - drag.last.y, s.bottom() - r.h))};
        annotationsChanged();
        drag.last = p;
        setHover(drag.index);
        setCursor(IDC_SIZEALL);
        break;
    }
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
        if (a.tool == Tool::pixelate) a.blocks = blocks(box(a));
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
    if (drag.kind == Drag::text && std::hypot(drag.last.x - drag.start.x, drag.last.y - drag.start.y) < 3) {
        Annotation a = annotations[drag.index];
        annotations.erase(annotations.begin() + drag.index);
        annotationsChanged();
        beginText(drag.start, a);
    }
    drag = {};
    layoutBar();
}

void Overlay::scrollWheel(Pt p, int delta) {
    if (!sel) return;
    scrollAcc += delta;
    if (std::abs(scrollAcc) < WHEEL_DELTA) return;
    double d = scrollAcc > 0 ? 1 : -1;
    scrollAcc = 0;
    // Text tool, pointer on an existing text: change the size of that text only.
    if (!editing && tool == Tool::text) {
        if (auto i = textIndex(p)) {
            setHover(hoverText);
            annotations[*i].width = std::clamp(annotations[*i].width + d, 1.0, 20.0);
            annotationsChanged();
            setHover(hoverText);
            return;
        }
    }
    double middle = fieldOrigin.y + fieldHeight() / 2;
    setWidth(std::clamp(width + d, 1.0, 20.0));
    if (editing) {
        fieldOrigin.y = middle - fieldHeight() / 2;
        fieldChanged();
    } else {
        setPreview(p);
        SetTimer(hwnd, 1, 800, NULL);
    }
    invalidate(chrome(*sel));  // the size label shows the width
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

Rect Overlay::textRect(const Annotation& a) const {
    Pt size = extent(layout(a.text, font(a.width)).Get());
    return {a.points[0].x, a.points[0].y, size.x, size.y};
}

std::optional<int> Overlay::textIndex(Pt p) const {
    for (int i = int(annotations.size()) - 1; i >= 0; i--)
        if (annotations[i].tool == Tool::text && textRect(annotations[i]).contains(p)) return i;
    return std::nullopt;
}

double Overlay::fieldHeight() const { return extent(layout(L"A", font(width)).Get()).y; }

/// The area that takes clicks: from the text to the right edge of the selection.
Rect Overlay::fieldFrame() const {
    return {fieldOrigin.x, fieldOrigin.y, std::max(40.0, sel->right() - fieldOrigin.x), fieldHeight()};
}

/// The dotted box, as wide as the text. 2 DIP of air on each side.
Rect Overlay::fieldBox() const {
    return {fieldOrigin.x - 2, fieldOrigin.y, std::max(40.0, extent(layout(text, font(width)).Get()).x + 4), fieldHeight()};
}

/// Redraws the field and keeps the IME candidate window at the caret.
void Overlay::fieldChanged() {
    Rect box = fieldBox().inset(-4, -4);
    invalidate(fieldDirty);
    invalidate(box);
    fieldDirty = box;
    if (!hwnd) return;
    FLOAT x, y;
    DWRITE_HIT_TEST_METRICS m;
    layout(text, font(width))->HitTestTextPosition(UINT32(caret), FALSE, &x, &y, &m);
    COMPOSITIONFORM form = {CFS_POINT, {std::lround((fieldOrigin.x + x) * k), std::lround(fieldOrigin.y * k)}};
    HIMC context = ImmGetContext(hwnd);
    ImmSetCompositionWindow(context, &form);
    ImmReleaseContext(hwnd, context);
}

void Overlay::beginText(Pt p, std::optional<Annotation> a) {
    if (!sel) return;
    if (a) {
        widthBeforeEdit = width;
        setWidth(a->width);
    }
    edited = a;
    text = a ? a->text : L"";
    undoText = text;
    caret = text.size();
    allSelected = a && !text.empty();  // As a text control does when it gets the focus.
    fieldColor = a ? a->color : palette[colorIndex].color;
    fieldOrigin = a ? a->points[0] : Pt{p.x, p.y - fieldHeight() / 2};
    editing = true;
    setHover(std::nullopt);
    fieldDirty = fieldBox().inset(-4, -4);
    fieldChanged();
}

void Overlay::commitText() {
    if (!editing) return;
    editing = false;
    invalidate(fieldDirty);
    if (!text.empty()) {
        annotations.push_back({Tool::text, fieldColor, width, {fieldOrigin}, text});
        annotationsChanged();
    } else if (edited) {
        annotations.push_back(*edited);  // a re-edit that ends empty keeps the original
        annotationsChanged();
    }
    if (edited) setWidth(widthBeforeEdit);
    edited = std::nullopt;
}

void Overlay::insertText(const std::wstring& s) {
    undoText = text;
    if (allSelected) {
        text.clear();
        caret = 0;
    }
    allSelected = false;
    text.insert(caret, s);
    caret += s.size();
    fieldChanged();
}

void Overlay::eraseChar(bool forward) {
    undoText = text;
    if (allSelected) {
        text.clear();
        caret = 0;
        allSelected = false;
    } else if (forward && caret < text.size()) {
        text.erase(caret, IS_HIGH_SURROGATE(text[caret]) ? 2 : 1);
    } else if (!forward && caret > 0) {
        size_t n = caret > 1 && IS_LOW_SURROGATE(text[caret - 1]) ? 2 : 1;
        caret -= n;
        text.erase(caret, n);
    }
    fieldChanged();
}

void Overlay::copyText() {
    if (!allSelected || !OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(mem), text.c_str(), bytes);
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

void Overlay::pasteText() {
    if (!OpenClipboard(hwnd)) return;
    std::wstring s;
    if (HGLOBAL mem = GetClipboardData(CF_UNICODETEXT)) {
        s = static_cast<const wchar_t*>(GlobalLock(mem));
        GlobalUnlock(mem);
    }
    CloseClipboard();
    s.erase(std::remove_if(s.begin(), s.end(), [](wchar_t c) { return c < 0x20; }), s.end());
    if (!s.empty()) insertText(s);
}

void Overlay::keyEdit(WPARAM wp, bool ctrl) {
    switch (wp) {
    case VK_RETURN:
        commitText();
        return;
    case VK_ESCAPE:  // Drop a new text, keep the original of a re-edited one.
        text.clear();
        commitText();
        return;
    case VK_LEFT:
        if (allSelected) caret = 0;
        else if (caret > 0) caret -= caret > 1 && IS_LOW_SURROGATE(text[caret - 1]) ? 2 : 1;
        break;
    case VK_RIGHT:
        if (allSelected) caret = text.size();
        else if (caret < text.size()) caret += IS_HIGH_SURROGATE(text[caret]) ? 2 : 1;
        break;
    case VK_HOME:
        caret = 0;
        break;
    case VK_END:
        caret = text.size();
        break;
    case VK_DELETE:
        return eraseChar(true);
    case 'A':
        if (!ctrl) return;
        allSelected = !text.empty();
        return fieldChanged();
    case 'C':
        if (ctrl) copyText();
        return;
    case 'X':
        if (!ctrl || !allSelected) return;
        copyText();
        return eraseChar(false);
    case 'V':
        if (ctrl) pasteText();
        return;
    case 'Z':
        if (!ctrl) return;
        text.swap(undoText);
        caret = text.size();
        break;
    default:
        return;
    }
    allSelected = false;
    fieldChanged();
}

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

Rect Overlay::box(const Annotation& a) const { return bbox({a.points[0], a.points.back()}); }

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
    drawAnnotations(t, s);

    if (preview) drawPreview(*preview);

    auto white = brush(t, D2D1::ColorF(D2D1::ColorF::White)), black = brush(t, D2D1::ColorF(D2D1::ColorF::Black));
    t->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);  // Chrome stays sharp at 125 %.
    t->DrawRectangle(dx(s.inset(-0.5, -0.5)), white.Get());
    for (const Handle& h : handlePoints(s)) {
        Rect r = {h.p.x - 3, h.p.y - 3, 6, 6};
        t->FillRectangle(dx(r), white.Get());
        t->DrawRectangle(dx(r), black.Get());
    }
    if (editing) dottedBox(fieldBox());
    else if (hoverText && *hoverText < int(annotations.size())) dottedBox(textRect(annotations[*hoverText]).inset(-2, -2));
    t->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (editing) drawField();

    wchar_t label[64];
    swprintf(label, 64, L" %ld × %ld   %d pt ", std::lround(s.w * k), std::lround(s.h * k), int(width));
    auto l = layout(label, labelFormat.Get());
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
    drawAnnotations(t.Get(), s);
    return SUCCEEDED(t->EndDraw()) ? out : nullptr;
}

/// Editor chrome around a text: black and white dashes, visible on any background.
void Overlay::dottedBox(const Rect& r) {
    ID2D1RenderTarget* t = rt.Get();
    double x0 = std::floor(r.x), y0 = std::floor(r.y);
    Rect q = {x0 + 0.5, y0 + 0.5, std::ceil(r.right()) - x0 - 1, std::ceil(r.bottom()) - y0 - 1};
    t->DrawRectangle(dx(q), brush(t, D2D1::ColorF(D2D1::ColorF::Black)).Get(), 1, dashStroke[0].Get());
    t->DrawRectangle(dx(q), brush(t, D2D1::ColorF(D2D1::ColorF::White)).Get(), 1, dashStroke[1].Get());
}

/// The text in edit, in the same font and place as the annotation it becomes.
void Overlay::drawField() {
    ID2D1RenderTarget* t = rt.Get();
    auto l = layout(text, font(width));
    Pt size = extent(l.Get());
    auto b = brush(t, fieldColor);
    t->PushAxisAlignedClip(dx(*sel), D2D1_ANTIALIAS_MODE_ALIASED);
    if (allSelected) t->FillRectangle(dx(Rect{fieldOrigin.x, fieldOrigin.y, size.x, size.y}), brush(t, D2D1::ColorF(0.1f, 0.5f, 1, 0.5f)).Get());
    t->DrawTextLayout(dx(fieldOrigin), l.Get(), b.Get());
    FLOAT x, y;
    DWRITE_HIT_TEST_METRICS m;
    l->HitTestTextPosition(UINT32(caret), FALSE, &x, &y, &m);
    if (!allSelected) t->FillRectangle(dx(Rect{fieldOrigin.x + x, fieldOrigin.y, 1, size.y}), b.Get());
    t->PopAxisAlignedClip();
}

/// The size of the next line or text, at the pointer. Not part of the output image.
void Overlay::drawPreview(Pt p) {
    ID2D1RenderTarget* t = rt.Get();
    if (tool == Tool::text) {
        auto l = layout(L"Aa", font(width));
        Pt size = extent(l.Get());
        t->DrawTextLayout(dx(Pt{p.x - size.x / 2, p.y - size.y / 2}), l.Get(), brush(t, palette[colorIndex].color).Get());
        return;
    }
    double d = tool == Tool::marker ? width * 5 : width;
    D2D1_COLOR_F c = palette[colorIndex].color;
    if (tool == Tool::marker) c.a = 0.4f;
    float r = float(d / 2);
    t->FillEllipse(D2D1::Ellipse(dx(p), r, r), brush(t, c).Get());
    t->DrawEllipse(D2D1::Ellipse(dx(p), r + 1.5f, r + 1.5f), brush(t, D2D1::ColorF(D2D1::ColorF::White)).Get());
}

void Overlay::drawAnnotations(ID2D1RenderTarget* t, const Rect& clip) {
    t->PushAxisAlignedClip(dx(clip), D2D1_ANTIALIAS_MODE_ALIASED);
    for (const Annotation& a : annotations) drawAnnotation(t, a);
    if (current) drawAnnotation(t, *current);
    t->PopAxisAlignedClip();
}

void Overlay::drawAnnotation(ID2D1RenderTarget* t, const Annotation& a) {
    const Pt a0 = a.points[0], a1 = a.points.back();
    D2D1_COLOR_F c = a.color;
    if (a.tool == Tool::marker) c.a = 0.4f;
    auto b = brush(t, c);
    float w = float(a.width);
    switch (a.tool) {
    case Tool::pen:
    case Tool::marker:
        if (a.tool == Tool::marker) w *= 5;
        // A click without movement: Direct2D draws no caps on a zero-length line.
        if (a.points.size() == 2 && a0 == a1) t->FillEllipse(D2D1::Ellipse(dx(a0), w / 2, w / 2), b.Get());
        else t->DrawGeometry(path(a.points, false).Get(), b.Get(), w, roundStroke.Get());
        break;
    case Tool::line:
        t->DrawLine(dx(a0), dx(a1), b.Get(), w, roundStroke.Get());
        break;
    case Tool::arrow: {
        double ang = std::atan2(a1.y - a0.y, a1.x - a0.x), head = std::min(a.width * 4 + 6, std::hypot(a1.x - a0.x, a1.y - a0.y));
        Pt base = {a1.x - head * std::cos(ang), a1.y - head * std::sin(ang)};
        double px = -std::sin(ang) * head / 2, py = std::cos(ang) * head / 2;
        t->DrawLine(dx(a0), dx(base), b.Get(), w, roundStroke.Get());
        t->FillGeometry(path({a1, {base.x + px, base.y + py}, {base.x - px, base.y - py}}, true).Get(), b.Get());
        break;
    }
    case Tool::rect:
        t->DrawRectangle(dx(box(a)), b.Get(), w, miterStroke.Get());
        break;
    case Tool::text:
        t->DrawTextLayout(dx(a0), layout(a.text, font(a.width)).Get(), b.Get());
        break;
    case Tool::pixelate: {
        ComPtr<IWICBitmap> scaled = a.blocks ? a.blocks : blocks(box(a));
        ComPtr<ID2D1Bitmap> bitmap;
        if (scaled && SUCCEEDED(t->CreateBitmapFromWicBitmap(scaled.Get(), &bitmap)))
            t->DrawBitmap(bitmap.Get(), dx(intersect(box(a), bounds)), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        break;
    }
    }
}

/// The source image in `r` as 10 DIP blocks, one pixel per block. Reads the source image only, not other annotations.
ComPtr<IWICBitmap> Overlay::blocks(Rect r) const {
    r = intersect(r, bounds);
    const double block = 10;
    if (r.w < 1 || r.h < 1) return nullptr;
    UINT w = std::max(1u, UINT(r.w / block)), h = std::max(1u, UINT(r.h / block));
    INT x0 = std::lround(r.x * k), y0 = std::lround(r.y * k);
    WICRect px = {x0, y0, std::lround(r.right() * k) - x0, std::lround(r.bottom() * k) - y0};
    ComPtr<IWICBitmapClipper> clipper;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICBitmap> out;
    if (SUCCEEDED(wic->CreateBitmapClipper(&clipper)) && SUCCEEDED(clipper->Initialize(image.Get(), &px))
        && SUCCEEDED(wic->CreateBitmapScaler(&scaler)) && SUCCEEDED(scaler->Initialize(clipper.Get(), w, h, WICBitmapInterpolationModeFant)))
        wic->CreateBitmapFromSource(scaler.Get(), WICBitmapCacheOnLoad, &out);
    return out;
}

void Overlay::drawBar() {
    ID2D1RenderTarget* t = rt.Get();
    t->FillRoundedRectangle(D2D1::RoundedRect(dx(barRect), 7, 7), brush(t, D2D1::ColorF(0.25f, 0.25f, 0.25f, 0.95f)).Get());
    if (tool) t->FillRoundedRectangle(D2D1::RoundedRect(dx(button(int(*tool))), 4, 4), brush(t, D2D1::ColorF(1, 1, 1, 0.3f)).Get());
    auto white = brush(t, D2D1::ColorF(D2D1::ColorF::White));
    // Segoe MDL2 Assets. Zero: drawn below with primitives.
    static const wchar_t glyphs[12] = {0xE70F, 0, 0, 0, 0xE7E6, 0xE8D2, 0, 0, 0xE7A7, 0xE8C8, 0xE74E, 0xE711};
    for (int i = 0; i < 12; i++) {
        Rect r = button(i);
        double cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        if (glyphs[i]) t->DrawTextW(&glyphs[i], 1, iconFormat.Get(), dx(r), white.Get());
        switch (i) {
        case 1:
            t->DrawLine(dx(Pt{cx - 6, cy + 6}), dx(Pt{cx + 6, cy - 6}), white.Get(), 1.5f);
            break;
        case 2:
            t->DrawLine(dx(Pt{cx - 6, cy + 6}), dx(Pt{cx + 3, cy - 3}), white.Get(), 1.5f);
            t->FillGeometry(path({{cx + 6, cy - 6}, {cx - 1, cy - 4}, {cx + 4, cy + 1}}, true).Get(), white.Get());
            break;
        case 3:
            t->DrawRectangle(dx(Rect{cx - 7, cy - 5, 14, 10}), white.Get(), 1.5f);
            break;
        case 6:
            t->FillRectangle(dx(Rect{cx - 6, cy - 6, 6, 6}), white.Get());
            t->FillRectangle(dx(Rect{cx, cy, 6, 6}), white.Get());
            break;
        case 7:
            t->FillEllipse(D2D1::Ellipse(dx(Pt{cx, cy}), 6, 6), brush(t, palette[colorIndex].color).Get());
            break;
        }
    }
}
