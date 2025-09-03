#ifdef CPPTRACE_GET_SYMBOLS_WITH_NOTHING

#include <cpptrace/basic.hpp>
#include "symbols/symbols.hpp"
#include "utils/common.hpp"

#include <vector>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
namespace nothing {
    std::vector<stacktrace_frame> resolve_frames(const std::vector<frame_ptr>& frames) {
        return std::vector<stacktrace_frame>(frames.size(), null_frame());
    }

    std::vector<stacktrace_frame> resolve_frames(const std::vector<object_frame>& frames) {
        return std::vector<stacktrace_frame>(frames.size(), null_frame());
    }
}
}
CPPTRACE_END_NAMESPACE

#endif
