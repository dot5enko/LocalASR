import AppKit
import Combine

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let store = ModelStore()
    private lazy var settings = Settings(store: store)
    private let server = ServerProcess()
    private lazy var dictation = DictationController(settings: settings)
    private lazy var systemModel = SystemModel(settings: settings, server: server, store: store) { [weak self] in
        await self?.reloadModel()
    }
    private lazy var settingsWindow = SettingsWindow(
        settings: settings, store: store, server: server, system: systemModel,
        onUse: { [weak self] entry in self?.use(entry) },
        onShortcut: { [weak self] in self?.showShortcut() })
    private lazy var shortcutWindow = ShortcutWindow(
        settings: settings,
        apply: { [weak self] hotKey in self?.apply(hotKey) ?? false },
        onClose: { [weak self] in self?.registerHotKey() })
    private var statusItem: NSStatusItem!
    private var hotKeyToken: UInt32?
    private var terminationSignal: DispatchSourceSignal?
    private var statusWatch: AnyCancellable?
    private var runtimeWatch: AnyCancellable?
    private var catalogRead = false

    func applicationDidFinishLaunching(_ notification: Notification) {
        // `pkill LocalASR` quits like the menu does, so the asrd the app started stops with it.
        signal(SIGTERM, SIG_IGN)
        terminationSignal = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        terminationSignal?.setEventHandler { NSApp.terminate(nil) }
        terminationSignal?.resume()

        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        statusItem.button?.image = NSImage(systemSymbolName: "waveform", accessibilityDescription: "LocalASR")
        let menu = NSMenu()
        menu.delegate = self
        statusItem.menu = menu
        registerHotKey()
        store.onDownloaded = { [weak self] entry in self?.downloaded(entry) }
        server.start(model: settings.modelID, settings: settings.loadSettings(modelID: settings.modelID))
        statusWatch = server.$status.sink { [weak self] status in
            guard status.reachable else { return }
            Task { @MainActor in await self?.readCatalogOnce(status) }
        }
        // The runtime that runs the model is the one whose load settings asrd starts with next time.
        runtimeWatch = server.$health.sink { [weak self] health in
            guard let runtime = health?.runtime, self?.settings.runtimeID != runtime else { return }
            self?.settings.runtimeID = runtime
        }
    }

    /// The first time asrd answers: the catalog it declares tells which models there are and which are downloaded.
    private func readCatalogOnce(_ status: ServerProcess.Status) async {
        guard !catalogRead else { return }
        catalogRead = true
        await store.reload()
        guard !store.models.isEmpty else {
            catalogRead = false
            return
        }
        settings.migrate()
        if settings.modelID == nil {
            switch status {
            case .loading(let id, _), .noModel(let id, _), .ready(_, let id, _), .loadFailed(let id, _, _):
                settings.modelID = id.isEmpty ? nil : id
            default: break
            }
        } else if case .ready(_, let id, _) = status, id != settings.modelID {
            // asrd runs another model than the one chosen, such as one started from a terminal: switch it.
            await reloadModel()
        }
        if let vad = store.vad, !store.isReady(vad) { store.download(vad) }
        if settings.model.map({ !store.isReady($0) }) ?? true { settingsWindow.show(.models) }
    }

    private func use(_ entry: ModelEntry) {
        guard entry.id != settings.modelID else { return }
        settings.modelID = entry.id
        Task { await reloadModel() }
    }

    /// Loads the chosen model in asrd with its settings; asrd switches without a restart, and a model not downloaded
    /// yet loads once it is. Nil when asrd took the request, else why not.
    @discardableResult
    private func reloadModel() async -> String? {
        guard let model = settings.modelID else { return "Choose a model first." }
        let failure = await server.load(model: model, settings: settings.loadSettings(modelID: model))
        store.message = failure
        return failure
    }

    /// asrd loads a model that waited for its files by itself; the library only needs reading again.
    private func downloaded(_ entry: ModelEntry) {
        Task { await store.reload() }
    }

    func applicationWillTerminate(_ notification: Notification) {
        server.stop()
    }

    private func registerHotKey() {
        HotKeyCenter.shared.unregister(hotKeyToken)
        hotKeyToken = HotKeyCenter.shared.register(settings.hotKey) { [weak self] in self?.dictation.toggle() }
        if hotKeyToken == nil {
            let alert = NSAlert()
            alert.messageText = "\(settings.hotKey.label) is taken by another app"
            alert.informativeText = "Choose another shortcut."
            alert.addButton(withTitle: "Choose Shortcut…")
            alert.addButton(withTitle: "Later")
            NSApp.activate()
            if alert.runModal() == .alertFirstButtonReturn { showShortcut() }
        }
    }

    /// Takes `hotKey` when it can be registered; the recorder's window keeps the shortcut unregistered until it closes.
    private func apply(_ hotKey: HotKey) -> Bool {
        guard let token = HotKeyCenter.shared.register(hotKey, handler: {}) else { return false }
        HotKeyCenter.shared.unregister(token)
        settings.hotKey = hotKey
        return true
    }

    // MARK: Menu

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        let dictate = item("Dictate", #selector(dictate))
        (dictate.keyEquivalent, dictate.keyEquivalentModifierMask) = settings.hotKey.menuKeyEquivalent
        menu.addItem(dictate)
        menu.addItem(.separator())
        menu.addItem(item(settings.model.map { "Model: \($0.name)…" } ?? "Models…", #selector(showModels)))
        let preferences = item("Settings…", #selector(showSettings))
        preferences.keyEquivalent = ","
        menu.addItem(preferences)
        menu.addItem(.separator())
        menu.addItem(item("Quit LocalASR", #selector(quit)))
    }

    private func item(_ title: String, _ action: Selector) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: action, keyEquivalent: "")
        item.target = self
        return item
    }

    @objc private func dictate() {
        // Choosing the menu item leaves the previous app in front, so the paste still lands there.
        dictation.toggle()
    }

    @objc private func showModels() {
        settingsWindow.show(.models)
    }

    @objc private func showSettings() {
        settingsWindow.show(.general)
    }

    @objc private func showShortcut() {
        HotKeyCenter.shared.unregister(hotKeyToken)
        hotKeyToken = nil
        shortcutWindow.show()
    }

    @objc private func quit() {
        NSApp.terminate(nil)
    }
}
