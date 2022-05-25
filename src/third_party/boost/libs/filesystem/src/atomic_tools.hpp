//  atomic_tools.hpp  ------------------------------------------------------------------//

//  Copyright 2021 Andrey Semashev

//  Distributed under the Boost Software License, Version 1.0.
//  See http://www.boost.org/LICENSE_1_0.txt

//  See library home page at http://www.boost.org/libs/filesystem

//--------------------------------------------------------------------------------------//

#ifndef BOOST_FILESYSTEM_SRC_ATOMIC_TOOLS_HPP_
#define BOOST_FILESYSTEM_SRC_ATOMIC_TOOLS_HPP_

#include <boost/filesystem/config.hpp>

#if !defined(BOOST_FILESYSTEM_SINGLE_THREADED)

#include "atomic_ref.hpp"

namespace boost {
namespace filesystem {
namespace detail {

//! Atomically loads the value
template< typename T >
BOOST_FORCEINLINE T atomic_load_relaxed(T& a)
{
    return atomic_ns::atomic_ref< T >(a).load(atomic_ns::memory_order_relaxed);
}

//! Atomically stores the value
template< typename T >
BOOST_FORCEINLINE void atomic_store_relaxed(T& a, T val)
{
    atomic_ns::atomic_ref< T >(a).store(val, atomic_ns::memory_order_relaxed);
}

} // namespace detail
} // namespace filesystem
} // namespace boost

#else // !defined(BOOST_FILESYSTEM_SINGLE_THREADED)

namespace boost {
namespace filesystem {
namespace detail {

//! Atomically loads the value
template< typename T >
BOOST_FORCEINLINE T atomic_load_relaxed(T const& a)
{
    return a;
}

//! Atomically stores the value
template< typename T >
BOOST_FORCEINLINE void atomic_store_relaxed(T& a, T val)
{
    a = val;
}

} // namespace detail
} // namespace filesystem
} // namespace boost

#endif // !defined(BOOST_FILESYSTEM_SINGLE_THREADED)

#endif // BOOST_FILESYSTEM_SRC_ATOMIC_TOOLS_HPP_
