import Foundation

/// A download as asrd runs it (GET /v1/downloads).
struct DownloadInfo: Decodable, Equatable {
    let id: String
    let model: String
    let variant: String
    /// queued, running, verifying, paused, done or failed.
    let state: String
    let received: Int64
    let total: Int64
    let bytesPerS: Double
    let error: String?
}

/// A format and quantization of a model's weights, and whether this Mac has it.
struct ModelVariant: Decodable, Identifiable, Equatable {
    struct File: Decodable, Equatable {
        let name: String
    }

    let id: String
    let format: String
    let architecture: String
    let quantization: String
    let bytes: Int64
    /// The files it downloads.
    let files: [File]
    let installed: Bool
    let path: String?
    /// The runtimes asrd has that run it.
    let runnableBy: [String]
    /// Whether its weights fit in the free memory of the device a load picks.
    let fits: Bool
    let download: DownloadInfo?
}

/// A model from asrd's library (GET /v1/models).
struct ModelEntry: Decodable, Identifiable, Equatable {
    let id: String
    /// "asr", or "vad" for Silero VAD, which every model needs.
    let kind: String
    let name: String
    /// "nvidia/parakeet-tdt-0.6b-v3": who publishes the original model, as its Hugging Face page heads it.
    let upstream: String
    /// "0.6B".
    let parameters: String
    let license: String
    /// The original model's card, which the license refers to.
    let source: String
    /// "catalog".
    let origin: String
    let languages: [String]
    /// Whether choosing languages changes what the model writes; Parakeet finds the language itself.
    let languageSteers: Bool
    let requires: [String]
    let loaded: Bool
    let downloaded: Bool
    let variants: [ModelVariant]

    /// The variant it runs: the installed one, else the catalog's default.
    var variant: ModelVariant? { variants.first(where: \.installed) ?? variants.first }
    var sourceURL: URL? { source.isEmpty ? nil : URL(string: source) }
    var sizeLabel: String { ByteCountFormatter.string(fromByteCount: variant?.bytes ?? 0, countStyle: .file) }
    /// "speech.cpp · GGUF Q8_0".
    var runtimeLabel: String? {
        guard let variant else { return nil }
        let format = ([variant.format.uppercased(), variant.quantization].filter { !$0.isEmpty }).joined(separator: " ")
        guard let runtime = variant.runnableBy.first else { return "\(format) · no runtime runs it" }
        return "\(runtime) · \(format)"
    }

    static func languageName(_ tag: String) -> String {
        Locale.current.localizedString(forLanguageCode: tag)?.localizedCapitalized ?? tag
    }
}

/// How far a download has got, for a progress line like "312 MB of 841 MB · 24.1 MB/s · 22 s left".
struct DownloadProgress: Equatable {
    var received: Int64 = 0
    var total: Int64
    var bytesPerSecond: Double = 0

    var fraction: Double { total > 0 ? min(Double(received) / Double(total), 1) : 0 }

    var label: String {
        let size = "\(Self.bytes(received)) of \(Self.bytes(total))"
        guard bytesPerSecond > 0 else { return size }
        let left = Double(total - received) / bytesPerSecond
        let formatter = DateComponentsFormatter()
        formatter.unitsStyle = .abbreviated
        formatter.allowedUnits = left >= 3600 ? [.hour, .minute] : left >= 60 ? [.minute, .second] : [.second]
        formatter.maximumUnitCount = 2
        return "\(size) · \(Self.bytes(Int64(bytesPerSecond)))/s · \(formatter.string(from: max(left, 1)) ?? "") left"
    }

    private static func bytes(_ count: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: count, countStyle: .file)
    }
}

/// The model library as asrd keeps it. asrd downloads, checks each file's SHA-256 against its catalog and installs;
/// this store shows that and asks for it, and reads the downloads again a few times a second while one runs.
@MainActor
final class ModelStore: ObservableObject {
    enum State: Equatable {
        case missing, downloading(DownloadProgress), paused(DownloadProgress), verifying, ready, failed(String)
    }

    @Published private(set) var models: [ModelEntry] = []
    /// Why the catalog could not be read, while asrd is not answering.
    @Published private(set) var catalogError: String?
    /// The last request asrd refused, such as deleting the model in use.
    @Published var message: String?
    @Published private(set) var downloads: [String: DownloadInfo] = [:]

