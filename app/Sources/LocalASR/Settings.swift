import Foundation

/// The user's choices, kept in UserDefaults.
@MainActor
final class Settings: ObservableObject {
    private let defaults = UserDefaults.standard
    private let store: ModelStore

    /// The recognition model's catalog id; nil until chosen, when asrd runs its default.
    @Published var modelID: String? {
        didSet { defaults.set(modelID, forKey: "modelID") }
    }

    @Published var hotKey: HotKey {
        didSet { defaults.set(try? JSONEncoder().encode(hotKey), forKey: "shortcut") }
    }
    /// The languages dictation keeps to, in the order chosen (the first breaks ties); empty for any language.
    @Published var languages: [String] {
        didSet { defaults.set(languages, forKey: "languages") }
    }
    @Published var restoreClipboard: Bool {
        didSet { defaults.set(restoreClipboard, forKey: "restoreClipboard") }
    }
    /// Load settings by runtime id, such as speech.cpp's device and threads, as Runtime & Hardware sets them.
    @Published var runtimeSettings: [String: [String: JSONValue]] {
        didSet { defaults.set(try? JSONEncoder().encode(runtimeSettings), forKey: "runtimeSettings") }
    }
    /// The runtime that ran the model last, whose load settings asrd starts with.
    @Published var runtimeID: String? {
        didSet { defaults.set(runtimeID, forKey: "runtimeID") }
    }

    init(store: ModelStore) {
        self.store = store
        modelID = defaults.string(forKey: "modelID")
        hotKey = defaults.data(forKey: "shortcut").flatMap { try? JSONDecoder().decode(HotKey.self, from: $0) } ?? .default
        languages = defaults.stringArray(forKey: "languages") ?? []
        restoreClipboard = defaults.object(forKey: "restoreClipboard") as? Bool ?? true
        runtimeSettings = Self.decode(defaults.data(forKey: "runtimeSettings")) ?? [:]
        defaults.removeObject(forKey: "modelSettings")
        runtimeID = defaults.string(forKey: "runtimeID")
        defaults.removeObject(forKey: "prompt")
        defaults.removeObject(forKey: "hotKey")
    }

    /// Earlier versions kept the model's file name; once the catalog is read, that becomes its id.
    func migrate() {
        guard modelID == nil, let file = defaults.string(forKey: "modelFile") else { return }
        if let entry = store.recognition.first(where: { $0.variants.contains { $0.files.contains { $0.name == file } } }) {
            modelID = entry.id
        }
        defaults.removeObject(forKey: "modelFile")
    }

    func toggle(language tag: String) {
        if let index = languages.firstIndex(of: tag) {
            languages.remove(at: index)
        } else {
            languages.append(tag)
        }
    }

    private static func decode(_ data: Data?) -> [String: [String: JSONValue]]? {
        data.flatMap { try? JSONDecoder().decode([String: [String: JSONValue]].self, from: $0) }
    }

    /// What a load of `modelID` sends asrd: the load settings of the runtime that runs it, such as the device.
    func loadSettings(modelID: String?) -> [String: JSONValue] {
        runtimeID.flatMap { runtimeSettings[$0] } ?? [:]
    }

    var model: ModelEntry? {
        modelID.flatMap(store.entry(id:))
    }

    /// The chosen languages that the model takes; the others stay chosen for when a model that takes them is back.
    var activeLanguages: [String] {
        guard let model, model.languageSteers else { return [] }
        return languages.filter(model.languages.contains)
    }

    /// "EN · RU", or nil for any language.
    var languagesLabel: String? {
        activeLanguages.isEmpty ? nil : activeLanguages.map { $0.uppercased() }.joined(separator: " · ")
    }

    var dictateURL: URL {
        var components = URLComponents(url: ServerAPI.baseURL.appendingPathComponent("v1/dictate"),
                                       resolvingAgainstBaseURL: false)!
        components.scheme = "ws"
        let languages = activeLanguages
        components.queryItems = [URLQueryItem(name: "language", value: languages.isEmpty ? "auto" : languages.joined(separator: ","))]
        return components.url!
    }
}
