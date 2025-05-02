/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2025 Andrey Semashev
 */
/*!
 * \file iterator/detail/type_traits/type_identity.hpp
 *
 * This header contains definition of \c negation type trait.
 */

#ifndef BOOST_ITERATOR_DETAIL_TYPE_TRAITS_TYPE_IDENTITY_HPP_INCLUDED_
#define BOOST_ITERATOR_DETAIL_TYPE_TRAITS_TYPE_IDENTITY_HPP_INCLUDED_

#include <type_traits>
#include <boost/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if (defined(__cpp_lib_type_identity) && (__cpp_lib_type_identity >= 201806l)) || \
    /* Note: MSVC 19.21 does not define _MSVC_LANG to 202002 in c++latest (C++20) mode but to a value larger than 201703 */ \
    (defined(BOOST_MSSTL_VERSION) && (BOOST_MSSTL_VERSION >= 142) && (_MSC_VER >= 1921) && (BOOST_CXX_VERSION > 201703l))

namespace boost {
namespace iterators {
namespace detail {

using std::type_identity;

} // namespace detail
} // namespace iterator
} // namespace boost

#else

#include <boost/type_traits/type_identity.hpp>

namespace boost {
namespace iterators {
namespace detail {

using boost::type_identity;

} // namespace detail
} // namespace iterator
} // namespace boost

#endif

#endif // BOOST_ITERATOR_DETAIL_TYPE_TRAITS_TYPE_IDENTITY_HPP_INCLUDED_
