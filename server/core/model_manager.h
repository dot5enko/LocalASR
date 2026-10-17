// The model dictations use, loaded and switched while asrd runs. A load resolves the model's installed variant, picks
// the runtime that runs it (the one asked for, or the first registered that takes its format and architecture),
// resolves what the model requires beside it (the voice activity detector) the same way, and loads them on a thread
// of its own; /health tells how far it is. A load whose files are not downloaded yet waits, and goes ahead when the
// download manager reports them installed.
#pragma once

#include "library.h"
#include "registry.h"
#include "session.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace asrd {

class ModelManager {
public:
    struct LoadRequest {
        std::string model;       // a model id, installed from the catalog or imported
        std::string variant;     // "" for the first installed one
        std::string runtime;     // "" for the first that runs it
        std::string path;        // instead of a model: a model file anywhere
        Settings settings;       // load- and request-scope settings, told apart by the runtime's and the model's specs
    };

    enum class State { unloaded, missing, loading, ready, failed };

    ModelManager(RuntimeRegistry & registry, ModelLibrary & library);
    // Frees the models: on Metal a process that exits holding one aborts.
    ~ModelManager();

    // Starts loading in the background, replacing a load that has not started yet. False for a request that names
    // nothing loadable (an unknown model or runtime).
    bool load(LoadRequest request, std::string & error);
    void unload();
    // A download has been installed: a load that waits for it goes ahead.
    void installed(const std::string & model, const std::string & variant);
    // The session dictations start on, or null while none is ready.
    std::shared_ptr<const Session> current() const;
    State state() const;
    bool loaded(const std::string & model, const std::string & variant) const;
    Json health() const;
    // Blocks until a load under way has ended (for the startup log).
    void wait();

private:
    struct Resolved {
        std::string model_id;
        std::string variant_id;
        std::string name;
        ModelFiles files;
        std::string format;
        std::string architecture;
        std::vector<std::string> requires;
    };

    void run();
    void load_now(const LoadRequest & request);
    // Empty `missing` and true when resolved; `missing` names the model not downloaded yet.
    bool resolve(const LoadRequest & request, Resolved & out, std::string & missing, std::string & error) const;
    Runtime * pick_runtime(const std::string & asked, const std::string & format, const std::string & architecture,
                           std::string & error) const;
    void set_state(State state, const std::string & id, const std::string & error);

    RuntimeRegistry & registry_;
    ModelLibrary & library_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable settled_;
    std::optional<LoadRequest> pending_;
    std::optional<LoadRequest> waiting_;   // the load that waits for a download
    bool busy_ = false;
    bool stopping_ = false;
    State state_ = State::unloaded;
    std::string state_id_;                 // the model the state is about
    std::string error_;
    std::shared_ptr<const Session> session_;
    std::thread loader_;
};

}  // namespace asrd
