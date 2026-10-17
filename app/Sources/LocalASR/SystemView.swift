import AppKit
import SwiftUI

/// A setting a runtime or a model takes, as asrd declares it; the window draws a control for it by its type, so a new
/// runtime needs no code here.
struct SettingSpec: Decodable, Equatable, Identifiable {
    let key: String
    let label: String
    let help: String
    /// "load" or "request".
    let scope: String
    /// boolean, integer, number or text.
    let type: String
    let `default`: JSONValue
    let range: [Double?]?
    let choices: [String]?
    /// How to show each choice, when not as it is.
    let choiceLabels: [String]?
    /// Tuning most people need not touch, kept under Advanced.
    let advanced: Bool?

    var id: String { key }
    var isAdvanced: Bool { advanced ?? false }
}

/// GET /v1/system: this Mac and the runtimes asrd carries.
struct SystemInfo: Decodable, Equatable {
    struct Host: Decodable, Equatable {
        let chip: String
        let model: String
        let os: String
        let performanceCores: Int
        let efficiencyCores: Int
        let memory: Int64
    }

    struct Device: Decodable, Equatable, Identifiable {
        let id: String
        let description: String
        /// cpu, gpu, igpu or npu.
        let kind: String
        let memoryTotal: Int64
        let memoryFree: Int64
    }

    struct Runtime: Decodable, Equatable, Identifiable {
        let id: String
        let name: String
        let version: String
        let formats: [String]
        let architectures: [String]
        let devices: [Device]
        let loadSettings: [SettingSpec]
    }

    let host: Host
    let runtimes: [Runtime]
}

/// Settings › Runtime & Hardware, as LM Studio has them: the Mac, each runtime with the devices it runs on and their
/// memory, and where the model runs. Apply reloads the model with it.
@MainActor
final class SystemModel: ObservableObject {
    let settings: Settings
    let server: ServerProcess
    let store: ModelStore
    let onApply: () async -> String?

    @Published var system: SystemInfo?
    @Published var error: String?
    @Published var applying = false
    @Published var applied: String?
    private var refresh: Timer?

    init(settings: Settings, server: ServerProcess, store: ModelStore, onApply: @escaping () async -> String?) {
        self.settings = settings
        self.server = server
        self.store = store
        self.onApply = onApply
    }

    func stop() {
        refresh?.invalidate()
        refresh = nil
    }

    func start() {
        Task { await reload() }
        // Free memory changes as models load; a few seconds is fresh enough.
        refresh?.invalidate()
        refresh = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in
            Task { @MainActor in await self?.reload() }
        }
    }

    func reload() async {
        do {
            let system = try await ServerAPI.get("v1/system", as: SystemInfo.self)
            if self.system != system { self.system = system }
            error = nil
        } catch {
            self.error = "Waiting for asrd…"
        }
    }

    func apply() {
        applying = true
        applied = nil
        Task {
            let failure = await onApply()
            applying = false
            applied = failure ?? "Reloading the model with these settings…"
        }
    }
}

struct SystemView: View {
    @ObservedObject var model: SystemModel
    @ObservedObject var settings: Settings
    @ObservedObject var server: ServerProcess

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            if let system = model.system {
                HostSection(host: system.host)
                ForEach(system.runtimes) { runtime in
                    let inUse = server.health?.runtime == runtime.id
                    RuntimeSection(runtime: runtime, inUse: inUse,
                                   device: inUse ? server.health?.device : nil,
                                   values: runtimeBinding(runtime.id), onChange: model.apply)
                }
                Text(model.applied ?? statusLine).font(.caption).foregroundStyle(.secondary)
            } else {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text(model.error ?? "Reading the system from asrd…").foregroundStyle(.secondary)
                }
                .padding(.vertical, 12)
            }
        }
        .padding(20)
        .frame(width: 560)
        .onAppear(perform: model.start)
        .onDisappear(perform: model.stop)
    }

    private var statusLine: String {
        switch server.status {
        case .ready(let name, _, _):
            return "\(name) runs on \(server.health?.device ?? "?") with \(server.health?.runtime ?? "?")."
        case .loading: return "Loading the model…"
        case .loadFailed(_, let message, _): return message
        case .noModel: return "The model is not downloaded yet."
        case .starting: return "Starting asrd…"
        case .failed(let message): return message
        }
    }

    private func runtimeBinding(_ id: String) -> Binding<[String: JSONValue]> {
        Binding(get: { settings.runtimeSettings[id] ?? [:] }, set: { settings.runtimeSettings[id] = $0 })
    }

}

private struct HostSection: View {
    let host: SystemInfo.Host

    var body: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 4) {
                Text("\(host.chip) · \(host.model) · macOS \(host.os)")
                Text("\(host.performanceCores) performance and \(host.efficiencyCores) efficiency cores · "
                     + "\(ByteCountFormatter.string(fromByteCount: host.memory, countStyle: .memory)) memory")
                    .foregroundStyle(.secondary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(6)
        } label: {
            Label("This Mac", systemImage: "laptopcomputer").fontWeight(.semibold)
        }
    }
}

private struct RuntimeSection: View {
    let runtime: SystemInfo.Runtime
    let inUse: Bool
    /// The device the model runs on, when it runs on this runtime.
    let device: String?
    @Binding var values: [String: JSONValue]
    /// A setting changed: the model loads again with it.
    let onChange: () -> Void

