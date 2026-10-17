// The Mac itself, as the Hardware settings show it; the devices each runtime runs on come from the runtimes.
#pragma once

#include "json.h"

#include <cstdint>
#include <string>

namespace asrd {

struct HostInfo {
    std::string chip;            // "Apple M3"
    std::string model;           // "Mac15,3"
    std::string os;              // "15.6"
    int performance_cores = 0;
    int efficiency_cores = 0;
    uint64_t memory = 0;

    Json json() const;
};

HostInfo host_info();

}  // namespace asrd
