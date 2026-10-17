import SwiftUI

/// Settings › Languages: the languages dictation keeps to, chosen from every recognition model's, whatever model is in
/// use. A notice tells when the model in use ignores the choice, or does not know some of the languages chosen.
struct LanguagesView: View {
    @ObservedObject var settings: Settings
    @ObservedObject var store: ModelStore

    var body: some View {
        Form {
            if let notice {
                Section {
                    Label(notice, systemImage: "info.circle")
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Section {
                Toggle("Any language (detected)", isOn: Binding(
                    get: { settings.languages.isEmpty },
                    set: { if $0 { settings.languages = [] } }))
                ScrollView {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 150), alignment: .leading)], alignment: .leading) {
                        ForEach(languages, id: \.tag) { language in
                            Toggle(isOn: Binding(
                                get: { settings.languages.contains(language.tag) },
                                set: { _ in settings.toggle(language: language.tag) })) {
                                Text(language.name)
                                    .foregroundStyle(supported(language.tag) ? .primary : .tertiary)
                            }
                            .toggleStyle(.checkbox)
                            .help(supported(language.tag) ? "" : "\(settings.model?.name ?? "The model") does not know it")
                        }
                    }
                }
                .frame(height: 260)
            } header: {
                Text("Languages to dictate in")
            } footer: {
                Text("With one chosen, the model writes in it. With several, each utterance is detected, and one heard "
                     + "in another language is read again as the chosen one its script points to.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .formStyle(.grouped)
        .frame(width: 560)
        .fixedSize(horizontal: false, vertical: true)
    }

    /// Every recognition model's languages, by name.
    private var languages: [(tag: String, name: String)] {
        let tags = Set(store.recognition.flatMap(\.languages) + (settings.model?.languages ?? []) + settings.languages)
        return tags.map { (tag: $0, name: ModelEntry.languageName($0)) }
            .sorted { $0.name.localizedStandardCompare($1.name) == .orderedAscending }
    }

    private func supported(_ tag: String) -> Bool {
        guard let model = settings.model else { return true }
        return model.languageSteers && model.languages.contains(tag)
    }

    private var notice: String? {
        guard let model = settings.model else { return nil }
        if !model.languageSteers {
            return "\(model.name) finds the language itself, so this choice has no effect while it is in use. "
                + "It applies again with a model that takes a language, such as Qwen3-ASR."
        }
        let unknown = settings.languages.filter { !model.languages.contains($0) }.map(ModelEntry.languageName)
        guard !unknown.isEmpty else { return nil }
        return "\(model.name) does not know \(unknown.joined(separator: ", ")); "
            + (unknown.count == 1 ? "it is" : "they are") + " left out while this model is in use."
    }
}
