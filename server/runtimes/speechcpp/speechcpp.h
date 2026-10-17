// speech.cpp (ggml; Metal on Apple silicon) as an asrd runtime: GGUF models of the families it was built with.
#pragma once

#include "runtime.h"

#include <memory>

namespace asrd {

// Null with `error` set when the library linked in speaks another major version of its C API than this was written for.
// Sets ggml up for the process, so it runs on the main thread before any other thread starts.
std::unique_ptr<Runtime> make_speechcpp_runtime(bool verbose, std::string & error);

}  // namespace asrd
