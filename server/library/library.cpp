#include "library.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace fs = std::filesystem;

namespace asrd {
namespace {

constexpr const char * kManifest = "manifest.json";

bool read_file(const std::string & path, std::string & out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

int64_t file_size(const std::string & path) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return ec ? -1 : static_cast<int64_t>(size);
}

InstalledVariant from_catalog(const ModelSpec & model, const Variant & variant) {
    InstalledVariant out;
    out.model = model.id;
    out.variant = variant.id;
    out.name = model.name;
    out.task = model.task;
    out.format = variant.format;
    out.architecture = variant.architecture;
    out.quantization = variant.quantization;
    out.origin = "catalog";
    for (const FileSpec & file : variant.files) out.files.push_back(file.name());
    out.bytes = variant.bytes();
    out.languages = model.languages;
    out.language_steers = model.language_steers;
    out.requires = model.requires;
    return out;
}

}  // namespace

ModelFiles InstalledVariant::model_files() const {
    ModelFiles out;
    out.format = format;
    for (const std::string & file : files) out.paths.push_back(dir + file);
    return out;
}

Json InstalledVariant::json() const {
    Json out = Json::object();
    out.set("model", model).set("variant", variant).set("name", name).set("task", task_name(task));
    out.set("format", format).set("architecture", architecture).set("quantization", quantization);
    out.set("origin", origin).set("dir", dir).set("files", Json(files)).set("bytes", bytes);
    out.set("languages", Json(languages)).set("language_steers", language_steers);
    out.set("requires", Json(requires));
    return out;
}

ModelLibrary::ModelLibrary(std::string root) : root_(std::move(root)) {
    if (!root_.empty() && root_.back() != '/') root_ += '/';
}

std::string ModelLibrary::default_root() {
    const char * home = std::getenv("HOME");
    return std::string(home ? home : "") + "/Library/Application Support/LocalASR/models/";
}

bool ModelLibrary::write_manifest(const InstalledVariant & v, const std::string & dir, std::string & error) const {
    Json manifest = Json::object();
    manifest.set("model", v.model).set("variant", v.variant).set("name", v.name).set("task", task_name(v.task));
    manifest.set("format", v.format).set("architecture", v.architecture).set("quantization", v.quantization);
    manifest.set("origin", v.origin).set("languages", Json(v.languages)).set("language_steers", v.language_steers);
    manifest.set("requires", Json(v.requires));
    Json files = Json::array();
    const ModelSpec * spec = catalog_find(v.model);
    const Variant * variant = spec ? spec->variant(v.variant) : nullptr;
    for (const std::string & name : v.files) {
        Json file = Json::object();
        file.set("name", name).set("bytes", file_size(dir + name));
        if (variant) {
            for (const FileSpec & f : variant->files) {
                if (f.name() == name) file.set("sha256", f.sha256).set("revision", f.revision).set("repository", f.repository);
            }
        }
        files.push(file);
    }
    manifest.set("files", files);
    // Written under another name and renamed, so a manifest is either whole or absent.
    const std::string tmp = dir + kManifest + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << manifest.dump() << "\n";
        if (!out) {
            error = "cannot write " + tmp;
            return false;
        }
    }
    std::error_code ec;
    fs::rename(tmp, dir + kManifest, ec);
    if (ec) error = "cannot write the manifest in " + dir + ": " + ec.message();
    return !ec;
}

void ModelLibrary::migrate() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const ModelSpec & model : catalog()) {
        for (const Variant & variant : model.variants) {
            if (variant.files.size() != 1) continue;
            const FileSpec & file = variant.files.front();
            const std::string flat = root_ + file.name();
            // The app named a file only once its hash matched, so a file of the right size is the file.
            if (file_size(flat) != file.bytes) continue;
            InstalledVariant v = from_catalog(model, variant);
            v.dir = root_ + model.id + "/" + variant.id + "/";
            std::error_code ec;
            fs::create_directories(v.dir, ec);
            fs::rename(flat, v.dir + file.name(), ec);
            std::string error;
            if (ec) error = ec.message();
            else write_manifest(v, v.dir, error);
            if (!error.empty()) std::fprintf(stderr, "asrd: cannot move %s into the library: %s\n", flat.c_str(), error.c_str());
            else std::fprintf(stderr, "asrd: moved %s to %s\n", file.name().c_str(), v.dir.c_str());
        }
    }
}