    /// The device the setting names: by name, or "auto"/"gpu" for the first GPU and "cpu" for the CPU.
    private func isChosen(_ d: SystemInfo.Device) -> Bool {
        let setting = values["device"]?.label.lowercased() ?? "auto"
        if setting == d.id.lowercased() { return true }
        let gpu = runtime.devices.first { $0.kind != "cpu" } ?? runtime.devices.first
        switch setting {
        case "auto", "gpu": return d.id == gpu?.id
        case "cpu": return d.kind == "cpu"
        default: return false
        }
    }

    /// Settings other than the device, which the device rows choose.
    private var otherSettings: [SettingSpec] {
        runtime.loadSettings.filter { !$0.isAdvanced && $0.key != "device" }
    }

    var body: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 10) {
                Text("Run the model on").font(.subheadline).foregroundStyle(.secondary)
                ForEach(runtime.devices) { d in
                    DeviceRow(device: d, chosen: isChosen(d), inUse: d.id == device) {
                        guard !isChosen(d) || d.id != device else { return }
                        values["device"] = .string(d.id)
                        onChange()
                    }
                    if d.id != runtime.devices.last?.id { Divider() }
                }
                if !otherSettings.isEmpty {
                    Divider()
                    SettingsForm(specs: otherSettings, values: $values)
                }
            }
            .padding(6)
        } label: {
            HStack(spacing: 6) {
                Label("\(runtime.name) \(runtime.version)", systemImage: "cpu").fontWeight(.semibold)
                if inUse { Text("In use").font(.caption).foregroundStyle(.secondary) }
            }
        }
    }
}

private struct DeviceRow: View {
    let device: SystemInfo.Device
    /// The device chosen for the model.
    let chosen: Bool
    /// The device the model runs on now.
    let inUse: Bool
    let onChoose: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: chosen ? "checkmark.circle.fill" : "circle")
                .font(.title3)
                .foregroundStyle(chosen ? Color.accentColor : .secondary)
            VStack(alignment: .leading, spacing: 3) {
                HStack {
                    Text(device.id).fontWeight(.semibold)
                    Text("\(device.description) \(kindLabel)").foregroundStyle(.secondary)
                    Spacer()
                    if inUse { Text("In use").foregroundStyle(.secondary) }
                }
                ProgressView(value: device.memoryTotal > 0
                             ? Double(device.memoryTotal - device.memoryFree) / Double(device.memoryTotal) : 0)
                Text("\(bytes(device.memoryFree)) free of \(bytes(device.memoryTotal))")
                    .font(.caption).foregroundStyle(.secondary).monospacedDigit()
            }
        }
        .contentShape(Rectangle())
        .onTapGesture(perform: onChoose)
    }

    private var kindLabel: String {
        switch device.kind {
        case "gpu": return "GPU"
        case "igpu": return "integrated GPU"
        case "npu": return "Neural Engine"
        default: return "CPU"
        }
    }

    private func bytes(_ count: Int64) -> String { ByteCountFormatter.string(fromByteCount: count, countStyle: .memory) }
}

/// A control per setting, by its type; a value equal to the default is not kept.
private struct SettingsForm: View {
    let specs: [SettingSpec]
    @Binding var values: [String: JSONValue]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(specs) { spec in
                VStack(alignment: .leading, spacing: 2) {
                    control(spec)
                    if !spec.help.isEmpty {
                        Text(spec.help).font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }

    @ViewBuilder private func control(_ spec: SettingSpec) -> some View {
        switch spec.type {
        case "boolean":
            Toggle(spec.label, isOn: Binding(
                get: { if case .bool(let v) = value(spec) { return v }; return false },
                set: { set(spec, .bool($0)) }))
        case "integer":
            let bounds = (Int(spec.range?.first.flatMap { $0 } ?? 0))...(Int(spec.range?.last.flatMap { $0 } ?? 1_000_000))
            Stepper(value: Binding(
                get: { if case .int(let v) = value(spec) { return v }; return 0 },
                set: { set(spec, .int($0)) }), in: bounds) {
                Text("\(spec.label): \(value(spec).label)")
            }
        case "number":
            HStack {
                Text(spec.label)
                TextField(spec.label, value: Binding(
                    get: {
                        switch value(spec) {
                        case .double(let v): return v
                        case .int(let v): return Double(v)
                        default: return 0
                        }
                    },
                    set: { set(spec, .double($0)) }), format: .number)
                    .frame(width: 90)
            }
        default:
            if let choices = spec.choices, !choices.isEmpty {
                Picker(spec.label, selection: Binding(
                    get: { value(spec).label },
                    set: { set(spec, .string($0)) })) {
                    ForEach(Array(choices.enumerated()), id: \.element) { index, choice in
                        Text(spec.choiceLabels.flatMap { index < $0.count ? $0[index] : nil } ?? choice).tag(choice)
                    }
                }
                .frame(maxWidth: 320)
            } else {
                HStack {
                    Text(spec.label)
                    TextField(spec.label, text: Binding(
                        get: { value(spec).label },
                        set: { set(spec, .string($0)) }))
                }
            }
        }
    }

    private func value(_ spec: SettingSpec) -> JSONValue { values[spec.key] ?? spec.default }

    private func set(_ spec: SettingSpec, _ value: JSONValue) {
        values[spec.key] = value == spec.default ? nil : value
    }
}
