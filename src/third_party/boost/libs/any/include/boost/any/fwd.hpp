// Copyright Antony Polukhin, 2021-2025.
//
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

// Contributed by Ruslan Arutyunyan
#ifndef BOOST_ANY_ANYS_FWD_HPP
#define BOOST_ANY_ANYS_FWD_HPP

#include <boost/config.hpp>
#ifdef BOOST_HAS_PRAGMA_ONCE
# pragma once
#endif

/// \file boost/any/fwd.hpp
/// \brief Forward declarations of Boost.Any library types.

/// @cond
namespace boost {

class any;

namespace anys {

class unique_any;

template<std::size_t OptimizeForSize = sizeof(void*), std::size_t OptimizeForAlignment = alignof(void*)>
class basic_any;

namespace detail {

    template <class T>
    struct is_basic_any: public std::false_type {};

    template<std::size_t OptimizeForSize, std::size_t OptimizeForAlignment>
    struct is_basic_any<boost::anys::basic_any<OptimizeForSize, OptimizeForAlignment> > : public std::true_type {};

    template <class T>
    struct is_some_any: public is_basic_any<T> {};

    template <>
    struct is_some_any<boost::any>: public std::true_type {};

    template <>
    struct is_some_any<boost::anys::unique_any>: public std::true_type {};

} // namespace detail

} // namespace anys

} // namespace boost
/// @endcond

#endif  // #ifndef BOOST_ANY_ANYS_FWD_HPP
