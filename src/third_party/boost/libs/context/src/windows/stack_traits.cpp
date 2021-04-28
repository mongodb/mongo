
//          Copyright Oliver Kowalke 2014.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include "boost/context/stack_traits.hpp"

extern "C" {
#include <windows.h>
}

//#if defined (BOOST_WINDOWS) || _POSIX_C_SOURCE >= 200112L

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <stdexcept>

#include <boost/assert.hpp>
#include <boost/context/detail/config.hpp>
#if defined(BOOST_NO_CXX11_HDR_MUTEX)
# include <boost/thread.hpp>
#else
# include <mutex>
#endif

#include <boost/context/stack_context.hpp>

// x86_64
// test x86_64 before i386 because icc might
// define __i686__ for x86_64 too
#if defined(__x86_64__) || defined(__x86_64) \
    || defined(__amd64__) || defined(__amd64) \
    || defined(_M_X64) || defined(_M_AMD64)

// Windows seams not to provide a constant or function
// telling the minimal stacksize
# define MIN_STACKSIZE  8 * 1024
#else
# define MIN_STACKSIZE  4 * 1024
#endif

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

namespace {

void system_info_( SYSTEM_INFO * si) BOOST_NOEXCEPT_OR_NOTHROW {
    ::GetSystemInfo( si);
}

SYSTEM_INFO system_info() BOOST_NOEXCEPT_OR_NOTHROW {
    static SYSTEM_INFO si;
#if defined(BOOST_NO_CXX11_HDR_MUTEX)
    static boost::once_flag flag = BOOST_ONCE_INIT;
    boost::call_once( flag, static_cast< void(*)( SYSTEM_INFO *) >( system_info_), & si);
#else
    static std::once_flag flag;
    std::call_once( flag, static_cast< void(*)( SYSTEM_INFO *) >( system_info_), & si);
#endif
    return si;
}

std::size_t pagesize() BOOST_NOEXCEPT_OR_NOTHROW {
    return static_cast< std::size_t >( system_info().dwPageSize);
}

}

namespace boost {
namespace context {

// Windows seams not to provide a limit for the stacksize
// libcoco uses 32k+4k bytes as minimum
BOOST_CONTEXT_DECL
bool
stack_traits::is_unbounded() BOOST_NOEXCEPT_OR_NOTHROW {
    return true;
}

BOOST_CONTEXT_DECL
std::size_t
stack_traits::page_size() BOOST_NOEXCEPT_OR_NOTHROW {
    return pagesize();
}

BOOST_CONTEXT_DECL
std::size_t
stack_traits::default_size() BOOST_NOEXCEPT_OR_NOTHROW {
    return 128 * 1024;
}

// because Windows seams not to provide a limit for minimum stacksize
BOOST_CONTEXT_DECL
std::size_t
stack_traits::minimum_size() BOOST_NOEXCEPT_OR_NOTHROW {
    return MIN_STACKSIZE;
}

// because Windows seams not to provide a limit for maximum stacksize
// maximum_size() can never be called (pre-condition ! is_unbounded() )
BOOST_CONTEXT_DECL
std::size_t
stack_traits::maximum_size() BOOST_NOEXCEPT_OR_NOTHROW {
    BOOST_ASSERT( ! is_unbounded() );
    return  1 * 1024 * 1024 * 1024; // 1GB
}

}}

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif
