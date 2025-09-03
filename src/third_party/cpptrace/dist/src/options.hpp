#ifndef OPTIONS_HPP
#define OPTIONS_HPP

#include <cpptrace/utils.hpp>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    // exported for test purposes
    CPPTRACE_EXPORT bool should_absorb_trace_exceptions();
    bool should_resolve_inlined_calls();
    cache_mode get_cache_mode();
}
CPPTRACE_END_NAMESPACE

#endif
