#include "downloads.h"

#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace asrd {
namespace {

const char * state_name(DownloadManager::State state) {
    static const char * names[] = {"queued", "running", "verifying", "paused", "done", "failed"};
    return names[static_cast<int>(state)];
}

int64_t file_size(const std::string & path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? -1 : static_cast<int64_t>(size);
}

bool sha256(const std::string & path, std::string & out) {
    std::FILE * file = std::fopen(path.c_str(), "rb");
    if (!file) return false;
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::vector<unsigned char> buffer(8 << 20);
    size_t n = 0;
    while ((n = std::fread(buffer.data(), 1, buffer.size(), file)) > 0)
        CC_SHA256_Update(&context, buffer.data(), static_cast<CC_LONG>(n));
    const bool ok = !std::ferror(file);
    std::fclose(file);
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    out.clear();
    for (const unsigned char byte : digest) {
        char hex[3];
        std::snprintf(hex, sizeof hex, "%02x", byte);
        out += hex;
    }
    return ok;
}

std::string job_id(const std::string & model, const std::string & variant) {
    return model + ":" + variant;
}

}  // namespace

Json DownloadManager::Progress::json() const {
    Json out = Json::object();
    out.set("id", id).set("model", model).set("variant", variant).set("state", state_name(state));
    out.set("received", received).set("total", total).set("bytes_per_s", bytes_per_s);
    if (!error.empty()) out.set("error", error);
    return out;
}

DownloadManager::DownloadManager(ModelLibrary & library, std::unique_ptr<Transport> transport)
    : library_(library), transport_(std::move(transport)) {
    worker_ = std::thread([this] { run(); });
}

DownloadManager::~DownloadManager() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        // What has arrived stays staged, and restore() picks it up next time.
        if (running_ && running_->cancel) running_->cancel->cancel();
    }
    wake_.notify_all();
    worker_.join();
}

void DownloadManager::on_installed(std::function<void(const std::string &, const std::string &)> callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_installed_ = std::move(callback);
}

void DownloadManager::restore() {
    std::error_code ec;
    for (const auto & entry : fs::directory_iterator(library_.staging_root(), ec)) {
        const std::string dir = entry.path().string() + "/";
        std::ifstream in(dir + "download.json");
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Json saved;
        std::string error;
        const ModelSpec * model = nullptr;
        const Variant * variant = nullptr;
        if (Json::parse(text, saved, error)) {
            model = catalog_find(saved["model"].as_string());
            variant = model ? model->variant(saved["variant"].as_string()) : nullptr;
        }
        // An import that did not finish, or a download of something no longer in the catalog or already installed.
        if (!variant || library_.find(model->id, variant->id)) {
            fs::remove_all(entry.path(), ec);
            continue;
        }
        auto restored = std::make_shared<Job>();
        restored->model = model;
        restored->variant = variant;
        restored->staged = dir;
        restored->progress.id = job_id(model->id, variant->id);
        restored->progress.model = model->id;
        restored->progress.variant = variant->id;
        restored->progress.total = variant->bytes();
        restored->progress.state = State::paused;
        for (const FileSpec & file : variant->files) {
            const int64_t whole = file_size(dir + file.name());
            const int64_t part = file_size(dir + file.name() + ".part");
            restored->progress.received += whole == file.bytes ? whole : std::max<int64_t>(part, 0);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!find_job(restored->progress.id)) jobs_.push_back(restored);
    }
}

std::shared_ptr<DownloadManager::Job> DownloadManager::find_job(const std::string & id) const {
    for (const auto & j : jobs_)
        if (j->progress.id == id) return j;
    return nullptr;
}

void DownloadManager::queue_locked(const std::shared_ptr<Job> & job) {
    job->progress.state = State::queued;
    job->progress.error.clear();
    job->progress.bytes_per_s = 0;
    job->pausing = false;
    if (std::find(queue_.begin(), queue_.end(), job) == queue_.end()) queue_.push_back(job);
    wake_.notify_all();
}

