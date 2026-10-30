#pragma once
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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

enum class Tool { pen, line, arrow, rect, marker, text, pixelate };

struct Annotation {
    Tool tool;
    D2D1_COLOR_F color;
    double width;
    std::vector<Pt> points;
    std::wstring text;
    /// Pixelate only: the block image, computed once the annotation is committed.
    ComPtr<IWICBitmap> blocks;
};

/// One window per monitor. Shows the frozen screen image, the selection and the annotations.
/// All coordinates are DIPs, origin top-left. A window of false is the self-test mode: no window, no toolbar.
class Overlay {
public:
    Overlay(ComPtr<IWICBitmap> image, RECT monitor, double k, bool window = true);
    ~Overlay();

    HWND hwnd = NULL;
    std::optional<Rect> sel;
    // Annotations are clipped to the selection.
    std::vector<Annotation> annotations;
    /// Called with the result image (null = cancel) and the save flag. The owner must not destroy this overlay inside the call.
    std::function<void(ComPtr<IWICBitmap>, bool)> onDone;
    /// Called when a new selection starts here, so the other monitors can clear theirs.
    std::function<void()> onSelectionStart;
    void clearSelection();
    /// The selection with its annotations, at the pixel resolution of the monitor.
    ComPtr<IWICBitmap> render();
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);

private:
    struct Drag {
        enum Kind { none, draw, resize, move, text } kind = none;
        Pt anchor;
        bool hx = false, hy = false;
        Rect orig;
        double dx = 0, dy = 0;
        int index = 0;
        Pt start, last;
    };
    struct Handle {
        Pt p;
        int hx, hy;
    };

    LRESULT handle(UINT, WPARAM, LPARAM);
    void createTarget();
    void paint();
    void draw();
    void drawBar();
    void drawPreview(Pt);
    void drawAnnotations(ID2D1RenderTarget*, const Rect& clip);
    void drawAnnotation(ID2D1RenderTarget*, const Annotation&);
    void cancel();
    void finish(bool save);
    void press(int button);
    void pickColor();
    void setColor(int);
    void setTool(std::optional<Tool>);
    void setWidth(double);
    void setPreview(std::optional<Pt>);
    void undo();
    void scrollWheel(Pt, int delta);
    Rect box(const Annotation&) const;
    ComPtr<IWICBitmap> blocks(Rect) const;

    Pt mouse(LPARAM) const;
    double snap(double v) const;
    void invalidate(const Rect&);
    void setSel(std::optional<Rect>);
    void annotationsChanged();
    void setHover(std::optional<int>);
    void setCursor(LPCWSTR);
    Rect chrome(const Rect&) const;

    Rect button(int) const;
    int buttonAt(Pt) const;
    void layoutBar();
    void hideBar();
    void nudge(double dx, double dy, bool shift);

    void mouseMoved(Pt);
    void mouseDown(Pt);
    void mouseDragged(Pt, bool shift);
    void mouseUp();
    std::vector<Handle> handlePoints(const Rect&) const;
    std::optional<Handle> handleHit(Pt, const Rect&) const;
    Pt constrain(Pt a, Pt p, bool square) const;
    Rect bbox(const std::vector<Pt>&) const;

    std::optional<int> textIndex(Pt) const;
    Rect textRect(const Annotation&) const;
    void commitText();
    void beginText(Pt);

    ComPtr<IWICBitmap> image;
    double k;
    Rect bounds;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1Bitmap> bmp;

    std::optional<Annotation> current;
    Drag drag;
    std::optional<Tool> tool;
    /// Text tool: the text under the pointer, shown with a dotted box.
    std::optional<int> hoverText;
    int colorIndex;
    double width;
    int scrollAcc = 0;
    std::optional<Pt> preview;
    Rect barRect;
    bool barHidden = true;
    HCURSOR cursor;
};
