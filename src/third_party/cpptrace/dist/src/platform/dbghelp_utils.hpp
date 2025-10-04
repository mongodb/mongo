#ifndef DBGHELP_UTILS_HPP
#define DBGHELP_UTILS_HPP

#if defined(CPPTRACE_UNWIND_WITH_DBGHELP) \
    || defined(CPPTRACE_GET_SYMBOLS_WITH_DBGHELP) \
    || defined(CPPTRACE_DEMANGLE_WITH_WINAPI)

#include "utils/common.hpp"

#include <unordered_map>
#include <mutex>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    class dbghelp_syminit_info {
        // `void*` is used to avoid including the (expensive) windows.h header here
        void* handle = nullptr;
        bool should_sym_cleanup; // true if cleanup is not managed by the syminit cache
        bool should_close_handle; // true if cleanup is not managed by the syminit cache and the handle was duplicated
        dbghelp_syminit_info(void* handle, bool should_sym_cleanup, bool should_close_handle);
    public:
        ~dbghelp_syminit_info();
        void release();

        NODISCARD static dbghelp_syminit_info make_not_owned(void* handle);
        NODISCARD static dbghelp_syminit_info make_owned(void* handle, bool should_close_handle);

        dbghelp_syminit_info(const dbghelp_syminit_info&) = delete;
        dbghelp_syminit_info(dbghelp_syminit_info&&);
        dbghelp_syminit_info& operator=(const dbghelp_syminit_info&) = delete;
        dbghelp_syminit_info& operator=(dbghelp_syminit_info&&);

        void* get_process_handle() const;
        dbghelp_syminit_info make_non_owning_view() const;
    };

    // Ensure SymInitialize is called on the process. This function either
    // - Finds that SymInitialize has been called for a handle to the current process already, in which case it returns
    //   a non-owning dbghelp_syminit_info instance holding the handle
    // - Calls SymInitialize a handle to the current process, caches it, and returns a non-owning dbghelp_syminit_info
    // - Calls SymInitialize and returns an owning dbghelp_syminit_info which will handle cleanup
    dbghelp_syminit_info ensure_syminit();

    NODISCARD std::unique_lock<std::recursive_mutex> get_dbghelp_lock();
}
CPPTRACE_END_NAMESPACE

#endif

#endif
