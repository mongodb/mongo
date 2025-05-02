/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2025 Andrey Semashev
 */

#ifndef BOOST_ITERATOR_DETAIL_IF_DEFAULT_HPP_INCLUDED_
#define BOOST_ITERATOR_DETAIL_IF_DEFAULT_HPP_INCLUDED_

#include <boost/core/use_default.hpp>

namespace boost {
namespace iterators {
namespace detail {

// If T is use_default, return Default, otherwise - Nondefault.
// By default, Nondefault is T, which means
// the metafunction can be called with just two parameters
// and in that case will return either T or Default.
template< typename T, typename Default, typename Nondefault = T >
struct if_default
{
    using type = Nondefault;
};

template< typename Default, typename Nondefault >
struct if_default< use_default, Default, Nondefault >
{
    using type = Default;
};

template< typename T, typename Default, typename Nondefault = T >
using if_default_t = typename if_default< T, Default, Nondefault >::type;

} // namespace detail
} // namespace iterators
} // namespace boost

#endif // BOOST_ITERATOR_DETAIL_IF_DEFAULT_HPP_INCLUDED_
