#ifdef CPPTRACE_UNWIND_WITH_UNWIND

#include "unwind/unwind.hpp"
#include "utils/common.hpp"
#include "utils/error.hpp"
#include "utils/utils.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <vector>

#include <unwind.h>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    struct unwind_state {
        std::size_t skip;
        std::size_t max_depth;
        std::vector<frame_ptr>& vec;
    };

    _Unwind_Reason_Code unwind_callback(_Unwind_Context* context, void* arg) {
        unwind_state& state = *static_cast<unwind_state*>(arg);
        if(state.skip) {
            state.skip--;
            if(_Unwind_GetIP(context) == frame_ptr(0)) {
                return _URC_END_OF_STACK;
            } else {
                return _URC_NO_REASON;
            }
        }

        ASSERT(
            state.vec.size() < state.max_depth,
            "Somehow cpptrace::detail::unwind_callback is being called beyond the max_depth"
        );
        int is_before_instruction = 0;
        frame_ptr ip = _Unwind_GetIPInfo(context, &is_before_instruction);
        if(!is_before_instruction && ip != frame_ptr(0)) {
            ip--;
        }
        if (ip == frame_ptr(0)) {
            return _URC_END_OF_STACK;
        } else {
            state.vec.push_back(ip);
            if(state.vec.size() >= state.max_depth) {
                return _URC_END_OF_STACK;
            } else {
                return _URC_NO_REASON;
            }
        }
    }

    CPPTRACE_FORCE_NO_INLINE
    std::vector<frame_ptr> capture_frames(std::size_t skip, std::size_t max_depth) {
        std::vector<frame_ptr> frames;
        unwind_state state{skip + 1, max_depth, frames};
        _Unwind_Backtrace(unwind_callback, &state); // presumably thread-safe
        return frames;
    }

    CPPTRACE_FORCE_NO_INLINE
    std::size_t safe_capture_frames(frame_ptr*, std::size_t, std::size_t, std::size_t) {
        // Can't safe trace with _Unwind
        return 0;
    }

    bool has_safe_unwind() {
        return false;
    }
}
CPPTRACE_END_NAMESPACE

#endif
