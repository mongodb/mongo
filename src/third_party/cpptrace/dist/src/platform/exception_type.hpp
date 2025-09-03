#ifndef EXCEPTION_TYPE_HPP
#define EXCEPTION_TYPE_HPP

#include <string>

#include "platform/platform.hpp"

// libstdc++ and libc++
#if defined(CPPTRACE_HAS_CXX_EXCEPTION_TYPE) && (IS_LIBSTDCXX || IS_LIBCXX)
 #include <typeinfo>
 #include <cxxabi.h>
 #include "demangle/demangle.hpp"
#endif

CPPTRACE_BEGIN_NAMESPACE
namespace detail {
    inline std::string exception_type_name() {
        #if defined(CPPTRACE_HAS_CXX_EXCEPTION_TYPE) && (IS_LIBSTDCXX || IS_LIBCXX)
         const std::type_info* t = abi::__cxa_current_exception_type();
         return t ? detail::demangle(t->name(), false) : "<unknown>";
        #else
         return "<unknown>";
        #endif
    }
}
CPPTRACE_END_NAMESPACE

#endif
