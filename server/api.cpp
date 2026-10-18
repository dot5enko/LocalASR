#include "api.h"

#include "dictation.h"
#include "hardware.h"
#include "languages.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace asrd {
namespace {

void reply(httplib::Response & res, int status, const Json & body) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void fail(httplib::Response & res, int status, const std::string & message) {
    reply(res, status, Json::object().set("error", message));
}

bool body_json(const httplib::Request & req, httplib::Response & res, Json & out) {
    std::string error;
    if (req.body.empty()) {
        out = Json::object();
        return true;
    }
    if (!Json::parse(req.body, out, error) || !out.is_object()) {
        fail(res, 400, "the body is not a JSON object: " + error);
        return false;
    }
    return true;
}

// The memory a model's weights would take from the device a load picks by default (the first GPU, else the CPU).
bool fits(Runtime & runtime, int64_t bytes) {
    const std::vector<Device> devices = runtime.devices();
    const Device * device = nullptr;
    for (const Device & d : devices)
        if (!device && d.kind != DeviceKind::cpu) device = &d;
    if (!device && !devices.empty()) device = &devices.front();
    return !device || device->memory_free == 0 || static_cast<uint64_t>(bytes) <= device->memory_free;
}

Json runnable_by(const Services & services, const std::string & format, const std::string & architecture) {
    Json out = Json::array();
    for (Runtime * runtime : services.registry.for_model(format, architecture)) out.push(runtime->info().id);
    return out;
}

Json download_json(const Services & services, const std::string & model, const std::string & variant) {
    const auto progress = services.downloads.find(model, variant);
    return progress && progress->state != DownloadManager::State::done ? progress->json() : Json();
}

std::string unknown_language(const std::vector<std::string> & languages, const ModelInfo & info) {
    for (const std::string & language : languages) {
        if (!language_in(language, info.languages)) {
            std::string known;
            for (const std::string & tag : info.languages) known += (known.empty() ? "" : ", ") + tag;
            return info.name + " does not know the language \"" + language + "\"; it knows " + known;
        }
    }
    return "";
}

std::string error_event(const std::string & message) {
    return Json::object().set("type", "error").set("message", message).dump();
}

void serve_dictation(Services & services, const httplib::Request & req, httplib::ws::WebSocket & ws) {
    std::shared_ptr<const Session> session = services.models.current();
    // A dictation started while the model loads (the app's first, as asrd starts) waits for it; the client keeps the
    // audio until `ready`.
    for (int waited_ms = 0; !session && services.models.state() == ModelManager::State::loading && waited_ms < 60000;
         waited_ms += 100) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        session = services.models.current();
    }
    if (!session) {
        const ModelManager::State state = services.models.state();
        ws.send(error_event(state == ModelManager::State::loading
                                ? "The model is still loading."
                                : "No model is loaded; see the app's Models window."));
        ws.close();
        return;
    }
    const ModelInfo & info = session->recognizer->info();
    DictationOptions options;
    options.languages = parse_languages(req.get_param_value("language"));
    for (const std::string & language : options.languages)
        options.language_list += (options.language_list.empty() ? "" : ",") + language;
    if (const std::string error = unknown_language(options.languages, info); !error.empty()) {
        ws.send(error_event(error));
        ws.close();
        return;
    }
    options.prompt = req.get_param_value("prompt");

    Dictation dictation(session, options, [&ws](const std::string & event) { ws.send(event); });
    std::string error;
    if (!dictation.start(error)) {
        ws.send(error_event(error));
        ws.close();
        return;
    }
    ws.send(Json::object().set("type", "ready").set("model", info.name).dump());
    if (services.verbose) {
        std::fprintf(stderr, "asrd: dictation opened (%s)\n",
                     options.languages.empty() ? "any language" : options.language_list.c_str());
    }

