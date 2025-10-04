#ifndef UNWIND_HPP
#define UNWIND_HPP

#include <cpptrace/basic.hpp>

#include <cstddef>
#include <vector>

#ifdef CPPTRACE_UNWIND_WITH_DBGHELP
 #define WIN32_LEAN_AND_MEAN
 #include <windows.h>
#endif


CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    #ifdef CPPTRACE_HARD_MAX_FRAMES
    constexpr std::size_t hard_max_frames = CPPTRACE_HARD_MAX_FRAMES;
    #else
    constexpr std::size_t hard_max_frames = 400;
    #endif

    #ifdef CPPTRACE_UNWIND_WITH_DBGHELP
     CPPTRACE_FORCE_NO_INLINE
     std::vector<frame_ptr> capture_frames(
         std::size_t skip,
         std::size_t max_depth,
         EXCEPTION_POINTERS* exception_pointers = nullptr
     );
    #else
     CPPTRACE_FORCE_NO_INLINE
     std::vector<frame_ptr> capture_frames(std::size_t skip, std::size_t max_depth);
    #endif

    CPPTRACE_FORCE_NO_INLINE
    std::size_t safe_capture_frames(frame_ptr* buffer, std::size_t size, std::size_t skip, std::size_t max_depth);

    bool has_safe_unwind();
}
CPPTRACE_END_NAMESPACE

#endif
