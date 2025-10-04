/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2021 Andrey Semashev
 */
/*!
 * \file   atomic/detail/type_traits/has_unique_object_representations.hpp
 *
 * This header defines \c has_unique_object_representations type trait
 */

#ifndef BOOST_ATOMIC_DETAIL_TYPE_TRAITS_HAS_UNIQUE_OBJECT_REPRESENTATIONS_HPP_INCLUDED_
#define BOOST_ATOMIC_DETAIL_TYPE_TRAITS_HAS_UNIQUE_OBJECT_REPRESENTATIONS_HPP_INCLUDED_

#include <boost/atomic/detail/config.hpp>
#if !defined(BOOST_ATOMIC_DETAIL_NO_CXX11_BASIC_HDR_TYPE_TRAITS)
#include <type_traits>
#endif

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if (defined(__cpp_lib_has_unique_object_representations) && __cpp_lib_has_unique_object_representations >= 201606) || \
    (defined(_CPPLIB_VER) && _CPPLIB_VER >= 650 && defined(_MSVC_STL_VERSION) && _MSVC_STL_VERSION >= 141 && defined(_HAS_CXX17) && _HAS_CXX17 != 0)

namespace boost {
namespace atomics {
namespace detail {

using std::has_unique_object_representations;

} // namespace detail
} // namespace atomics
} // namespace boost

#else // defined(__cpp_lib_has_unique_object_representations) ...

#if (defined(__GNUC__) && __GNUC__ >= 7) || (defined(BOOST_MSVC) && BOOST_MSVC >= 1929) || \
    (defined(__INTEL_COMPILER) && __INTEL_COMPILER >= 1900)
#define BOOST_ATOMIC_DETAIL_HAS_UNIQUE_OBJECT_REPRESENTATIONS(x) __has_unique_object_representations(x)
#elif defined(__is_identifier)
#if !__is_identifier(__has_unique_object_representations)
#define BOOST_ATOMIC_DETAIL_HAS_UNIQUE_OBJECT_REPRESENTATIONS(x) __has_unique_object_representations(x)
#endif
#endif

#if defined(BOOST_ATOMIC_DETAIL_HAS_UNIQUE_OBJECT_REPRESENTATIONS)

#include <cstddef>
#include <boost/atomic/detail/type_traits/integral_constant.hpp>

namespace boost {
namespace atomics {
namespace detail {

template< typename T >
struct has_unique_object_representations :
    public atomics::detail::integral_constant< bool, BOOST_ATOMIC_DETAIL_HAS_UNIQUE_OBJECT_REPRESENTATIONS(T) >
{
};

template< typename T >
struct has_unique_object_representations< T[] > :
    public atomics::detail::has_unique_object_representations< T >
{
};

template< typename T, std::size_t N >
struct has_unique_object_representations< T[N] > :
    public atomics::detail::has_unique_object_representations< T >
{
};

template< typename T >
struct has_unique_object_representations< const T > :
    public atomics::detail::has_unique_object_representations< T >
{
};

template< typename T >
struct has_unique_object_representations< volatile T > :
    public atomics::detail::has_unique_object_representations< T >
{
};

template< typename T >
struct has_unique_object_representations< const volatile T > :
    public atomics::detail::has_unique_object_representations< T >
{
};

template< typename T >
struct has_unique_object_representations< const T[] > :
    public atomics::detail::has_unique_object_representations< T >
{
};

template< typename T >
struct has_unique_object_representations< volatile T[] > :
    public atomics::detail::has_unique_object_representations< T >
{
};

template< typename T >
struct has_unique_object_representations< const volatile T[] > :
    public atomics::detail::has_unique_object_representations< T >
{
};

template< typename T, std::size_t N >
struct has_unique_object_representations< const T[N] > :
    public atomics::detail::has_unique_object_representations< T >
{
};

template< typename T, std::size_t N >
struct has_unique_object_representations< volatile T[N] > :
    public atomics::detail::has_unique_object_representations< T >
{
};

template< typename T, std::size_t N >
struct has_unique_object_representations< const volatile T[N] > :
    public atomics::detail::has_unique_object_representations< T >
{
};

} // namespace detail
} // namespace atomics
} // namespace boost

#else // defined(BOOST_ATOMIC_DETAIL_HAS_UNIQUE_OBJECT_REPRESENTATIONS)

#define BOOST_ATOMIC_DETAIL_NO_HAS_UNIQUE_OBJECT_REPRESENTATIONS

#endif // defined(BOOST_ATOMIC_DETAIL_HAS_UNIQUE_OBJECT_REPRESENTATIONS)

#endif // defined(__cpp_lib_has_unique_object_representations) ...

#endif // BOOST_ATOMIC_DETAIL_TYPE_TRAITS_HAS_UNIQUE_OBJECT_REPRESENTATIONS_HPP_INCLUDED_
