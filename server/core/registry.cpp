#include "registry.h"

namespace asrd {

void RuntimeRegistry::add(std::unique_ptr<Runtime> runtime) {
    runtimes_.push_back(std::move(runtime));
}

Runtime * RuntimeRegistry::find(const std::string & id) const {
    for (const auto & runtime : runtimes_)
        if (runtime->info().id == id) return runtime.get();
    return nullptr;
}

std::vector<Runtime *> RuntimeRegistry::for_model(const std::string & format, const std::string & architecture) const {
    std::vector<Runtime *> out;
    for (const auto & runtime : runtimes_)
        if (runtime->supports(format, architecture)) out.push_back(runtime.get());
    return out;
}

}  // namespace asrd
