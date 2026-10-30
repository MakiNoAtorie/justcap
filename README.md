# justcap

A small screenshot tool for macOS and Windows. justcap replaces Lightshot.
It has no upload, no web search and no network access.

- Native Swift and AppKit on macOS, C++ and Win32 on Windows. No dependencies.
- Idle footprint on macOS: 11 MB of memory, 0% CPU. Binary size: 200 KB.
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

## Windows

Requirements: Windows 10 1703 or later, Visual Studio Build Tools with the C++ workload.

Run these commands in a Developer Command Prompt:

```bat
windows\build.cmd            & rem builds windows\build\justcap.exe and does the self-test
windows\build.cmd install    & rem also copies the exe to %LOCALAPPDATA%\Programs\justcap and starts it
```

justcap has a tray icon. A left click on the icon starts a capture. The right-click menu has Capture, Launch at Login and Quit.
The keys are the same as on macOS, with Ctrl in place of ⌘ (Ctrl+C, Ctrl+S, Ctrl+Z).

The default hotkey is PrtScn. To change the hotkey, set a virtual-key code and a modifier mask (`MOD_ALT` 1, `MOD_CONTROL` 2, `MOD_SHIFT` 4, `MOD_WIN` 8), then start justcap again:

```bat
reg add HKCU\Software\justcap /v hotkeyVK /t REG_DWORD /d 0x33 /f
reg add HKCU\Software\justcap /v hotkeyMods /t REG_DWORD /d 6 /f
```

Ctrl+S opens the save dialog in `Pictures\Screenshots`, else on the Desktop.
Launch at Login adds the value `justcap` to `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.

Differences from macOS:

- The default hotkey is PrtScn, not Ctrl+Shift+9. On Windows 11 the Snipping Tool owns PrtScn by default. justcap then shows an alert at launch. Turn off Settings > Accessibility > Keyboard > "Use the Print screen key to open screen capture", or set another hotkey.
- Monitors are captured a few milliseconds apart, not at the same moment.
- The clipboard holds PNG and CF_DIB, not PNG and TIFF.
- Touchpad momentum is not filtered.
- The toolbar has no tooltips.
- The move cursor is `IDC_SIZEALL` and the text hover cursor is `IDC_HAND`. Windows has no open and closed hand cursors.
- Task Manager > Startup apps can disable Launch at Login. justcap then still shows it as enabled.
- The exe has no signature. SmartScreen does not warn for a locally built exe, but it can warn for a downloaded copy.
- Exclusive-fullscreen games and the secure desktop (UAC) capture black.
- The build is x64. For Arm64, run the same `cl` command from `vcvarsall.bat x64_arm64`.

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
| `windows/src/main.cpp` | Windows: tray icon, hotkey, screen capture, copy and save, self-test |
| `windows/src/overlay.h`, `windows/src/overlay.cpp` | Windows: selection, toolbar, annotation tools, text editor, render |
| `windows/build.cmd` | Windows: build, version stamp, self-test, install |

## License

MIT. Refer to `LICENSE`.
