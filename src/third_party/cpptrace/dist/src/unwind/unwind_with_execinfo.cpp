#ifdef CPPTRACE_UNWIND_WITH_EXECINFO

#include "unwind/unwind.hpp"
#include "utils/common.hpp"
#include "utils/utils.hpp"

#include <algorithm>
#include <climits>
#include <cstddef>
#include <vector>

#include <execinfo.h>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    CPPTRACE_FORCE_NO_INLINE
    std::vector<frame_ptr> capture_frames(std::size_t skip, std::size_t max_depth) {
        skip++;
        std::vector<void*> addrs(skip + std::min(hard_max_frames, max_depth), nullptr);
        // thread safe
        const int n_frames = backtrace(addrs.data(), static_cast<int>(addrs.size()));
        // I hate the copy here but it's the only way that isn't UB
        std::vector<frame_ptr> frames(n_frames - skip, 0);
        for(int i = skip; i < n_frames; i++) {
            // On x86/x64/arm, as far as I can tell, the frame return address is always one after the call
            // So we just decrement to get the pc back inside the `call` / `bl`
            // This is done with _Unwind too but conditionally based on info from _Unwind_GetIPInfo.
            frames[i - skip] = reinterpret_cast<frame_ptr>(addrs[i]) - 1;
        }
        return frames;
    }

    CPPTRACE_FORCE_NO_INLINE
    std::size_t safe_capture_frames(frame_ptr*, std::size_t, std::size_t, std::size_t) {
        // Can't safe trace with execinfo
        return 0;
    }

    bool has_safe_unwind() {
        return false;
    }
}
CPPTRACE_END_NAMESPACE

#endif
