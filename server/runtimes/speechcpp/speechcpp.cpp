#include "speechcpp.h"

#include <speech.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

namespace asrd {
namespace {

bool verbose_log = false;

void on_log(speech_log_level level, const char * text, void *) {
    if (level >= SPEECH_LOG_WARN || (verbose_log && level >= SPEECH_LOG_INFO)) std::fputs(text, stderr);
}

// speech_last_error() belongs to the thread whose call failed, so it is read right after the call.
std::string error_text(speech_status status) {
    const char * message = speech_last_error();
    std::string out = speech_status_name(status);
    if (message && *message) out += std::string(": ") + message;
    return out;
}

std::string trim(const char * text) {
    const std::string s = text ? text : "";
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// "en" for "en-US" or "EN".
std::string base_language(const std::string & tag) {
    std::string out;
    for (const char c : tag) {
        if (c == '-' || c == '_') break;
        out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

// "decode_on_cpu" -> "Decode on CPU".
std::string label_of(const std::string & key) {
    std::string out;
    size_t at = 0;
    while (at <= key.size()) {
        const size_t end = std::min(key.find('_', at), key.size());
        std::string word = key.substr(at, end - at);
        if (word == "cpu" || word == "gpu" || word == "vad") {
            for (char & c : word) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        if (!out.empty()) out += ' ';
        out += word;
        at = end + 1;
    }
    if (!out.empty()) out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

// The options the dictation pipeline sets itself, which are not offered as settings.
bool pipeline_option(speech_option option) {
    return option == SPEECH_OPT_LANGUAGE || option == SPEECH_OPT_PROMPT || option == SPEECH_OPT_DRAFT ||
           option == SPEECH_OPT_TIMESTAMPS || option == SPEECH_OPT_MIN_SILENCE_DURATION_MS ||
           option == SPEECH_OPT_MAX_SPEECH_DURATION_S;
}

SettingSpec option_spec(const speech_model_info * info, speech_option option) {
    SettingSpec spec;
    spec.key = speech_option_name(option);
    spec.label = label_of(spec.key);
    spec.scope = SettingSpec::Scope::request;
    // Tuning a recognition takes knowing the model; the defaults are the model's own.
    spec.advanced = true;
    const bool has_default = speech_model_info_option_has_default(info, option);
    switch (speech_option_type(option)) {
        case SPEECH_TYPE_BOOL: {
            spec.type = SettingSpec::Type::boolean;
            int value = 0;
            if (has_default) speech_model_info_option_default_bool(info, option, &value);
            spec.fallback = value != 0;
            break;
        }
        case SPEECH_TYPE_INT: {
            spec.type = SettingSpec::Type::integer;
            int64_t value = 0;
            if (has_default) speech_model_info_option_default_int(info, option, &value);
            spec.fallback = value;
            break;
        }
        case SPEECH_TYPE_FLOAT: {
            spec.type = SettingSpec::Type::number;
            double value = 0;
            if (has_default) speech_model_info_option_default_float(info, option, &value);
            spec.fallback = value;
            break;
        }
        case SPEECH_TYPE_STRING: {
            spec.type = SettingSpec::Type::text;
            const char * value = "";
            if (has_default) speech_model_info_option_default_string(info, option, &value);
            spec.fallback = std::string(value ? value : "");
            for (size_t i = 0; i < speech_model_info_option_choice_count(info, option); ++i)
                spec.choices.emplace_back(speech_model_info_option_choice(info, option, i));
            break;
        }
    }
    double minimum = 0;
    double maximum = 0;
    int exclusive = 0;
    if ((spec.type == SettingSpec::Type::integer || spec.type == SettingSpec::Type::number) &&
        speech_model_info_option_range(info, option, &minimum, &maximum, &exclusive) == SPEECH_OK)
        spec.range = std::make_pair(minimum, maximum);
    return spec;
}

void describe(const speech_model_info * info, ModelInfo & out) {
    out.name = speech_model_info_name(info);
    out.architecture = speech_model_info_architecture(info);
    out.task = speech_model_info_task(info) == SPEECH_TASK_DETECTION ? Task::detection : Task::recognition;
    out.sample_rate = speech_model_info_sample_rate(info);
    out.weight_bytes = speech_model_info_weight_bytes(info);
    out.languages.clear();
    for (size_t i = 0; i < speech_model_info_language_count(info); ++i) {
        const std::string tag = base_language(speech_model_info_language(info, i));
        bool seen = false;
        for (const std::string & known : out.languages) seen = seen || known == tag;
        if (!seen) out.languages.push_back(tag);
    }
    out.language_steers = speech_model_info_takes(info, SPEECH_OPT_LANGUAGE) &&
                          speech_model_info_option_steers(info, SPEECH_OPT_LANGUAGE);
    out.takes_prompt = speech_model_info_takes(info, SPEECH_OPT_PROMPT);
    out.takes_draft = speech_model_info_takes(info, SPEECH_OPT_DRAFT);
    // speech.cpp's recognition families read whole utterances.
    out.streams = false;
    out.request_settings.clear();
    for (size_t i = 0; i < speech_model_info_option_count(info); ++i) {
        const speech_option option = speech_model_info_option(info, i);
        if (!pipeline_option(option)) out.request_settings.push_back(option_spec(info, option));
    }
}

// Sets a request option from a setting, by the option's name and type.
speech_status set_option(speech_request * request, const std::string & key, const Value & value, std::string & error) {
    speech_option option{};
    if (speech_option_from_name(key.c_str(), &option) != SPEECH_OK) {
        error = "speech.cpp has no option \"" + key + "\"";
        return SPEECH_ERROR_INVALID_ARGUMENT;
    }
    speech_status status = SPEECH_ERROR_INVALID_ARGUMENT;
    switch (speech_option_type(option)) {
        case SPEECH_TYPE_BOOL:
            if (const bool * b = std::get_if<bool>(&value)) status = speech_request_set_bool(request, option, *b);
            break;
        case SPEECH_TYPE_INT:
            if (const int64_t * i = std::get_if<int64_t>(&value)) status = speech_request_set_int(request, option, *i);
            break;
        case SPEECH_TYPE_FLOAT:
            if (const double * d = std::get_if<double>(&value)) status = speech_request_set_float(request, option, *d);
            else if (const int64_t * i = std::get_if<int64_t>(&value))
                status = speech_request_set_float(request, option, static_cast<double>(*i));
            break;
        case SPEECH_TYPE_STRING:
            if (const std::string * s = std::get_if<std::string>(&value))
                status = speech_request_set_string(request, option, s->c_str());
            break;
    }
    if (status != SPEECH_OK) error = key + ": " + (status == SPEECH_ERROR_INVALID_ARGUMENT ? "not a value it takes" : error_text(status));
    return status;
}

// Owns a loaded model; on Metal a process that exits holding one aborts, so every model is freed by its owner.
struct LoadedModel {
    speech_model * model = nullptr;
    ModelInfo info;
    std::string device;

    ~LoadedModel() { speech_model_free(model); }
};

bool load_model(const ModelFiles & files, const Settings & settings, LoadedModel & out, std::string & error) {
    if (files.paths.size() != 1) {
        error = "speech.cpp loads one GGUF file";
        return false;
    }
    speech_load_params * params = nullptr;
    speech_status status = speech_load_params_new(&params);
    for (const auto & [key, value] : settings) {
        if (status != SPEECH_OK) break;
        if (key == "device") {
            const std::string * device = std::get_if<std::string>(&value);
            status = device ? speech_load_params_set_device(params, device->c_str()) : SPEECH_ERROR_INVALID_ARGUMENT;
        } else if (key == "threads") {
            const int64_t * threads = std::get_if<int64_t>(&value);
            if (!threads) status = SPEECH_ERROR_INVALID_ARGUMENT;
            else if (*threads > 0) status = speech_load_params_set_threads(params, static_cast<int>(*threads));
        } else if (key == "warmup") {
            const bool * warmup = std::get_if<bool>(&value);
            status = warmup ? speech_load_params_set_warmup(params, *warmup) : SPEECH_ERROR_INVALID_ARGUMENT;
        } else {
            error = "speech.cpp has no load setting \"" + key + "\"";
            speech_load_params_free(params);
            return false;
        }
        if (status != SPEECH_OK) error = key + ": " + error_text(status);
    }
    if (status == SPEECH_OK) {
        status = speech_model_load(files.paths.front().c_str(), params, &out.model);
        if (status != SPEECH_OK) error = "cannot load " + files.paths.front() + ": " + error_text(status);
    }
    speech_load_params_free(params);
    if (status != SPEECH_OK) return false;

    speech_model_info * info = nullptr;
    if (speech_model_get_info(out.model, &info) != SPEECH_OK) {
        error = "cannot read the model's information: " + error_text(SPEECH_ERROR_INTERNAL);
        return false;
    }
    describe(info, out.info);
    const char * device = speech_model_info_device(info);
    out.device = device ? device : "";
    speech_model_info_free(info);
    return true;
}

class SpeechCppRecognizer : public Recognizer {
public:
    explicit SpeechCppRecognizer(std::unique_ptr<LoadedModel> model) : model_(std::move(model)) {}

    const ModelInfo & info() const override { return model_->info; }
    std::string device() const override { return model_->device; }

    Outcome transcribe(const RecognizeRequest & in, Cancellation & cancel, Transcript & out,
                       std::string & error) override {
        out = {};
        speech_request * request = nullptr;
        speech_status status = speech_request_new(model_->model, &request);
        if (status == SPEECH_OK)
            status = speech_request_set_audio(request, in.audio.samples, in.audio.count, in.audio.sample_rate);
        if (status == SPEECH_OK && !in.language.empty())
            status = speech_request_set_string(request, SPEECH_OPT_LANGUAGE, in.language.c_str());
        if (status == SPEECH_OK && model_->info.takes_prompt && !in.prompt.empty())
            status = speech_request_set_string(request, SPEECH_OPT_PROMPT, in.prompt.c_str());
        if (status == SPEECH_OK && model_->info.takes_draft && !in.draft.empty())
            status = speech_request_set_string(request, SPEECH_OPT_DRAFT, in.draft.c_str());
        if (status != SPEECH_OK) error = error_text(status);
        for (const auto & [key, value] : in.settings) {
            if (status != SPEECH_OK) break;
            status = set_option(request, key, value, error);
        }
        if (status == SPEECH_OK) {
            if (!cancel.arm([request] { speech_request_cancel(request); })) {
                speech_request_free(request);
                return Outcome::cancelled;
            }
            status = speech_transcribe(request);
            if (status != SPEECH_OK && status != SPEECH_CANCELLED) error = error_text(status);
            cancel.disarm();
        }
        if (status == SPEECH_OK) {
            const speech_result * result = speech_request_result(request);
            out.text = trim(speech_result_text(result));
            if (speech_result_language_count(result) > 0) out.language = speech_result_language(result, 0);
        }
        speech_request_free(request);
        return status == SPEECH_OK ? Outcome::ok : status == SPEECH_CANCELLED ? Outcome::cancelled : Outcome::failed;
    }

private:
    std::unique_ptr<LoadedModel> model_;
};

class SpeechCppDetection : public DetectionStream {
public:
    explicit SpeechCppDetection(speech_detection * detection) : detection_(detection) {}
    ~SpeechCppDetection() override { speech_detection_free(detection_); }

    bool push(Audio audio, std::string & error) override {
        const speech_status status = speech_detection_push(detection_, audio.samples, audio.count);
        if (status != SPEECH_OK) error = error_text(status);
        return status == SPEECH_OK;
    }

    bool end(std::string & error) override {
        const speech_status status = speech_detection_end(detection_);
        if (status != SPEECH_OK) error = error_text(status);
        return status == SPEECH_OK;
    }

    size_t region_count() const override { return speech_detection_region_count(detection_); }

    std::pair<double, double> region(size_t index) const override {
        double start = 0;
        double end = 0;
        speech_detection_region(detection_, index, &start, &end);
        return {start, end};
    }

    std::optional<double> speaking_since() const override {
        double start = 0;
        if (!speech_detection_speaking(detection_, &start, nullptr)) return std::nullopt;
        return start;
    }

private:
    speech_detection * detection_;
};

class SpeechCppDetector : public VoiceDetector {
public:
    explicit SpeechCppDetector(std::unique_ptr<LoadedModel> model) : model_(std::move(model)) {}

    const ModelInfo & info() const override { return model_->info; }

    std::unique_ptr<DetectionStream> open(int sample_rate, int min_silence_ms, double max_speech_s,
                                          std::string & error) override {
        speech_request * request = nullptr;
        speech_detection * detection = nullptr;
        speech_status status = speech_request_new(model_->model, &request);
        if (status == SPEECH_OK)
            status = speech_request_set_int(request, SPEECH_OPT_MIN_SILENCE_DURATION_MS, min_silence_ms);
        if (status == SPEECH_OK)
            status = speech_request_set_float(request, SPEECH_OPT_MAX_SPEECH_DURATION_S, max_speech_s);
        if (status == SPEECH_OK) status = speech_detection_start(request, sample_rate, &detection);
        if (status != SPEECH_OK) error = error_text(status);
        speech_request_free(request);
        if (status != SPEECH_OK) return nullptr;
        return std::make_unique<SpeechCppDetection>(detection);
    }

private:
    std::unique_ptr<LoadedModel> model_;
};

class SpeechCppRuntime : public Runtime {
public:
    SpeechCppRuntime() {
        info_.id = "speech.cpp";
        info_.name = "speech.cpp";
        info_.version = speech_version();
        info_.formats = {"gguf"};
        // The recognition and detection families of the version linked in; its synthesis families are of no use here.
        info_.architectures = {"qwen3-asr", "fastconformer", "silero-vad"};
        // The first call that touches a device sets ggml up for the process, which is done here, on the main thread.
        speech_device_count();
    }

    const RuntimeInfo & info() const override { return info_; }

    std::vector<Device> devices() override {
        std::vector<Device> out;
        for (size_t i = 0; i < speech_device_count(); ++i) {
            Device device;
            device.id = speech_device_name(i);
            const char * description = speech_device_description(i);
            device.description = description ? description : "";
            speech_device_kind kind = SPEECH_DEVICE_CPU;
            speech_device_get_kind(i, &kind);
            device.kind = kind == SPEECH_DEVICE_GPU ? DeviceKind::gpu : kind == SPEECH_DEVICE_IGPU ? DeviceKind::igpu
                                                                                                 : DeviceKind::cpu;
            speech_device_memory(i, &device.memory_total, &device.memory_free);
            out.push_back(device);
        }
        return out;
    }

    std::vector<SettingSpec> load_settings() const override {
        SettingSpec device;
        device.key = "device";
        device.label = "Run on";
        device.type = SettingSpec::Type::text;
        device.fallback = std::string("auto");
        device.choices = {"auto", "gpu", "cpu"};
        device.choice_labels = {"Automatic (GPU when there is one)", "GPU", "CPU"};
        // Devices by name only where "gpu" leaves a choice open: a machine with several GPUs.
        std::vector<size_t> gpus;
        for (size_t i = 0; i < speech_device_count(); ++i) {
            speech_device_kind kind = SPEECH_DEVICE_CPU;
            speech_device_get_kind(i, &kind);
            if (kind != SPEECH_DEVICE_CPU) gpus.push_back(i);
        }
        if (gpus.size() > 1) {
            for (const size_t i : gpus) {
                const char * description = speech_device_description(i);
                device.choices.emplace_back(speech_device_name(i));
                device.choice_labels.push_back(std::string(speech_device_name(i)) + " · " + (description ? description : ""));
            }
        }

        SettingSpec threads;
        threads.key = "threads";
        threads.label = "CPU threads";
        threads.help = "0: the performance cores";
        threads.type = SettingSpec::Type::integer;
        threads.advanced = true;
        threads.fallback = int64_t{0};
        threads.range = std::make_pair(0.0, 64.0);

        SettingSpec warmup;
        warmup.key = "warmup";
        warmup.label = "Warm up on load";
        warmup.help = "Runs a short recognition as the model loads, so the first dictation does not compile GPU kernels";
        warmup.type = SettingSpec::Type::boolean;
        warmup.advanced = true;
        warmup.fallback = false;
        return {device, threads, warmup};
    }

    bool inspect(const ModelFiles & files, ModelInfo & out, std::string & error) override {
        if (files.format != "gguf" || files.paths.size() != 1) {
            error = "speech.cpp reads one GGUF file";
            return false;
        }
        speech_model_info * info = nullptr;
        const speech_status status = speech_model_info_open(files.paths.front().c_str(), &info);
        if (status != SPEECH_OK) {
            error = error_text(status);
            return false;
        }
        describe(info, out);
        speech_model_info_free(info);
        return true;
    }

    std::unique_ptr<Recognizer> load_recognizer(const ModelFiles & files, const Settings & settings,
                                                std::string & error) override {
        auto model = std::make_unique<LoadedModel>();
        if (!load_model(files, settings, *model, error)) return nullptr;
        if (model->info.task != Task::recognition) {
            error = model->info.name + " is not a recognition model";
            return nullptr;
        }
        return std::make_unique<SpeechCppRecognizer>(std::move(model));
    }

    std::unique_ptr<VoiceDetector> load_detector(const ModelFiles & files, const Settings & settings,
                                                 std::string & error) override {
        auto model = std::make_unique<LoadedModel>();
        if (!load_model(files, settings, *model, error)) return nullptr;
        if (model->info.task != Task::detection) {
            error = model->info.name + " is not a voice activity detector";
            return nullptr;
        }
        return std::make_unique<SpeechCppDetector>(std::move(model));
    }

private:
    RuntimeInfo info_;
};

}  // namespace

std::unique_ptr<Runtime> make_speechcpp_runtime(bool verbose, std::string & error) {
    if (speech_api_version_major() != SPEECH_API_VERSION_MAJOR) {
        error = "speech.cpp speaks C API " + std::to_string(speech_api_version_major()) + ", not " +
                std::to_string(SPEECH_API_VERSION_MAJOR);
        return nullptr;
    }
    verbose_log = verbose;
    speech_log_set(on_log, nullptr);
    return std::make_unique<SpeechCppRuntime>();
}

}  // namespace asrd