std::vector<InstalledVariant> ModelLibrary::installed() const {
    std::vector<InstalledVariant> out;
    std::error_code ec;
    for (const auto & model_dir : fs::directory_iterator(root_, ec)) {
        const std::string model_name = model_dir.path().filename().string();
        if (!model_dir.is_directory() || model_name.empty() || model_name[0] == '.') continue;
        for (const auto & variant_dir : fs::directory_iterator(model_dir.path(), ec)) {
            if (!variant_dir.is_directory()) continue;
            const std::string dir = variant_dir.path().string() + "/";
            std::string text;
            Json manifest;
            std::string error;
            if (!read_file(dir + kManifest, text) || !Json::parse(text, manifest, error)) continue;
            InstalledVariant v;
            v.model = manifest["model"].as_string();
            v.variant = manifest["variant"].as_string();
            v.name = manifest["name"].as_string();
            v.task = manifest["task"].as_string() == "detection" ? Task::detection : Task::recognition;
            v.format = manifest["format"].as_string();
            v.architecture = manifest["architecture"].as_string();
            v.quantization = manifest["quantization"].as_string();
            v.origin = manifest["origin"].as_string();
            v.languages = manifest["languages"].as_strings();
            v.language_steers = manifest["language_steers"].as_bool();
            v.requires = manifest["requires"].as_strings();
            v.dir = dir;
            bool whole = !v.model.empty() && !v.variant.empty();
            for (const Json & file : manifest["files"].items()) {
                const std::string name = file["name"].as_string();
                const int64_t size = file_size(dir + name);
                whole = whole && !name.empty() && size == file["bytes"].as_int(-1);
                v.files.push_back(name);
                v.bytes += size > 0 ? size : 0;
            }
            if (whole && !v.files.empty()) out.push_back(std::move(v));
        }
    }
    return out;
}

std::optional<InstalledVariant> ModelLibrary::find(const std::string & model, const std::string & variant) const {
    std::vector<InstalledVariant> all = installed();
    if (!variant.empty()) {
        for (InstalledVariant & v : all)
            if (v.model == model && v.variant == variant) return std::move(v);
        return std::nullopt;
    }
    // The catalog's order of variants decides between several installed.
    if (const ModelSpec * spec = catalog_find(model)) {
        for (const Variant & candidate : spec->variants)
            for (InstalledVariant & v : all)
                if (v.model == model && v.variant == candidate.id) return std::move(v);
    }
    for (InstalledVariant & v : all)
        if (v.model == model) return std::move(v);
    return std::nullopt;
}

bool ModelLibrary::install(const ModelSpec & model, const Variant & variant, const std::string & staged,
                           std::string & error) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string dir = staged;
    if (!dir.empty() && dir.back() != '/') dir += '/';
    InstalledVariant v = from_catalog(model, variant);
    for (const FileSpec & file : variant.files) {
        if (file_size(dir + file.name()) != file.bytes) {
            error = file.name() + " is not whole in " + dir;
            return false;
        }
    }
    std::error_code ec;
    fs::remove(dir + "download.json", ec);
    if (!write_manifest(v, dir, error)) return false;
    const std::string target = root_ + model.id + "/" + variant.id;
    fs::create_directories(root_ + model.id, ec);
    fs::remove_all(target, ec);
    fs::rename(fs::path(dir).parent_path(), target, ec);
    if (ec) {
        error = "cannot move the download into " + target + ": " + ec.message();
        return false;
    }
    return true;
}

bool ModelLibrary::remove(const std::string & model, const std::string & variant, std::string & error) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string dir = root_ + model + "/" + variant;
    std::error_code ec;
    if (model.empty() || variant.empty() || model[0] == '.' || model.find('/') != std::string::npos ||
        variant.find('/') != std::string::npos || !fs::exists(dir + "/" + kManifest, ec)) {
        error = model + "/" + variant + " is not installed";
        return false;
    }
    fs::remove_all(dir, ec);
    if (ec) {
        error = "cannot delete " + dir + ": " + ec.message();
        return false;
    }
    // The model's folder goes too once its last variant has.
    fs::remove(root_ + model, ec);
    return true;
}

}  // namespace asrd
