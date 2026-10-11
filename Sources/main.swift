import AppKit
import Carbon.HIToolbox
import ScreenCaptureKit
import ServiceManagement
import UniformTypeIdentifiers

// Global hotkey: ⇧⌘9 (the Lightshot default on macOS).
let hotkeyCode = UInt32(kVK_ANSI_9)
let hotkeyModifiers = UInt32(cmdKey | shiftKey)

final class App: NSObject, NSApplicationDelegate {
    private var item: NSStatusItem!
    private var windows: [NSWindow] = []
    private var busy = false

    func applicationDidFinishLaunching(_ n: Notification) {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.image = NSImage(systemSymbolName: "camera.viewfinder", accessibilityDescription: "Makishot")
        let menu = NSMenu()
        menu.addItem(withTitle: "Capture  ⇧⌘9", action: #selector(capture), keyEquivalent: "")
        let login = menu.addItem(withTitle: "Launch at Login", action: #selector(toggleLogin), keyEquivalent: "")
        login.state = SMAppService.mainApp.status == .enabled ? .on : .off
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit Makishot", action: #selector(NSApplication.terminate), keyEquivalent: "q")
        item.menu = menu

        // Carbon hotkeys need no Accessibility permission.
        var ref: EventHotKeyRef?
        RegisterEventHotKey(hotkeyCode, hotkeyModifiers, EventHotKeyID(signature: 0x4D4B_5348, id: 1), GetEventDispatcherTarget(), 0, &ref)
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetEventDispatcherTarget(), { _, _, _ in
            MainActor.assumeIsolated { (NSApp.delegate as? App)?.capture() }
            return noErr
        }, 1, &spec, nil, nil)
    }

    @objc func toggleLogin(_ sender: NSMenuItem) {
        let service = SMAppService.mainApp
        do {
            if service.status == .enabled { try service.unregister() } else { try service.register() }
        } catch {
            NSAlert(error: error).runModal()
        }
        sender.state = service.status == .enabled ? .on : .off
    }

    @objc func capture() {
        guard windows.isEmpty, !busy else { return }
        guard CGPreflightScreenCaptureAccess() else {
            CGRequestScreenCaptureAccess()
            return
        }
        busy = true
        Task { @MainActor in
            defer { busy = false }
            guard let content = try? await SCShareableContent.current else { return }
            for screen in NSScreen.screens {
                guard let id = screen.deviceDescription[NSDeviceDescriptionKey("NSScreenNumber")] as? CGDirectDisplayID,
                      let display = content.displays.first(where: { $0.displayID == id }) else { continue }
                let config = SCStreamConfiguration()
                config.width = Int(screen.frame.width * screen.backingScaleFactor)
                config.height = Int(screen.frame.height * screen.backingScaleFactor)
                config.showsCursor = false
                guard let image = try? await SCScreenshotManager.captureImage(
                    contentFilter: SCContentFilter(display: display, excludingWindows: []), configuration: config) else { continue }
                show(image, on: screen)
            }
            guard !windows.isEmpty else { return }
            NSApp.activate(ignoringOtherApps: true)
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
            let stamp = DateFormatter()
            stamp.dateFormat = "yyyy-MM-dd HH.mm.ss"
            panel.nameFieldStringValue = "Screenshot \(stamp.string(from: Date())).png"
            guard panel.runModal() == .OK, let url = panel.url else { return }
            do { try png.write(to: url) } catch { NSAlert(error: error).runModal() }
        } else {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setData(png, forType: .png)
        }
    }
}

/// `Makishot --selftest`: renders a selection with one annotation and checks the output.
func selftest() {
    let space = CGColorSpaceCreateDeviceRGB()
    let ctx = CGContext(data: nil, width: 200, height: 100, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)!
    ctx.setFillColor(red: 1, green: 0, blue: 0, alpha: 1)
    ctx.fill(CGRect(x: 0, y: 0, width: 200, height: 100))
    let view = OverlayView(frame: NSRect(x: 0, y: 0, width: 100, height: 50), image: ctx.makeImage()!)  // scale 2
    view.sel = NSRect(x: 10, y: 10, width: 40, height: 20)
    view.annotations = [
        Annotation(tool: .line, color: .blue, width: 4, points: [NSPoint(x: 10, y: 20), NSPoint(x: 50, y: 20)]),
        Annotation(tool: .pixelate, color: .blue, width: 4, points: [NSPoint(x: 0, y: 0), NSPoint(x: 30, y: 15)]),
    ]
    let out = NSBitmapImageRep(cgImage: view.render()!)
    precondition(out.pixelsWide == 80 && out.pixelsHigh == 40, "size")
    let line = out.colorAt(x: 40, y: 20)!, corner = out.colorAt(x: 2, y: 2)!, blocks = out.colorAt(x: 2, y: 37)!
    precondition(line.blueComponent > 0.9 && line.redComponent < 0.1, "annotation")
    precondition(corner.redComponent > 0.9 && corner.blueComponent < 0.1, "source image")
    precondition(blocks.redComponent > 0.9, "pixelate")
    print("selftest ok")
}

let app = NSApplication.shared
if CommandLine.arguments.contains("--selftest") {
    selftest()
    exit(0)
}
let delegate = App()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
