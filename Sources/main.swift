import AppKit
import Carbon.HIToolbox
import ScreenCaptureKit
import ServiceManagement
import UniformTypeIdentifiers

// Global hotkey: ⇧⌘9 (the Lightshot default on macOS). Change it with
// `defaults write com.makinoatorie.justcap hotkeyCode -int 20` (Carbon key code) and `hotkeyModifiers` (Carbon mask).
let defaultHotkey = (code: kVK_ANSI_9, modifiers: cmdKey | shiftKey)

@MainActor final class App: NSObject, NSApplicationDelegate {
    private var item: NSStatusItem!
    private var windows: [NSWindow] = []
    private var busy = false
    private var modal = false  // A save panel or an alert is open: no capture over it.
    private var polling = false
    private var hotkey: EventHotKeyRef?
    private static let stamp: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH.mm.ss"
        return f
    }()

    func applicationDidFinishLaunching(_ n: Notification) {
        // Hidden Edit menu: the text field gets ⌘X/⌘C/⌘V/⌘A/⌘Z from it. The overlay handles its own ⌘C/⌘S/⌘Z first.
        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        let editItem = NSMenuItem()
        editItem.submenu = edit
        NSApp.mainMenu = NSMenu()
        NSApp.mainMenu?.addItem(editItem)

        UserDefaults.standard.register(defaults: ["hotkeyCode": defaultHotkey.code, "hotkeyModifiers": defaultHotkey.modifiers])
        let code = UserDefaults.standard.integer(forKey: "hotkeyCode"), mods = UserDefaults.standard.integer(forKey: "hotkeyModifiers")
        let custom = code != defaultHotkey.code || mods != defaultHotkey.modifiers

        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.image = NSImage(systemSymbolName: "camera.viewfinder", accessibilityDescription: "justcap")
        let menu = NSMenu()
        menu.addItem(withTitle: custom ? "Capture" : "Capture  ⇧⌘9", action: #selector(captureFromMenu), keyEquivalent: "")
        let login = menu.addItem(withTitle: "Launch at Login", action: #selector(toggleLogin), keyEquivalent: "")
        login.state = SMAppService.mainApp.status == .enabled ? .on : .off
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit justcap", action: #selector(NSApplication.terminate), keyEquivalent: "q")
        item.menu = menu

        // Carbon hotkeys need no Accessibility permission.
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetEventDispatcherTarget(), { _, _, _ in
            MainActor.assumeIsolated { (NSApp.delegate as? App)?.capture() }
            return noErr
        }, 1, &spec, nil, nil)
        let status = RegisterEventHotKey(UInt32(code), UInt32(mods), EventHotKeyID(signature: 0x4A43_4150 /* JCAP */, id: 1),
                                         GetEventDispatcherTarget(), 0, &hotkey)
        if status != noErr {
            let a = NSAlert()
            a.messageText = "The justcap hotkey is not available"
            a.informativeText = "Another app uses it (error \(status)). Use Capture in the menu bar, or set another hotkey with "
                + "defaults write com.makinoatorie.justcap hotkeyCode -int <key code>, then start justcap again."
            alert(a)
        }
    }

    private func alert(_ a: NSAlert) {
        modal = true
        NSApp.activate()
        a.runModal()
        modal = false
    }

    @objc func toggleLogin(_ sender: NSMenuItem) {
        let service = SMAppService.mainApp
        do {
            if service.status == .enabled { try service.unregister() } else { try service.register() }
            if service.status == .requiresApproval { SMAppService.openSystemSettingsLoginItems() }
        } catch {
            alert(NSAlert(error: error))
        }
        sender.state = service.status == .enabled ? .on : .off
    }

    /// Lets the status menu fade out first, so it is not in the capture.
    @objc func captureFromMenu() {
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(150)) { self.capture() }
    }

    func capture() {
        guard windows.isEmpty, !busy, !modal else { return }
        guard CGPreflightScreenCaptureAccess() else {
            CGRequestScreenCaptureAccess()
            guard !polling else { return }
            polling = true
            // TCC applies the permission to a new process only: relaunch when it is given.
            Task {
                defer { polling = false }
                for _ in 0..<120 {
                    try? await Task.sleep(for: .seconds(1))
                    guard CGPreflightScreenCaptureAccess() else { continue }
                    if let hotkey { UnregisterEventHotKey(hotkey) }  // Free it for the new instance.
                    let config = NSWorkspace.OpenConfiguration()
                    config.createsNewApplicationInstance = true
                    _ = try? await NSWorkspace.shared.openApplication(at: Bundle.main.bundleURL, configuration: config)
                    NSApp.terminate(nil)
                    return
                }
            }
            return
        }
        busy = true
        Task {
            defer { busy = false }
            do {
                let content = try await SCShareableContent.current
                let jobs = NSScreen.screens.compactMap { screen -> (NSScreen, SCContentFilter, SCStreamConfiguration)? in
                    guard let id = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID,
                          let display = content.displays.first(where: { $0.displayID == id }) else { return nil }
                    let config = SCStreamConfiguration()
                    config.width = Int(screen.frame.width * screen.backingScaleFactor)
                    config.height = Int(screen.frame.height * screen.backingScaleFactor)
                    config.showsCursor = false
                    return (screen, SCContentFilter(display: display, excludingWindows: []), config)
                }
                // All displays at the same moment, then all overlays at once.
                let images = try await withThrowingTaskGroup(of: (Int, CGImage).self) { group in
                    for (i, job) in jobs.enumerated() {
                        group.addTask { (i, try await SCScreenshotManager.captureImage(contentFilter: job.1, configuration: job.2)) }
                    }
                    return try await group.reduce(into: [Int: CGImage]()) { $0[$1.0] = $1.1 }
                }
                for (i, job) in jobs.enumerated() { if let image = images[i] { show(image, on: job.0) } }
            } catch {
                alert(NSAlert(error: error))
                return
            }
            guard !windows.isEmpty else { return }
            NSApp.activate()
            (windows.first { $0.frame.contains(NSEvent.mouseLocation) } ?? windows[0]).makeKey()
        }
    }

    private func show(_ image: CGImage, on screen: NSScreen) {
        let w = OverlayWindow(contentRect: screen.frame, styleMask: .borderless, backing: .buffered, defer: false)
        // Above the menu bar and the Dock, below pop-up menus (the color menu).
        w.level = NSWindow.Level(rawValue: NSWindow.Level.popUpMenu.rawValue - 1)
        w.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        w.isReleasedWhenClosed = false
        w.animationBehavior = .none
        let view = OverlayView(frame: NSRect(origin: .zero, size: screen.frame.size), image: image)
        view.onDone = { [weak self] image, save, size in self?.finish(image, save: save, size: size) }
        // A new selection on one screen clears the selection on the others.
        view.onSelectionStart = { [weak self, weak view] in
            self?.windows.compactMap { $0.contentView as? OverlayView }.filter { $0 !== view }.forEach { $0.clearSelection() }
        }
        w.contentView = view
        w.makeFirstResponder(view)
        w.orderFrontRegardless()
        windows.append(w)
    }

    private func finish(_ image: CGImage?, save: Bool, size: NSSize) {
        // Release the windows: they hold the full-screen images.
        windows.forEach { $0.close() }
        windows = []
        defer { NSApp.hide(nil) }  // Give the focus back to the previous app.
        guard let image else { return }
        let rep = NSBitmapImageRep(cgImage: image)
        rep.size = size
        guard let png = rep.representation(using: .png, properties: [:]) else { return }
        if save {
            let panel = NSSavePanel()
            panel.allowedContentTypes = [.png]
            panel.nameFieldStringValue = "Screenshot \(Self.stamp.string(from: Date())).png"
            // Where the macOS screenshots go: `defaults write com.apple.screencapture location`, else the Desktop.
            panel.directoryURL = UserDefaults(suiteName: "com.apple.screencapture")?.string(forKey: "location")
                .map { URL(fileURLWithPath: ($0 as NSString).expandingTildeInPath) }
                ?? FileManager.default.urls(for: .desktopDirectory, in: .userDomainMask).first
            modal = true
            let ok = panel.runModal() == .OK
            modal = false
            guard ok, let url = panel.url else { return }
            do { try png.write(to: url) } catch { alert(NSAlert(error: error)) }
        } else {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setData(png, forType: .png)
            NSPasteboard.general.setData(rep.tiffRepresentation, forType: .tiff)
            NSSound(named: "Pop")?.play()
        }
    }
}

