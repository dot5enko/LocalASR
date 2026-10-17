// asrd's HTTP and WebSocket routes over the services main() builds.
#pragma once

#include "downloads.h"
#include "library.h"
#include "model_manager.h"
#include "registry.h"

#include <httplib.h>

#include <string>

namespace asrd {

struct Services {
    RuntimeRegistry & registry;
    ModelLibrary & library;
    DownloadManager & downloads;
    ModelManager & models;
    bool verbose = false;
};

void install_routes(httplib::Server & http, Services & services);

// GET /v1/models: the catalog's models and the imported ones, each with its variants and their state.
Json models_json(const Services & services);

// "true", "12", "0.5" or text, as the setting a command line or query string gives.
Value parse_value(const std::string & text);

}  // namespace asrd
