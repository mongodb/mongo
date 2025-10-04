#ifdef CPPTRACE_DEMANGLE_WITH_NOTHING

#include "demangle/demangle.hpp"

#include <string>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    std::string demangle(const std::string& name, bool) {
        return name;
    }
}
CPPTRACE_END_NAMESPACE

#endif
