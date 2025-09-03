#ifndef SYMBOLS_HPP
#define SYMBOLS_HPP

#include <cpptrace/basic.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    using collated_vec = std::vector<
        std::pair<std::reference_wrapper<const object_frame>, std::reference_wrapper<stacktrace_frame>>
    >;
    struct frame_with_inlines {
        stacktrace_frame frame;
        std::vector<stacktrace_frame> inlines;
    };
    using collated_vec_with_inlines = std::vector<
        std::pair<std::reference_wrapper<const object_frame>, std::reference_wrapper<frame_with_inlines>>
    >;

    // These two helpers create a map from a target object to a vector of frames to resolve
    std::unordered_map<std::string, collated_vec> collate_frames(
        const std::vector<object_frame>& frames,
        std::vector<stacktrace_frame>& trace
    );
    std::unordered_map<std::string, collated_vec_with_inlines> collate_frames(
        const std::vector<object_frame>& frames,
        std::vector<frame_with_inlines>& trace
    );

    #ifdef CPPTRACE_GET_SYMBOLS_WITH_LIBBACKTRACE
    namespace libbacktrace {
        std::vector<stacktrace_frame> resolve_frames(const std::vector<frame_ptr>& frames);
    }
    #endif
    #ifdef CPPTRACE_GET_SYMBOLS_WITH_LIBDWARF
    namespace libdwarf {
        std::vector<stacktrace_frame> resolve_frames(const std::vector<object_frame>& frames);
    }
    #endif
    #ifdef CPPTRACE_GET_SYMBOLS_WITH_LIBDL
    namespace libdl {
        std::vector<stacktrace_frame> resolve_frames(const std::vector<frame_ptr>& frames);
    }
    #endif
    #ifdef CPPTRACE_GET_SYMBOLS_WITH_ADDR2LINE
    namespace addr2line {
        std::vector<stacktrace_frame> resolve_frames(const std::vector<object_frame>& frames);
    }
    #endif
    #ifdef CPPTRACE_GET_SYMBOLS_WITH_DBGHELP
    namespace dbghelp {
        std::vector<stacktrace_frame> resolve_frames(const std::vector<frame_ptr>& frames);
    }
    #endif
    #ifdef CPPTRACE_GET_SYMBOLS_WITH_NOTHING
    namespace nothing {
        std::vector<stacktrace_frame> resolve_frames(const std::vector<object_frame>& frames);
        std::vector<stacktrace_frame> resolve_frames(const std::vector<frame_ptr>& frames);
    }
    #endif

    std::vector<stacktrace_frame> resolve_frames(const std::vector<object_frame>& frames);
    std::vector<stacktrace_frame> resolve_frames(const std::vector<frame_ptr>& frames);
}
CPPTRACE_END_NAMESPACE

#endif
