import AVFoundation

/// The default microphone as 16-bit mono PCM at 16 kHz, in ~100 ms chunks, plus a level for the waveform.
/// Both callbacks run on the audio thread.
final class AudioCapture {
    enum Failure: LocalizedError {
        case noInput, converter

        var errorDescription: String? {
            switch self {
            case .noInput: return "No microphone is available."
            case .converter: return "The microphone's format cannot be converted to 16 kHz."
            }
        }
    }

    var onChunk: ((Data) -> Void)?
    var onLevel: ((Float) -> Void)?

    private let engine = AVAudioEngine()
    private let outputFormat = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: 16_000, channels: 1, interleaved: true)!
    private var converter: AVAudioConverter?
    private var pending = Data()
    private let chunkBytes = 3_200

    static func requestPermission() async -> Bool {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized: return true
        case .notDetermined: return await AVCaptureDevice.requestAccess(for: .audio)
        default: return false
        }
    }

    func start() throws {
        let input = engine.inputNode
        let inputFormat = input.outputFormat(forBus: 0)
        guard inputFormat.sampleRate > 0, inputFormat.channelCount > 0 else { throw Failure.noInput }
        guard let converter = AVAudioConverter(from: inputFormat, to: outputFormat) else { throw Failure.converter }
        converter.downmix = true
        self.converter = converter
        pending.removeAll(keepingCapacity: true)
        input.installTap(onBus: 0, bufferSize: 1_024, format: inputFormat) { [weak self] buffer, _ in
            self?.process(buffer)
        }
        engine.prepare()
        do {
            try engine.start()
        } catch {
            input.removeTap(onBus: 0)
            throw error
        }
    }

    /// Stops the microphone and hands over the audio still held back.
    func stop() {
        guard engine.isRunning else { return }
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        if !pending.isEmpty {
            onChunk?(pending)
            pending.removeAll()
        }
    }

    private func process(_ buffer: AVAudioPCMBuffer) {
        if let samples = buffer.floatChannelData?[0], buffer.frameLength > 0 {
            var sum: Float = 0
            for i in 0..<Int(buffer.frameLength) { sum += samples[i] * samples[i] }
            let rms = sqrt(sum / Float(buffer.frameLength))
            // -55 dBFS (room noise) maps to 0, -10 dBFS (loud speech) to 1.
            let db = 20 * log10(max(rms, 1e-7))
            onLevel?(min(max((db + 55) / 45, 0), 1))
        }

        guard let converter else { return }
        let capacity = AVAudioFrameCount(Double(buffer.frameLength) * outputFormat.sampleRate / buffer.format.sampleRate) + 64
        guard let output = AVAudioPCMBuffer(pcmFormat: outputFormat, frameCapacity: capacity) else { return }
        var consumed = false
        var error: NSError?
        let status = converter.convert(to: output, error: &error) { _, inputStatus in
            if consumed {
                inputStatus.pointee = .noDataNow
                return nil
            }
            consumed = true
            inputStatus.pointee = .haveData
            return buffer
        }
        guard status != .error, output.frameLength > 0, let samples = output.int16ChannelData?[0] else { return }
        pending.append(Data(bytes: samples, count: Int(output.frameLength) * 2))
        if pending.count >= chunkBytes {
            onChunk?(pending)
            pending.removeAll(keepingCapacity: true)
        }
    }
}
