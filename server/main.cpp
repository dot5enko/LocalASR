// asrd: local dictation server. Runs speech recognition models through the runtimes compiled in (speech.cpp: ggml,
// Metal on Apple silicon), keeps the model library and its downloads, and streams transcripts back over a WebSocket
// while the client streams microphone audio in.
//
//   asrd [--host 127.0.0.1] [--port 8765] [--models DIR] [-v]
//   asrd pull ID[:VARIANT]...   downloads models (and what they require) into the library and exits
//
// asrd starts loading the catalog's default model and listens before it has loaded, and also when it is not downloaded
// yet: the app reads the catalog, downloads through asrd, and loads the model it was set to (POST /v1/models/load).
// A load waits for its files, and goes ahead once they are there.
//
// GET  /health                  {"status":"ok"|"loading"|"no_model"|"failed"|"unloaded","id":...,"model":...,"runtime":...}
// GET  /v1/system               {"host":{...},"runtimes":[{"id","version","formats","devices","load_settings"}]}
// GET  /v1/models               the catalog's models, each variant with installed, runnable_by, download
// POST /v1/models/load          {"model","variant","runtime","settings":{}} or {"path"}; switches without a restart
// POST /v1/models/unload
// DELETE /v1/models/ID/VARIANT
// GET  /v1/downloads            [{"id":"ID:VARIANT","state":"queued|running|verifying|paused|done|failed","received","total","bytes_per_s"}]
// POST /v1/downloads            {"model","variant"}; also queues what the model requires
// POST /v1/downloads/ID:VARIANT/pause | /resume      DELETE /v1/downloads/ID:VARIANT
// WS   /v1/dictate?language=en,ru&prompt=     language: auto, one tag, or a comma-separated set to keep to
//   client -> binary frames   16-bit little-endian mono PCM at 16 kHz
//   client -> {"type":"stop"} end of audio; a final event follows and the server closes the socket
//   client -> {"type":"reset"}   drop the text so far and keep listening
//   client -> {"type":"cancel"}
//   server -> {"type":"ready","model":"..."}
//   server -> {"type":"partial","committed":"...","live":"...","epoch":0}   show committed + live; epoch counts resets
//   server -> {"type":"final","text":"...","language":"en","audio_s":4.2,"finalize_ms":180}
//   server -> {"type":"error","message":"..."}
#include "api.h"
#include "downloads.h"
#include "library.h"
#include "model_manager.h"
#include "registry.h"
#include "transport.h"

#include <httplib.h>

#include <pthread.h>
#include <sys/socket.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Args {
    std::string host = "127.0.0.1";
    int port = 8765;
    std::string models_dir = asrd::ModelLibrary::default_root();
    bool verbose = false;
    std::vector<std::string> pull;   // asrd pull
};

[[noreturn]] void usage(const char * error) {
    if (error) std::fprintf(stderr, "asrd: %s\n\n", error);
    std::fprintf(stderr,
                 "usage: asrd [options]\n"
                 "       asrd pull ID[:VARIANT]...   download models into the library, then exit\n"
                 "  --host HOST        address to listen on (127.0.0.1)\n"
                 "  --port PORT        port to listen on (8765)\n"
                 "  --models DIR       the model library (~/Library/Application Support/LocalASR/models)\n"
                 "  -v                 log the runtime's info messages and each dictation\n");
    std::exit(error ? 2 : 0);
}

Args parse(int argc, char ** argv) {
    Args args;
    int i = 1;
    const bool pull = argc > 1 && std::string(argv[1]) == "pull";
    if (pull) i = 2;
    for (; i < argc; ++i) {
        const std::string flag = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage(("missing value for " + flag).c_str());
            return argv[++i];
        };
        if (flag == "--host") args.host = value();
        else if (flag == "--port") args.port = std::atoi(value().c_str());
        else if (flag == "--models") args.models_dir = value();
        else if (flag == "-v") args.verbose = true;
        else if (flag == "-h" || flag == "--help") usage(nullptr);
        else if (pull && flag[0] != '-') args.pull.push_back(flag);
        else usage(("unknown option " + flag).c_str());
    }
    if (pull && args.pull.empty()) args.pull.push_back(asrd::catalog_default_recognizer().id);
    return args;
}

std::pair<std::string, std::string> split_variant(const std::string & name) {
    const size_t colon = name.find(':');
    if (colon == std::string::npos) return {name, ""};
    return {name.substr(0, colon), name.substr(colon + 1)};
}

std::string megabytes(int64_t bytes) {
    char out[32];
    std::snprintf(out, sizeof out, "%.0f MB", static_cast<double>(bytes) / 1e6);
    return out;
}

