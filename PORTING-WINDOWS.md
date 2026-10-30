# Porting justcap to Windows

Instructions for an agent that ports justcap (macOS, Swift + AppKit) to Windows.
Read this file first, then the two Swift sources. They are the specification: `Sources/main.swift` (239 lines) and `Sources/Overlay.swift` (673 lines). `README.md` is the behaviour contract. `REVIEW.md` explains why the code looks the way it does (partial redraw, the drag state machine, the text editor); skim its sections 1 and 2 before touching the overlay.

The goal is the same program with the same feel: a tray icon, one hotkey, a frozen full-screen overlay per monitor, a drag selection with handles, seven annotation tools, copy or save, and nothing else. No upload, no network, no settings window, no installer.

---

## 1. Stack decision

**Use C++17 with the Win32 API, Direct2D 1.0 + DirectWrite for drawing, WIC (Windows Imaging Component) for bitmaps and PNG, GDI `BitBlt` for the capture. Build with MSVC `cl.exe`. No third-party code, no package manager, no project files.**

Why this and not C#: the macOS version is 200 KB, 11 MB idle, no runtime dependency. Win32 + Direct2D gives the same numbers. A .NET WinForms port would be faster to write but idles at 60–80 MB and needs the .NET runtime or a 60 MB self-contained publish. If the person asking for the port prefers C#, everything in this document still applies except the API names (WinForms + System.Drawing + `Bitmap.Save`). Decide once, at the start, and say which.

Why Direct2D and not GDI: GDI has no anti-aliased lines and no alpha stroke for the marker. Direct2D has both, draws text through DirectWrite, measures DPI in the same unit AppKit uses (see §3), and renders to a WIC bitmap for the output image. Do not use GDI+ either: it is CPU-only and slower on a 4K screen.

Why `BitBlt` and not Windows.Graphics.Capture: `BitBlt` from the screen DC is synchronous, needs no permission prompt, no WinRT, no yellow capture border, and captures the composed desktop including DirectX windows (DWM is always on since Windows 8). Lightshot and ShareX do the same. Limits: exclusive-fullscreen games and the secure desktop (UAC) capture black. Accept that.

Target: Windows 10 1703 or later, x64. Arm64 is one extra compiler invocation (§9).

---

## 2. Repository layout

Create a `windows/` folder in this repository (the LICENSE and the top-level README are shared):

```
windows/
  build.cmd          Build, version stamp, self-test, optional install (mirrors build.sh)
  src/main.cpp       Everything in main.swift: tray, hotkey, settings, capture orchestration, finish, self-test, wWinMain
  src/overlay.h      The Overlay class interface
  src/overlay.cpp    Everything in Overlay.swift
```

Three source files. Do not split further. No `.rc` file, no manifest file, no `.vcxproj`: the two things a manifest would give (DPI awareness, common controls) are done with API calls instead.

---

## 3. Coordinates and DPI (read this before writing any geometry)

This is the one place where a careless port breaks everything. Two facts about the Swift code:

1. All geometry in `OverlayView` is in **points** (device-independent units). `scale = image.width / bounds.width` converts to pixels and is used only in `pixels()`, `render()` and the size label.
2. AppKit's y axis points **up**. Windows' points **down**.

Do this on Windows:

- At startup call `SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)`. All window rectangles, monitor rectangles and mouse messages are then in physical pixels.
- For each monitor, read `GetDpiForMonitor(hmon, MDT_EFFECTIVE_DPI, …)`. Let `k = dpi / 96.0`. This is the Swift `scale`.
- Create the Direct2D HWND render target with `dpiX = dpiY = dpi` in its `D2D1_RENDER_TARGET_PROPERTIES`. Direct2D then works in DIPs (device-independent pixels, 1/96 inch), which are exactly AppKit points. **Every number in Overlay.swift (6 pt handles, 9 pt hit radius, 26 × 24 toolbar buttons, 10 pt pixelate blocks, 11 pt label font, `10 + width * 2` text font) is used unchanged as a DIP.**
- `bounds` = monitor size in DIPs = pixel size / k.
- Convert mouse coordinates once, on arrival: `p = { GET_X_LPARAM(lp) / k, GET_Y_LPARAM(lp) / k }`. `WM_MOUSEWHEEL` gives **screen** coordinates; call `ScreenToClient` first.
- Because `k` can be 1.25 or 1.5 on Windows (never on macOS), round selection edges to the pixel grid, not the DIP grid: where Swift does `.rounded()` on a selection coordinate, do `round(v * k) / k`. Otherwise crops are blurry at 125 %.
- `pixels(r)` loses its flip: `{ r.x * k, r.y * k, r.w * k, r.h * k }`.

