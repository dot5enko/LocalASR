import AppKit
import SwiftUI

/// A floating panel that never takes keyboard focus: the input the user was typing in stays focused while they
/// dictate, so the paste lands there. Clicks still reach its buttons.
final class OverlayPanel: NSPanel {
    private enum Anchor { case top(CGFloat), bottom(CGFloat) }

    private var anchor: Anchor = .top(0)

    init(model: OverlayModel) {
        super.init(contentRect: NSRect(x: 0, y: 0, width: OverlayView.width, height: 90),
                   styleMask: [.nonactivatingPanel, .borderless], backing: .buffered, defer: true)
        isFloatingPanel = true
        level = .statusBar
        backgroundColor = .clear
        isOpaque = false
        hasShadow = true
        hidesOnDeactivate = false
        isMovableByWindowBackground = true
        becomesKeyOnlyIfNeeded = true
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .ignoresCycle]
        let host = FirstMouseHostingView(rootView: OverlayView(model: model))
        host.sizingOptions = []
        contentView = host
    }

    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }

    /// Shows the panel under the text cursor when the focused app reveals it, else high on the mouse's screen.
    func present(near caret: NSRect?) {
        let mouse = NSEvent.mouseLocation
        let screen = NSScreen.screens.first { $0.frame.contains(caret?.origin ?? mouse) } ?? NSScreen.main
        guard let visible = screen?.visibleFrame else { return }
        let width = OverlayView.width
        let height = frame.height
        var x: CGFloat
        if let caret {
            x = caret.minX - 20
            if caret.minY - 8 - height >= visible.minY {
                anchor = .top(caret.minY - 8)
            } else {
                anchor = .bottom(caret.maxY + 8)
            }
        } else {
            x = visible.midX - width / 2
            anchor = .top(visible.maxY - visible.height * 0.2)
        }
        x = min(max(x, visible.minX + 8), visible.maxX - width - 8)
        setFrame(frame(width: width, height: height, x: x), display: false)
        orderFrontRegardless()
    }

    func fit(height: CGFloat) {
        guard height > 0, abs(height - frame.height) > 0.5 else { return }
        setFrame(frame(width: frame.width, height: height, x: frame.minX), display: true)
    }

    private func frame(width: CGFloat, height: CGFloat, x: CGFloat) -> NSRect {
        switch anchor {
        case .top(let top): return NSRect(x: x, y: top - height, width: width, height: height)
        case .bottom(let bottom): return NSRect(x: x, y: bottom, width: width, height: height)
        }
    }
}

private final class FirstMouseHostingView<Content: View>: NSHostingView<Content> {
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}
