#ifndef BOOST_MP11_DETAIL_MP_IS_VALUE_LIST_HPP_INCLUDED
#define BOOST_MP11_DETAIL_MP_IS_VALUE_LIST_HPP_INCLUDED

// Copyright 2023 Peter Dimov
// Distributed under the Boost Software License, Version 1.0.
// https://www.boost.org/LICENSE_1_0.txt

#include <boost/mp11/integral.hpp>
#include <boost/mp11/detail/config.hpp>

namespace boost
{
namespace mp11
{

// mp_is_value_list<L>
namespace detail
{

template<class L> struct mp_is_value_list_impl
{
    using type = mp_false;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto... A> struct mp_is_value_list_impl<L<A...>>
{
    using type = mp_true;
};

#endif

} // namespace detail

template<class L> using mp_is_value_list = typename detail::mp_is_value_list_impl<L>::type;

} // namespace mp11
} // namespace boost

#endif // #ifndef BOOST_MP11_DETAIL_MP_IS_VALUE_LIST_HPP_INCLUDED
