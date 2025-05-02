/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * https://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2025 Andrey Semashev
 */

#ifndef BOOST_ITERATOR_DETAIL_EVAL_IF_DEFAULT_HPP_INCLUDED_
#define BOOST_ITERATOR_DETAIL_EVAL_IF_DEFAULT_HPP_INCLUDED_

#include <boost/core/use_default.hpp>
#include <boost/iterator/detail/type_traits/type_identity.hpp>

namespace boost {
namespace iterators {
namespace detail {

// If T is use_default, return the result of invoking
// DefaultNullaryFn, otherwise - of NondefaultNullaryFn.
// By default, NondefaultNullaryFn returns T, which means
// the metafunction can be called with just two parameters
// and in that case will return either T or the result of
// invoking DefaultNullaryFn.
template< typename T, typename DefaultNullaryFn, typename NondefaultNullaryFn = detail::type_identity< T > >
struct eval_if_default
{
    using type = typename NondefaultNullaryFn::type;
};

template< typename DefaultNullaryFn, typename NondefaultNullaryFn >
struct eval_if_default< use_default, DefaultNullaryFn, NondefaultNullaryFn >
{
    using type = typename DefaultNullaryFn::type;
};

template< typename T, typename DefaultNullaryFn, typename NondefaultNullaryFn = detail::type_identity< T > >
using eval_if_default_t = typename eval_if_default< T, DefaultNullaryFn, NondefaultNullaryFn >::type;

} // namespace detail
} // namespace iterators
} // namespace boost

#endif // BOOST_ITERATOR_DETAIL_EVAL_IF_DEFAULT_HPP_INCLUDED_