    /// Called when a model's files have been downloaded and installed.
    var onDownloaded: ((ModelEntry) -> Void)?

    private var polling: Task<Void, Never>?

    var recognition: [ModelEntry] { models.filter { $0.kind == "asr" } }
    var vad: ModelEntry? { models.first { $0.kind == "vad" } }

    func entry(id: String) -> ModelEntry? { models.first { $0.id == id } }

    func state(_ entry: ModelEntry) -> State {
        if entry.variant?.installed == true { return .ready }
        guard let download = downloads[entry.id] ?? entry.variant?.download else { return .missing }
        let progress = DownloadProgress(received: download.received, total: download.total,
                                        bytesPerSecond: download.bytesPerS)
        switch download.state {
        case "queued", "running": return .downloading(progress)
        case "verifying": return .verifying
        case "paused": return .paused(progress)
        case "failed": return .failed(download.error ?? "The download failed.")
        default: return .ready
        }
    }

    func isReady(_ entry: ModelEntry) -> Bool { state(entry) == .ready }

    var activeDownloads: [(entry: ModelEntry, progress: DownloadProgress)] {
        models.compactMap { entry in
            if case .downloading(let progress) = state(entry) { return (entry, progress) }
            return nil
        }
    }

    /// Reads the library from asrd again: which models there are, which are downloaded, and the downloads.
    func reload() async {
        do {
            models = try await ServerAPI.get("v1/models", as: [ModelEntry].self)
            catalogError = nil
            await readDownloads()
        } catch is ServerAPI.Failure {
            catalogError = "asrd has no model library; it may be an older build."
        } catch {
            catalogError = "Waiting for asrd…"
        }
    }

    func download(_ entry: ModelEntry) {
        Task {
            if case .paused = state(entry), let id = downloads[entry.id]?.id {
                await run { try await ServerAPI.post("v1/downloads/\(id)/resume") }
            } else {
                var body: [String: JSONValue] = ["model": .string(entry.id)]
                if let variant = entry.variant?.id { body["variant"] = .string(variant) }
                await run { try await ServerAPI.post("v1/downloads", body) }
            }
        }
    }

    func pause(_ entry: ModelEntry) {
        guard let id = downloads[entry.id]?.id else { return }
        Task { await run { try await ServerAPI.post("v1/downloads/\(id)/pause") } }
    }

    func cancel(_ entry: ModelEntry) {
        guard let id = downloads[entry.id]?.id else { return }
        Task { await run { try await ServerAPI.delete("v1/downloads/\(id)") } }
    }

    func delete(_ entry: ModelEntry) {
        guard let variant = entry.variant?.id else { return }
        Task {
            await run { try await ServerAPI.delete("v1/models/\(entry.id)/\(variant)") }
            await reload()
        }
    }

    /// Sends one request, keeps what asrd says when it refuses, and reads the downloads again.
    private func run(_ request: () async throws -> JSONValue) async {
        do {
            _ = try await request()
            message = nil
        } catch {
            message = error.localizedDescription
        }
        await readDownloads()
    }

    private func readDownloads() async {
        guard let list = try? await ServerAPI.get("v1/downloads", as: [DownloadInfo].self) else { return }
        let before = downloads
        downloads = Dictionary(list.map { ($0.model, $0) }, uniquingKeysWith: { _, last in last })
        let finished = downloads.values.filter { $0.state == "done" && before[$0.model].map { $0.state != "done" } ?? false }
        if !finished.isEmpty {
            models = (try? await ServerAPI.get("v1/models", as: [ModelEntry].self)) ?? models
            for download in finished {
                if let entry = entry(id: download.model) { onDownloaded?(entry) }
            }
        }
        let active = downloads.values.contains { ["queued", "running", "verifying"].contains($0.state) }
        if active, polling == nil {
            polling = Task { [weak self] in
                while !Task.isCancelled {
                    try? await Task.sleep(for: .milliseconds(500))
                    guard let self else { return }
                    await self.readDownloads()
                }
            }
        } else if !active {
            polling?.cancel()
            polling = nil
        }
    }
}