bool DownloadManager::start(const std::string & model_id, const std::string & variant_id, Progress & out,
                            std::string & error) {
    const ModelSpec * model = catalog_find(model_id);
    if (!model) {
        error = "no model \"" + model_id + "\" in the catalog";
        return false;
    }
    const Variant * variant = variant_id.empty() ? &model->variants.front() : model->variant(variant_id);
    if (!variant) {
        error = model->name + " has no variant \"" + variant_id + "\"";
        return false;
    }
    const std::string id = job_id(model->id, variant->id);
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto existing = find_job(id); existing && existing->progress.state != State::done) {
        if (existing->progress.state == State::paused || existing->progress.state == State::failed)
            queue_locked(existing);
        out = existing->progress;
        return true;
    }
    if (library_.find(model->id, variant->id)) {
        error = model->name + " (" + variant->id + ") is downloaded already";
        return false;
    }
    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(), [&](const auto & j) { return j->progress.id == id; }),
                jobs_.end());
    auto job = std::make_shared<Job>();
    job->model = model;
    job->variant = variant;
    std::string folder = id;
    std::replace(folder.begin(), folder.end(), ':', '@');
    job->staged = library_.staging_root() + folder + "/";
    job->progress.id = id;
    job->progress.model = model->id;
    job->progress.variant = variant->id;
    job->progress.total = variant->bytes();
    jobs_.push_back(job);
    queue_locked(job);
    out = job->progress;
    return true;
}

bool DownloadManager::pause(const std::string & id, std::string & error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto j = find_job(id);
    if (!j || (j->progress.state != State::queued && j->progress.state != State::running)) {
        error = "no download " + id + " under way";
        return false;
    }
    if (j == running_) {
        j->pausing = true;
        if (j->cancel) j->cancel->cancel();
    } else {
        queue_.erase(std::remove(queue_.begin(), queue_.end(), j), queue_.end());
    }
    j->progress.state = State::paused;
    j->progress.bytes_per_s = 0;
    return true;
}

bool DownloadManager::resume(const std::string & id, std::string & error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto j = find_job(id);
    if (!j || (j->progress.state != State::paused && j->progress.state != State::failed)) {
        error = "no paused download " + id;
        return false;
    }
    // A pause the download thread has not noticed yet: it queues the job again once it has.
    if (j == running_) j->pausing = false;
    queue_locked(j);
    return true;
}

bool DownloadManager::cancel(const std::string & id, std::string & error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto j = find_job(id);
    if (!j || j->progress.state == State::done) {
        error = "no download " + id;
        return false;
    }
    queue_.erase(std::remove(queue_.begin(), queue_.end(), j), queue_.end());
    jobs_.erase(std::remove(jobs_.begin(), jobs_.end(), j), jobs_.end());
    if (j == running_) {
        // The download thread deletes the staged files once the transfer has stopped writing them.
        j->cancelling = true;
        if (j->cancel) j->cancel->cancel();
    } else {
        std::error_code ec;
        fs::remove_all(j->staged, ec);
    }
    return true;
}

std::vector<DownloadManager::Progress> DownloadManager::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Progress> out;
    for (const auto & j : jobs_) out.push_back(j->progress);
    return out;
}

std::optional<DownloadManager::Progress> DownloadManager::get(const std::string & id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (auto j = find_job(id)) return j->progress;
    return std::nullopt;
}

std::optional<DownloadManager::Progress> DownloadManager::find(const std::string & model,
                                                               const std::string & variant) const {
    return get(job_id(model, variant));
}

void DownloadManager::wait_idle() {
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [&] { return stopping_ || (queue_.empty() && !running_); });
}

