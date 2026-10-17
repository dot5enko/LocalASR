#include "dictation.h"

#include "json.h"
#include "languages.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace asrd {
namespace {

size_t to_samples(double seconds) {
    return seconds <= 0 ? 0 : static_cast<size_t>(std::llround(seconds * kSampleRate));
}

// Languages written without spaces between words, whose utterances join without one.
bool joins_without_space(const std::string & language) {
    return language == "zh" || language == "yue" || language == "ja" || language == "th";
}

}  // namespace

Dictation::Dictation(std::shared_ptr<const Session> session, DictationOptions options, Emit emit)
    : session_(std::move(session)),
      model_(session_->recognizer->info()),
      options_(std::move(options)),
      emit_(std::move(emit)) {}

Dictation::~Dictation() {
    cancel();
    join();
}

bool Dictation::start(std::string & error) {
    if (model_.streams) {
        // A model that streams takes audio as it comes rather than utterance by utterance; no runtime has one yet.
        error = model_.name + " streams, which dictation does not take yet";
        return false;
    }
    if (!start_detection(error)) return false;
    worker_ = std::thread([this] { run(); });
    return true;
}

bool Dictation::start_detection(std::string & error) {
    detection_ = session_->detector->open(kSampleRate, options_.min_silence_ms, options_.max_utterance_s, error);
    if (!detection_) error = "vad: " + error;
    return detection_ != nullptr;
}

void Dictation::push_pcm16(const char * bytes, size_t n) {
    const auto sample = [](unsigned char lo, unsigned char hi) {
        return static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(lo | (hi << 8)))) / 32768.0f;
    };
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || cancelled_ || n == 0) return;
    size_t i = 0;
    if (odd_byte_ >= 0) {
        audio_.push_back(sample(static_cast<unsigned char>(odd_byte_), static_cast<unsigned char>(bytes[0])));
        odd_byte_ = -1;
        i = 1;
    }
    for (; i + 1 < n; i += 2)
        audio_.push_back(sample(static_cast<unsigned char>(bytes[i]), static_cast<unsigned char>(bytes[i + 1])));
    if (i < n) odd_byte_ = static_cast<unsigned char>(bytes[i]);
    wake_.notify_one();
}

void Dictation::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || cancelled_) return;
    stopping_ = true;
    stop_time_ = std::chrono::steady_clock::now();
    // The final reading of the last utterance supersedes a live one under way.
    if (running_ && running_live_) running_->cancel();
    wake_.notify_one();
}

void Dictation::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || cancelled_) return;
    reset_pending_ = true;
    reset_at_ = audio_.size();
    // A live reading under way reads audio the reset discards.
    if (running_ && running_live_) running_->cancel();
    wake_.notify_one();
}

bool Dictation::reset_pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    return reset_pending_;
}

void Dictation::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    if (running_) running_->cancel();
    wake_.notify_one();
}

void Dictation::join() {
    if (worker_.joinable()) worker_.join();
}

void Dictation::run() {
    size_t fed = 0;
    size_t live_at = 0;
    bool ended = false;
    while (step(fed, live_at, ended)) {
    }
    done_ = true;
}

// One turn of the worker: give the VAD what arrived, commit the utterances it closed, re-read the one under way.
// False once the dictation is over.
bool Dictation::step(size_t & fed, size_t & live_at, bool & ended) {
    std::vector<float> fresh;
    bool stopping = false;
    bool reset = false;
    std::chrono::steady_clock::time_point stop_time;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [&] { return cancelled_ || stopping_ || reset_pending_ || audio_.size() > fed; });
        if (cancelled_) return false;
        stopping = stopping_;
        stop_time = stop_time_;
        reset = reset_pending_;
        reset_pending_ = false;
        if (reset) fed = reset_at_;
        fresh.assign(audio_.begin() + static_cast<std::ptrdiff_t>(fed), audio_.end());
    }
    if (reset) {
        // Everything heard so far is dropped: a fresh VAD starts at the reset, so an utterance under way is cut there.
        std::string error;
        if (!start_detection(error)) {
            emit_error(error);
            return false;
        }
        base_ = fed;
        live_at = fed;
        regions_done_ = 0;
        committed_.clear();
        live_.clear();
        ++epoch_;
        emit_partial();
    }
    if (!fresh.empty()) {
        std::string error;
        if (!detection_->push({fresh.data(), fresh.size(), kSampleRate}, error)) {
            emit_error("vad: " + error);
            return false;
        }
        fed += fresh.size();
    }
    if (stopping && !ended) {
        std::string error;
        if (!detection_->end(error)) {
            emit_error("vad: " + error);
            return false;
        }
        ended = true;
    }

    const size_t regions = detection_->region_count();
    for (; regions_done_ < regions; ++regions_done_) {
        const auto [start, end] = detection_->region(regions_done_);
        std::string text;
        std::string language;
        const Outcome outcome =
            recognize(base_ + to_samples(start), base_ + to_samples(end), live_, false, text, &language);
        if (outcome != Outcome::ok) return false;
        if (reset_pending()) return true;
        commit(text, language);
        live_.clear();
        live_at = fed;
        emit_partial();
    }

    if (stopping) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stop_time);
        char audio_s[32];
        std::snprintf(audio_s, sizeof audio_s, "%.2f", static_cast<double>(fed - base_) / kSampleRate);
        emit_(R"({"type":"final","text":)" + json_quote(committed_) + R"(,"language":)" + json_quote(language_) +
              R"(,"audio_s":)" + audio_s + R"(,"finalize_ms":)" + std::to_string(ms.count()) + "}");
        return false;
    }

    if (const std::optional<double> start = detection_->speaking_since()) {
        if (fed - live_at >= static_cast<size_t>(options_.live_interval_s * kSampleRate)) {
            std::string text;
            const Outcome outcome = recognize(base_ + to_samples(*start), fed, live_, true, text, nullptr);
            live_at = fed;
            if (reset_pending()) return true;
            if (outcome == Outcome::ok && text != live_) {
                live_ = text;
                emit_partial();
            } else if (outcome == Outcome::failed) {
                return false;
            }
        }
    } else if (!live_.empty()) {
        // The VAD dropped what it heard as too short to be speech.
        live_.clear();
        emit_partial();
    }
    return true;
}