    // A short timeout lets the loop notice that the dictation has finished and close the socket.
    ws.set_read_timeout(std::chrono::milliseconds(100));
    std::string message;
    while (!dictation.done()) {
        const httplib::ws::ReadResult got = ws.read(message);
        if (got == httplib::ws::Fail) {
            dictation.cancel();
            break;
        }
        if (got == httplib::ws::Binary) {
            dictation.push_pcm16(message.data(), message.size());
        } else if (got == httplib::ws::Text) {
            const std::string type = json_string_field(message, "type");
            if (type == "stop") {
                dictation.stop();
            } else if (type == "reset") {
                dictation.reset();
            } else if (type == "cancel") {
                dictation.cancel();
                break;
            } else {
                ws.send(error_event("unknown message type \"" + type + "\"; send stop, reset or cancel"));
            }
        }
    }
    dictation.join();
    if (ws.is_open()) ws.close();
    if (services.verbose) std::fprintf(stderr, "asrd: dictation closed\n");
}

}  // namespace

Value parse_value(const std::string & text) {
    if (text == "true") return true;
    if (text == "false") return false;
    char * end = nullptr;
    const long long integer = std::strtoll(text.c_str(), &end, 10);
    if (!text.empty() && end && *end == '\0') return static_cast<int64_t>(integer);
    const double number = std::strtod(text.c_str(), &end);
    if (!text.empty() && end && *end == '\0') return number;
    return text;
}

Json models_json(const Services & services) {
    const std::vector<InstalledVariant> installed = services.library.installed();
    const auto installed_variant = [&](const std::string & model, const std::string & variant) -> const InstalledVariant * {
        for (const InstalledVariant & v : installed)
            if (v.model == model && v.variant == variant) return &v;
        return nullptr;
    };
    Json out = Json::array();
    for (const ModelSpec & model : catalog()) {
        Json m = Json::object();
        m.set("id", model.id).set("task", task_name(model.task));
        m.set("kind", model.task == Task::recognition ? "asr" : "vad");
        m.set("name", model.name).set("upstream", model.upstream).set("parameters", model.parameters);
        m.set("license", model.license);
        m.set("source", model.source).set("origin", "catalog");
        m.set("languages", Json(model.languages)).set("language_steers", model.language_steers);
        m.set("requires", Json(model.requires));
        m.set("loaded", services.models.loaded(model.id, ""));
        Json variants = Json::array();
        bool any_installed = false;
        for (const Variant & variant : model.variants) {
            const InstalledVariant * local = installed_variant(model.id, variant.id);
            any_installed = any_installed || local;
            Json v = Json::object();
            v.set("id", variant.id).set("format", variant.format).set("architecture", variant.architecture);
            v.set("quantization", variant.quantization).set("bytes", variant.bytes());
            Json files = Json::array();
            for (const FileSpec & file : variant.files) {
                files.push(Json::object().set("name", file.name()).set("url", file.url()).set("bytes", file.bytes)
                               .set("sha256", file.sha256));
            }
            v.set("files", files);
            v.set("installed", local != nullptr);
            v.set("path", local ? Json(local->dir) : Json());
            v.set("runnable_by", runnable_by(services, variant.format, variant.architecture));
            const auto runtimes = services.registry.for_model(variant.format, variant.architecture);
            v.set("fits", runtimes.empty() || fits(*runtimes.front(), variant.bytes()));
            v.set("download", download_json(services, model.id, variant.id));
            variants.push(v);
        }
        m.set("variants", variants).set("downloaded", any_installed);
        out.push(m);
    }
    return out;
}

