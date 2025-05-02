/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */
/*!
 * \file scope/detail/type_traits/is_nothrow_swappable.hpp
 *
 * This header contains definition of \c is_nothrow_swappable type trait.
 */

#ifndef BOOST_SCOPE_DETAIL_TYPE_TRAITS_IS_NOTHROW_SWAPPABLE_HPP_INCLUDED_
#define BOOST_SCOPE_DETAIL_TYPE_TRAITS_IS_NOTHROW_SWAPPABLE_HPP_INCLUDED_

#include <type_traits>
#include <boost/scope/detail/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if (defined(__cpp_lib_is_swappable) && (__cpp_lib_is_swappable >= 201603l)) || \
    (defined(BOOST_MSSTL_VERSION) && (BOOST_MSSTL_VERSION >= 140) && (_MSC_FULL_VER >= 190024210) && (BOOST_CXX_VERSION >= 201703l))

namespace boost {
namespace scope {
namespace detail {

using std::is_nothrow_swappable;

} // namespace detail
} // namespace scope
} // namespace boost

#else

#include <boost/type_traits/is_nothrow_swappable.hpp>

namespace boost {
namespace scope {
namespace detail {

using boost::is_nothrow_swappable;

} // namespace detail
} // namespace scope
} // namespace boost

#endif

#endif // BOOST_SCOPE_DETAIL_TYPE_TRAITS_IS_NOTHROW_SWAPPABLE_HPP_INCLUDED_
