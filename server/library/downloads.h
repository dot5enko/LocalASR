// Downloads of catalog variants, one at a time, into the library's staging folder: each file arrives as <name>.part,
// is checked against the catalog's SHA-256 and renamed, and once every file of the variant is there the library
// installs the folder. A pause or a quit keeps what has arrived; the download resumes from there, also after asrd
// starts again (restore()).
#pragma once

#include "library.h"
#include "transport.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace asrd {

class DownloadManager {
public:
    enum class State { queued, running, verifying, paused, done, failed };

    struct Progress {
        std::string id;              // "<model>:<variant>"
        std::string model;
        std::string variant;
        State state = State::queued;
        int64_t received = 0;
        int64_t total = 0;
        double bytes_per_s = 0;
        std::string error;

        Json json() const;
    };

    DownloadManager(ModelLibrary & library, std::unique_ptr<Transport> transport);
    ~DownloadManager();

    // Picks up the downloads a previous run left in the staging folder, paused.
    void restore();
    // Queues the variant ("" for the model's default); resumes it when it is paused or failed.
    bool start(const std::string & model, const std::string & variant, Progress & out, std::string & error);
    bool pause(const std::string & id, std::string & error);
    bool resume(const std::string & id, std::string & error);
    // Stops the download and deletes what it fetched.
    bool cancel(const std::string & id, std::string & error);
    std::vector<Progress> list() const;
    std::optional<Progress> get(const std::string & id) const;
    std::optional<Progress> find(const std::string & model, const std::string & variant) const;

    // Called on the download thread when a variant has been installed.
    void on_installed(std::function<void(const std::string & model, const std::string & variant)> callback);
    // Blocks until nothing is queued or running.
    void wait_idle();

private:
    struct Job {
        Progress progress;
        const ModelSpec * model = nullptr;
        const Variant * variant = nullptr;
        std::string staged;          // ends in '/'
        Cancellation * cancel = nullptr;
        bool pausing = false;
        bool cancelling = false;
        std::chrono::steady_clock::time_point sampled;
        int64_t sampled_bytes = 0;
    };

    void run();
    // False when the job did not finish: failed (with `error`) or cut short by a pause or cancel.
    bool fetch(Job & job, std::string & error);
    void set_bytes(Job & job, int64_t received);
    std::shared_ptr<Job> find_job(const std::string & id) const;
    void queue_locked(const std::shared_ptr<Job> & job);

    ModelLibrary & library_;
    std::unique_ptr<Transport> transport_;
    std::function<void(const std::string &, const std::string &)> on_installed_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    std::vector<std::shared_ptr<Job>> jobs_;
    std::deque<std::shared_ptr<Job>> queue_;
    std::shared_ptr<Job> running_;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace asrd
