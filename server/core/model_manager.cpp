#include "model_manager.h"

#include <algorithm>
#include <cstdio>

namespace asrd {
namespace {

std::string base_name(const std::string & path) {
    return path.substr(path.find_last_of('/') + 1);
}

bool has_spec(const std::vector<SettingSpec> & specs, const std::string & key) {
    return std::any_of(specs.begin(), specs.end(), [&](const SettingSpec & spec) { return spec.key == key; });
}

// A detector runs beside the recognizer: on the CPU, with one thread, where the runtime lets it choose.
Settings detector_settings(const Runtime & runtime) {
    Settings out;
    for (const SettingSpec & spec : runtime.load_settings()) {
        if (spec.key == "device" && std::find(spec.choices.begin(), spec.choices.end(), "cpu") != spec.choices.end())
            out["device"] = std::string("cpu");
        if (spec.key == "threads") out["threads"] = int64_t{1};
    }
    return out;
}

}  // namespace

ModelManager::ModelManager(RuntimeRegistry & registry, ModelLibrary & library)
    : registry_(registry), library_(library) {
    loader_ = std::thread([this] { run(); });
}

ModelManager::~ModelManager() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    loader_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    session_.reset();
}

bool ModelManager::load(LoadRequest request, std::string & error) {
    if (!request.runtime.empty() && !registry_.find(request.runtime)) {
        error = "no runtime \"" + request.runtime + "\"";
        return false;
    }
    if (request.path.empty() && !catalog_find(request.model) && !library_.find(request.model)) {
        error = "no model \"" + request.model + "\" in the catalog or the library";
        return false;
    }
    // What can be checked without loading is checked here, so a request that cannot work leaves the model in use.
    Resolved model;
    std::string missing;
    if (resolve(request, model, missing, error)) {
        Runtime * runtime = pick_runtime(request.runtime, model.format, model.architecture, error);
        if (!runtime) return false;
        ModelInfo info;
        if (!runtime->inspect(model.files, info, error)) return false;
        const std::vector<SettingSpec> load_specs = runtime->load_settings();
        for (const auto & [key, value] : request.settings) {
            if (!has_spec(load_specs, key) && !has_spec(info.request_settings, key)) {
                error = info.name + " with " + runtime->info().id + " has no setting \"" + key + "\"";
                return false;
            }
        }
    } else if (missing.empty()) {
        return false;
    }
    error.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    waiting_.reset();
    state_ = State::loading;
    state_id_ = model.model_id.empty() ? request.model : model.model_id;
    error_.clear();
    pending_ = std::move(request);
    busy_ = true;
    wake_.notify_all();
    return true;
}

void ModelManager::unload() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.reset();
    waiting_.reset();
    session_.reset();
    state_ = State::unloaded;
    state_id_.clear();
    error_.clear();
}

void ModelManager::installed(const std::string &, const std::string &) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!waiting_ || pending_) return;
    pending_ = std::move(waiting_);
    waiting_.reset();
    busy_ = true;
    wake_.notify_all();
}

std::shared_ptr<const Session> ModelManager::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_ == State::ready ? session_ : nullptr;
}

ModelManager::State ModelManager::state() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

bool ModelManager::loaded(const std::string & model, const std::string & variant) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return session_ && session_->model_id == model && (variant.empty() || session_->variant_id == variant);
}

void ModelManager::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    settled_.wait(lock, [&] { return !busy_ || stopping_; });
}

void ModelManager::set_state(State state, const std::string & id, const std::string & error) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_ = state;
    state_id_ = id;
    error_ = error;
}

Json ModelManager::health() const {
    std::lock_guard<std::mutex> lock(mutex_);
    static const char * names[] = {"unloaded", "no_model", "loading", "ok", "failed"};
    Json out = Json::object();
    out.set("status", names[static_cast<int>(state_)]);
    if (state_ != State::ready || !session_) {
        out.set("id", state_id_);
        if (!error_.empty()) out.set("error", error_);
        return out;
    }
    const ModelInfo & info = session_->recognizer->info();
    out.set("id", session_->model_id).set("variant", session_->variant_id).set("runtime", session_->runtime_id);
    out.set("model", info.name).set("file", session_->file).set("device", session_->recognizer->device());
    out.set("languages", Json(info.languages)).set("language_steers", info.language_steers);
    out.set("prompt", info.takes_prompt).set("streams", info.streams);
    Json settings = Json::object();
    for (const auto & [key, value] : session_->request_settings) settings.set(key, value_json(value));
    out.set("request_settings", settings);
    Json specs = Json::array();
    for (const SettingSpec & spec : info.request_settings) specs.push(spec.json());
    out.set("request_setting_specs", specs);
    return out;
}

Runtime * ModelManager::pick_runtime(const std::string & asked, const std::string & format,
                                     const std::string & architecture, std::string & error) const {
    if (!asked.empty()) {
        Runtime * runtime = registry_.find(asked);
        if (runtime && runtime->supports(format, architecture)) return runtime;
        error = asked + " does not run " + format + " models of " + (architecture.empty() ? "this" : architecture) +
                " architecture";
        return nullptr;
    }
    const std::vector<Runtime *> runtimes = registry_.for_model(format, architecture);
    if (runtimes.empty()) {
        error = "no runtime runs " + format + " models of " + (architecture.empty() ? "this" : architecture) +
                " architecture";
        return nullptr;
    }
    return runtimes.front();
}

