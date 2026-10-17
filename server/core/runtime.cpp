#include "runtime.h"

#include <algorithm>
#include <cmath>

namespace asrd {

Json value_json(const Value & value) {
    return std::visit([](const auto & v) { return Json(v); }, value);
}

bool value_from_json(const Json & json, Value & out) {
    switch (json.type()) {
        case Json::Type::boolean: out = json.as_bool(); return true;
        case Json::Type::string: out = json.as_string(); return true;
        case Json::Type::number: {
            const double n = json.as_number();
            if (n == std::floor(n) && std::fabs(n) < 1e15) out = static_cast<int64_t>(n);
            else out = n;
            return true;
        }
        default: return false;
    }
}

std::string value_text(const Value & value) {
    if (const bool * b = std::get_if<bool>(&value)) return *b ? "true" : "false";
    if (const int64_t * i = std::get_if<int64_t>(&value)) return std::to_string(*i);
    if (const double * d = std::get_if<double>(&value)) return Json(*d).dump();
    return std::get<std::string>(value);
}

Json SettingSpec::json() const {
    static const char * scopes[] = {"load", "request"};
    static const char * types[] = {"boolean", "integer", "number", "text"};
    Json out = Json::object();
    out.set("key", key).set("label", label).set("help", help);
    out.set("scope", scopes[static_cast<int>(scope)]).set("type", types[static_cast<int>(type)]);
    out.set("default", value_json(fallback));
    if (range) {
        Json r = Json::array();
        r.push(std::isfinite(range->first) ? Json(range->first) : Json());
        r.push(std::isfinite(range->second) ? Json(range->second) : Json());
        out.set("range", r);
    }
    if (!choices.empty()) out.set("choices", Json(choices));
    if (!choice_labels.empty()) out.set("choice_labels", Json(choice_labels));
    out.set("advanced", advanced);
    return out;
}

Json Device::json() const {
    static const char * kinds[] = {"cpu", "gpu", "igpu", "npu"};
    Json out = Json::object();
    out.set("id", id).set("description", description).set("kind", kinds[static_cast<int>(kind)]);
    out.set("memory_total", memory_total).set("memory_free", memory_free);
    return out;
}

const char * task_name(Task task) {
    return task == Task::recognition ? "recognition" : "detection";
}

void Cancellation::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    if (hook_) hook_();
}

bool Cancellation::cancelled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_;
}

bool Cancellation::arm(std::function<void()> hook) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_) return false;
    hook_ = std::move(hook);
    return true;
}

void Cancellation::disarm() {
    std::lock_guard<std::mutex> lock(mutex_);
    hook_ = nullptr;
}

bool Runtime::supports(const std::string & format, const std::string & architecture) const {
    const RuntimeInfo & i = info();
    return std::find(i.formats.begin(), i.formats.end(), format) != i.formats.end() &&
           (architecture.empty() ||
            std::find(i.architectures.begin(), i.architectures.end(), architecture) != i.architectures.end());
}

Json Runtime::json() {
    const RuntimeInfo & i = info();
    Json out = Json::object();
    out.set("id", i.id).set("name", i.name).set("version", i.version);
    out.set("formats", Json(i.formats)).set("architectures", Json(i.architectures));
    Json devices = Json::array();
    for (const Device & device : this->devices()) devices.push(device.json());
    out.set("devices", devices);
    Json settings = Json::array();
    for (const SettingSpec & spec : load_settings()) settings.push(spec.json());
    out.set("load_settings", settings);
    return out;
}

}  // namespace asrd