void install_routes(httplib::Server & http, Services & s) {
    // Any web page can reach localhost; only native clients, which send no Origin, are served.
    http.set_pre_routing_handler([](const httplib::Request & req, httplib::Response & res) {
        if (!req.has_header("Origin")) return httplib::Server::HandlerResponse::Unhandled;
        fail(res, 403, "requests from web pages are refused");
        return httplib::Server::HandlerResponse::Handled;
    });

    http.Get("/health", [&s](const httplib::Request &, httplib::Response & res) {
        reply(res, 200, s.models.health());
    });

    http.Get("/v1/system", [&s](const httplib::Request &, httplib::Response & res) {
        Json runtimes = Json::array();
        for (const auto & runtime : s.registry.all()) runtimes.push(runtime->json());
        reply(res, 200, Json::object().set("host", host_info().json()).set("runtimes", runtimes));
    });

    http.Get("/v1/models", [&s](const httplib::Request &, httplib::Response & res) {
        reply(res, 200, models_json(s));
    });

    // {"model":"qwen3-asr-0.6b","variant":"q8_0","runtime":"speech.cpp","settings":{"device":"gpu"}} or {"path":...}
    http.Post("/v1/models/load", [&s](const httplib::Request & req, httplib::Response & res) {
        Json body;
        if (!body_json(req, res, body)) return;
        ModelManager::LoadRequest request;
        request.model = body["model"].as_string();
        request.variant = body["variant"].as_string();
        request.runtime = body["runtime"].as_string();
        request.path = body["path"].as_string();
        for (const auto & [key, value] : body["settings"].members()) {
            Value v;
            if (!value_from_json(value, v)) return fail(res, 400, "setting \"" + key + "\" is not a value");
            request.settings[key] = v;
        }
        if (request.model.empty() && request.path.empty()) return fail(res, 400, "name a model or a path");
        std::string error;
        if (!s.models.load(std::move(request), error)) return fail(res, 400, error);
        reply(res, 202, s.models.health());
    });

    http.Post("/v1/models/unload", [&s](const httplib::Request &, httplib::Response & res) {
        s.models.unload();
        reply(res, 200, s.models.health());
    });

    http.Delete("/v1/models/:model/:variant", [&s](const httplib::Request & req, httplib::Response & res) {
        const std::string model = req.path_params.at("model");
        const std::string variant = req.path_params.at("variant");
        if (s.models.loaded(model, variant)) return fail(res, 409, model + " is in use; load another model first");
        std::string error;
        if (!s.library.remove(model, variant, error)) return fail(res, 404, error);
        reply(res, 200, Json::object().set("deleted", model + ":" + variant));
    });

    http.Get("/v1/downloads", [&s](const httplib::Request &, httplib::Response & res) {
        Json out = Json::array();
        for (const auto & progress : s.downloads.list()) out.push(progress.json());
        reply(res, 200, out);
    });

    // {"model":"parakeet-tdt-0.6b-v3","variant":"q8_0"}: also queues what the model requires and is missing.
    http.Post("/v1/downloads", [&s](const httplib::Request & req, httplib::Response & res) {
        Json body;
        if (!body_json(req, res, body)) return;
        const std::string model = body["model"].as_string();
        const ModelSpec * spec = catalog_find(model);
        if (!spec) return fail(res, 404, "no model \"" + model + "\" in the catalog");
        Json started = Json::array();
        std::string error;
        for (const std::string & requirement : spec->requires) {
            DownloadManager::Progress progress;
            if (!s.library.find(requirement) && s.downloads.start(requirement, "", progress, error))
                started.push(progress.json());
        }
        DownloadManager::Progress progress;
        if (!s.downloads.start(model, body["variant"].as_string(), progress, error)) return fail(res, 409, error);
        started.push(progress.json());
        reply(res, 202, started);
    });

    http.Post("/v1/downloads/:id/pause", [&s](const httplib::Request & req, httplib::Response & res) {
        std::string error;
        if (!s.downloads.pause(req.path_params.at("id"), error)) return fail(res, 404, error);
        const auto progress = s.downloads.get(req.path_params.at("id"));
        reply(res, 200, progress ? progress->json() : Json::object());
    });

    http.Post("/v1/downloads/:id/resume", [&s](const httplib::Request & req, httplib::Response & res) {
        std::string error;
        if (!s.downloads.resume(req.path_params.at("id"), error)) return fail(res, 404, error);
        const auto progress = s.downloads.get(req.path_params.at("id"));
        reply(res, 200, progress ? progress->json() : Json::object());
    });

    http.Delete("/v1/downloads/:id", [&s](const httplib::Request & req, httplib::Response & res) {
        std::string error;
        if (!s.downloads.cancel(req.path_params.at("id"), error)) return fail(res, 404, error);
        reply(res, 200, Json::object().set("cancelled", req.path_params.at("id")));
    });

    http.WebSocket("/v1/dictate", [&s](const httplib::Request & req, httplib::ws::WebSocket & ws) {
        serve_dictation(s, req, ws);
    });
}

}  // namespace asrd
