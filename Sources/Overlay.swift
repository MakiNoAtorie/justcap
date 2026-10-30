import AppKit
import Carbon.HIToolbox

enum Tool: Int, CaseIterable {
    case pen, line, arrow, rect, marker, text, pixelate

    var symbol: String { info.0 }
    var tip: String { info.1 }
    private var info: (String, String) {
        switch self {
        case .pen: ("scribble", "Pen")
        case .line: ("line.diagonal", "Line")
        case .arrow: ("arrow.up.right", "Arrow")
        case .rect: ("rectangle", "Rectangle")
        case .marker: ("highlighter", "Marker")
        case .text: ("textformat", "Text")
        case .pixelate: ("checkerboard.rectangle", "Pixelate")
        }
    }
}

struct Annotation {
    var tool: Tool
    var color: NSColor
    var width: CGFloat
    var points: [NSPoint]
    var text = ""
    /// Pixelate only: the block image, computed once the annotation is committed.
    var blocks: CGImage? = nil
}

@MainActor let palette: [(String, NSColor)] = [
    ("Red", NSColor(srgbRed: 1, green: 0.15, blue: 0.15, alpha: 1)),
    ("Orange", NSColor(srgbRed: 1, green: 0.55, blue: 0, alpha: 1)),
    ("Yellow", NSColor(srgbRed: 1, green: 0.9, blue: 0, alpha: 1)),
    ("Green", NSColor(srgbRed: 0.1, green: 0.8, blue: 0.2, alpha: 1)),
    ("Blue", NSColor(srgbRed: 0.1, green: 0.5, blue: 1, alpha: 1)),
    ("White", .white),
    ("Black", .black),
]

final class OverlayWindow: NSWindow {
    override var canBecomeKey: Bool { true }
}

/// One view per screen. Shows the frozen screen image, the selection and the annotations.
/// All coordinates are view points, origin bottom-left.
@MainActor final class OverlayView: NSView, NSTextFieldDelegate {
    private enum Drag {
        case none, draw
        case resize(anchor: NSPoint, hx: Bool, hy: Bool, orig: NSRect)
        case move(dx: CGFloat, dy: CGFloat)
        case text(index: Int, start: NSPoint, last: NSPoint)
    }

    let image: CGImage
    var sel: NSRect? { didSet { for s in [oldValue, sel].compactMap({ $0 }) { setNeedsDisplay(chrome(s)) } } }
    // Annotations are clipped to the selection.
    var annotations: [Annotation] = [] {
        didSet {
            if let s = sel { setNeedsDisplay(s) }
            if let i = hoverText, i >= annotations.count { hoverText = nil }
        }
    }
    /// Called with the result image (nil = cancel), the save flag and the size in points.
    var onDone: (CGImage?, Bool, NSSize) -> Void = { _, _, _ in }
    /// Called when a new selection starts here, so the other screens can clear theirs.
    var onSelectionStart: () -> Void = {}

    private var current: Annotation?
    private var drag = Drag.none
    private var tool: Tool? { didSet { syncBar(); hoverText = nil } }
    /// Text tool: the text under the pointer, shown with a dotted box.
    private var hoverText: Int? {
        didSet {
            for i in [oldValue, hoverText].compactMap({ $0 }) where i < annotations.count {
                setNeedsDisplay(textRect(annotations[i]).insetBy(dx: -6, dy: -6))
            }
        }
    }
    private var colorIndex = max(0, min(UserDefaults.standard.integer(forKey: "color"), palette.count - 1)) { didSet { syncBar() } }
    private var width = CGFloat(max(1, min(UserDefaults.standard.object(forKey: "width") as? Double ?? 3, 20))) {
        didSet { UserDefaults.standard.set(Double(width), forKey: "width") }
    }
    private var scrollAcc: CGFloat = 0
    private var field: NSTextField?
    /// The text being re-edited, and the width to restore after the edit.
    private var edited: Annotation?
    private var widthBeforeEdit: CGFloat = 3
    private var preview: NSPoint? {
        didSet { for p in [oldValue, preview].compactMap({ $0 }) { setNeedsDisplay(NSRect(x: p.x - 60, y: p.y - 60, width: 120, height: 120)) } }
    }
    private let bar = NSStackView()
    private var toolButtons: [NSButton] = []
    private var colorButton: NSButton!

