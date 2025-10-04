#ifndef CPPTRACE_FORWARD_HPP
#define CPPTRACE_FORWARD_HPP

#include <cstdint>

#define CPPTRACE_BEGIN_NAMESPACE \
    namespace cpptrace { \
    inline namespace v1 {
#define CPPTRACE_END_NAMESPACE \
    } \
    }

CPPTRACE_BEGIN_NAMESPACE
    // Some type sufficient for an instruction pointer, currently always an alias to std::uintptr_t
    using frame_ptr = std::uintptr_t;

    struct raw_trace;
    struct object_trace;
    struct stacktrace;

    struct object_frame;
    struct stacktrace_frame;
    struct safe_object_frame;
CPPTRACE_END_NAMESPACE

#endif
