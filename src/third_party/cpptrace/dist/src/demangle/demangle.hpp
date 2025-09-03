#ifndef DEMANGLE_HPP
#define DEMANGLE_HPP

#include <cpptrace/forward.hpp>

#include <string>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    std::string demangle(const std::string& name, bool check_prefix);
}
CPPTRACE_END_NAMESPACE

#endif
