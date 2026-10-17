// A loaded model ready for dictations: the recognizer, the voice activity detector that cuts its utterances (which may
// come from another runtime), and the settings each recognition takes. A dictation holds its session to the end, so
// loading another model never frees one under it.
#pragma once

#include "runtime.h"

#include <memory>
#include <string>

namespace asrd {

struct Session {
    std::shared_ptr<Recognizer> recognizer;
    std::shared_ptr<VoiceDetector> detector;
    std::string model_id;
    std::string variant_id;
    std::string runtime_id;
    std::string file;                 // the model's file name, for /health
    Settings request_settings;
};

}  // namespace asrd
