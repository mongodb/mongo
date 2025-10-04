#ifdef CPPTRACE_DEMANGLE_WITH_WINAPI

#include "demangle/demangle.hpp"
#include "platform/dbghelp_utils.hpp"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    std::string demangle(const std::string& name, bool) {
        // Dbghelp is is single-threaded, so acquire a lock.
        auto lock = get_dbghelp_lock();
        char buffer[500];
        auto ret = UnDecorateSymbolName(name.c_str(), buffer, sizeof(buffer) - 1, 0);
        if(ret == 0) {
            return name;
        } else {
            buffer[ret] = 0; // just in case, ms' docs unclear if null terminator inserted
            return buffer;
        }
    }
}
CPPTRACE_END_NAMESPACE

#endif
