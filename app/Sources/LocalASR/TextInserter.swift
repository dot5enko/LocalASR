import AppKit
import ApplicationServices
import Carbon.HIToolbox

/// Puts text into whichever input has focus: through the pasteboard and a synthesized ⌘V, which works the same in
/// native apps, browsers, Electron apps and terminals. Posting the keystroke needs the Accessibility permission;
/// without it the text is left on the pasteboard for the user to paste.
enum TextInserter {
    enum Outcome { case pasted, copiedOnly }

    static var isTrusted: Bool { AXIsProcessTrusted() }

    @MainActor private static var prompted = false

    /// Shows the system prompt that sends the user to Privacy & Security › Accessibility; `always` from the menu,
    /// otherwise once per launch.
    @MainActor
    static func requestTrust(always: Bool = false) {
        guard always || !prompted else { return }
        prompted = true
        let options = [kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String: true] as CFDictionary
        _ = AXIsProcessTrustedWithOptions(options)
    }

    @MainActor
    static func insert(_ text: String, into target: NSRunningApplication?, restoreClipboard: Bool) -> Outcome {
        let pasteboard = NSPasteboard.general
        let saved = restoreClipboard ? snapshot(pasteboard) : nil
        pasteboard.clearContents()
        pasteboard.setString(text, forType: .string)
        // Clipboard managers skip items marked transient (nspasteboard.org).
        pasteboard.setData(Data(), forType: NSPasteboard.PasteboardType("org.nspasteboard.TransientType"))
        let ours = pasteboard.changeCount

        guard isTrusted else { return .copiedOnly }

        var delay = 0.03
        if let target, target != NSWorkspace.shared.frontmostApplication {
            target.activate()
            delay = 0.15
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
            postCommandV()
            guard let saved else { return }
            // Some apps read the pasteboard a moment after the keystroke; restore only after that, and only if
            // nothing else has written to it since.
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) {
                if pasteboard.changeCount == ours { restore(saved, to: pasteboard) }
            }
        }
        return .pasted
    }

    private static func postCommandV() {
        let source = CGEventSource(stateID: .combinedSessionState)
        let key = CGKeyCode(kVK_ANSI_V)
        let down = CGEvent(keyboardEventSource: source, virtualKey: key, keyDown: true)
        let up = CGEvent(keyboardEventSource: source, virtualKey: key, keyDown: false)
        // Exactly ⌘, whatever modifiers the user still holds from the shortcut.
        down?.flags = .maskCommand
        up?.flags = .maskCommand
        down?.post(tap: .cghidEventTap)
        up?.post(tap: .cghidEventTap)
    }

    private static func snapshot(_ pasteboard: NSPasteboard) -> [NSPasteboardItem] {
        (pasteboard.pasteboardItems ?? []).map { item in
            let copy = NSPasteboardItem()
            for type in item.types {
                if let data = item.data(forType: type) { copy.setData(data, forType: type) }
            }
            return copy
        }
    }

    private static func restore(_ items: [NSPasteboardItem], to pasteboard: NSPasteboard) {
        pasteboard.clearContents()
        if !items.isEmpty { pasteboard.writeObjects(items) }
    }
}

/// Where the text cursor of the focused input is on screen, in AppKit coordinates, when the app exposes it through
/// Accessibility (native text views and most browsers do; terminals and some Electron apps do not).
enum CaretLocator {
    static func caretRect() -> NSRect? {
        guard AXIsProcessTrusted() else { return nil }
        let system = AXUIElementCreateSystemWide()
        // A hung app would otherwise block the main thread for the default six seconds.
        AXUIElementSetMessagingTimeout(system, 0.15)
        var focused: CFTypeRef?
        guard AXUIElementCopyAttributeValue(system, kAXFocusedUIElementAttribute as CFString, &focused) == .success,
              let element = focused, CFGetTypeID(element) == AXUIElementGetTypeID() else { return nil }
        let field = element as! AXUIElement
        var range: CFTypeRef?
        guard AXUIElementCopyAttributeValue(field, kAXSelectedTextRangeAttribute as CFString, &range) == .success,
              let range else { return nil }
        var bounds: CFTypeRef?
        guard AXUIElementCopyParameterizedAttributeValue(field, kAXBoundsForRangeParameterizedAttribute as CFString,
                                                         range, &bounds) == .success,
              let bounds, CFGetTypeID(bounds) == AXValueGetTypeID() else { return nil }
        var rect = CGRect.zero
        guard AXValueGetValue(bounds as! AXValue, .cgRect, &rect), rect.width >= 0, rect.height > 0 else { return nil }
        // Accessibility measures from the top of the main screen, AppKit from its bottom.
        guard let mainHeight = NSScreen.screens.first?.frame.height else { return nil }
        return NSRect(x: rect.minX, y: mainHeight - rect.maxY, width: max(rect.width, 1), height: rect.height)
    }
}
