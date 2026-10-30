# justcap

A small screenshot tool for macOS. justcap replaces Lightshot.
It has no upload, no web search and no network access.

- Native Swift and AppKit. No dependencies.
- Idle footprint: 11 MB of memory, 0% CPU. Binary size: 200 KB.
- The full-screen images stay in memory only while the overlay is open.

## Requirements

- macOS 14 or later.
- Xcode command line tools (`swiftc`).

## Build

```bash
./build.sh            # builds build/justcap.app (universal: arm64 and x86_64) and does the self-test
./build.sh install    # also copies the app to /Applications and starts it
```

At the first capture, macOS asks for the Screen Recording permission.
Give the permission in System Settings > Privacy & Security. justcap restarts itself when it gets the permission (it waits up to 2 minutes).

`build.sh` signs the app with the `Apple Development` identity. Set `SIGN_ID` to use a different identity.
If no identity is available, the signature is ad hoc and macOS asks for the permission again after each build.
`CFBundleVersion` is set to the number of git commits.

Hardened runtime and notarization are intentionally not done: the app is for personal use.
`HARDENED=1 ./build.sh` adds `--options runtime --timestamp` to the signature; notarization (`notarytool`) stays a manual step.

The self-test creates no window and no toolbar, but it needs a user login session (it does not run over plain SSH without one).

## Use

Press ⇧⌘9, or select Capture in the menu bar. Then drag to select an area.
A new selection on one screen removes the selection on the other screens.

| Input | Function |
| --- | --- |
| Drag in the selection (no tool) | Moves the selection |
| Drag a handle | Changes the size of the selection. The pointer shows a resize cursor near a handle |
| Arrow keys | Move the selection by 1 pt. Shift + arrow: 10 pt |
| ⌘C or Return | Copies the image to the clipboard (PNG and TIFF) and plays a sound |
| ⌘S | Saves the PNG to a file. The save panel opens in the macOS screenshot folder (`com.apple.screencapture location`), else on the Desktop |
| ⌘Z | Removes the last annotation |
| Esc | Closes the overlay |
| Shift + drag | Line and arrow: 45° steps. Rectangle and pixelate: square |
| Scroll | Changes the line width and the text size. A preview shows the size at the pointer. justcap keeps the width for the next capture. Trackpad momentum is ignored |
| ⌘X, ⌘C, ⌘V, ⌘A, ⌘Z during text entry | Cut, copy, paste, select all and undo in the text. They do not copy, save or undo the screenshot |
| Scroll during text entry | Changes the size of that text |
| Text tool: drag a text | Moves the text |
| Text tool: click a text | Edits the text again |
| Text tool: scroll on a text | Changes the size of that text |

Tools: pen, line, arrow, rectangle, marker, text, pixelate, color.
Click the active tool again to go back to move mode.

To change the hotkey, set a Carbon key code and modifier mask, then start justcap again:

```bash
defaults write com.makinoatorie.justcap hotkeyCode -int 20         # kVK_ANSI_3
defaults write com.makinoatorie.justcap hotkeyModifiers -int 768   # cmdKey (256) + shiftKey (512)
defaults delete com.makinoatorie.justcap hotkeyCode                # back to ⇧⌘9
```

With a custom hotkey, the menu shows "Capture" without the shortcut.
If another app already uses the hotkey, justcap shows an alert at launch and Capture in the menu bar still works.

## Differences from Lightshot

- Removed: upload, share, web search, print.
- Added: pixelate tool, Shift constraint, line width control, Return to copy.
- A click outside the selection does not delete annotations.

## Limits

- Text annotations have one line. Press Return to complete the text. Esc cancels the edit.
- A selection stays on one screen.
- The pixelate tool reads the screen image only. It does not hide other annotations.

## Files

| File | Content |
| --- | --- |
| `Sources/main.swift` | Menu bar item, hotkey, screen capture, copy and save, self-test |
| `Sources/Overlay.swift` | Selection, toolbar, annotation tools, render |
| `Info.plist` | Bundle data. `LSUIElement` hides the Dock icon |
| `build.sh` | Universal build, version stamp, signature, self-test, install |

## License

MIT. Refer to `LICENSE`.
