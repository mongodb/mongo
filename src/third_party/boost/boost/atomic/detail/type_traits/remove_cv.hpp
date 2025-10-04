/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2023 Andrey Semashev
 */
/*!
 * \file   atomic/detail/type_traits/remove_cv.hpp
 *
 * This header defines \c remove_cv type trait
 */

#ifndef BOOST_ATOMIC_DETAIL_TYPE_TRAITS_REMOVE_CV_HPP_INCLUDED_
#define BOOST_ATOMIC_DETAIL_TYPE_TRAITS_REMOVE_CV_HPP_INCLUDED_

#include <boost/atomic/detail/config.hpp>
#if !defined(BOOST_ATOMIC_DETAIL_NO_CXX11_BASIC_HDR_TYPE_TRAITS)
#include <type_traits>
#else
#include <boost/type_traits/remove_cv.hpp>
#endif

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

namespace boost {
namespace atomics {
namespace detail {

#if !defined(BOOST_ATOMIC_DETAIL_NO_CXX11_BASIC_HDR_TYPE_TRAITS)
using std::remove_cv;
#else
using boost::remove_cv;
#endif

} // namespace detail
} // namespace atomics
} // namespace boost

#endif // BOOST_ATOMIC_DETAIL_TYPE_TRAITS_REMOVE_CV_HPP_INCLUDED_
