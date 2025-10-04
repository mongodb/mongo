/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */
/*!
 * \file scope/detail/type_traits/is_nothrow_invocable.hpp
 *
 * This header contains definition of \c is_nothrow_invocable type trait.
 */

#ifndef BOOST_SCOPE_DETAIL_TYPE_TRAITS_IS_NOTHROW_INVOCABLE_HPP_INCLUDED_
#define BOOST_SCOPE_DETAIL_TYPE_TRAITS_IS_NOTHROW_INVOCABLE_HPP_INCLUDED_

#include <type_traits>
#include <boost/scope/detail/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if (defined(__cpp_lib_is_invocable) && (__cpp_lib_is_invocable >= 201703l)) || \
    (defined(BOOST_MSSTL_VERSION) && (BOOST_MSSTL_VERSION >= 140) && (BOOST_CXX_VERSION >= 201703l))

namespace boost {
namespace scope {
namespace detail {

using std::is_nothrow_invocable;

} // namespace detail
} // namespace scope
} // namespace boost

#else

#include <boost/scope/detail/type_traits/is_invocable.hpp>

namespace boost {
namespace scope {
namespace detail {

template< bool, typename Func, typename... Args >
struct is_nothrow_invocable_impl
{
    using type = std::false_type;
};

template< typename Func, typename... Args >
struct is_nothrow_invocable_impl< true, Func, Args... >
{
    using type = std::integral_constant< bool, noexcept(std::declval< Func >()(std::declval< Args >()...)) >;
};

template< typename Func, typename... Args >
struct is_nothrow_invocable :
    public is_nothrow_invocable_impl< detail::is_invocable< Func, Args... >::value, Func, Args... >::type
{
};

} // namespace detail
} // namespace scope
} // namespace boost

#endif

#endif // BOOST_SCOPE_DETAIL_TYPE_TRAITS_IS_NOTHROW_INVOCABLE_HPP_INCLUDED_