Flip the y axis in exactly these places. Everything else in the geometry (handles, `constrain`, arrow head, `bbox`, drag state machine) is orientation-agnostic and ports line by line:

| Swift location | macOS (y up) | Windows (y down) |
| --- | --- | --- |
| `layoutBar` "below the selection" | `y = s.minY - h - 6` | `y = s.bottom + 6` |
| `layoutBar` "above" | `y = s.maxY + 6` | `y = s.top - h - 6` |
| `layoutBar` "inside" | `y = s.minY + 6` | `y = s.bottom - h - 6` |
| `nudge` up / down arrow | up = `+1`, down = `-1` | up = `-1`, down = `+1` |
| `draw` size label | above: `s.maxY + 4`; else inside: `s.maxY - h - 4` | above: `s.top - h - 4`; else inside: `s.top + 4` |
| `mouseMoved` diagonal resize cursor | `hx * hy < 0` is NW–SE | `hx * hy > 0` is NW–SE (`IDC_SIZENWSE`), else `IDC_SIZENESW` |
| `pixels(r)` | flips y | no flip |
| `render()` | `scaleBy(k)`, `translateBy(-s.minX, -s.minY)` | same transform, no flip; WIC and D2D rows start at the top like `CGImage` |
| Text anchor `points[0]` | bottom-left of the text box | top-left of the text box; `textRect = (anchor, measured size)` is then identical |

Keep the sign conventions of `hx` / `hy` from `handlePoints` (`-1` = min edge, `+1` = max edge). Only the cursor mapping table above cares about which edge is visually on top.

Ignore `WM_DPICHANGED`: the overlay windows are per-monitor and live for seconds.

---

## 4. API mapping

