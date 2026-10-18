import SwiftUI

/// What the overlay shows; the controller writes it, the view reads it.
@MainActor
final class OverlayModel: ObservableObject {
    enum Phase: Equatable { case connecting, listening, finishing, failed(String), notice(String) }

    @Published var phase: Phase = .connecting
    @Published var committed = ""
    @Published var live = ""
    @Published var hotKeyLabel = HotKey.default.label
    @Published var languagesLabel: String?

    var onCommit: () -> Void = {}
    var onCancel: () -> Void = {}
    var onReset: () -> Void = {}
    var onHeightChange: (CGFloat) -> Void = { _ in }

    // Its own object, so a level tick redraws only the waveform, not the whole overlay.
    let meter = LevelMeter()

    var isEmpty: Bool { committed.isEmpty && live.isEmpty }

    func reset() {
        phase = .connecting
        committed = ""
        live = ""
        meter.reset()
    }
}

/// The recent microphone levels the waveform draws, newest last.
@MainActor
final class LevelMeter: ObservableObject {
    @Published private(set) var levels = [Float](repeating: 0, count: 36)

    func push(level: Float) {
        var next = levels
        next.removeFirst()
        next.append(level)
        levels = next
    }

    func reset() {
        levels = [Float](repeating: 0, count: levels.count)
    }
}

struct OverlayView: View {
    @ObservedObject var model: OverlayModel
    @State private var textHeight: CGFloat = 20

    static let width: CGFloat = 520

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                Waveform(meter: model.meter, active: model.phase == .listening)
                status
                Spacer(minLength: 8)
                RoundButton(symbol: "xmark", tint: .secondary, help: "Cancel (Esc)", action: model.onCancel)
                RoundButton(symbol: "arrow.counterclockwise", tint: .secondary, help: "Reset: clear the text, keep listening",
                            action: model.onReset)
                    .disabled(model.isEmpty || !(model.phase == .listening || model.phase == .connecting))
                RoundButton(symbol: "checkmark", tint: .accentColor, help: "Insert (\(model.hotKeyLabel))",
                            action: model.onCommit)
                    .disabled(isFailed)
            }
            transcript
        }
        .padding(14)
        .frame(width: Self.width)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).strokeBorder(.white.opacity(0.12)))
        .fixedSize(horizontal: false, vertical: true)
        .background(GeometryReader { geometry in
            Color.clear.preference(key: HeightKey.self, value: geometry.size.height)
        })
        .onPreferenceChange(HeightKey.self) { model.onHeightChange($0) }
    }

    private var isFailed: Bool {
        if case .failed = model.phase { return true }
        return false
    }

    @ViewBuilder private var status: some View {
        switch model.phase {
        case .connecting: Label("Starting…", systemImage: "hourglass").labelStyle(.titleOnly).foregroundStyle(.secondary)
        case .listening: Text(model.languagesLabel.map { "Listening · \($0)" } ?? "Listening").foregroundStyle(.secondary)
        case .finishing: HStack(spacing: 6) { ProgressView().controlSize(.small); Text("Finishing…") }.foregroundStyle(.secondary)
        case .failed: Text("Error").foregroundStyle(.red)
        case .notice: Text("Copied").foregroundStyle(.secondary)
        }
    }

    @ViewBuilder private var transcript: some View {
        if case .failed(let message) = model.phase {
            Text(message).font(.callout).foregroundStyle(.red).textSelection(.enabled)
        } else if case .notice(let message) = model.phase {
            Text(message).font(.callout).foregroundStyle(.secondary)
        } else {
            ScrollViewReader { proxy in
                ScrollView(.vertical, showsIndicators: false) {
                    Group {
                        if model.isEmpty {
                            Text("Speak! Esc for cancel.").foregroundStyle(.tertiary)
                        } else {
                            Text(model.committed).foregroundStyle(.primary) + Text(model.live).foregroundStyle(.secondary)
                        }
                    }
                    .font(.system(size: 15))
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fixedSize(horizontal: false, vertical: true)
                    .background(GeometryReader { geometry in
                        Color.clear.preference(key: TextHeightKey.self, value: geometry.size.height)
                    })
                    .id("text")
                }
                .frame(height: min(max(textHeight, 20), 220))
                .onPreferenceChange(TextHeightKey.self) { textHeight = $0 }
                .onChange(of: model.live) { proxy.scrollTo("text", anchor: .bottom) }
                .onChange(of: model.committed) { proxy.scrollTo("text", anchor: .bottom) }
            }
        }
    }
}

private struct HeightKey: PreferenceKey {
    static var defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = max(value, nextValue()) }
}

private struct TextHeightKey: PreferenceKey {
    static var defaultValue: CGFloat = 0
    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) { value = max(value, nextValue()) }
}

/// One bar per level, drawn in a single Canvas pass; each tick redraws once, with no animation running between ticks.
private struct Waveform: View {
    @ObservedObject var meter: LevelMeter
    let active: Bool

    private static let barWidth: CGFloat = 3
    private static let spacing: CGFloat = 2.5
    private static let height: CGFloat = 28

    var body: some View {
        let levels = meter.levels
        let width = CGFloat(levels.count) * Self.barWidth + CGFloat(max(levels.count - 1, 0)) * Self.spacing
        Canvas { context, size in
            let color = active ? Color.accentColor : Color.secondary.opacity(0.5)
            for (i, level) in levels.enumerated() {
                let barHeight = 3 + CGFloat(level) * 25
                let rect = CGRect(x: CGFloat(i) * (Self.barWidth + Self.spacing), y: (size.height - barHeight) / 2,
                                  width: Self.barWidth, height: barHeight)
                context.fill(Capsule().path(in: rect), with: .color(color))
            }
        }
        .frame(width: width, height: Self.height)
    }
}

private struct RoundButton: View {
    let symbol: String
    let tint: Color
    let help: String
    let action: () -> Void
    @Environment(\.isEnabled) private var isEnabled

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 13, weight: .bold))
                .frame(width: 30, height: 30)
                .background(Circle().fill(tint.opacity(0.18)))
                .foregroundStyle(tint)
        }
        .buttonStyle(.plain)
        .opacity(isEnabled ? 1 : 0.35)
        .help(help)
    }
}
