# Code review: justcap

Scope: `Sources/main.swift`, `Sources/Overlay.swift`, `build.sh`, `Info.plist`, `README.md` (state of commit `eb2d43a`).
Nothing was changed. Each item has a severity, a location, the problem and the smallest fix.

Overall: the code is small, readable and does what the README says. It compiles without warnings in Swift 5 mode.
The findings below are mostly edge cases, three real bugs, and one performance problem that matters on large Retina screens.

Severity: **High** = user-visible bug or data loss. **Medium** = wrong behaviour in a reachable case, or noticeable performance cost. **Low** = polish, robustness, maintainability.

**Status (2026-10-30):** every finding in sections 1–6 is applied, except 2.6 (not worth it, as the finding says). Section 7 (ideas) is untouched. The build and the self-test pass. Not yet checked by hand in the running app: Edit-menu shortcuts in the text field, cursors, arrow-key nudge, partial redraw, parallel multi-screen capture, the relaunch after the permission grant. On macOS 14 corner handles show a crosshair (no public diagonal cursor); macOS 15 shows the diagonal one.

---

## 1. Bugs

### 1.1 High: ⌘C / ⌘Z / ⌘S are hijacked while a text annotation is being typed
`Overlay.swift:187-196` (`performKeyEquivalent`).
The window sends key equivalents down the view tree, so `OverlayView` gets them before the `NSTextField`. While the user types a text:
- ⌘C copies the *screenshot* (and commits the text) instead of copying the selected text.
- ⌘Z calls `undo()`, which commits the text and then pops it. Pressing ⌘Z to undo a typo deletes the whole text being edited.
- ⌘S commits and opens the save panel.

Fix: `guard field == nil` at the top of `performKeyEquivalent` (return `false` otherwise).

### 1.2 High: ⌘V, ⌘A, ⌘X do not work in the text field
`main.swift:16-25`, `Overlay.swift:345-363`.
The app has no main menu. AppKit routes ⌘V/⌘A/⌘X for text fields through the Edit menu, so pasting into a text annotation is impossible.
Fix: set `NSApp.mainMenu` with a hidden Edit menu (Cut/Copy/Paste/Select All bound to `cut:`, `copy:`, `paste:`, `selectAll:`), or forward those in `performKeyEquivalent` with `NSApp.sendAction(#selector(NSText.paste(_:)), to: nil, from: nil)` when `field != nil`. The menu version is ~6 lines and also fixes 1.1 for ⌘C when combined with the guard.

### 1.3 Medium: annotations survive a selection that vanished, and reappear in the next selection
`Overlay.swift:272` (`mouseUp`) and `Overlay.swift:226-230` (`mouseDown`).
Resizing a selection below 2 pt sets `sel = nil` but keeps `annotations`. The next `mouseDown` sees `sel == nil`, skips the "do not delete annotations" guard, starts a new selection and the old annotations show up inside it at their old coordinates.
Fix: in `mouseUp`, when the selection is discarded also set `annotations = []` (or set it in the new-selection branch of `mouseDown`).

### 1.4 Medium: a text annotation can be dragged outside the selection and becomes unreachable
`Overlay.swift:250-253` (`.text` drag).
The move path for texts does not clamp to `sel` (the `.move` path for the selection does). A text dragged outside is clipped away, and a click there is blocked by the "click outside does not delete annotations" guard. The only way back is ⌘Z, which pops the *last* annotation, not necessarily that text.
Fix: clamp `points[0]` to `sel` in the `.text` case, like `.move` does for the selection.

### 1.5 Medium: the hotkey works while the save panel or an alert is modal
`main.swift:47-53` (`capture`), `main.swift:98-105`.
`busy` is reset before `NSSavePanel.runModal()` (the `Task` ends when `finish` returns, but `finish` is called from `onDone`, i.e. from the overlay, not from the Task). While the panel is open, `windows.isEmpty` is true and `busy` is false, so ⇧⌘9 starts a new capture over the modal panel. The deferred `NSApp.hide(nil)` of the first `finish` then hides the new overlay.
Fix: a `saving` flag set around `runModal` (or reuse `busy`), checked in `capture()`.

### 1.6 Low: trackpad momentum keeps changing the line width after the fingers lift
`Overlay.swift:280-284`.
`scrollWheel` accumulates `scrollingDeltaY` for momentum events too. A flick on the trackpad changes the width by several steps after the gesture ends.
Fix: `guard e.momentumPhase.isEmpty else { return }` at the top.