| Concern | macOS (current code) | Windows |
| --- | --- | --- |
| Process | `NSApplication` with `.accessory` policy, `LSUIElement` | `wWinMain`, `/SUBSYSTEM:WINDOWS`, a hidden message window (`CreateWindowEx` with `HWND_MESSAGE` parent) owns the tray icon and the hotkey |
| Menu bar item | `NSStatusItem` + `NSMenu` | `Shell_NotifyIcon` (`NIF_ICON | NIF_MESSAGE | NIF_TIP`, `NOTIFYICON_VERSION_4`) + `CreatePopupMenu` / `TrackPopupMenu` |
| Tray icon image | SF Symbol `camera.viewfinder` | Render the Segoe MDL2 Assets glyph U+E722 (Camera) into a 32 × 32 DIB with GDI `DrawText`, then `CreateIconIndirect`. No `.ico` asset |
| Global hotkey | Carbon `RegisterEventHotKey`, default ⇧⌘9 | `RegisterHotKey(hwnd, 1, mods | MOD_NOREPEAT, vk)`, `WM_HOTKEY`. Default: `VK_SNAPSHOT` (PrtScn), mods 0, the Lightshot default on Windows |
| Hotkey config | `UserDefaults` `hotkeyCode`, `hotkeyModifiers` | Registry `HKCU\Software\justcap`, DWORD `hotkeyVK`, `hotkeyMods` (`MOD_ALT` 1, `MOD_CONTROL` 2, `MOD_SHIFT` 4, `MOD_WIN` 8) |
| Other settings `color`, `width` | `UserDefaults` | Same key, DWORD values (`width` is an integer 1…20 in practice) |
| Launch at login | `SMAppService.mainApp` | `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`, value `justcap` = quoted exe path. State = value exists |
| Screen Recording permission | `CGPreflightScreenCaptureAccess`, polling, relaunch | **None.** Delete the whole permission block in `capture()` |
| Screen list | `NSScreen.screens`, `NSScreenNumber` | `EnumDisplayMonitors`, `GetMonitorInfo` (`rcMonitor`, physical virtual-screen pixels, may be negative), `GetDpiForMonitor` |
| Capture | `SCScreenshotManager.captureImage` per display, in parallel | `GetDC(NULL)` → `CreateCompatibleDC` + `CreateCompatibleBitmap(w, h)` → `BitBlt(mem, 0, 0, w, h, screen, mon.left, mon.top, SRCCOPY | CAPTUREBLT)` per monitor, sequentially |
| Captured image type | `CGImage` | `IWICBitmap` from `IWICImagingFactory::CreateBitmapFromHBITMAP(hbm, NULL, WICBitmapIgnoreAlpha)`, converted to `GUID_WICPixelFormat32bppPBGRA` with `IWICFormatConverter`; delete the HBITMAP at once. Cache an `ID2D1Bitmap` per render target via `CreateBitmapFromWicBitmap` |
| Overlay window | borderless `NSWindow`, level just below pop-up menus, `canJoinAllSpaces` | `WS_POPUP`, `WS_EX_TOPMOST | WS_EX_TOOLWINDOW` (no taskbar button), positioned at `rcMonitor`, `hbrBackground = NULL`, `WM_ERASEBKGND` returns 1, `WM_MOUSEACTIVATE` returns `MA_ACTIVATE`. Popup menus are always above topmost windows, so the color menu needs nothing special |
| Key window | `makeKey()` on the screen under the mouse | `GetCursorPos` + `MonitorFromPoint`, then `SetForegroundWindow` + `SetFocus`. Record `GetForegroundWindow()` before showing the overlays and restore it in `finish` (replaces `NSApp.hide`) |
| Drawing | `draw(_:)` with `setNeedsDisplay(rect)` | `InvalidateRect(hwnd, &rectPx, FALSE)`; in `WM_PAINT`: `BeginPaint`, convert `ps.rcPaint` to DIPs, `BeginDraw`, `PushAxisAlignedClip(dirty)`, draw, `PopAxisAlignedClip`, `EndDraw`; on `D2DERR_RECREATE_TARGET` recreate the target and its bitmap from the WIC bitmap |
| Even-odd dim fill | `NSBezierPath` with `.evenOdd` | Draw the image, fill the whole dirty rect with black α 0.5, draw the image again clipped to `sel`. Two calls, no geometry |
| Paths, strokes | `NSBezierPath`, round caps/joins | `ID2D1PathGeometry` rebuilt per draw for pen/marker; `DrawLine`, `DrawRectangle`; `ID2D1StrokeStyle` with round cap/join (miter join for rect) |
| Marker alpha | `color.withAlphaComponent(0.4)` | `CreateSolidColorBrush` with alpha 0.4. One stroked geometry per annotation, so overlaps inside one stroke do not darken, same as AppKit |
| Dotted text box | `setLineDash([3, 3], phase)` | `D2D1_DASH_STYLE_CUSTOM`, dashes `{3, 3}`, `dashOffset` 0 for black, 3 for white |
| Text | `NSString.draw(at:withAttributes:)`, `boldSystemFont(ofSize: 10 + width * 2)` | `IDWriteTextFormat` "Segoe UI", `DWRITE_FONT_WEIGHT_BOLD`, size `10 + width * 2` DIPs; `IDWriteTextLayout` + `GetMetrics` for `textRect`; `DrawTextLayout` |
| Text entry | `NSTextField` subview + hidden Edit menu | A custom single-line editor drawn inside the overlay (§6.4). Delete the Edit-menu code |
| Cursors | `.crosshair`, `.openHand`, `.closedHand`, `frameResize` | `WM_SETCURSOR` → `SetCursor(LoadCursor(NULL, …))`: `IDC_CROSS`, `IDC_SIZEALL` (move), `IDC_HAND` (text hover), `IDC_SIZEWE`, `IDC_SIZENS`, `IDC_SIZENWSE`, `IDC_SIZENESW`, `IDC_ARROW` over the toolbar. Return `TRUE` |
| Mouse events | `mouseDown/Dragged/Up/Moved`, tracking area | `WM_LBUTTONDOWN` (+ `SetCapture`), `WM_MOUSEMOVE` (dragged if `wParam & MK_LBUTTON`, else moved), `WM_LBUTTONUP` (+ `ReleaseCapture`) |
| Scroll | `scrollWheel`, 1 step per notch, 12 for precise, momentum ignored | `WM_MOUSEWHEEL`: accumulate `GET_WHEEL_DELTA_WPARAM`; one step per `WHEEL_DELTA` (120). Precision touchpads send small deltas; the accumulator handles them. Momentum cannot be told apart; accept |
| Keys | `keyDown` with `kVK_*`, `performKeyEquivalent` with ⌘ | `WM_KEYDOWN`: `VK_ESCAPE`, `VK_RETURN`, arrows; Ctrl+C/S/Z = `'C'`/`'S'`/`'Z'` with `GetKeyState(VK_CONTROL) & 0x8000`; Shift via `GetKeyState(VK_SHIFT)` |
| Toolbar | `NSStackView` of `NSButton`s with SF Symbols, tooltips | Drawn in Direct2D inside the overlay: a rounded rect (radius 7, white 0.25 α 0.95), 26 × 24 button rects, spacing 2, insets 4/6, extra 10 after the tools and after Undo. Hit-test by rect in `WM_LBUTTONDOWN`. Icons: Segoe MDL2 Assets glyphs where one exists, simple D2D primitives for the rest (§6.3). Tooltips: skip |
| Color menu | `NSMenu.popUp` with 14 pt circle images | `TrackPopupMenu` at the button; each item gets a 14 × 14 colour swatch via `SetMenuItemBitmaps` (both states); prefix the current colour's name with `✓ ` |
| Timers | `perform(afterDelay:)`, `asyncAfter` | `SetTimer` / `KillTimer`: 800 ms preview hide, 150 ms menu fade before capture |
| Pixelate blocks | `CGImage.cropping` + small `CGContext`, draw with `.none` interpolation | `IWICBitmapClipper` on the capture → `IWICBitmapScaler` to `w × h` with `WICBitmapInterpolationModeFant` → store the `IWICBitmap` in the annotation; draw with `D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR` into `box` |
| Output render | `CGContext` of `s.size * k` px, `scaleBy`, `translateBy` | `IWICImagingFactory::CreateBitmap(w_px, h_px, 32bppPBGRA)` → `ID2D1Factory::CreateWicBitmapRenderTarget` with `dpi = 96 * k` → `SetTransform(Translation(-s.x, -s.y))` → draw the clipped source + `drawAnnotations(clip: s)`. Direct2D 1.0 brushes and bitmaps belong to one render target: make every draw function take `ID2D1RenderTarget*` and create brushes on the fly (`CreateSolidColorBrush` is cheap) |
| Clipboard | PNG + TIFF | `OpenClipboard`, `EmptyClipboard`, `SetClipboardData(RegisterClipboardFormat(L"PNG"), hGlobalPng)` and `SetClipboardData(CF_DIB, hGlobalDib)` (32 bpp, bottom-up rows, positive `biHeight`), `CloseClipboard`. Chrome, Slack, Discord, Paint.NET read "PNG"; everything else reads `CF_DIB` |
| PNG encode | `NSBitmapImageRep.representation(using: .png)` | `IWICImagingFactory::CreateEncoder(GUID_ContainerFormatPng)` on an `IStream` (`CreateStreamOnHGlobal` for the clipboard, `SHCreateStreamOnFileEx` for the file) |
| Sound | `NSSound(named: "Pop")` | `PlaySound(L"SystemAsterisk", NULL, SND_ALIAS | SND_ASYNC)` |
| Save panel | `NSSavePanel`, default dir from `com.apple.screencapture location` else Desktop | `IFileSaveDialog`: `SetFileTypes` (PNG), `SetDefaultExtension(L"png")`, `SetFileName(L"Screenshot yyyy-MM-dd HH.mm.ss.png")`, `SetFolder` = `FOLDERID_Screenshots` (where Win+PrtScn saves) if it exists, else `FOLDERID_Desktop` |
| Alerts | `NSAlert.runModal` | `MessageBoxW(NULL, text, L"justcap", MB_ICONWARNING)` |
| Modal guard | `modal` flag around `runModal` | Same flag around `MessageBox`, `IFileSaveDialog::Show`, `TrackPopupMenu` is fine without. `WM_HOTKEY` still arrives during a modal loop, so the guard is needed |
| Memory release | `windows.forEach { $0.close() }` | `DestroyWindow` each overlay; release its `ID2D1Bitmap`, render target and `IWICBitmap` in `WM_DESTROY` |
| Self-test output | `print("selftest ok")`, `exit(0)` | Exit code 0 / 1. For the message: `AttachConsole(ATTACH_PARENT_PROCESS)` then `WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), …)`. `build.cmd` checks `%errorlevel%` |
| Bundle version | `CFBundleVersion` = git commit count | `/DBUILD_NUMBER=N` from `git rev-list --count HEAD`; show it in the tray tooltip `justcap 0.1.0 (N)` |