bool ModelManager::resolve(const LoadRequest & request, Resolved & out, std::string & missing,
                           std::string & error) const {
    if (!request.path.empty()) {
        out.files.format = request.path.size() > 5 && request.path.substr(request.path.size() - 5) == ".gguf" ? "gguf" : "";
        out.files.paths = {request.path};
        out.format = out.files.format;
        out.name = base_name(request.path);
        out.model_id = out.name;
        // A catalog model's file, named by its path, is that model.
        for (const ModelSpec & model : catalog())
            for (const Variant & variant : model.variants)
                for (const FileSpec & file : variant.files)
                    if (file.name() == out.name) {
                        out.model_id = model.id;
                        out.variant_id = variant.id;
                        out.architecture = variant.architecture;
                    }
        out.requires = {"silero-vad"};
        if (out.format.empty()) {
            error = request.path + " is not a model file asrd knows (.gguf)";
            return false;
        }
        return true;
    }
    const std::optional<InstalledVariant> installed = library_.find(request.model, request.variant);
    if (!installed) {
        missing = request.model;
        return false;
    }
    out.model_id = installed->model;
    out.variant_id = installed->variant;
    out.name = installed->name;
    out.files = installed->model_files();
    out.format = installed->format;
    out.architecture = installed->architecture;
    out.requires = installed->requires;
    return true;
}

void ModelManager::load_now(const LoadRequest & request) {
    const std::string id = request.path.empty() ? request.model : base_name(request.path);
    std::string error;
    std::string missing;
    Resolved model;
    std::vector<Resolved> required;
    if (resolve(request, model, missing, error)) {
        for (const std::string & requirement : model.requires) {
            Resolved r;
            LoadRequest sub;
            sub.model = requirement;
            if (!resolve(sub, r, missing, error)) break;
            required.push_back(r);
        }
    }
    if (!missing.empty()) {
        std::fprintf(stderr, "asrd: %s is not downloaded; it loads once it is\n", missing.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        session_.reset();
        state_ = State::missing;
        state_id_ = id;
        error_.clear();
        if (!pending_) waiting_ = request;
        return;
    }
    if (!error.empty()) {
        set_state(State::failed, id, error);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The model in use goes first: dictations under way keep it until they end, new ones wait for the next.
        session_.reset();
        state_ = State::loading;
        state_id_ = model.model_id;
        error_.clear();
    }
    Runtime * runtime = pick_runtime(request.runtime, model.format, model.architecture, error);
    if (!runtime) {
        set_state(State::failed, model.model_id, error);
        return;
    }
    // Settings the runtime loads with, and the rest for each recognition, checked against the model's own below.
    Settings load_settings;
    Settings request_settings;
    const std::vector<SettingSpec> load_specs = runtime->load_settings();
    for (const auto & [key, value] : request.settings)
        (has_spec(load_specs, key) ? load_settings : request_settings)[key] = value;

    auto session = std::make_shared<Session>();
    session->model_id = model.model_id;
    session->variant_id = model.variant_id;
    session->runtime_id = runtime->info().id;
    session->file = model.files.paths.empty() ? "" : base_name(model.files.paths.front());
    std::fprintf(stderr, "asrd: loading %s with %s\n", model.name.c_str(), runtime->info().id.c_str());
    session->recognizer = runtime->load_recognizer(model.files, load_settings, error);
    if (!session->recognizer) {
        set_state(State::failed, model.model_id, error);
        std::fprintf(stderr, "asrd: %s\n", error.c_str());
        return;
    }
    for (const auto & [key, value] : request_settings) {
        if (!has_spec(session->recognizer->info().request_settings, key)) {
            set_state(State::failed, model.model_id,
                      session->recognizer->info().name + " has no setting \"" + key + "\"");
            return;
        }
    }
    session->request_settings = request_settings;
    for (const Resolved & r : required) {
        Runtime * detector_runtime = pick_runtime("", r.format, r.architecture, error);
        if (detector_runtime) session->detector = detector_runtime->load_detector(r.files, detector_settings(*detector_runtime), error);
        if (!session->detector) {
            set_state(State::failed, model.model_id, r.name + ": " + error);
            std::fprintf(stderr, "asrd: %s: %s\n", r.name.c_str(), error.c_str());
            return;
        }
    }
    if (!session->detector && !session->recognizer->info().streams) {
        set_state(State::failed, model.model_id, model.name + " needs a voice activity detector, and requires none");
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_) return;  // superseded while loading; the next load starts now
    session_ = std::move(session);
    state_ = State::ready;
    state_id_ = model.model_id;
    error_.clear();
    std::fprintf(stderr, "asrd: %s ready on %s (%s)\n", session_->recognizer->info().name.c_str(),
                 session_->recognizer->device().c_str(), session_->runtime_id.c_str());
}

void ModelManager::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [&] { return stopping_ || pending_; });
        if (stopping_) break;
        const LoadRequest request = std::move(*pending_);
        pending_.reset();
        lock.unlock();
        load_now(request);
        lock.lock();
        if (!pending_) {
            busy_ = false;
            settled_.notify_all();
        }
    }
    busy_ = false;
    settled_.notify_all();
}

}  // namespace asrd
