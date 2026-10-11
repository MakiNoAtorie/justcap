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
./build.sh            # builds build/justcap.app and does the self-test
./build.sh install    # also copies the app to /Applications and starts it
```

At the first capture, macOS asks for the Screen Recording permission.
Give the permission in System Settings > Privacy & Security, then start justcap again.

`build.sh` signs the app with the `Apple Development` identity. Set `SIGN_ID` to use a different identity.
If no identity is available, the signature is ad hoc and macOS asks for the permission again after each build.

## Use

Press ⇧⌘9, or select Capture in the menu bar. Then drag to select an area.

| Input | Function |
| --- | --- |
| Drag in the selection (no tool) | Moves the selection |
| Drag a handle | Changes the size of the selection |
| ⌘C or Return | Copies the PNG to the clipboard |
| ⌘S | Saves the PNG to a file |
| ⌘Z | Removes the last annotation |
| Esc | Closes the overlay |
| Shift + drag | Line and arrow: 45° steps. Rectangle and pixelate: square |
| Scroll | Changes the line width and the text size. A preview shows the size at the pointer |
| Scroll during text entry | Changes the size of that text |
| Text tool: drag a text | Moves the text |
| Text tool: click a text | Edits the text again |
| Text tool: scroll on a text | Changes the size of that text |

Tools: pen, line, arrow, rectangle, marker, text, pixelate, color.
Click the active tool again to go back to move mode.

To change the hotkey, edit `hotkeyCode` and `hotkeyModifiers` in `Sources/main.swift`.

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
| `build.sh` | Build, signature, self-test, install |
