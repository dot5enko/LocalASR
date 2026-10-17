// The runtimes compiled into asrd. Each registers here explicitly: a runtime in a static library that registered
// itself from a static initializer would be dropped by the linker, as nothing refers to it.
#include "registry.h"

#if ASRD_WITH_SPEECHCPP
#include "speechcpp/speechcpp.h"
#endif

#include <cstdio>

namespace asrd {

void register_builtin_runtimes(RuntimeRegistry & registry, bool verbose) {
    std::string error;
#if ASRD_WITH_SPEECHCPP
    if (auto runtime = make_speechcpp_runtime(verbose, error)) registry.add(std::move(runtime));
    else std::fprintf(stderr, "asrd: speech.cpp is not available: %s\n", error.c_str());
#endif
    (void)verbose;
}

}  // namespace asrd