// asrd pull: what the models require first, then the models; progress on stderr.
int pull(const Args & args, asrd::ModelLibrary & library) {
    asrd::DownloadManager downloads(library, asrd::make_url_session_transport());
    downloads.restore();
    std::vector<std::pair<std::string, std::string>> wanted;
    for (const std::string & name : args.pull) {
        const auto [model, variant] = split_variant(name);
        const asrd::ModelSpec * spec = asrd::catalog_find(model);
        if (!spec) {
            std::string known;
            for (const asrd::ModelSpec & m : asrd::catalog()) known += " " + m.id;
            std::fprintf(stderr, "asrd: no model %s in the catalog; it has%s\n", model.c_str(), known.c_str());
            return 2;
        }
        for (const std::string & requirement : spec->requires) wanted.emplace_back(requirement, "");
        wanted.emplace_back(model, variant);
    }
    int code = 0;
    for (const auto & [model, variant] : wanted) {
        if (library.find(model, variant)) {
            std::fprintf(stderr, "ok      %s%s\n", model.c_str(), variant.empty() ? "" : (":" + variant).c_str());
            continue;
        }
        asrd::DownloadManager::Progress progress;
        std::string error;
        if (!downloads.start(model, variant, progress, error)) {
            std::fprintf(stderr, "asrd: %s\n", error.c_str());
            code = 1;
            continue;
        }
        if (progress.state == asrd::DownloadManager::State::paused) downloads.resume(progress.id, error);
        std::thread watch([&downloads, id = progress.id] {
            while (true) {
                const auto p = downloads.get(id);
                if (!p || p->state == asrd::DownloadManager::State::done ||
                    p->state == asrd::DownloadManager::State::failed)
                    break;
                std::fprintf(stderr, "\rfetch   %s  %s of %s  %.1f MB/s   ", id.c_str(), megabytes(p->received).c_str(),
                             megabytes(p->total).c_str(), p->bytes_per_s / 1e6);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });
        downloads.wait_idle();
        watch.join();
        const auto done = downloads.get(progress.id);
        if (done && done->state == asrd::DownloadManager::State::done) {
            std::fprintf(stderr, "\rdone    %s                                        \n", progress.id.c_str());
        } else {
            std::fprintf(stderr, "\nasrd: %s: %s\n", progress.id.c_str(), done ? done->error.c_str() : "stopped");
            code = 1;
        }
    }
    return code;
}

}  // namespace

int main(int argc, char ** argv) {
    const Args args = parse(argc, argv);

    // Signals go to one thread that stops the server, so the models are freed before exit: on Metal a process that
    // exits with a model loaded aborts. They are blocked before any other thread starts, which inherits the mask.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    asrd::ModelLibrary library(args.models_dir);
    library.migrate();
    if (!args.pull.empty()) {
        pthread_sigmask(SIG_UNBLOCK, &signals, nullptr);
        return pull(args, library);
    }

    // Runtimes start on the main thread, before any other: ggml sets the process environment up as it starts.
    asrd::RuntimeRegistry registry;
    asrd::register_builtin_runtimes(registry, args.verbose);
    if (registry.all().empty()) {
        std::fprintf(stderr, "asrd: no runtime is available\n");
        return 1;
    }

    asrd::DownloadManager downloads(library, asrd::make_url_session_transport());
    downloads.restore();
    asrd::ModelManager models(registry, library);
    downloads.on_installed([&models](const std::string & model, const std::string & variant) {
        models.installed(model, variant);
    });

    asrd::ModelManager::LoadRequest initial;
    initial.model = asrd::catalog_default_recognizer().id;
    std::string error;
    if (!models.load(initial, error)) {
        std::fprintf(stderr, "asrd: %s\n", error.c_str());
        return 1;
    }

    asrd::Services services{registry, library, downloads, models, args.verbose};
    httplib::Server http;
    // Not httplib's SO_REUSEPORT: a second asrd would share the port and take some of the connections.
    http.set_socket_options([](socket_t sock) {
        int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    });
    asrd::install_routes(http, services);

    std::thread stopper([&] {
        int signal = 0;
        sigwait(&signals, &signal);
        http.stop();
    });

    int code = 0;
    if (!http.bind_to_port(args.host, args.port)) {
        std::fprintf(stderr, "asrd: cannot listen on %s:%d\n", args.host.c_str(), args.port);
        code = 1;
    } else {
        std::fprintf(stderr, "asrd: listening on http://%s:%d (dictations on ws://%s:%d/v1/dictate)\n",
                     args.host.c_str(), args.port, args.host.c_str(), args.port);
        if (!http.listen_after_bind()) code = 1;
    }
    // Wakes the stopper if the server ended on its own; after a signal it has already returned.
    pthread_kill(stopper.native_handle(), SIGTERM);
    stopper.join();
    // The models go first (~ModelManager), then the downloads stop with what they have staged, then the runtimes.
    return code;
}