void DownloadManager::set_bytes(Job & job, int64_t received) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - job.sampled).count();
    job.progress.received = received;
    // A few samples a second keep the speed from jittering.
    if (elapsed >= 0.25) {
        const double rate = static_cast<double>(received - job.sampled_bytes) / elapsed;
        job.progress.bytes_per_s = job.progress.bytes_per_s == 0 ? rate : 0.8 * job.progress.bytes_per_s + 0.2 * rate;
        job.sampled = now;
        job.sampled_bytes = received;
    }
}

bool DownloadManager::fetch(Job & job, std::string & error) {
    std::error_code ec;
    fs::create_directories(job.staged, ec);
    {
        Json saved = Json::object();
        saved.set("model", job.model->id).set("variant", job.variant->id);
        std::ofstream out(job.staged + "download.json", std::ios::trunc);
        out << saved.dump() << "\n";
    }
    int64_t done_bytes = 0;
    for (const FileSpec & file : job.variant->files) {
        const std::string whole = job.staged + file.name();
        if (file_size(whole) == file.bytes) {
            done_bytes += file.bytes;
            continue;
        }
        const std::string part = whole + ".part";
        int64_t offset = std::max<int64_t>(file_size(part), 0);
        if (offset > file.bytes) offset = 0;
        if (offset < file.bytes) {
            Cancellation cancel;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (job.pausing || job.cancelling || stopping_) return false;
                job.cancel = &cancel;
                job.sampled = std::chrono::steady_clock::now();
                job.sampled_bytes = done_bytes + offset;
            }
            const std::function<void(int64_t)> progress = [&](int64_t bytes) { set_bytes(job, done_bytes + bytes); };
            const bool fetched = transport_->fetch(file.url(), part, offset, progress, cancel, error);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                job.cancel = nullptr;
            }
            if (!fetched) return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job.progress.state = State::verifying;
            job.progress.bytes_per_s = 0;
        }
        std::string digest;
        if (!sha256(part, digest) || digest != file.sha256) {
            fs::remove(part, ec);
            error = file.name() + " does not match its checksum; try again";
            return false;
        }
        fs::rename(part, whole, ec);
        if (ec) {
            error = "cannot rename " + part + ": " + ec.message();
            return false;
        }
        done_bytes += file.bytes;
        std::lock_guard<std::mutex> lock(mutex_);
        if (job.progress.state == State::verifying) job.progress.state = State::running;
    }
    return library_.install(*job.model, *job.variant, job.staged, error);
}

void DownloadManager::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
        if (stopping_) break;
        std::shared_ptr<Job> job = queue_.front();
        queue_.pop_front();
        running_ = job;
        job->progress.state = State::running;
        lock.unlock();

        std::string error;
        const bool installed = fetch(*job, error);

        lock.lock();
        running_ = nullptr;
        job->progress.bytes_per_s = 0;
        if (installed) {
            job->progress.state = State::done;
            job->progress.received = job->progress.total;
            const auto callback = on_installed_;
            const std::string model = job->model->id;
            const std::string variant = job->variant->id;
            lock.unlock();
            std::fprintf(stderr, "asrd: downloaded %s:%s\n", model.c_str(), variant.c_str());
            if (callback) callback(model, variant);
            lock.lock();
        } else if (job->cancelling) {
            std::error_code ec;
            fs::remove_all(job->staged, ec);
        } else if (job->pausing || stopping_) {
            job->progress.state = State::paused;
        } else if (job->progress.state == State::queued) {
            // Paused and resumed while the transfer was stopping: it goes on from where it is.
            if (std::find(queue_.begin(), queue_.end(), job) == queue_.end()) queue_.push_back(job);
        } else {
            job->progress.state = State::failed;
            job->progress.error = error;
            std::fprintf(stderr, "asrd: download of %s failed: %s\n", job->progress.id.c_str(), error.c_str());
        }
        job->pausing = false;
        if (queue_.empty()) idle_.notify_all();
    }
    idle_.notify_all();
}

}  // namespace asrd