/// `justcap --selftest`: renders a selection with annotations and checks the output.
@MainActor func selftest() {
    // Source: 200 × 100 px, left half red, right half blue.
    let space = CGColorSpaceCreateDeviceRGB()
    let ctx = CGContext(data: nil, width: 200, height: 100, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    ctx.setFillColor(red: 1, green: 0, blue: 0, alpha: 1)
    ctx.fill(CGRect(x: 0, y: 0, width: 100, height: 100))
    ctx.setFillColor(red: 0, green: 0, blue: 1, alpha: 1)
    ctx.fill(CGRect(x: 100, y: 0, width: 100, height: 100))
    let view = OverlayView(frame: NSRect(x: 0, y: 0, width: 100, height: 50), image: ctx.makeImage()!, toolbar: false)  // scale 2
    view.sel = NSRect(x: 20, y: 10, width: 40, height: 20)
    view.annotations = [
        Annotation(tool: .line, color: .green, width: 4, points: [NSPoint(x: 36, y: 25), NSPoint(x: 60, y: 25)]),
        // One 10 pt block across the red/blue boundary at x = 50.
        Annotation(tool: .pixelate, color: .blue, width: 4, points: [NSPoint(x: 45, y: 12), NSPoint(x: 55, y: 22)]),
        Annotation(tool: .text, color: .white, width: 3, points: [NSPoint(x: 22, y: 12)], text: "H"),
    ]
    let out = NSBitmapImageRep(cgImage: view.render()!)  // Pixel rows start at the top.
    precondition(out.pixelsWide == 80 && out.pixelsHigh == 40, "size")
    let line = out.colorAt(x: 40, y: 10)!, red = out.colorAt(x: 2, y: 2)!, blue = out.colorAt(x: 78, y: 2)!
    let blocks = out.colorAt(x: 60, y: 26)!
    precondition(line.greenComponent > 0.8 && line.redComponent < 0.2, "annotation")
    precondition(red.redComponent > 0.9 && red.blueComponent < 0.1, "source image, red half")
    precondition(blue.blueComponent > 0.9 && blue.redComponent < 0.1, "source image, blue half")
    precondition((0.2...0.8).contains(blocks.redComponent) && (0.2...0.8).contains(blocks.blueComponent), "pixelate")
    let glyph = (4..<28).contains { x in (0..<36).contains { y in
        let c = out.colorAt(x: x, y: y)!
        return c.redComponent > 0.9 && c.greenComponent > 0.9 && c.blueComponent > 0.9
    } }
    precondition(glyph, "text")
    print("selftest ok")
}

MainActor.assumeIsolated {
    let app = NSApplication.shared
    if CommandLine.arguments.contains("--selftest") {
        selftest()
        exit(0)
    }
    let delegate = App()
    app.delegate = delegate
    app.setActivationPolicy(.accessory)
    withExtendedLifetime(delegate) { app.run() }
}
