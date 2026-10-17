// A dictation: microphone PCM arrives a frame at a time, and text goes back as it is heard.
//
// The session's voice activity detector cuts the audio into utterances. An utterance the detector has closed is
// recognized once and its text is committed for good. The utterance still being spoken is re-recognized from its
// start every time enough new audio has come in, each reading given the previous one as its draft so the model only
// checks what it already wrote; that reading is the live text, which may still change. On stop, the detector closes
// the last utterance, it is recognized, and the committed texts joined are the final transcript. A reset drops
// everything heard so far and starts over from the audio after it.
//
// Which runtime runs the models makes no difference here: the dictation sees the Recognizer and VoiceDetector
// interfaces only.
#pragma once

#include "session.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace asrd {

constexpr int kSampleRate = 16000;

struct DictationOptions {
    std::vector<std::string> languages; // the languages to recognize in; none lets the model detect any of its own
    std::string language_list;          // the same, comma-separated, for the log
    std::string prompt;                 // names and terms to steer recognition toward
    double live_interval_s = 0.3;       // new audio needed before the live text is read again
    int min_silence_ms = 500;           // a pause this long ends an utterance
    double max_utterance_s = 15.0;      // the detector cuts longer speech at its longest silence
};

class Dictation {
public:
    using Emit = std::function<void(const std::string & event)>;

    Dictation(std::shared_ptr<const Session> session, DictationOptions options, Emit emit);
    ~Dictation();
    Dictation(const Dictation &) = delete;
    Dictation & operator=(const Dictation &) = delete;

    bool start(std::string & error);
    // 16-bit little-endian mono PCM at kSampleRate, any number of bytes (an odd byte waits for the next frame).
    void push_pcm16(const char * bytes, size_t n);
    // Ends the audio; a "final" event follows, after which the dictation is done.
    void stop();
    // Clears the transcript and keeps listening; partials after it carry the next epoch.
    void reset();
    // Drops the dictation without a final event.
    void cancel();
    bool done() const { return done_; }
    void join();

private:
    bool start_detection(std::string & error);
    bool reset_pending();
    void run();
    bool step(size_t & fed, size_t & live_at, bool & ended);
    Outcome recognize(size_t from, size_t to, const std::string & draft, bool live, std::string & text,
                      std::string * language);
    Outcome transcribe(const std::vector<float> & samples, const std::string & language, const std::string & draft,
                       bool live, std::string & text, std::string & heard);
    void commit(const std::string & text, const std::string & language);
    void emit_partial();
    void emit_error(const std::string & message);

    const std::shared_ptr<const Session> session_;
    const ModelInfo & model_;
    const DictationOptions options_;
    const Emit emit_;
    std::unique_ptr<DetectionStream> detection_;
    std::thread worker_;

    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<float> audio_;          // everything heard, at kSampleRate; only grows
    bool stopping_ = false;
    bool cancelled_ = false;
    bool reset_pending_ = false;
    size_t reset_at_ = 0;
    Cancellation * running_ = nullptr;  // the recognition under way, so stop/cancel can cut it short
    bool running_live_ = false;
    std::chrono::steady_clock::time_point stop_time_;
    int odd_byte_ = -1;

    // Owned by the worker thread.
    size_t base_ = 0;                   // the sample the current detection started at: 0, or the last reset
    size_t regions_done_ = 0;
    int epoch_ = 0;
    std::string committed_;
    std::string live_;
    std::string language_;

    std::atomic<bool> done_{false};
};

}  // namespace asrd
