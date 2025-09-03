#ifndef COMMON_HPP
#define COMMON_HPP

#include <cpptrace/basic.hpp>

#include "platform/platform.hpp"

#include <cstdint>

#define ESC     "\033["
#define RESET   ESC "0m"
#define RED     ESC "31m"
#define GREEN   ESC "32m"
#define YELLOW  ESC "33m"
#define BLUE    ESC "34m"
#define MAGENTA ESC "35m"
#define CYAN    ESC "36m"

#if IS_GCC || IS_CLANG
 #define NODISCARD __attribute__((warn_unused_result))
// #elif IS_MSVC && _MSC_VER >= 1700
//  #define NODISCARD _Check_return_
#else
 #define NODISCARD
#endif

// workaround a bizarre gcc bug https://godbolt.org/z/s78vnf7jv
// https://github.com/jeremy-rifkin/cpptrace/issues/220
#if defined(__GNUC__) && (__GNUC__ < 7)
 #undef NODISCARD
 #define NODISCARD
#endif

#if IS_MSVC
 #define MSVC_CDECL __cdecl
#else
 #define MSVC_CDECL
#endif

// support is pretty good https://godbolt.org/z/djTqv7WMY, checked in cmake during config
#ifdef HAS_ATTRIBUTE_PACKED
 #define PACKED __attribute__((packed))
#else
 #define PACKED
#endif

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    inline stacktrace_frame null_frame() {
        return {
            0,
            0,
            nullable<std::uint32_t>::null(),
            nullable<std::uint32_t>::null(),
            "",
            "",
            false
        };
    }
}
CPPTRACE_END_NAMESPACE

#endif
