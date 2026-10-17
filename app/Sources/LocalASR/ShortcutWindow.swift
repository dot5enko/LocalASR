import AppKit
import SwiftUI

/// Records the dictation shortcut: while the window is open, the next key press with ⌘, ⌥ or ⌃ becomes it. The
/// current shortcut is let go meanwhile, so pressing it again can choose it again instead of starting a dictation.
@MainActor
final class ShortcutWindow: NSObject, NSWindowDelegate {
    private var window: NSWindow?
    private var monitor: Any?
    private let model = ShortcutModel()
    private let settings: Settings
    /// Registers a shortcut, or answers false when another app holds it.
    private let apply: (HotKey) -> Bool
    private let onClose: () -> Void

    init(settings: Settings, apply: @escaping (HotKey) -> Bool, onClose: @escaping () -> Void) {
        self.settings = settings
        self.apply = apply
        self.onClose = onClose
        super.init()
        model.onRestoreDefault = { [weak self] in self?.choose(.default) }
    }

    func show() {
        if window == nil {
            let host = NSHostingView(rootView: ShortcutView(model: model))
            let window = NSWindow(contentRect: NSRect(origin: .zero, size: host.fittingSize),
                                  styleMask: [.titled, .closable], backing: .buffered, defer: false)
            window.title = "Dictation Shortcut"
            window.contentView = host
            window.isReleasedWhenClosed = false
            window.level = .floating
            window.delegate = self
            window.center()
            self.window = window
        }
        model.current = settings.hotKey.label
        model.message = nil
        if monitor == nil {
            monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] event in
                guard let self, event.window === self.window else { return event }
                self.record(event)
                return nil
            }
        }
        NSApp.activate()
        window?.makeKeyAndOrderFront(nil)
    }

    private func record(_ event: NSEvent) {
        if event.keyCode == UInt16(HotKey.escape.keyCode), event.modifierFlags.intersection([.command, .option, .control]).isEmpty {
            window?.close()
            return
        }
        guard let hotKey = HotKey(event: event) else {
            model.message = "Hold ⌘, ⌥ or ⌃ with the key, or use a function key."
            NSSound.beep()
            return
        }
        choose(hotKey)
    }

    private func choose(_ hotKey: HotKey) {
        if apply(hotKey) {
            model.current = hotKey.label
            model.message = "Saved."
        } else {
            model.message = "\(hotKey.label) is taken by another app or by macOS; try another."
            NSSound.beep()
        }
    }

    func windowWillClose(_ notification: Notification) {
        if let monitor { NSEvent.removeMonitor(monitor) }
        monitor = nil
        onClose()
    }
}

@MainActor
private final class ShortcutModel: ObservableObject {
    @Published var current = ""
    @Published var message: String?
    var onRestoreDefault: () -> Void = {}
}

private struct ShortcutView: View {
    @ObservedObject var model: ShortcutModel

    var body: some View {
        VStack(spacing: 14) {
            Text("Press the shortcut that starts and inserts a dictation.")
                .foregroundStyle(.secondary)
            Text(model.current)
                .font(.system(size: 28, weight: .medium, design: .rounded))
                .frame(minWidth: 200, minHeight: 56)
                .padding(.horizontal, 16)
                .background(RoundedRectangle(cornerRadius: 10).fill(.quaternary))
                .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Color.accentColor, lineWidth: 2))
            Text(model.message ?? "Hold ⌘, ⌥ or ⌃ with a key. Esc closes.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            Button("Restore Default (\(HotKey.default.label))") { model.onRestoreDefault() }
        }
        .padding(24)
        .frame(width: 380)
    }
}