Needed libraries: `user32 gdi32 shell32 ole32 uuid d2d1 dwrite windowscodecs winmm shcore advapi32 imm32`. COM: `CoInitializeEx(NULL, COINIT_APARTMENTTHREADED)` once in `wWinMain` (WIC, the save dialog and `SHGetKnownFolderPath` need it).

---

## 5. Behaviour contract (must stay identical)

Everything in the README "Use" table, plus:

- The capture starts only when no overlay is open, no capture is running and no modal dialog is open (`capture()` guards).
- A new selection on one monitor clears the selection and annotations on the others (`onSelectionStart` → `clearSelection`).
- A click outside the selection does nothing while annotations exist. A selection smaller than 2 × 2 is discarded together with its annotations.
- Text: a drag moves it, a click (movement < 3 DIP) re-edits it, scroll over it resizes only that text, hover shows the dotted box. Esc on a re-edit keeps the original; Return with empty text on a re-edit keeps the original. Editing a text does not change the global width after the edit (`widthBeforeEdit`).
- Pixelate reads the frozen screen only, never other annotations.
- Width is clamped to 1…20 and persisted. Colour index is clamped to the palette and persisted.
- Copy and Return produce the same result. The output image is at physical resolution; its logical size is the selection in DIPs (on Windows nothing consumes that, so just produce the pixels).
- Idle: no window, no timer, no thread. The frozen images exist only while the overlay is open.

