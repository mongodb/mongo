
//          Copyright Oliver Kowalke 2009.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include "boost/coroutine/stack_traits.hpp"

extern "C" {
#include <signal.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <unistd.h>
}

//#if _POSIX_C_SOURCE >= 200112L

#include <algorithm>
#include <cmath>

#include <boost/assert.hpp>
#include <boost/config.hpp>

#if !defined (SIGSTKSZ)
# define SIGSTKSZ (8 * 1024)
# define UDEF_SIGSTKSZ
#endif

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

namespace {

std::size_t pagesize()
{
    // conform to POSIX.1-2001
    return static_cast<std::size_t>( ::sysconf( _SC_PAGESIZE) );
}

rlim_t stacksize_limit_()
{
    rlimit limit;
    // conforming to POSIX.1-2001
#if defined(BOOST_DISABLE_ASSERTS) || defined(NDEBUG)
    ::getrlimit( RLIMIT_STACK, & limit);
#else
    const int result = ::getrlimit( RLIMIT_STACK, & limit);
    BOOST_ASSERT( 0 == result);
#endif
    return limit.rlim_max;
}

rlim_t stacksize_limit() BOOST_NOEXCEPT_OR_NOTHROW {
    static rlim_t limit = stacksize_limit_();
    return limit;
}

}

namespace boost {
namespace coroutines {

bool
stack_traits::is_unbounded() BOOST_NOEXCEPT
{ return RLIM_INFINITY == stacksize_limit(); }

std::size_t
stack_traits::page_size() BOOST_NOEXCEPT
{
    static std::size_t size = pagesize();
    return size;
}

std::size_t
stack_traits::default_size() BOOST_NOEXCEPT
{
    std::size_t size = 8 * minimum_size();
    if ( is_unbounded() ) return size;

    BOOST_ASSERT( maximum_size() >= minimum_size() );
    return maximum_size() == size
        ? size
        : (std::min)( size, maximum_size() );
}

std::size_t
stack_traits::minimum_size() BOOST_NOEXCEPT
{ return static_cast<std::size_t>( SIGSTKSZ ); }

std::size_t
stack_traits::maximum_size() BOOST_NOEXCEPT
{
    BOOST_ASSERT( ! is_unbounded() );
    return static_cast< std::size_t >( stacksize_limit() );
}

}}

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif

#ifdef UDEF_SIGSTKSZ
# undef SIGSTKSZ
#endif
