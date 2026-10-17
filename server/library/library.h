// The models on this Mac, under ~/Library/Application Support/LocalASR/models:
//
//   <model>/<variant>/manifest.json    what the folder holds: the model, its format and architecture, each file's hash
//   <model>/<variant>/<files>
//   .staging/<download>/               downloads under way, renamed into place once every file's hash matched
//
// A variant is installed when its manifest is there; the manifest is written last and the folder renamed into place,
// so a half-done download never counts. Runtimes only ever see the files of an installed variant.
#pragma once

#include "catalog.h"
#include "runtime.h"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace asrd {

struct InstalledVariant {
    std::string model;
    std::string variant;
    std::string name;
    Task task = Task::recognition;
    std::string format;
    std::string architecture;
    std::string quantization;
    std::string origin;                  // "catalog"
    std::string dir;                     // ends in '/'
    std::vector<std::string> files;      // names in dir
    int64_t bytes = 0;
    std::vector<std::string> languages;
    bool language_steers = false;
    std::vector<std::string> requires;

    ModelFiles model_files() const;
    Json json() const;
};

class ModelLibrary {
public:
    explicit ModelLibrary(std::string root = default_root());
    // ~/Library/Application Support/LocalASR/models/
    static std::string default_root();

    const std::string & root() const { return root_; }
    std::string staging_root() const { return root_ + ".staging/"; }

    // Files kept flat in the folder before models had variants move into <model>/<variant>/ with a manifest.
    void migrate();

    // Read from disk on every call: the folders are few, and nothing goes stale.
    std::vector<InstalledVariant> installed() const;
    // The installed variant of `model`: `variant`, or the first of the catalog's that is installed.
    std::optional<InstalledVariant> find(const std::string & model, const std::string & variant = "") const;

    // Moves a staged folder that holds the variant's verified files into place, with its manifest.
    bool install(const ModelSpec & model, const Variant & variant, const std::string & staged, std::string & error);
    bool remove(const std::string & model, const std::string & variant, std::string & error);

private:
    bool write_manifest(const InstalledVariant & variant, const std::string & dir, std::string & error) const;

    std::string root_;
    mutable std::mutex mutex_;           // installs and removals one at a time
};

}  // namespace asrd