### 1.7 Low: editing an existing text changes the global line width
`Overlay.swift:347`.
`beginText(editing:)` sets `width = a.width` so the field is sized right, but the value stays after the edit. Editing a size-12 text then drawing a line gives a size-12 line.
Fix: save and restore `width` around the edit, or keep a separate `textWidth` for the field.

### 1.8 Low: key equivalents depend on the keyboard layout
`Overlay.swift:189`.
`charactersIgnoringModifiers` returns the layout's character. On a Cyrillic, Greek or Hebrew layout ⌘C/⌘S/⌘Z are not matched. `keyDown` already uses key codes.
Fix: switch on `e.keyCode` (`kVK_ANSI_C` = 8, `kVK_ANSI_S` = 1, `kVK_ANSI_Z` = 6). `Carbon.HIToolbox` is already imported in `main.swift`; import it here too and replace the magic numbers 53/36/76 with `kVK_Escape`, `kVK_Return`, `kVK_ANSI_KeypadEnter`.

### 1.9 Low: negative stored color index crashes at launch
`Overlay.swift:55`.
`min(UserDefaults.integer(forKey: "color"), palette.count - 1)` with a negative value (a stray `defaults write`) indexes `palette[-1]`.
Fix: `max(0, min(..., palette.count - 1))`.

### 1.10 Low: capture from the menu can include the fading status menu
`main.swift:20`, `main.swift:47`.
When "Capture" is picked from the status item menu, the capture starts while the menu is still animating out. macOS' own screenshot has the same artefact, but a `DispatchQueue.main.asyncAfter(.milliseconds(150))` before `SCShareableContent.current` when invoked from the menu avoids it.

---

## 2. Performance

### 2.1 Medium: every redraw re-rasterises the full-screen image
`Overlay.swift:392-395` and every `needsDisplay = true` (`sel`, `annotations`, `preview` didSet, `mouseDragged`, `scrollWheel`).
`draw(_:)` blits the whole capture with `ctx.draw(image, in: bounds)` and fills the dim path over the whole view. Because `needsDisplay = true` dirties the entire bounds, every mouse-drag event re-draws the full image: ~59 MB of pixels per frame on a 5K display, on the CPU (the view is not layer-backed). That is the main cost while dragging a selection or drawing.
Two fixes, pick one:
- **Small (recommended):** invalidate only what changed. In `sel.didSet`: `setNeedsDisplay(oldValue.union(sel).insetBy(dx: -40, dy: -40))` (margin for handles and the size label), and in `current`/`annotations` changes the annotation's bounds plus `width`. AppKit clips the context to the dirty rect, so `ctx.draw(image, in: bounds)` then only rasterises the dirty area. ~10 lines.
- **Larger:** put the capture in a layer (`wantsLayer = true` on a background view with `layer.contents = image`) and draw only dim + annotations in `OverlayView`. The image is then composited by the window server for free. Requires a container view.

### 2.2 Medium: pixelate recomputes its block image on every frame
`Overlay.swift:490-508`.
Each `draw(_:)` crops the source, allocates a `CGContext`, downsamples with `.high` quality and calls `makeImage()` for every pixelate annotation. While drawing a pen stroke (many events per second) over a selection with two or three pixelate areas, that is several allocations and downsamples per frame.
Fix: compute the block image once when the annotation is committed (in `mouseUp`) and store it in `Annotation` (`var cache: CGImage?`). Recompute only while it is `current`.

### 2.3 Low: `render()` draws the whole screen image into the selection-sized context
`Overlay.swift:521`.
`ctx.draw(image, in: bounds)` with the context clipped to the selection. Core Graphics usually optimises this, but `image.cropping(to:)` the selection first and drawing that is explicit, cheaper and matches what `pixelate` already does.

### 2.4 Low: multi-screen capture is sequential
`main.swift:57-67`.
Each display is captured with `await` one after the other, and the first overlay is shown before the second capture starts. On two 4K/5K screens that is a visible delay and the screens are captured at slightly different moments.
Fix: `withTaskGroup` to capture all displays in parallel, then show all windows. Also moves the "which window gets key" logic after all windows exist, which it already assumes.

### 2.5 Low: redraw for nothing
`Overlay.swift:298-302`.
`scrollWheel` sets `preview` and `needsDisplay` even when there is no selection; `draw` returns before drawing the preview in that case. Guard on `sel != nil`.

### 2.6 Low: `handlePoints` allocates an 8-element array of tuples per call
`Overlay.swift:314-323`, called in `draw` and in each `mouseDown`. Trivial cost; only worth it if 2.1 is done and the profile still shows it.

