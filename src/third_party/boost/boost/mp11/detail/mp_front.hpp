#ifndef BOOST_MP11_DETAIL_MP_FRONT_HPP_INCLUDED
#define BOOST_MP11_DETAIL_MP_FRONT_HPP_INCLUDED

//  Copyright 2015-2023 Peter Dimov.
//
//  Distributed under the Boost Software License, Version 1.0.
//
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include <boost/mp11/detail/mp_value.hpp>
#include <boost/mp11/detail/config.hpp>

namespace boost
{
namespace mp11
{

// mp_front<L>
namespace detail
{

template<class L> struct mp_front_impl
{
// An error "no type named 'type'" here means that the argument to mp_front
// is either not a list, or is an empty list
};

template<template<class...> class L, class T1, class... T> struct mp_front_impl<L<T1, T...>>
{
    using type = T1;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto A1, auto... A> struct mp_front_impl<L<A1, A...>>
{
    using type = mp_value<A1>;
};

#endif

} // namespace detail

template<class L> using mp_front = typename detail::mp_front_impl<L>::type;

} // namespace mp11
} // namespace boost

#endif // #ifndef BOOST_MP11_DETAIL_MP_FRONT_HPP_INCLUDED
