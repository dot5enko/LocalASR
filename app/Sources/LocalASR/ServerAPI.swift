import Foundation

/// asrd's HTTP API: JSON in and out, snake_case keys.
enum ServerAPI {
    static let baseURL = URL(string: "http://127.0.0.1:8765")!

    struct Failure: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    private struct ErrorReply: Decodable {
        let error: String
    }

    static func get<T: Decodable>(_ path: String, as type: T.Type = T.self, timeout: TimeInterval = 2) async throws -> T {
        try await send("GET", path, body: nil as [String: JSONValue]?, timeout: timeout)
    }

    @discardableResult
    static func post<T: Decodable>(_ path: String, _ body: [String: JSONValue] = [:], as type: T.Type = JSONValue.self)
        async throws -> T {
        try await send("POST", path, body: body, timeout: 5)
    }

    @discardableResult
    static func delete<T: Decodable>(_ path: String, as type: T.Type = JSONValue.self) async throws -> T {
        try await send("DELETE", path, body: nil as [String: JSONValue]?, timeout: 5)
    }

    private static func send<T: Decodable>(_ method: String, _ path: String, body: [String: JSONValue]?,
                                           timeout: TimeInterval) async throws -> T {
        var request = URLRequest(url: baseURL.appendingPathComponent(path))
        request.httpMethod = method
        request.timeoutInterval = timeout
        if let body {
            request.httpBody = try JSONEncoder().encode(body)
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        }
        let (data, response) = try await URLSession.shared.data(for: request)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        let decoder = JSONDecoder()
        decoder.keyDecodingStrategy = .convertFromSnakeCase
        guard (200..<300).contains(status) else {
            let message = (try? decoder.decode(ErrorReply.self, from: data))?.error ?? "asrd answered \(status)"
            throw Failure(message: message)
        }
        return try decoder.decode(T.self, from: data)
    }
}

/// Any JSON value: setting values, request bodies, and replies the app does not read.
enum JSONValue: Codable, Equatable, Hashable {
    case bool(Bool)
    case int(Int)
    case double(Double)
    case string(String)
    case array([JSONValue])
    case object([String: JSONValue])
    case null

    init(from decoder: Decoder) throws {
        let container = try decoder.singleValueContainer()
        if container.decodeNil() { self = .null }
        else if let value = try? container.decode(Bool.self) { self = .bool(value) }
        else if let value = try? container.decode(Int.self) { self = .int(value) }
        else if let value = try? container.decode(Double.self) { self = .double(value) }
        else if let value = try? container.decode(String.self) { self = .string(value) }
        else if let value = try? container.decode([JSONValue].self) { self = .array(value) }
        else { self = .object(try container.decode([String: JSONValue].self)) }
    }

    func encode(to encoder: Encoder) throws {
        var container = encoder.singleValueContainer()
        switch self {
        case .bool(let value): try container.encode(value)
        case .int(let value): try container.encode(value)
        case .double(let value): try container.encode(value)
        case .string(let value): try container.encode(value)
        case .array(let value): try container.encode(value)
        case .object(let value): try container.encode(value)
        case .null: try container.encodeNil()
        }
    }

    var label: String {
        switch self {
        case .bool(let value): return value ? "on" : "off"
        case .int(let value): return String(value)
        case .double(let value): return String(value)
        case .string(let value): return value
        case .array, .object, .null: return ""
        }
    }
}
