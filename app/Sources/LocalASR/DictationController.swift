import AppKit

/// One dictation at a time: shortcut → overlay and microphone → live text → ✓ (or the shortcut again) → paste into
/// the app that was in front.
@MainActor
final class DictationController {
    private let settings: Settings
    private let model = OverlayModel()
    private lazy var panel = OverlayPanel(model: model)
    private var client: ASRClient?
    private var capture: AudioCapture?
    private var target: NSRunningApplication?
    private var escapeToken: UInt32?
    private var finishDeadline: DispatchWorkItem?
    private var hideNotice: DispatchWorkItem?

    private(set) var isActive = false

    init(settings: Settings) {
        self.settings = settings
        model.onCommit = { [weak self] in self?.commit() }
        model.onCancel = { [weak self] in self?.cancel() }
        model.onReset = { [weak self] in self?.reset() }
        model.onHeightChange = { [weak self] height in self?.panel.fit(height: height) }
    }

    func toggle() {
        isActive ? commit() : start()
    }

    func start() {
        guard !isActive else { return }
        isActive = true
        hideNotice?.cancel()
        let front = NSWorkspace.shared.frontmostApplication
        target = front?.processIdentifier == ProcessInfo.processInfo.processIdentifier ? nil : front
        model.reset()
        model.hotKeyLabel = settings.hotKey.label
        model.languagesLabel = settings.languagesLabel
        panel.present(near: CaretLocator.caretRect())
        escapeToken = HotKeyCenter.shared.register(.escape) { [weak self] in self?.cancel() }

        Task {
            guard await AudioCapture.requestPermission() else {
                fail("Microphone access is off. Turn it on in System Settings › Privacy & Security › Microphone.")
                return
            }
            if isActive, client == nil { begin() }
        }
    }

    /// Ends the audio and inserts the final transcript once it arrives.
    func commit() {
        guard isActive else {
            panel.orderOut(nil)
            return
        }
        switch model.phase {
        case .failed, .notice: cancel(); return
        case .finishing: return
        default: break
        }
        guard let client else { cancel(); return }
        model.phase = .finishing
        capture?.stop()
        capture = nil
        client.stop()
        // A final transcript that is late gives way to the text on screen.
        let deadline = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.finish(self.model.committed + self.model.live)
        }
        finishDeadline = deadline
        DispatchQueue.main.asyncAfter(deadline: .now() + 4, execute: deadline)
    }

    /// Clears the text and keeps listening.
    func reset() {
        guard isActive, model.phase == .listening || model.phase == .connecting else { return }
        client?.reset()
        model.committed = ""
        model.live = ""
    }

    func cancel() {
        if isActive { teardown() }
        hideNotice?.cancel()
        panel.orderOut(nil)
    }

    private func begin() {
        let client = ASRClient()
        client.onEvent = { [weak self] event in self?.handle(event) }
        client.connect(url: settings.dictateURL)
        self.client = client

        let capture = AudioCapture()
        let model = self.model
        capture.onChunk = { [weak client] data in
            DispatchQueue.main.async { MainActor.assumeIsolated { client?.send(audio: data) } }
        }
        capture.onLevel = { level in
            DispatchQueue.main.async { MainActor.assumeIsolated { model.push(level: level) } }
        }
        do {
            try capture.start()
        } catch {
            fail("Cannot open the microphone: \(error.localizedDescription)")
            return
        }
        self.capture = capture
        model.phase = .listening
    }

    private func handle(_ event: ASRClient.Event) {
        guard isActive else { return }
        switch event {
        case .partial(let committed, let live):
            model.committed = committed
            model.live = live
        case .final(let text):
            finish(text)
        case .failed(let message):
            fail(message)
        }
    }

    private func finish(_ text: String) {
        guard isActive, model.phase == .finishing else { return }
        let target = self.target
        teardown()
        let text = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else {
            panel.orderOut(nil)
            return
        }
        switch TextInserter.insert(text, into: target, restoreClipboard: settings.restoreClipboard) {
        case .pasted:
            panel.orderOut(nil)
        case .copiedOnly:
            TextInserter.requestTrust()
            model.phase = .notice("Copied — press ⌘V. Allow LocalASR in Privacy & Security › Accessibility to paste automatically.")
            let hide = DispatchWorkItem { [weak self] in self?.panel.orderOut(nil) }
            hideNotice = hide
            DispatchQueue.main.asyncAfter(deadline: .now() + 4, execute: hide)
        }
    }

    /// Keeps the overlay up with the message until the user dismisses it.
    private func fail(_ message: String) {
        guard isActive else { return }
        capture?.stop()
        capture = nil
        client?.cancel()
        client = nil
        finishDeadline?.cancel()
        model.phase = .failed(message)
    }

    private func teardown() {
        finishDeadline?.cancel()
        finishDeadline = nil
        capture?.stop()
        capture = nil
        client?.cancel()
        client = nil
        HotKeyCenter.shared.unregister(escapeToken)
        escapeToken = nil
        target = nil
        isActive = false
    }
}
