// The runtimes this build of asrd carries, in the order they were registered: the first that runs a model's format
// and architecture is the one a load uses when it names none.
#pragma once

#include "runtime.h"

#include <memory>
#include <string>
#include <vector>

namespace asrd {

class RuntimeRegistry {
public:
    void add(std::unique_ptr<Runtime> runtime);
    const std::vector<std::unique_ptr<Runtime>> & all() const { return runtimes_; }
    Runtime * find(const std::string & id) const;
    // The runtimes that run models of `format` and `architecture` ("" for any).
    std::vector<Runtime *> for_model(const std::string & format, const std::string & architecture) const;

private:
    std::vector<std::unique_ptr<Runtime>> runtimes_;
};

// Registers every runtime compiled in (runtimes/builtin.cpp). Called on the main thread before any other thread starts:
// a runtime may set the process up as it starts, as ggml does.
void register_builtin_runtimes(RuntimeRegistry & registry, bool verbose);

}  // namespace asrd