---

## 6. Detailed notes per module

### 6.1 `main.cpp` — the App

Port `App` as a set of functions over a few globals, or a struct; one hidden window procedure handles `WM_HOTKEY`, the tray callback (`WM_APP + 1`), `WM_TIMER` (menu-fade delay), `WM_COMMAND` (menu ids), and the `TaskbarCreated` registered message (re-add the tray icon after Explorer restarts).

Tray menu quirks: call `SetForegroundWindow(hwnd)` before `TrackPopupMenu` and `PostMessage(hwnd, WM_NULL, 0, 0)` after, or the menu does not close on an outside click.

Menu items: `Capture  PrtScn` (or `Capture` when the hotkey is custom, as in Swift), `Launch at Login` (`MF_CHECKED` when the Run value exists), separator, `Quit justcap`. A left click on the tray icon (`NIN_SELECT`) also captures.

Hotkey failure: `RegisterHotKey` returns 0 when PrtScn is taken. Windows 11 binds PrtScn to Snipping Tool by default (Settings › Accessibility › Keyboard › "Use the Print screen key to open screen capture"). The alert must say: turn that off, or set another key with

```
reg add HKCU\Software\justcap /v hotkeyVK /t REG_DWORD /d 0x33 /f
reg add HKCU\Software\justcap /v hotkeyMods /t REG_DWORD /d 6 /f
```

