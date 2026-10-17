import Foundation

/// One dictation over asrd's WebSocket (`/v1/dictate`): PCM goes out, transcripts come back.
@MainActor
final class ASRClient {
    enum Event {
        case partial(committed: String, live: String)
        case final(String)
        case failed(String)
    }

    var onEvent: ((Event) -> Void)?

    private var task: URLSessionWebSocketTask?
    private var ready = false
    private var finished = false
    // Audio recorded before the server said ready, sent in order once it has.
    private var pending: [Data] = []
    // Resets sent; partials the server made before the latest one are stale.
    private var epoch = 0

    func connect(url: URL) {
        let task = URLSession.shared.webSocketTask(with: url)
        self.task = task
        task.resume()
        receive(on: task)
    }

    func send(audio: Data) {
        guard let task, !finished else { return }
        if ready {
            task.send(.data(audio)) { _ in }
        } else {
            pending.append(audio)
        }
    }

    /// Drops the transcript so far; the dictation goes on.
    func reset() {
        guard let task, !finished else { return }
        if ready {
            epoch += 1
            task.send(.string(#"{"type":"reset"}"#)) { _ in }
        } else {
            pending.removeAll()
        }
    }

    /// Ends the audio; the final transcript follows as `.final`.
    func stop() {
        task?.send(.string(#"{"type":"stop"}"#)) { _ in }
    }

    func cancel() {
        finished = true
        task?.send(.string(#"{"type":"cancel"}"#)) { _ in }
        task?.cancel(with: .normalClosure, reason: nil)
        task = nil
    }

    private func receive(on task: URLSessionWebSocketTask) {
        task.receive { [weak self] result in
            Task { @MainActor in
                guard let self, self.task === task else { return }
                switch result {
                case .success(.string(let text)):
                    self.handle(text)
                    if !self.finished { self.receive(on: task) }
                case .success:
                    self.receive(on: task)
                case .failure(let error):
                    self.fail("Cannot reach asrd: \(error.localizedDescription)")
                }
            }
        }
    }

    private func handle(_ text: String) {
        guard let data = text.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let type = object["type"] as? String else { return }
        switch type {
        case "ready":
            ready = true
            for chunk in pending { task?.send(.data(chunk)) { _ in } }
            pending.removeAll()
        case "partial":
            guard (object["epoch"] as? Int ?? 0) >= epoch else { return }
            onEvent?(.partial(committed: object["committed"] as? String ?? "", live: object["live"] as? String ?? ""))
        case "final":
            finished = true
            onEvent?(.final(object["text"] as? String ?? ""))
            task?.cancel(with: .normalClosure, reason: nil)
            task = nil
        case "error":
            fail(object["message"] as? String ?? "asrd reported an error")
        default:
            break
        }
    }

    private func fail(_ message: String) {
        guard !finished else { return }
        finished = true
        onEvent?(.failed(message))
        task?.cancel(with: .abnormalClosure, reason: nil)
        task = nil
    }
}
