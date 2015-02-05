
//          Copyright Oliver Kowalke 2009.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#ifndef BOOST_COROUTINES_DETAIL_TRAMPOLINE_H
#define BOOST_COROUTINES_DETAIL_TRAMPOLINE_H

#include <boost/assert.hpp>
#include <boost/config.hpp>
#include <boost/cstdint.hpp>

#include <boost/coroutine/detail/config.hpp>

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_PREFIX
#endif

namespace boost {
namespace coroutines {
namespace detail {

template< typename Coro >
void trampoline( intptr_t vp)
{
    typedef typename Coro::param_type   param_type;

    BOOST_ASSERT( 0 != vp);

    param_type * param(
        reinterpret_cast< param_type * >( vp) );
    BOOST_ASSERT( 0 != param);
    BOOST_ASSERT( 0 != param->data);

    Coro * coro(
        reinterpret_cast< Coro * >( param->coro) );
    BOOST_ASSERT( 0 != coro);

    coro->run( param->data);
}

template< typename Coro >
void trampoline_void( intptr_t vp)
{
    typedef typename Coro::param_type   param_type;

    BOOST_ASSERT( 0 != vp);

    param_type * param(
        reinterpret_cast< param_type * >( vp) );
    BOOST_ASSERT( 0 != param);

    Coro * coro(
        reinterpret_cast< Coro * >( param->coro) );
    BOOST_ASSERT( 0 != coro);
    
    coro->run();
}

}}}

#ifdef BOOST_HAS_ABI_HEADERS
#  include BOOST_ABI_SUFFIX
#endif

#endif // BOOST_COROUTINES_DETAIL_TRAMPOLINE_H
