// Copyright (C) 2007, 2008 Steven Watanabe, Joseph Gauterin, Niels Dekker
// Copyright (C) 2023 Andrey Semashev
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)
// For more information, see http://www.boost.org

#ifndef BOOST_CORE_INVOKE_SWAP_HPP
#define BOOST_CORE_INVOKE_SWAP_HPP

// Note: the implementation of this utility contains various workarounds:
// - invoke_swap_impl is put outside the boost namespace, to avoid infinite
// recursion (causing stack overflow) when swapping objects of a primitive
// type.
// - std::swap is imported with a using-directive, rather than
// a using-declaration, because some compilers (including MSVC 7.1,
// Borland 5.9.3, and Intel 8.1) don't do argument-dependent lookup
// when it has a using-declaration instead.
// - The main entry function is called invoke_swap rather than swap
// to avoid forming an infinite recursion when the arguments are not
// swappable.

#include <boost/core/enable_if.hpp>
#include <boost/config.hpp>
#if __cplusplus >= 201103L || defined(BOOST_DINKUMWARE_STDLIB)
#include <utility> // for std::swap (C++11)
#else
#include <algorithm> // for std::swap (C++98)
#endif
#include <cstddef> // for std::size_t

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if defined(BOOST_GCC) && (BOOST_GCC < 40700)
// gcc 4.6 ICEs on noexcept specifications below
#define BOOST_CORE_SWAP_NOEXCEPT_IF(x)
#else
#define BOOST_CORE_SWAP_NOEXCEPT_IF(x) BOOST_NOEXCEPT_IF(x)
#endif

namespace boost_swap_impl {

// we can't use type_traits here

template<class T> struct is_const { enum _vt { value = 0 }; };
template<class T> struct is_const<T const> { enum _vt { value = 1 }; };

// Use std::swap if argument dependent lookup fails.
// We need to have this at namespace scope to be able to use unqualified swap() call
// in noexcept specification.
using namespace std;

template<class T>
BOOST_GPU_ENABLED
inline void invoke_swap_impl(T& left, T& right) BOOST_CORE_SWAP_NOEXCEPT_IF(BOOST_NOEXCEPT_EXPR(swap(left, right)))
{
    swap(left, right);
}

template<class T, std::size_t N>
BOOST_GPU_ENABLED
inline void invoke_swap_impl(T (& left)[N], T (& right)[N])
    BOOST_CORE_SWAP_NOEXCEPT_IF(BOOST_NOEXCEPT_EXPR(::boost_swap_impl::invoke_swap_impl(left[0], right[0])))
{
    for (std::size_t i = 0; i < N; ++i)
    {
        ::boost_swap_impl::invoke_swap_impl(left[i], right[i]);
    }
}

} // namespace boost_swap_impl

namespace boost {
namespace core {

template<class T>
BOOST_GPU_ENABLED
inline typename enable_if_c< !::boost_swap_impl::is_const<T>::value >::type
invoke_swap(T& left, T& right)
    BOOST_CORE_SWAP_NOEXCEPT_IF(BOOST_NOEXCEPT_EXPR(::boost_swap_impl::invoke_swap_impl(left, right)))
{
    ::boost_swap_impl::invoke_swap_impl(left, right);
}

} // namespace core
} // namespace boost

#undef BOOST_CORE_SWAP_NOEXCEPT_IF

#endif // BOOST_CORE_INVOKE_SWAP_HPP
