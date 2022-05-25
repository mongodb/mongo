
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

std::size_t pagesize() BOOST_NOEXCEPT_OR_NOTHROW {
    SYSTEM_INFO si;
    ::GetSystemInfo(&si);
    return static_cast< std::size_t >( si.dwPageSize );
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
    static std::size_t size = pagesize();
    return size;
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