---

## 3. Robustness and error handling

### 3.1 Medium: capture failures are silent
`main.swift:56`, `main.swift:64`.
`try? await SCShareableContent.current` and `try? await SCScreenshotManager.captureImage` swallow errors. If the permission was revoked (preflight can still return true until relaunch), or ScreenCaptureKit fails, the hotkey does nothing and the user has no clue.
Fix: `do/catch` and show `NSAlert(error:)` (the pattern used in `toggleLogin`) or at least `NSSound.beep()`.

### 3.2 Medium: the first-run permission flow requires a manual restart
`main.swift:49-52`, README "start justcap again".
After `CGRequestScreenCaptureAccess()` the process must relaunch for TCC to apply. The app could do it: show an alert "Permission granted? Relaunch", then `NSWorkspace.shared.openApplication(at: Bundle.main.bundleURL, ...)` followed by `NSApp.terminate`. Alternatively poll `CGPreflightScreenCaptureAccess()` every second after the request and relaunch when it becomes true.

### 3.3 Low: hotkey registration result is ignored
`main.swift:29-34`.
`RegisterEventHotKey` returns an `OSStatus`. If another app owns ⇧⌘9 the registration fails silently and the menu bar item is the only way to capture. Check the status and show an alert once.

### 3.4 Low: the hotkey is only changeable by recompiling
`main.swift:8-9`, README.
Reading `hotkeyCode`/`hotkeyModifiers` from `UserDefaults` (with the current values as defaults) lets users change it with `defaults write com.makinoatorie.justcap hotkeyCode -int 20`. Two lines, no UI needed.

### 3.5 Low: `SMAppService.register()` can land in `.requiresApproval`
`main.swift:37-45`.
The menu state then shows "off" although the user clicked. Check `service.status == .requiresApproval` after `register()` and call `SMAppService.openSystemSettingsLoginItems()`.

### 3.6 Low: `width` is not persisted
`Overlay.swift:56` vs `Overlay.swift:158`.
The color is remembered across captures, the line width resets to 3 every time. Same `UserDefaults` pattern as `color`.

### 3.7 Low: `perform(afterDelay:)` keeps the view alive after the window is closed
`Overlay.swift:299-300`.
Harmless (≤0.8 s), but it delays releasing the full-screen image. `NSObject.cancelPreviousPerformRequests(withTarget: self)` in `finish`/`cancel` or in the window close path.

### 3.8 Low: "MKSH" hotkey signature and "Makishot" repo name
`main.swift:29`. Leftover from the rename. Cosmetic.

---

## 4. UX

### 4.1 Medium: no resize cursors and the crosshair stays over the toolbar
`Overlay.swift:77`.
`resetCursorRects` covers the whole view with a crosshair, including the toolbar buttons and the handles. Add `addCursorRect(bar.frame, cursor: .arrow)` after the crosshair, and a tracking area (`mouseMoved`) that shows `.openHand`/resize cursors over the selection and handles. Cheap and makes the move/resize affordances discoverable.

### 4.2 Low: handle hit area is 12 × 12 pt
`Overlay.swift:326`. The tolerance is 6 pt around a 6 pt handle. 8-10 pt tolerance makes corners easier to grab without visual change.

### 4.3 Low: keyboard nudge
`Overlay.swift:179-185`. Arrow keys to move the selection by 1 pt (⇧ = 10 pt) are a common expectation and ~8 lines in `keyDown`.

### 4.4 Low: no feedback on copy
`main.swift:107-108`. The overlay disappears and nothing confirms the copy. `NSSound(named: "Pop")?.play()` or the system screenshot sound is enough.

### 4.5 Low: clipboard contains PNG only
`main.swift:108`. Some apps (Office, older Java apps) look for TIFF. `setData(rep.tiffRepresentation, forType: .tiff)` alongside PNG covers them at the cost of a second encode. Only worth it if someone reports it.

### 4.6 Low: save panel has no default folder
`main.swift:99-104`. Reading `defaults read com.apple.screencapture location` and using it as `directoryURL` puts files where the user's other screenshots go. Otherwise `.downloadsDirectory` is a reasonable default.

### 4.7 Low: text re-edit that ends empty deletes the text, without undo
`Overlay.swift:274`, `Overlay.swift:376`. The annotation is removed at `mouseUp` and only re-added if non-empty. Clearing the field and pressing Return deletes it; ⌘Z cannot bring it back. Either re-insert the original when the result is empty, or accept and document it.

### 4.8 Low: the arrow head is longer than the arrow for short arrows
`Overlay.swift:466-467`. `head = width * 4 + 6` is applied even when the arrow is shorter than that, so the base lands behind the start point. `let head = min(a.width * 4 + 6, hypot(dx, dy))`.

---

## 5. Code quality

### 5.1 Low: `Tool.symbol` / `Tool.tip` index parallel arrays by `rawValue`
`Overlay.swift:6-11`. Adding a case in the middle of the enum silently shifts the icons. A `switch` is longer but the compiler checks it. Alternatively one array of `(symbol, tip)` next to the enum so there is a single place to keep in sync.

### 5.2 Low: `OverlayView` reaches into sibling windows through `NSApp.windows`
`Overlay.swift:228`. Works, but couples the view to the app's window list. An `onSelectionStart` callback set by `App.show` (next to `onDone`) is the same amount of code and testable.

### 5.3 Low: magic numbers
`Overlay.swift:181-182` (53, 36, 76), `Overlay.swift:282` (12), `Overlay.swift:496` (10 pt block), `Overlay.swift:342` (`10 + width * 2`). Key codes: see 1.8. The rest: a named constant each where the number appears twice.

### 5.4 Low: strict concurrency
`swiftc -strict-concurrency=complete` reports 52 warnings (`App` and `OverlayView` are not `@MainActor`, the Carbon callback captures, `palette` is a global `let` of non-Sendable `NSColor`). Not a problem in Swift 5 mode, but it is the cost of moving to Swift 6 later. Marking `App` and `OverlayView` `@MainActor` removes most of them.

### 5.5 Low: `NSApp.activate(ignoringOtherApps:)` is deprecated on macOS 14
`main.swift:69`. `NSApp.activate()` is the replacement; the old one still works.

### 5.6 Low: `DateFormatter` built on every save
`main.swift:101-102`. Trivial, but a `static let` or `Date().formatted(...)` reads cleaner.

---

## 6. Build, packaging, tests

### 6.1 Medium: the self-test cannot detect a broken pixelate
`main.swift:115-131`. The source image is solid red, so the pixelated area is red whether or not `pixelate` did anything. Fill the left half red and the right half blue and assert that a block on the boundary is purple-ish (both components between 0.2 and 0.8). The test also covers neither text rendering through `render()` (the only path that draws text without a focused view) nor arrow/rect/marker. One text annotation with an assert on a pixel inside the glyph would protect the most fragile path.

### 6.2 Low: single-architecture binary
`build.sh:9`. `-target $(uname -m)-apple-macos14` builds for the build machine only. Two `swiftc` runs plus `lipo -create` gives a universal binary if the app is ever distributed.

### 6.3 Low: no hardened runtime / notarization path
`build.sh:11`. Fine for personal use. For distribution outside the Mac: `--options runtime` and `--timestamp`, then `notarytool`. Document that it is intentionally skipped.

### 6.4 Low: self-test needs a GUI session
`build.sh:12`. `NSApplication.shared` and `NSButton` creation require a window server; the build fails over plain SSH or in CI. Either accept (document) or guard the test with `--selftest` not touching `buildBar` (pass a flag to skip the toolbar).

### 6.5 Low: `Info.plist` has no icon and no category
No `CFBundleIconFile`, so System Settings > Screen Recording and Login Items show the generic icon. `LSApplicationCategoryType` = `public.app-category.utilities` and `NSHumanReadableCopyright` are one line each. `CFBundleVersion` is fixed at 1; `build.sh` could stamp `git rev-list --count HEAD`.

### 6.6 Low: README promises are slightly off
- "then start justcap again": see 3.2, could be automated.
- The hotkey section says to edit the source: see 3.4.
- "Scroll during text entry: changes the size of that text": correct, but note that it also changes the width used for the next tool (see 1.7).

---

## 7. Ideas (not defects)

- Capture a window (click on a window with no drag) using the `SCWindow` list that `SCShareableContent.current` already returns.
- "Copy and save" in one action, like Lightshot's default.
- Number/step tool (circled 1, 2, 3) is a common ask for documentation screenshots.
- Remember the last selection rectangle (⇧⌘9 twice reuses it).
- Esc during a drag should abort the drag, not close the overlay.

---

## Suggested order

1. 1.1 + 1.2 (text field key handling, one hidden Edit menu + one guard).
2. 1.3, 1.4, 1.5 (small guards).
3. 2.1 small variant (dirty rects) and 2.2 (pixelate cache).
4. 3.1 (surface capture errors) and 6.1 (self-test that can fail).
5. Everything else as time allows.
