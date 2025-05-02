/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2024 Georgiy Guminov
 */
/*!
 * \file iterator/detail/type_traits/conjunction.hpp
 *
 * This header contains definition of \c conjunction type trait.
 */

#ifndef BOOST_ITERATOR_DETAIL_TYPE_TRAITS_CONJUNCTION_HPP_INCLUDED_
#define BOOST_ITERATOR_DETAIL_TYPE_TRAITS_CONJUNCTION_HPP_INCLUDED_

#include <type_traits>
#include <boost/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if (defined(__cpp_lib_logical_traits) && (__cpp_lib_logical_traits >= 201510l)) || \
    (defined(BOOST_MSSTL_VERSION) && (BOOST_MSSTL_VERSION >= 140) && (_MSC_FULL_VER >= 190023918) && (BOOST_CXX_VERSION >= 201703l))

namespace boost {
namespace iterators {
namespace detail {

using std::conjunction;

} // namespace detail
} // namespace iterator
} // namespace boost

#else

#include <boost/type_traits/conjunction.hpp>

namespace boost {
namespace iterators {
namespace detail {

using boost::conjunction;

} // namespace detail
} // namespace iterator
} // namespace boost

#endif

#endif // BOOST_ITERATOR_DETAIL_TYPE_TRAITS_CONJUNCTION_HPP_INCLUDED_