(0x33 = `'3'`, 6 = Ctrl+Shift) and restart justcap. Capture from the tray keeps working, as on macOS.

`capture()`: enumerate monitors, `BitBlt` each, build the WIC bitmaps, then create and show all overlay windows, then focus the one under the cursor. Everything on the UI thread; no task group. Total time for three 4K monitors is well under 100 ms.

`finish(image, save, size)`: destroy the overlays first (frees the images), restore the previous foreground window, then encode/copy/save. Same order as Swift.

### 6.2 `overlay.cpp` — the Overlay

One class, one instance per monitor, `WndProc` dispatching to the instance through `GWLP_USERDATA`. Port `OverlayView` member by member. Keep the Swift names (`sel`, `annotations`, `current`, `drag`, `tool`, `hoverText`, `colorIndex`, `width`, `scrollAcc`, `edited`, `widthBeforeEdit`, `preview`) and the `Drag` enum with its four payloads (`std::variant` or a tagged struct). Keep `didSet` side effects: the Swift property observers that call `setNeedsDisplay` become explicit `invalidate(rect)` calls in small setter functions, so partial redraw survives the port (REVIEW.md §2.1 explains why this matters on large screens).

Port these functions line for line, applying only the §3 table: `mouseMoved`, `layoutBar`, `nudge`, `mouseDown`, `mouseDragged`, `mouseUp`, `scrollWheel`, `handlePoints`, `handleHit`, `handle`, `constrain`, `textRect`, `textIndex`, `chrome`, `bbox`, `box`, `draw`, `dottedBox`, `drawPreview`, `drawAnnotations`, `draw(_ a:)`, `pixels`, `blocks`, `render`.

When a mouse button goes down on an overlay that is not the foreground window, call `SetForegroundWindow` + `SetFocus` on it so the keyboard follows the mouse, as AppKit does with `canBecomeKey`.

### 6.3 Toolbar icons

Segoe MDL2 Assets ships with Windows 10 and 11 (Segoe Fluent Icons is Windows 11 only; do not use it). Draw glyphs with DirectWrite at 16 DIP, centred in the 26 × 24 button. Verify the code points in Character Map (`charmap.exe`) before relying on them:

| Button | Glyph |
| --- | --- |
| Pen | U+E70F (Edit) |
| Marker | U+E7E6 (Highlight) |
| Text | U+E8D2 (Font) |
| Undo | U+E7A7 (Undo) |
| Copy | U+E8C8 (Copy) |
| Save | U+E74E (Save) |
| Close | U+E711 (Cancel) |
| Color | a filled 12 DIP circle in the current colour |
| Line, Arrow, Rectangle, Pixelate | draw with D2D primitives: a diagonal line, a diagonal line with a head, a stroked rect, a 2 × 2 checkerboard |

The active tool gets a white α 0.3 rounded background (radius 4), as in `syncBar`.

### 6.4 Text editor

Do not use an `EDIT` control: it cannot be transparent over a custom-drawn parent without artifacts, and its GDI text does not match the DirectWrite text of the committed annotation, so the text jumps on commit. Implement the single-line editor in the overlay:

- State: `std::wstring text`, `size_t caret`, `bool allSelected`, `std::wstring undoText` (one level), plus the Swift fields `edited`, `widthBeforeEdit`, and a bool `editing` that replaces `field != nil`.
- `WM_CHAR`: printable characters (≥ 0x20) insert at the caret (replace all if `allSelected`); 0x08 is Backspace. Ignore control characters otherwise.
- `WM_KEYDOWN` while editing: `VK_RETURN` commit, `VK_ESCAPE` cancel (Swift: empty the text, then commit, so a re-edit keeps the original), `VK_LEFT/RIGHT/HOME/END` move the caret, `VK_DELETE`, Ctrl+A select all, Ctrl+C/X copy/cut `CF_UNICODETEXT`, Ctrl+V paste with newlines stripped, Ctrl+Z restore `undoText`. **Arrows and Ctrl+C/S/Z must not reach the selection handlers while editing** (Swift gets this from first-responder routing; the Windows code needs an explicit `if (editing)` branch first).
- IME: call `ImmSetCompositionWindow` with `CFS_POINT` at the caret on each caret move so East-Asian candidate windows appear at the text. Composition results arrive as `WM_CHAR`; nothing else is needed.
- Draw: the text with the annotation font and colour at the anchor, a 1 DIP caret at `IDWriteTextLayout::HitTestTextPosition(caret)`, and `dottedBox(fieldBox)` where `fieldBox.width = max(40, textWidth + 4)`. Invalidate `fieldBox` inset by −4 on every change (`controlTextDidChange`).
- The "2 pt NSTextField inset" correction in `beginText` and `commitText` disappears: the anchor is the text box's top-left in both the editor and the annotation.
- `scrollWheel` while editing changes `width`, re-measures, and keeps the vertical centre (`f.frame.midY - h / 2`).

### 6.5 Self-test

Port `selftest()` unchanged: a 200 × 100 source (left red, right blue) rendered through a 100 × 50 DIP view at `k = 2`, with a line, a pixelate block across x = 50 and a white "H"; the same five pixel checks. Build the source with a WIC bitmap + WIC render target (no window, no tray). Read pixels with `IWICBitmap::Lock`. Pixel rows start at the top in WIC, as in `NSBitmapImageRep`. The Overlay constructor needs a mode without a window for this: let it take an optional `HWND` and skip the render target and toolbar when it is null, like the Swift `toolbar: false` parameter. The test runs in a normal interactive session; it needs no desktop interaction.

---

## 7. Porting plan

Work in this order. Each phase ends when its "done when" holds. Commit after each phase.

1. **Skeleton.** `build.cmd`, `main.cpp` with the hidden window, tray icon, menu, hotkey, Quit, registry helpers. Done when: the icon shows, `Capture` and the hotkey each show a `MessageBox`, Quit exits, the exe is under 300 KB.
2. **Capture and frozen overlay.** `EnumDisplayMonitors` + `BitBlt` → WIC → one topmost popup per monitor drawing the image with Direct2D; Esc destroys all and restores focus. Done when: every monitor appears frozen with no visible seam or offset at 100 %, 125 %, 150 % and 200 % scaling, and Task Manager shows the memory return to idle after Esc.
3. **Selection.** Drag to select, handles, move, resize, arrow nudge, dim, size label, cursors, cross-monitor clearing, toolbar placement (empty bar). Done when: every row of the README table that does not mention a tool works.
4. **Output.** `render()`, copy (PNG + CF_DIB + sound), save dialog with default folder and name. Done when: a pasted image in Paint and in a browser is pixel-exact, and the saved PNG has the selection's pixel size.
5. **Tools.** Pen, line, arrow, rect, marker, pixelate; toolbar buttons and icons; colour menu; width by scroll with the preview; Shift constraints; undo. Done when: the README rows for Shift and Scroll work and the self-test (without the text check) passes.
6. **Text.** §6.4 fully, including move, re-edit, hover box, scroll on text. Done when: the full self-test passes and every text row of the README table works.
7. **Finish.** Launch at Login, hotkey alert text, `build.cmd install`, Windows section in the README (§10). Done when: a fresh clone builds with one command in a Developer Command Prompt and the installed app survives a logout/login.

---

## 8. Known and accepted differences

Write these into the README's Windows section. Do not silently paper over them.

