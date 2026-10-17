import AppKit
import SwiftUI

/// The Settings window, as macOS apps have it: a toolbar of tabs (General, Languages, Models, Runtime & Hardware), each tab's
/// content below, and the window resizing to the tab chosen.
@MainActor
final class SettingsWindow {
    enum Tab: Int {
        case general, languages, models, system
    }

    private var window: NSWindow?
    private let tabs = NSTabViewController()

    init(settings: Settings, store: ModelStore, server: ServerProcess, system: SystemModel,
         onUse: @escaping (ModelEntry) -> Void, onShortcut: @escaping () -> Void) {
        tabs.tabStyle = .toolbar
        tabs.transitionOptions = [.allowUserInteraction]
        add(GeneralView(settings: settings, store: store, server: server, onShortcut: onShortcut),
            label: "General", symbol: "gearshape")
        add(LanguagesView(settings: settings, store: store), label: "Languages", symbol: "globe")
        add(ModelsView(store: store, settings: settings, onUse: onUse), label: "Models", symbol: "square.stack.3d.up")
        add(SystemView(model: system, settings: settings, server: server), label: "Runtime & Hardware", symbol: "cpu")
    }

    private func add(_ view: some View, label: String, symbol: String) {
        // The controller gives the tab the view's size, which the window takes when the tab is chosen.
        let controller = NSHostingController(rootView: view)
        controller.sizingOptions = [.preferredContentSize]
        controller.title = label
        let item = NSTabViewItem(viewController: controller)
        item.label = label
        item.image = NSImage(systemSymbolName: symbol, accessibilityDescription: label)
        tabs.addTabViewItem(item)
    }

    func show(_ tab: Tab) {
        if window == nil {
            let window = NSWindow(contentViewController: tabs)
            window.styleMask = [.titled, .closable]
            window.toolbarStyle = .preference
            window.isReleasedWhenClosed = false
            window.level = .floating
            self.window = window
        }
        tabs.selectedTabViewItemIndex = tab.rawValue
        window?.layoutIfNeeded()
        if window?.isVisible == false { window?.center() }
        NSApp.activate()
        window?.makeKeyAndOrderFront(nil)
    }
}

/// Settings › General: the shortcut, pasting, and asrd.
private struct GeneralView: View {
    @ObservedObject var settings: Settings
    @ObservedObject var store: ModelStore
    @ObservedObject var server: ServerProcess
    let onShortcut: () -> Void
    @State private var trusted = TextInserter.isTrusted

    var body: some View {
        Form {
            Section("Dictation") {
                LabeledContent("Shortcut") {
                    HStack {
                        Text(settings.hotKey.label).monospaced()
                        Button("Change…", action: onShortcut)
                    }
                }
            }
            Section("Pasting") {
                Toggle("Restore the clipboard after pasting", isOn: $settings.restoreClipboard)
                LabeledContent("Paste into the previous app") {
                    if trusted {
                        Label("Allowed", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                    } else {
                        Button("Allow (Accessibility)…") { TextInserter.requestTrust(always: true) }
                    }
                }
            }
            Section("Server") {
                LabeledContent("asrd", value: status)
                LabeledContent("Log") {
                    Button("Show in Finder") {
                        let log = FileManager.default.homeDirectoryForCurrentUser
                            .appendingPathComponent("Library/Logs/LocalASR/asrd.log")
                        NSWorkspace.shared.activateFileViewerSelecting([log])
                    }
                }
            }
        }
        .formStyle(.grouped)
        .frame(width: 560)
        .fixedSize(horizontal: false, vertical: true)
        .onReceive(Timer.publish(every: 2, on: .main, in: .common).autoconnect()) { _ in
            trusted = TextInserter.isTrusted
        }
    }

    private var status: String {
        switch server.status {
        case .starting: return "Starting…"
        case .loading(let id, _): return "Loading \(store.entry(id: id)?.name ?? id)…"
        case .noModel(let id, _): return "\(store.entry(id: id)?.name ?? "The model") is not downloaded"
        case .ready(let model, _, let external): return external ? "\(model) ready (started outside the app)" : "\(model) ready"
        case .loadFailed(_, let message, _): return message
        case .failed(let message): return message
        }
    }
}
