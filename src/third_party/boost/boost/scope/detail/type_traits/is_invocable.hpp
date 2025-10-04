/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */
/*!
 * \file scope/detail/type_traits/is_invocable.hpp
 *
 * This header contains definition of \c is_invocable type trait.
 */

#ifndef BOOST_SCOPE_DETAIL_TYPE_TRAITS_IS_INVOCABLE_HPP_INCLUDED_
#define BOOST_SCOPE_DETAIL_TYPE_TRAITS_IS_INVOCABLE_HPP_INCLUDED_

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

using std::is_invocable;

} // namespace detail
} // namespace scope
} // namespace boost

#else

namespace boost {
namespace scope {
namespace detail {

// A simplified implementation that does not support member function pointers
template< typename Func, typename... Args >
struct is_invocable_impl
{
    template< typename F = Func, typename = decltype(std::declval< F >()(std::declval< Args >()...)) >
    static std::true_type _check_invocable(int);
    static std::false_type _check_invocable(...);

    using type = decltype(is_invocable_impl::_check_invocable(0));
};

template< typename Func, typename... Args >
struct is_invocable : public is_invocable_impl< Func, Args... >::type { };

} // namespace detail
} // namespace scope
} // namespace boost

#endif

#endif // BOOST_SCOPE_DETAIL_TYPE_TRAITS_IS_INVOCABLE_HPP_INCLUDED_
