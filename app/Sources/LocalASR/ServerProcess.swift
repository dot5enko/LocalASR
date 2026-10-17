import Foundation

/// Keeps asrd running: uses one that already listens (such as `make serve` in a terminal), or else starts the copy
/// bundled in Contents/MacOS, and stops it when the app quits. Models switch through asrd's API without a restart, so
/// downloads that asrd runs carry on across a switch. asrd answers even without a model downloaded, with the catalog
/// to download one from.
@MainActor
final class ServerProcess: ObservableObject {
    enum Status: Equatable {
        case starting
        /// asrd answers but is loading `id`, or waits for its files to be downloaded.
        case loading(id: String, external: Bool)
        case noModel(id: String, external: Bool)
        /// `id` is the model asrd holds; `external` when asrd was started outside the app.
        case ready(model: String, id: String, external: Bool)
        /// asrd answers, but the model did not load.
        case loadFailed(id: String, message: String, external: Bool)
        /// asrd does not answer.
        case failed(String)

        var isExternal: Bool {
            switch self {
            case .loading(_, let external), .noModel(_, let external), .ready(_, _, let external),
                 .loadFailed(_, _, let external):
                return external
            default: return false
            }
        }

        var reachable: Bool {
            switch self {
            case .loading, .noModel, .ready, .loadFailed: return true
            default: return false
            }
        }
    }

    /// GET /health, for the menu and the Runtime & Hardware window.
    struct Health: Decodable, Equatable {
        let status: String
        let id: String?
        let variant: String?
        let model: String?
        let runtime: String?
        let device: String?
        let error: String?
        let requestSettings: [String: JSONValue]?
        let requestSettingSpecs: [SettingSpec]?

        func status(external: Bool) -> Status {
            switch status {
            case "ok": return .ready(model: model ?? "unknown", id: id ?? "", external: external)
            case "loading": return .loading(id: id ?? "", external: external)
            case "failed": return .loadFailed(id: id ?? "", message: error ?? "the model did not load", external: external)
            default: return .noModel(id: id ?? "", external: external)
            }
        }
    }

    @Published private(set) var status: Status = .starting
    @Published private(set) var health: Health?

    private var process: Process?
    private var monitor: Timer?
    /// What a launched asrd loads once it answers: the last model and settings asked for.
    private var model: String?
    private var settings: [String: JSONValue] = [:]
    /// A launched asrd starts on the catalog's default model; the app's own goes to it once it answers.
    private var loadPending = false

    /// Starts asrd and loads `model` with the runtime's load settings in it, or uses an asrd that already runs.
    func start(model: String?, settings: [String: JSONValue]) {
        self.model = model
        self.settings = settings
        Task {
            if let health = await Self.health() {
                self.health = health
                status = health.status(external: true)
            } else {
                launch()
            }
            monitor = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
                Task { @MainActor in await self?.poll() }
            }
        }
    }

    /// Loads `model` in the running asrd with `settings`; nil when the request was taken, else why not.
    func load(model: String, settings: [String: JSONValue]) async -> String? {
        self.model = model
        self.settings = settings
        do {
            let health = try await ServerAPI.post("v1/models/load",
                                                  ["model": .string(model), "settings": .object(settings)],
                                                  as: Health.self)
            self.health = health
            status = health.status(external: process == nil)
            return nil
        } catch {
            return error.localizedDescription
        }
    }

    func stop() {
        monitor?.invalidate()
        guard let process else { return }
        self.process = nil
        guard process.isRunning else { return }
        // asrd frees its models on SIGTERM; on Metal a process that exits holding one aborts.
        process.terminate()
        process.waitUntilExit()
    }

    private func launch() {
        guard let executable = Bundle.main.url(forAuxiliaryExecutable: "asrd") else {
            status = .failed("asrd is not running and is not bundled; run `make serve`.")
            return
        }
        let process = Process()
        process.executableURL = executable
        let logs = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/LocalASR")
        try? FileManager.default.createDirectory(at: logs, withIntermediateDirectories: true)
        let logURL = logs.appendingPathComponent("asrd.log")
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        if let log = try? FileHandle(forWritingTo: logURL) {
            process.standardOutput = log
            process.standardError = log
        }
        do {
            try process.run()
            self.process = process
            loadPending = model != nil
            status = .starting
        } catch {
            status = .failed("Cannot start asrd: \(error.localizedDescription)")
        }
    }

    private func poll() async {
        if let health = await Self.health() {
            if loadPending, process != nil, let model {
                loadPending = false
                // Settings saved for another model or runtime do not keep the model from loading: it loads without them.
                if await load(model: model, settings: settings) != nil, !settings.isEmpty {
                    _ = await load(model: model, settings: [:])
                }
                return
            }
            if self.health != health { self.health = health }
            let status = health.status(external: process == nil)
            if self.status != status { self.status = status }
        } else if let process, !process.isRunning {
            status = .failed("asrd exited (\(process.terminationStatus)); see ~/Library/Logs/LocalASR/asrd.log")
        } else if process == nil, status.reachable {
            // An external asrd went away; take over.
            launch()
        }
    }

    private static func health() async -> Health? {
        try? await ServerAPI.get("health", as: Health.self, timeout: 1)
    }
}