    private var color: NSColor { palette[colorIndex].1 }
    private var scale: CGFloat { CGFloat(image.width) / bounds.width }

    init(frame: NSRect, image: CGImage, toolbar: Bool = true) {
        self.image = image
        super.init(frame: frame)
        if toolbar { buildBar() }
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .activeAlways, .inVisibleRect], owner: self))
    }
    required init?(coder: NSCoder) { fatalError() }

    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override func resetCursorRects() {
        addCursorRect(bounds, cursor: .crosshair)
        if !bar.isHidden { addCursorRect(bar.frame, cursor: .arrow) }
    }

    override func mouseMoved(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        let onBar = !bar.isHidden && bar.frame.contains(p)
        let hover = tool == .text && field == nil && !onBar ? textIndex(at: p) : nil
        if hover != hoverText { hoverText = hover }
        guard !onBar else { return }  // arrow cursor rect
        guard let s = sel else { NSCursor.crosshair.set(); return }
        if let h = handleHit(at: p, in: s) {
            if #available(macOS 15, *) {
                let bits = (h.hy > 0 ? 1 : h.hy < 0 ? 4 : 0) | (h.hx < 0 ? 2 : h.hx > 0 ? 8 : 0)
                NSCursor.frameResize(position: NSCursor.FrameResizePosition(rawValue: UInt(bits))!, directions: .all).set()
            } else {
                (h.hy == 0 ? NSCursor.resizeLeftRight : h.hx == 0 ? .resizeUpDown : .crosshair).set()
            }
        } else {
            (tool == nil && s.contains(p) || hoverText != nil ? NSCursor.openHand : .crosshair).set()
        }
    }

    // MARK: Toolbar

    private func buildBar() {
        bar.orientation = .horizontal
        bar.spacing = 2
        bar.edgeInsets = NSEdgeInsets(top: 4, left: 6, bottom: 4, right: 6)
        bar.wantsLayer = true
        bar.layer?.backgroundColor = NSColor(white: 0.25, alpha: 0.95).cgColor
        bar.layer?.cornerRadius = 7
        toolButtons = Tool.allCases.map { button($0.symbol, $0.tip, #selector(pickTool), tag: $0.rawValue) }
        colorButton = button("circle.fill", "Color", #selector(pickColor))
        bar.setCustomSpacing(10, after: toolButtons.last!)
        let undo = button("arrow.uturn.backward", "Undo (⌘Z)", #selector(undo))
        bar.setCustomSpacing(10, after: undo)
        _ = button("doc.on.doc", "Copy (⌘C, Return)", #selector(copyImage))
        _ = button("square.and.arrow.down", "Save (⌘S)", #selector(saveImage))
        _ = button("xmark", "Close (Esc)", #selector(cancel))
        bar.isHidden = true
        addSubview(bar)
        syncBar()
    }

    private func button(_ symbol: String, _ tip: String, _ action: Selector, tag: Int = 0) -> NSButton {
        let b = NSButton(image: NSImage(systemSymbolName: symbol, accessibilityDescription: tip)!, target: self, action: action)
        b.isBordered = false
        b.toolTip = tip
        b.tag = tag
        b.contentTintColor = .white
        b.refusesFirstResponder = true
        b.wantsLayer = true
        b.layer?.cornerRadius = 4
        b.widthAnchor.constraint(equalToConstant: 26).isActive = true
        b.heightAnchor.constraint(equalToConstant: 24).isActive = true
        bar.addArrangedSubview(b)
        return b
    }

    private func syncBar() {
        for b in toolButtons {
            b.layer?.backgroundColor = b.tag == tool?.rawValue ? NSColor(white: 1, alpha: 0.3).cgColor : nil
        }
        colorButton?.contentTintColor = color
    }

    private func layoutBar() {
        guard let s = sel else { bar.isHidden = true; return }
        let size = bar.fittingSize
        let x = max(4, min(s.maxX - size.width, bounds.maxX - size.width - 4))
        var y = s.minY - size.height - 6          // below the selection
        if y < 4 { y = s.maxY + 6 }               // above
        if y + size.height > bounds.maxY - 4 { y = s.minY + 6 }  // inside
        bar.frame = NSRect(origin: NSPoint(x: x, y: y), size: size)
        bar.isHidden = bar.superview == nil
        window?.invalidateCursorRects(for: self)
    }

    @objc private func pickTool(_ b: NSButton) {
        commitText()
        tool = tool?.rawValue == b.tag ? nil : Tool(rawValue: b.tag)
    }

    @objc private func pickColor(_ b: NSButton) {
        let menu = NSMenu()
        for (i, c) in palette.enumerated() {
            let item = NSMenuItem(title: c.0, action: #selector(setColor), keyEquivalent: "")
            item.target = self
            item.tag = i
            item.state = i == colorIndex ? .on : .off
            item.image = NSImage(size: NSSize(width: 14, height: 14), flipped: false) { r in
                c.1.setFill()
                NSBezierPath(ovalIn: r.insetBy(dx: 1, dy: 1)).fill()
                return true
            }
            menu.addItem(item)
        }
        menu.popUp(positioning: nil, at: NSPoint(x: 0, y: b.bounds.maxY + 4), in: b)
    }

    @objc private func setColor(_ item: NSMenuItem) {
        colorIndex = item.tag
        UserDefaults.standard.set(colorIndex, forKey: "color")
        field?.textColor = color
    }

    @objc private func undo() {
        commitText()
        _ = annotations.popLast()
    }

    @objc private func copyImage() { finish(save: false) }
    @objc private func saveImage() { finish(save: true) }
    @objc private func cancel() {
        NSObject.cancelPreviousPerformRequests(withTarget: self)
        onDone(nil, false, .zero)
    }

    private func finish(save: Bool) {
        commitText()
        guard let s = sel, let out = render() else { return }
        NSObject.cancelPreviousPerformRequests(withTarget: self)
        onDone(out, save, s.size)
    }

    // MARK: Keyboard

    override func keyDown(with e: NSEvent) {
        switch Int(e.keyCode) {
        case kVK_Escape: cancel()
        case kVK_Return, kVK_ANSI_KeypadEnter: finish(save: false)
        case kVK_LeftArrow: nudge(-1, 0, e)
        case kVK_RightArrow: nudge(1, 0, e)
        case kVK_DownArrow: nudge(0, -1, e)
        case kVK_UpArrow: nudge(0, 1, e)
        default: break
        }
    }

    /// Arrow keys move the selection by 1 pt, with Shift by 10 pt.
    private func nudge(_ dx: CGFloat, _ dy: CGFloat, _ e: NSEvent) {
        guard var r = sel else { return }
        let k: CGFloat = e.modifierFlags.contains(.shift) ? 10 : 1
        r.origin.x = max(0, min(r.minX + dx * k, bounds.width - r.width))
        r.origin.y = max(0, min(r.minY + dy * k, bounds.height - r.height))
        sel = r
        layoutBar()
    }

    override func performKeyEquivalent(with e: NSEvent) -> Bool {
        // While a text is typed, the Edit menu handles the shortcuts for the text field.
        guard field == nil, e.modifierFlags.contains(.command), window?.isKeyWindow == true else { return false }
        switch Int(e.keyCode) {
        case kVK_ANSI_C: finish(save: false)
        case kVK_ANSI_S: finish(save: true)
        case kVK_ANSI_Z: undo()
        default: return false
        }
        return true
    }

    // MARK: Mouse

    override func mouseDown(with e: NSEvent) {
        let wasEditing = field != nil
        commitText()
        preview = nil
        hoverText = nil
        let p = convert(e.locationInWindow, from: nil)
        if let s = sel {
            if let h = handle(at: p, in: s) {
                drag = .resize(anchor: h.anchor, hx: h.hx, hy: h.hy, orig: s)
                bar.isHidden = true
                return
            }
            if s.contains(p) {
                if tool == .text {
                    // Drag moves an existing text. A click without movement edits it (see mouseUp).
                    // A click away from a text in edit only completes that text.
                    if let i = textIndex(at: p) { drag = .text(index: i, start: p, last: p) } else if !wasEditing { beginText(at: p) }
                } else if let t = tool {
                    current = Annotation(tool: t, color: color, width: width, points: [p, p])
                    drag = .draw
                } else {
                    drag = .move(dx: p.x - s.minX, dy: p.y - s.minY)
                    bar.isHidden = true
                }
                return
            }
            // A click outside must not delete annotations.
            if !annotations.isEmpty { return }
        }
        onSelectionStart()
        drag = .resize(anchor: p, hx: true, hy: true, orig: .zero)
        sel = NSRect(origin: p, size: .zero)
        bar.isHidden = true
    }

    override func mouseDragged(with e: NSEvent) {
        var p = convert(e.locationInWindow, from: nil)
        p.x = max(bounds.minX, min(p.x, bounds.maxX))
        p.y = max(bounds.minY, min(p.y, bounds.maxY))
        switch drag {
        case .none:
            break
        case let .resize(a, hx, hy, o):
            let x0 = hx ? min(a.x, p.x) : o.minX, x1 = hx ? max(a.x, p.x) : o.maxX
            let y0 = hy ? min(a.y, p.y) : o.minY, y1 = hy ? max(a.y, p.y) : o.maxY
            sel = NSRect(x: x0.rounded(), y: y0.rounded(), width: (x1 - x0).rounded(), height: (y1 - y0).rounded())
        case let .move(dx, dy):
            guard var r = sel else { break }
            r.origin.x = max(0, min(p.x - dx, bounds.width - r.width)).rounded()
            r.origin.y = max(0, min(p.y - dy, bounds.height - r.height)).rounded()
            sel = r
        case let .text(i, start, last):
            guard let s = sel else { break }
            // Keep the text inside the selection, or it can no longer be reached.
            let size = textRect(annotations[i]).size, o = annotations[i].points[0]
            annotations[i].points[0] = NSPoint(x: max(s.minX, min(o.x + p.x - last.x, s.maxX - size.width)),
                                               y: max(s.minY, min(o.y + p.y - last.y, s.maxY - size.height)))
            drag = .text(index: i, start: start, last: p)
            hoverText = i
            NSCursor.closedHand.set()
        case .draw:
            guard var a = current else { break }
            let old = a.points
            if a.tool == .pen || a.tool == .marker {
                a.points.append(p)
            } else {
                a.points[1] = e.modifierFlags.contains(.shift) ? constrain(a.points[0], p, square: a.tool != .line && a.tool != .arrow) : p
            }
            current = a
            let pts = a.tool == .pen || a.tool == .marker ? Array(a.points.suffix(2)) : old + a.points
            setNeedsDisplay(bbox(pts).insetBy(dx: -(a.width * 5 + 10), dy: -(a.width * 5 + 10)))  // marker width, arrow head
        }
    }

    override func mouseUp(with e: NSEvent) {
        if var a = current {
            let freehand = a.tool == .pen || a.tool == .marker
            current = nil
            if a.tool == .pixelate { a.blocks = blocks(box(a)) }
            if freehand || a.points[0] != a.points[1] { annotations.append(a) } else if let s = sel { setNeedsDisplay(s) }
        }
        if let s = sel, s.width < 2 || s.height < 2 {
            annotations = []
            sel = nil
        }
        if case let .text(i, start, last) = drag, hypot(last.x - start.x, last.y - start.y) < 3 {
            beginText(at: start, editing: annotations.remove(at: i))
        }
        drag = .none
        layoutBar()
    }

    override func scrollWheel(with e: NSEvent) {
        // Trackpad momentum would keep changing the width after the fingers lift.
        guard e.momentumPhase.isEmpty, let s = sel else { return }
        scrollAcc += e.scrollingDeltaY
        guard abs(scrollAcc) >= (e.hasPreciseScrollingDeltas ? 12 : 1) else { return }
        let d: CGFloat = scrollAcc > 0 ? 1 : -1
        scrollAcc = 0
        let p = convert(e.locationInWindow, from: nil)
        // Text tool, pointer on an existing text: change the size of that text only.
        if field == nil, tool == .text, let i = textIndex(at: p) {
            annotations[i].width = max(1, min(annotations[i].width + d, 20))
            return
        }
        width = max(1, min(width + d, 20))
        if let f = field {
            f.font = font(width)
            (f.currentEditor() as? NSTextView)?.font = font(width)
            let h = f.fittingSize.height
            f.frame = NSRect(x: f.frame.minX, y: f.frame.midY - h / 2, width: f.frame.width, height: h)
        } else {
            preview = p
            NSObject.cancelPreviousPerformRequests(withTarget: self, selector: #selector(hidePreview), object: nil)
            perform(#selector(hidePreview), with: nil, afterDelay: 0.8)
        }
        setNeedsDisplay(chrome(s))  // the size label shows the width
    }

    @objc private func hidePreview() { preview = nil }

    func clearSelection() {
        commitText()
        annotations = []
        sel = nil
        bar.isHidden = true
    }

    private func handlePoints(_ s: NSRect) -> [(p: NSPoint, hx: Int, hy: Int)] {
        var out: [(NSPoint, Int, Int)] = []
        for hx in -1...1 {
            for hy in -1...1 where hx != 0 || hy != 0 {
                out.append((NSPoint(x: hx < 0 ? s.minX : hx > 0 ? s.maxX : s.midX,
                                    y: hy < 0 ? s.minY : hy > 0 ? s.maxY : s.midY), hx, hy))
            }
        }
        return out
    }

    private func handleHit(at p: NSPoint, in s: NSRect) -> (p: NSPoint, hx: Int, hy: Int)? {
        handlePoints(s).first { abs($0.p.x - p.x) <= 9 && abs($0.p.y - p.y) <= 9 }
    }

    private func handle(at p: NSPoint, in s: NSRect) -> (anchor: NSPoint, hx: Bool, hy: Bool)? {
        guard let h = handleHit(at: p, in: s) else { return nil }
        return (NSPoint(x: h.hx < 0 ? s.maxX : s.minX, y: h.hy < 0 ? s.maxY : s.minY), h.hx != 0, h.hy != 0)
    }

    private func constrain(_ a: NSPoint, _ p: NSPoint, square: Bool) -> NSPoint {
        let dx = p.x - a.x, dy = p.y - a.y
        if square {
            let side = max(abs(dx), abs(dy))
            return NSPoint(x: a.x + (dx < 0 ? -side : side), y: a.y + (dy < 0 ? -side : side))
        }
        let ang = (atan2(dy, dx) / (.pi / 4)).rounded() * (.pi / 4), len = hypot(dx, dy)
        return NSPoint(x: a.x + len * cos(ang), y: a.y + len * sin(ang))
    }

    // MARK: Text

    private func font(_ width: CGFloat) -> NSFont { .boldSystemFont(ofSize: 10 + width * 2) }

    // ponytail: single-line text, Return commits. Use an NSTextView if multi-line text becomes necessary.
    private func beginText(at p: NSPoint, editing a: Annotation? = nil) {
        guard let s = sel else { return }
        if let a {
            widthBeforeEdit = width
            width = a.width
        }
        edited = a
        let f = NSTextField(string: a?.text ?? "")
        f.isBordered = false
        f.drawsBackground = false
        f.focusRingType = .none
        f.font = font(width)
        f.textColor = a?.color ?? color
        f.delegate = self
        let h = f.fittingSize.height
        // 2 pt = the horizontal text inset of NSTextField.
        let o = a.map { NSPoint(x: $0.points[0].x - 2, y: $0.points[0].y) } ?? NSPoint(x: p.x, y: p.y - h / 2)
        f.frame = NSRect(x: o.x, y: o.y, width: max(40, s.maxX - o.x), height: h)
        addSubview(f)
        window?.makeFirstResponder(f)
        field = f
        hoverText = nil
        setNeedsDisplay(f.frame.insetBy(dx: -4, dy: -4))
    }

    /// The field's frame, as wide as its text.
    private func fieldBox(_ f: NSTextField) -> NSRect {
        var r = f.frame
        r.size.width = max(40, (f.stringValue as NSString).size(withAttributes: [.font: f.font ?? font(width)]).width + 4)
        return r
    }

    private func textRect(_ a: Annotation) -> NSRect {
        NSRect(origin: a.points[0], size: (a.text as NSString).size(withAttributes: [.font: font(a.width)]))
    }

    private func textIndex(at p: NSPoint) -> Int? {
        annotations.lastIndex { $0.tool == .text && textRect($0).contains(p) }
    }

    private func commitText() {
        guard let f = field else { return }
        field = nil
        setNeedsDisplay(f.frame.insetBy(dx: -4, dy: -4))
        if !f.stringValue.isEmpty {
            annotations.append(Annotation(tool: .text, color: f.textColor ?? color, width: width,
                                          points: [NSPoint(x: f.frame.minX + 2, y: f.frame.minY)], text: f.stringValue))
        } else if let a = edited {
            annotations.append(a)  // a re-edit that ends empty keeps the original
        }
        if edited != nil { width = widthBeforeEdit }
        edited = nil
        f.removeFromSuperview()
        window?.makeFirstResponder(self)
    }

    func controlTextDidChange(_ obj: Notification) {
        if let f = field { setNeedsDisplay(f.frame.insetBy(dx: -4, dy: -4)) }  // the dotted box follows the text
    }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        if selector == #selector(insertNewline(_:)) { commitText(); return true }
        // Esc: drop a new text, keep the original of a re-edited one.
        if selector == #selector(cancelOperation(_:)) { field?.stringValue = ""; commitText(); return true }
        return false
    }

    // MARK: Drawing

    /// The area drawn for a selection: border, handles and size label.
    private func chrome(_ s: NSRect) -> NSRect {
        NSRect(x: s.minX - 40, y: s.minY - 40, width: max(s.width, 200) + 80, height: s.height + 80)
    }

    private func bbox(_ pts: [NSPoint]) -> NSRect {
        let xs = pts.map(\.x), ys = pts.map(\.y)
        return NSRect(x: xs.min()!, y: ys.min()!, width: xs.max()! - xs.min()!, height: ys.max()! - ys.min()!)
    }

    private func box(_ a: Annotation) -> NSRect { bbox([a.points[0], a.points[a.points.count - 1]]) }

    override func draw(_ dirty: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.interpolationQuality = .none
        ctx.draw(image, in: bounds)
        let dim = NSBezierPath(rect: bounds)
        if let s = sel { dim.appendRect(s) }
        dim.windingRule = .evenOdd
        NSColor(white: 0, alpha: 0.5).setFill()
        dim.fill()
        guard let s = sel else { return }
        drawAnnotations(clip: s)

        if let p = preview { drawPreview(at: p) }

        NSColor.white.setStroke()
        NSBezierPath(rect: s.insetBy(dx: -0.5, dy: -0.5)).stroke()
        for h in handlePoints(s) {
            let r = NSRect(x: h.p.x - 3, y: h.p.y - 3, width: 6, height: 6)
            NSColor.white.setFill()
            r.fill()
            NSColor.black.setStroke()
            NSBezierPath(rect: r).stroke()
        }
        if let f = field {
            dottedBox(fieldBox(f))
        } else if let i = hoverText, i < annotations.count {
            dottedBox(textRect(annotations[i]).insetBy(dx: -2, dy: -2))
        }

        let label = " \(Int(s.width * scale)) × \(Int(s.height * scale))   \(Int(width)) pt " as NSString
        let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.systemFont(ofSize: 11), .foregroundColor: NSColor.white,
                                                    .backgroundColor: NSColor(white: 0, alpha: 0.7)]
        let h = label.size(withAttributes: attrs).height
        label.draw(at: NSPoint(x: s.minX, y: s.maxY + h + 4 > bounds.maxY ? s.maxY - h - 4 : s.maxY + 4), withAttributes: attrs)
    }

    /// Editor chrome around a text: black and white dashes, visible on any background.
    private func dottedBox(_ r: NSRect) {
        let path = NSBezierPath(rect: r.integral.insetBy(dx: 0.5, dy: 0.5))
        path.lineWidth = 1
        for (c, phase) in [(NSColor.black, CGFloat(0)), (NSColor.white, 3)] {
            path.setLineDash([3, 3], count: 2, phase: phase)
            c.setStroke()
            path.stroke()
        }
    }

    /// The size of the next line or text, at the pointer. Not part of the output image.
    private func drawPreview(at p: NSPoint) {
        if tool == .text {
            let attrs: [NSAttributedString.Key: Any] = [.font: font(width), .foregroundColor: color]
            let size = ("Aa" as NSString).size(withAttributes: attrs)
            ("Aa" as NSString).draw(at: NSPoint(x: p.x - size.width / 2, y: p.y - size.height / 2), withAttributes: attrs)
            return
        }
        let d = tool == .marker ? width * 5 : width
        let dot = NSBezierPath(ovalIn: NSRect(x: p.x - d / 2, y: p.y - d / 2, width: d, height: d))
        (tool == .marker ? color.withAlphaComponent(0.4) : color).setFill()
        dot.fill()
        NSColor.white.setStroke()
        NSBezierPath(ovalIn: dot.bounds.insetBy(dx: -1.5, dy: -1.5)).stroke()
    }

    private func drawAnnotations(clip s: NSRect) {
        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: s).addClip()
        for a in annotations { draw(a) }
        if let a = current { draw(a) }
        NSGraphicsContext.restoreGraphicsState()
    }

    private func draw(_ a: Annotation) {
        let a0 = a.points[0], a1 = a.points[a.points.count - 1], box = box(a)
        (a.tool == .marker ? a.color.withAlphaComponent(0.4) : a.color).set()
        let path = NSBezierPath()
        path.lineWidth = a.width
        path.lineCapStyle = .round
        path.lineJoinStyle = .round
        switch a.tool {
        case .pen, .marker:
            if a.tool == .marker { path.lineWidth = a.width * 5 }
            path.move(to: a0)
            a.points.dropFirst().forEach { path.line(to: $0) }
            path.stroke()
        case .line:
            path.move(to: a0)
            path.line(to: a1)
            path.stroke()
        case .arrow:
            let ang = atan2(a1.y - a0.y, a1.x - a0.x), head = min(a.width * 4 + 6, hypot(a1.x - a0.x, a1.y - a0.y))
            let base = NSPoint(x: a1.x - head * cos(ang), y: a1.y - head * sin(ang))
            let px = -sin(ang) * head / 2, py = cos(ang) * head / 2
            path.move(to: a0)
            path.line(to: base)
            path.stroke()
            let tip = NSBezierPath()
            tip.move(to: a1)
            tip.line(to: NSPoint(x: base.x + px, y: base.y + py))
            tip.line(to: NSPoint(x: base.x - px, y: base.y - py))
            tip.close()
            tip.fill()
        case .rect:
            path.lineJoinStyle = .miter
            path.appendRect(box)
            path.stroke()
        case .text:
            (a.text as NSString).draw(at: a0, withAttributes: [.font: font(a.width), .foregroundColor: a.color])
        case .pixelate:
            guard let b = a.blocks ?? blocks(box), let ctx = NSGraphicsContext.current?.cgContext else { break }
            ctx.saveGState()
            ctx.interpolationQuality = .none
            ctx.draw(b, in: box.intersection(bounds))
            ctx.restoreGState()
        }
    }

    /// The pixels of the source image under `r`. CGImage rows start at the top.
    private func pixels(_ r: NSRect) -> CGRect {
        let k = scale
        return CGRect(x: r.minX * k, y: (bounds.height - r.maxY) * k, width: r.width * k, height: r.height * k)
    }

    /// The source image in `r` as 10 pt blocks, one pixel per block. Reads the source image only, not other annotations.
    private func blocks(_ r: NSRect) -> CGImage? {
        let r = r.intersection(bounds), block: CGFloat = 10
        guard r.width >= 1, r.height >= 1 else { return nil }
        let w = max(1, Int(r.width / block)), h = max(1, Int(r.height / block))
        guard let src = image.cropping(to: pixels(r)),
              let small = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                                    space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
        else { return nil }
        small.interpolationQuality = .high
        small.draw(src, in: CGRect(x: 0, y: 0, width: w, height: h))
        return small.makeImage()
    }

    /// The selection with its annotations, at the pixel resolution of the screen.
    func render() -> CGImage? {
        guard let s = sel else { return nil }
        let k = scale
        guard let ctx = CGContext(data: nil, width: Int(s.width * k), height: Int(s.height * k), bitsPerComponent: 8, bytesPerRow: 0,
                                  space: image.colorSpace ?? CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
        else { return nil }
        ctx.scaleBy(x: k, y: k)
        ctx.translateBy(x: -s.minX, y: -s.minY)
        ctx.interpolationQuality = .none
        if let src = image.cropping(to: pixels(s)) { ctx.draw(src, in: s) }
        let old = NSGraphicsContext.current
        NSGraphicsContext.current = NSGraphicsContext(cgContext: ctx, flipped: false)
        drawAnnotations(clip: s)
        NSGraphicsContext.current = old
        return ctx.makeImage()
    }
}
