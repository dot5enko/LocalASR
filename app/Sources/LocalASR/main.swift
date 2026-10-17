import AppKit

MainActor.assumeIsolated {
    let app = NSApplication.shared
    let delegate = AppDelegate()
    app.delegate = delegate
    // A menu bar app: no Dock icon, and it never becomes the active app while dictating.
    app.setActivationPolicy(.accessory)
    app.run()
}
