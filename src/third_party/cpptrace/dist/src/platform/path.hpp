#ifndef PATH_HPP
#define PATH_HPP

#include "platform/platform.hpp"
#include "utils/string_view.hpp"

#include <string>
#include <cctype>

#if IS_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    #if IS_WINDOWS
    constexpr char PATH_SEP = '\\';
    inline bool is_absolute(string_view path) {
        // I don't want to bring in shlwapi as a dependency just for PathIsRelativeA so I'm following the guidance of
        // https://stackoverflow.com/a/71941552/15675011 and
        // https://github.com/wine-mirror/wine/blob/b210a204137dec8d2126ca909d762454fd47e963/dlls/kernelbase/path.c#L982
        if(path.empty() || IsDBCSLeadByte(path[0])) {
            return false;
        }
        if(path[0] == '\\') {
            return true;
        }
        if(path.size() >= 2 && std::isalpha(path[0]) && path[1] == ':') {
            return true;
        }
        return false;
    }
    #else
    constexpr char PATH_SEP = '/';
    inline bool is_absolute(string_view path) {
        if(path.empty()) {
            return false;
        }
        return path[0] == '/';
    }
    #endif
}
CPPTRACE_END_NAMESPACE

#endif
