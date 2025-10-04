/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */
/*!
 * \file scope/detail/is_nonnull_default_constructible.hpp
 *
 * This header contains definition of \c is_nonnull_default_constructible
 * and \c is_nothrow_nonnull_default_constructible type traits. The type
 * traits are useful for preventing default-construction of pointers to
 * functions where a default-constructed function object is expected.
 * Without it, default- or value-constructing a pointer to function would
 * produce a function object that is not callable.
 */

#ifndef BOOST_SCOPE_DETAIL_IS_NONNULL_DEFAULT_CONSTRUCTIBLE_HPP_INCLUDED_
#define BOOST_SCOPE_DETAIL_IS_NONNULL_DEFAULT_CONSTRUCTIBLE_HPP_INCLUDED_

#include <type_traits>
#include <boost/scope/detail/config.hpp>
#include <boost/scope/detail/header.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace scope {
namespace detail {

//! The type trait checks if \c T is not a pointer and is default-constructible
template< typename T >
struct is_nonnull_default_constructible :
    public std::is_default_constructible< T >
{
};

template< typename T >
struct is_nonnull_default_constructible< T* > :
    public std::false_type
{
};

//! The type trait checks if \c T is not a pointer and is nothrow-default-constructible
template< typename T >
struct is_nothrow_nonnull_default_constructible :
    public std::is_nothrow_default_constructible< T >
{
};

template< typename T >
struct is_nothrow_nonnull_default_constructible< T* > :
    public std::false_type
{
};

} // namespace detail
} // namespace scope
} // namespace boost

#include <boost/scope/detail/footer.hpp>

#endif // BOOST_SCOPE_DETAIL_IS_NONNULL_DEFAULT_CONSTRUCTIBLE_HPP_INCLUDED_