- Default hotkey is PrtScn, not Ctrl+Shift+9. It fails to register when Snipping Tool owns the key; the alert explains the fix.
- Monitors are captured a few milliseconds apart, not simultaneously.
- The clipboard holds PNG and CF_DIB instead of PNG and TIFF.
- Touchpad momentum is not filtered.
- No tooltips on the toolbar.
- Move cursor is `IDC_SIZEALL`, text hover is `IDC_HAND`; Windows has no open/closed hand cursors.
- Launch at Login uses the Run key; Task Manager › Startup apps can disable it, and justcap then still shows it as enabled. Accept.
- No code signing. SmartScreen does not warn for a locally built exe; it may for one copied from a download.
- Exclusive-fullscreen games and the secure desktop capture black.
- x64 by default; Arm64 is a separate build.

---

## 9. `build.cmd`

Mirror `build.sh`: one compile, version stamp, self-test, optional install. Run from a "Developer Command Prompt for VS" (or after `vcvarsall.bat x64`). No solution file.

```bat
@echo off
setlocal
cd /d %~dp0
if not exist build mkdir build
for /f %%n in ('git rev-list --count HEAD 2^>nul') do set BUILD=%%n
if not defined BUILD set BUILD=1
cl /nologo /std:c++17 /O2 /W4 /EHsc /DUNICODE /D_UNICODE /DBUILD_NUMBER=%BUILD% ^
   src\main.cpp src\overlay.cpp /Fo:build\ /Fe:build\justcap.exe ^
   /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib ole32.lib uuid.lib d2d1.lib dwrite.lib ^
   windowscodecs.lib winmm.lib shcore.lib advapi32.lib imm32.lib || exit /b 1
build\justcap.exe --selftest || (echo selftest FAILED & exit /b 1)
if "%1"=="install" (
    taskkill /f /im justcap.exe >nul 2>&1
    set DEST=%LOCALAPPDATA%\Programs\justcap
    if not exist "%DEST%" mkdir "%DEST%"
    copy /y build\justcap.exe "%DEST%\" >nul
    start "" "%DEST%\justcap.exe"
)
```

Arm64: the same `cl` line from `vcvarsall.bat x64_arm64` with `/Fe:build\justcap-arm64.exe`. Optional.

Code signing: `signtool sign /fd SHA256 /a build\justcap.exe` if a certificate exists. Optional, as `HARDENED=1` is on macOS.

---

## 10. README changes

Add a "Windows" section after "Build" with: requirements (Windows 10 1703+, Visual Studio Build Tools with the C++ workload), the build command, the default hotkey and how to change it (the `reg add` lines from §6.1), where saves go (`Pictures\Screenshots`, else Desktop), the Launch at Login mechanism, and the list from §8. Update the "Files" table with the `windows/` entries. Keep the README's style: short declarative sentences, tables for key bindings.

---

## 11. Verification checklist

Run by hand before calling the port done. Use at least two monitors with different scaling if possible.

- [ ] Idle: no window in Task Manager other than the hidden one, memory under 15 MB, 0 % CPU.
- [ ] Hotkey and tray Capture freeze all monitors; the tray menu is not in the capture.
- [ ] Hotkey during an open overlay, during the save dialog and during an alert does nothing.
- [ ] 100 / 125 / 150 / 200 % scaling: frozen image is pixel-exact (no blur, no offset), selection label shows pixel sizes, output PNG has those sizes.
- [ ] Every row of the README "Use" table.
- [ ] New selection on monitor B clears monitor A.
- [ ] Esc restores the previously active application.
- [ ] Paste into Paint, a browser, and a chat app: all receive the image.
- [ ] Save: dialog opens in Pictures\Screenshots, name is `Screenshot yyyy-MM-dd HH.mm.ss.png`, the file opens.
- [ ] Hotkey conflict: with Snipping Tool bound to PrtScn, the alert appears at launch and tray Capture still works.
- [ ] Launch at Login toggles the Run value; the app starts after logout/login.
- [ ] Explorer restart (`taskkill /f /im explorer.exe && start explorer`) brings the tray icon back.
- [ ] `build\justcap.exe --selftest` exits 0; break the pixelate scaler on purpose and confirm it exits 1.
