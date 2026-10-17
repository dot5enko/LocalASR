#include "hardware.h"

#include <sys/sysctl.h>

namespace asrd {
namespace {

std::string sysctl_string(const char * name) {
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) return "";
    std::string out(size, '\0');
    if (sysctlbyname(name, out.data(), &size, nullptr, 0) != 0) return "";
    out.resize(out.find('\0') == std::string::npos ? size : out.find('\0'));
    return out;
}

template <typename T>
T sysctl_number(const char * name) {
    T value{};
    size_t size = sizeof value;
    return sysctlbyname(name, &value, &size, nullptr, 0) == 0 ? value : T{};
}

}  // namespace

HostInfo host_info() {
    HostInfo host;
    host.chip = sysctl_string("machdep.cpu.brand_string");
    host.model = sysctl_string("hw.model");
    host.os = sysctl_string("kern.osproductversion");
    host.performance_cores = sysctl_number<int32_t>("hw.perflevel0.physicalcpu");
    host.efficiency_cores = sysctl_number<int32_t>("hw.perflevel1.physicalcpu");
    if (host.performance_cores == 0) host.performance_cores = sysctl_number<int32_t>("hw.physicalcpu");
    host.memory = sysctl_number<uint64_t>("hw.memsize");
    return host;
}

Json HostInfo::json() const {
    Json out = Json::object();
    out.set("chip", chip).set("model", model).set("os", os);
    out.set("performance_cores", performance_cores).set("efficiency_cores", efficiency_cores);
    out.set("memory", memory);
    return out;
}

}  // namespace asrd