// Recognizes audio_[from, to) in one of the dictation's languages. A failure other than a cancellation is reported
// to the client.
Outcome Dictation::recognize(size_t from, size_t to, const std::string & draft, bool live, std::string & text,
                                   std::string * language) {
    text.clear();
    std::vector<float> samples;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        to = std::min(to, audio_.size());
        if (from < to) samples.assign(audio_.begin() + static_cast<std::ptrdiff_t>(from),
                                      audio_.begin() + static_cast<std::ptrdiff_t>(to));
    }
    // A tenth of a second holds no word, and Qwen3-ASR refuses audio shorter than two mel frames.
    if (samples.size() < kSampleRate / 10) return Outcome::ok;

    // A model that finds the language itself (Parakeet) takes no language to keep to.
    static const std::vector<std::string> any;
    const std::vector<std::string> & allowed = model_.language_steers ? options_.languages : any;
    const std::string forced = allowed.size() == 1 ? allowed.front() : "";
    std::string heard;
    Outcome outcome = transcribe(samples, forced, draft, live, text, heard);
    // Detected outside the set (or as a language the model does not have): read it again as the set's language that
    // its script points to. Empty text is silence or noise, which a forced language would only make up words for.
    if (outcome == Outcome::ok && allowed.size() > 1 && !text.empty() && !language_in(heard, allowed)) {
        const std::string closest = closest_language(text, allowed);
        std::fprintf(stderr, "asrd: heard %s, outside %s; reading again as %s\n", heard.empty() ? "?" : heard.c_str(),
                     options_.language_list.c_str(), closest.c_str());
        outcome = transcribe(samples, closest, draft, live, text, heard);
    }
    if (outcome == Outcome::ok && language) *language = heard;
    return outcome;
}

Outcome Dictation::transcribe(const std::vector<float> & samples, const std::string & language,
                              const std::string & draft, bool live, std::string & text, std::string & heard) {
    text.clear();
    heard.clear();
    RecognizeRequest request;
    request.audio = {samples.data(), samples.size(), kSampleRate};
    request.language = language;
    request.prompt = options_.prompt;
    request.draft = draft;
    request.settings = session_->request_settings;
    Cancellation cancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (cancelled_ || (live && stopping_)) return Outcome::cancelled;
        running_ = &cancel;
        running_live_ = live;
    }
    Transcript transcript;
    std::string error;
    const Outcome outcome = session_->recognizer->transcribe(request, cancel, transcript, error);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = nullptr;
    }
    if (outcome == Outcome::ok) {
        text = transcript.text;
        heard = transcript.language;
    } else if (outcome == Outcome::failed) {
        emit_error("recognition: " + error);
    }
    return outcome;
}

void Dictation::commit(const std::string & text, const std::string & language) {
    if (!language.empty()) language_ = language;
    if (text.empty()) return;
    if (!committed_.empty() && !joins_without_space(language_)) committed_ += ' ';
    committed_ += text;
}

void Dictation::emit_partial() {
    // The live text carries the separator it needs after the committed text, so a client shows committed + live.
    std::string live = live_;
    if (!committed_.empty() && !live.empty() && !joins_without_space(language_)) live.insert(0, " ");
    emit_(R"({"type":"partial","committed":)" + json_quote(committed_) + R"(,"live":)" + json_quote(live) +
          R"(,"epoch":)" + std::to_string(epoch_) + "}");
}

void Dictation::emit_error(const std::string & message) {
    std::fprintf(stderr, "asrd: %s\n", message.c_str());
    emit_(R"({"type":"error","message":)" + json_quote(message) + "}");
}

}  // namespace asrd
