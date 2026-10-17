// What asrd asks of a runtime: a library or program that loads models of some formats and runs them on this Mac's
// devices. speech.cpp is the first (runtimes/speechcpp); another one plugs in by implementing these classes and
// registering in runtimes/builtin.cpp. Nothing past this header knows which runtime runs a model, and nothing here
// knows about speech.cpp.
//
// A runtime does not download models or choose where they live: the model library hands it the files of an installed
// model (ModelFiles), and it says what they are (inspect) and loads them.
#pragma once

#include "json.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace asrd {

using Value = std::variant<bool, int64_t, double, std::string>;
// Setting values by key, as SettingSpec declares them.
using Settings = std::map<std::string, Value>;

Json value_json(const Value & value);
// The JSON value as the setting's type; false when it does not fit.
bool value_from_json(const Json & json, Value & out);
std::string value_text(const Value & value);

// A choice a runtime or a model exposes. The app draws its settings from these, with no code of its own per runtime.
struct SettingSpec {
    enum class Scope { load, request };
    enum class Type { boolean, integer, number, text };

    std::string key;             // "threads"
    std::string label;           // "CPU threads"
    std::string help;
    Scope scope = Scope::load;   // load: applied when the model loads; request: with each recognition
    Type type = Type::text;
    Value fallback;              // the value in effect when none is set
    std::optional<std::pair<double, double>> range;
    std::vector<std::string> choices;  // for text: the values it takes
    std::vector<std::string> choice_labels;  // how to show each choice, when not as it is
    bool advanced = false;       // shown only when asked for: most people need not touch it

    Json json() const;
};

enum class DeviceKind { cpu, gpu, igpu, npu };

struct Device {
    std::string id;              // what the "device" setting names: "MTL0", "CPU"
    std::string description;     // "Apple M3"
    DeviceKind kind = DeviceKind::cpu;
    uint64_t memory_total = 0;
    uint64_t memory_free = 0;

    Json json() const;
};

struct RuntimeInfo {
    std::string id;                          // "speech.cpp"
    std::string name;
    std::string version;                     // "0.8.8"
    std::vector<std::string> formats;        // "gguf"
    std::vector<std::string> architectures;  // "qwen3-asr", "fastconformer", "silero-vad"
};

enum class Task { recognition, detection };
const char * task_name(Task task);

// An installed model's files: one GGUF, or the files of a folder for formats that split a model.
struct ModelFiles {
    std::string format;
    std::vector<std::string> paths;
};

// What a runtime reads from a model's files.
struct ModelInfo {
    Task task = Task::recognition;
    std::string name;
    std::string architecture;
    int sample_rate = 16000;
    uint64_t weight_bytes = 0;
    std::vector<std::string> languages;      // two- or three-letter tags
    bool language_steers = false;            // false: the model finds the language itself
    bool takes_prompt = false;
    bool takes_draft = false;
    bool streams = false;                    // writes text as audio arrives, rather than reading whole utterances
    std::vector<SettingSpec> request_settings;
};

struct Audio {
    const float * samples = nullptr;
    size_t count = 0;
    int sample_rate = 16000;
};

// Cuts a call under way short from another thread. The caller cancels; the runtime arms a hook for the work it runs
// and disarms it before that work's resources go away.
class Cancellation {
public:
    void cancel();
    bool cancelled() const;
    // False when cancelled already, and the hook is not kept.
    bool arm(std::function<void()> hook);
    void disarm();

private:
    mutable std::mutex mutex_;
    bool cancelled_ = false;
    std::function<void()> hook_;
};

enum class Outcome { ok, cancelled, failed };

struct RecognizeRequest {
    Audio audio;
    std::string language;        // "" detects
    std::string prompt;          // only for a model that takes_prompt
    std::string draft;           // only for a model that takes_draft
    Settings settings;           // request-scope settings
};

struct Transcript {
    std::string text;
    std::string language;
};

// Text as a streaming model has it: committed is final, live may still change.
class RecognitionStream {
public:
    virtual ~RecognitionStream() = default;
    virtual bool push(Audio audio, std::string & error) = 0;
    virtual void text(std::string & committed, std::string & live) const = 0;
    virtual Outcome finish(Cancellation & cancel, Transcript & out, std::string & error) = 0;
};

// A loaded recognition model.
class Recognizer {
public:
    virtual ~Recognizer() = default;
    virtual const ModelInfo & info() const = 0;
    virtual std::string device() const = 0;
    // Blocks until done; callable from any thread. Calls on one model may wait for each other inside the runtime.
    virtual Outcome transcribe(const RecognizeRequest & request, Cancellation & cancel, Transcript & out,
                               std::string & error) = 0;
    // For a model that streams.
    virtual std::unique_ptr<RecognitionStream> open_stream(const RecognizeRequest &, std::string & error) {
        error = info().name + " does not stream";
        return nullptr;
    }
};

// Voice activity over a stream that grows: the regions of speech it has closed, and the one under way.
class DetectionStream {
public:
    virtual ~DetectionStream() = default;
    virtual bool push(Audio audio, std::string & error) = 0;
    // No more audio: the region under way closes.
    virtual bool end(std::string & error) = 0;
    virtual size_t region_count() const = 0;
    // Start and end of a closed region, in seconds from the stream's start.
    virtual std::pair<double, double> region(size_t index) const = 0;
    // The start of the speech under way, if any.
    virtual std::optional<double> speaking_since() const = 0;
};

// A loaded voice activity detector.
class VoiceDetector {
public:
    virtual ~VoiceDetector() = default;
    virtual const ModelInfo & info() const = 0;
    virtual std::unique_ptr<DetectionStream> open(int sample_rate, int min_silence_ms, double max_speech_s,
                                                  std::string & error) = 0;
};

class Runtime {
public:
    virtual ~Runtime() = default;
    virtual const RuntimeInfo & info() const = 0;
    // The devices it runs models on, with their free memory as of the call.
    virtual std::vector<Device> devices() = 0;
    // What a load takes: device, threads and the like.
    virtual std::vector<SettingSpec> load_settings() const = 0;
    // Reads what the files hold without loading them.
    virtual bool inspect(const ModelFiles & files, ModelInfo & out, std::string & error) = 0;
    virtual std::unique_ptr<Recognizer> load_recognizer(const ModelFiles & files, const Settings & settings,
                                                        std::string & error) = 0;
    virtual std::unique_ptr<VoiceDetector> load_detector(const ModelFiles &, const Settings &, std::string & error) {
        error = info().name + " runs no voice activity detector";
        return nullptr;
    }

    bool supports(const std::string & format, const std::string & architecture) const;
    Json json();
};

}  // namespace asrd
