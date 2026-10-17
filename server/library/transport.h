// Moves one file over HTTPS. The download manager queues, stages, hashes and installs; a transport only fetches.
#pragma once

#include "runtime.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace asrd {

class Transport {
public:
    virtual ~Transport() = default;
    // Writes `url` into `dest` from byte `offset` on (a range request, to resume what an earlier fetch left; a server
    // that sends the whole file instead starts the file over). `progress` gets the bytes `dest` holds as they come.
    // False with `error` set on failure, or with `cancel` cancelled.
    virtual bool fetch(const std::string & url, const std::string & dest, int64_t offset,
                       const std::function<void(int64_t bytes)> & progress, Cancellation & cancel,
                       std::string & error) = 0;
};

// Foundation's URLSession: the system's proxies and certificates, as every other app on the Mac uses them.
std::unique_ptr<Transport> make_url_session_transport();

}  // namespace asrd
