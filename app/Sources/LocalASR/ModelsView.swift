import AppKit
import SwiftUI

/// Settings › Models: the models asrd's library lists, downloaded through asrd with their progress, the one dictation
/// uses chosen, and the others deleted.
struct ModelsView: View {
    @ObservedObject var store: ModelStore
    @ObservedObject var settings: Settings
    let onUse: (ModelEntry) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Speech recognition model").font(.headline)
            if store.models.isEmpty {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text(store.catalogError ?? "Reading the catalog from asrd…").foregroundStyle(.secondary)
                }
                .padding(.vertical, 12)
            }
            ForEach(store.recognition) { entry in
                ModelRow(entry: entry, store: store, inUse: entry.id == settings.modelID, onUse: { onUse(entry) })
                if entry != store.recognition.last { Divider() }
            }
            if let vad = store.vad, !store.isReady(vad) {
                Divider()
                ModelRow(entry: vad, store: store, inUse: false, onUse: {})
            }
            if let message = store.message {
                Text(message).font(.caption).foregroundStyle(.red)
            }
            Text("Models are stored in ~/Library/Application Support/LocalASR/models and run only on this Mac.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(20)
        .frame(width: 560)
        .task { await store.reload() }
    }
}

private struct ModelRow: View {
    let entry: ModelEntry
    @ObservedObject var store: ModelStore
    let inUse: Bool
    let onUse: () -> Void

    private var state: ModelStore.State { store.state(entry) }

    @ViewBuilder private var header: some View {
        let owner = entry.upstream.split(separator: "/").first.map { "\($0)/" } ?? ""
        let title = Text(owner).foregroundStyle(.secondary) + Text(entry.name).fontWeight(.semibold)
        if let source = entry.sourceURL {
            Link(destination: source) { title }.foregroundStyle(.primary)
        } else {
            title
        }
    }

    /// "Automatic Speech Recognition · 0.6B params · CC-BY-4.0 · 668 MB".
    private var tags: String {
        [entry.kind == "asr" ? "Automatic Speech Recognition" : "Voice Activity Detection",
         entry.parameters.isEmpty ? "" : "\(entry.parameters) params", entry.license, entry.sizeLabel]
            .filter { !$0.isEmpty }.joined(separator: " · ")
    }
    private var selectable: Bool { entry.kind == "asr" }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .top, spacing: 12) {
                if selectable {
                    Image(systemName: inUse ? "checkmark.circle.fill" : "circle")
                        .font(.title3)
                        .foregroundStyle(inUse ? Color.accentColor : .secondary)
                } else {
                    Image(systemName: "waveform.badge.magnifyingglass").font(.title3).foregroundStyle(.secondary)
                }
                VStack(alignment: .leading, spacing: 3) {
                    // The Hugging Face header: owner/name, then the task, size and license.
                    header
                    Text(tags).foregroundStyle(.secondary)
                    if let runtime = entry.runtimeLabel {
                        Text(runtime).font(.caption).foregroundStyle(.tertiary)
                    }
                    if case .failed(let message) = state {
                        Text(message).foregroundStyle(.red).font(.caption)
                    }
                }
                Spacer(minLength: 8)
                actions
            }
            switch state {
            case .downloading(let progress): ProgressLine(progress: progress, paused: false)
            case .paused(let progress): ProgressLine(progress: progress, paused: true)
            case .verifying: ProgressLine(progress: nil, paused: false)
            default: EmptyView()
            }
        }
        .contentShape(Rectangle())
        .onTapGesture { if selectable, state == .ready, !inUse { onUse() } }
    }

    @ViewBuilder private var actions: some View {
        switch state {
        case .ready:
            if !selectable {
                EmptyView()
            } else if inUse {
                Text("In use").foregroundStyle(.secondary)
            } else {
                HStack(spacing: 6) {
                    Button("Use", action: onUse)
                    Button(action: { store.delete(entry) }) { Image(systemName: "trash") }
                        .buttonStyle(.borderless)
                        .help("Delete the downloaded files")
                }
            }
        case .downloading:
            HStack(spacing: 4) {
                IconButton(symbol: "pause.circle.fill", help: "Pause") { store.pause(entry) }
                IconButton(symbol: "xmark.circle.fill", help: "Cancel the download") { store.cancel(entry) }
            }
        case .paused:
            HStack(spacing: 4) {
                IconButton(symbol: "play.circle.fill", help: "Resume") { store.download(entry) }
                IconButton(symbol: "xmark.circle.fill", help: "Cancel the download") { store.cancel(entry) }
            }
        case .verifying:
            EmptyView()
        case .missing, .failed:
            Button(action: { store.download(entry) }) {
                Label("Download", systemImage: "arrow.down.circle")
            }
        }
    }
}

/// A full-width bar with what has come in, the speed and the time left, as LM Studio shows its downloads.
private struct ProgressLine: View {
    /// nil while the checksum is checked.
    let progress: DownloadProgress?
    let paused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            if let progress {
                ProgressView(value: progress.fraction)
                    .tint(paused ? .secondary : .accentColor)
                HStack {
                    Text(paused ? "Paused · \(progress.label)" : progress.label)
                    Spacer()
                    Text(progress.fraction, format: .percent.precision(.fractionLength(0))).monospacedDigit()
                }
            } else {
                ProgressView().progressViewStyle(.linear)
                Text("Checking the download…")
            }
        }
        .font(.caption)
        .foregroundStyle(.secondary)
        .monospacedDigit()
        .padding(.leading, 34)
    }
}

private struct IconButton: View {
    let symbol: String
    let help: String
    let action: () -> Void

    var body: some View {
        Button(action: action) { Image(systemName: symbol).font(.title3) }
            .buttonStyle(.borderless)
            .help(help)
    }
}
