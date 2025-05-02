#ifndef BOOST_MP11_DETAIL_MP_FOLD_HPP_INCLUDED
#define BOOST_MP11_DETAIL_MP_FOLD_HPP_INCLUDED

//  Copyright 2015-2017 Peter Dimov.
//
//  Distributed under the Boost Software License, Version 1.0.
//
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include <boost/mp11/detail/config.hpp>
#include <boost/mp11/detail/mp_defer.hpp>
#include <boost/mp11/detail/mp_rename.hpp>
#include <boost/mp11/detail/mp_list.hpp>

namespace boost
{
namespace mp11
{

// mp_fold<L, V, F>
namespace detail
{

template<class L, class V, template<class...> class F> struct mp_fold_impl
{
// An error "no type named 'type'" here means that the first argument to mp_fold is not a list
};

#if BOOST_MP11_WORKAROUND( BOOST_MP11_MSVC, <= 1800 )

template<template<class...> class L, class... T, class V, template<class...> class F> struct mp_fold_impl<L<T...>, V, F>
{
    static_assert( sizeof...(T) == 0, "T... must be empty" );
    using type = V;
};

#else

template<template<class...> class L, class V, template<class...> class F> struct mp_fold_impl<L<>, V, F>
{
    using type = V;
};

#endif

//

template<class V, template<class...> class F> struct mp_fold_Q1
{
    template<class T1>
        using fn = F<V, T1>;
};

template<class V, template<class...> class F> struct mp_fold_Q2
{
    template<class T1, class T2>
        using fn = F<F<V, T1>, T2>;
};

template<class V, template<class...> class F> struct mp_fold_Q3
{
    template<class T1, class T2, class T3>
        using fn = F<F<F<V, T1>, T2>, T3>;
};

template<class V, template<class...> class F> struct mp_fold_Q4
{
    template<class T1, class T2, class T3, class T4>
        using fn = F<F<F<F<V, T1>, T2>, T3>, T4>;
};

template<class V, template<class...> class F> struct mp_fold_Q5
{
    template<class T1, class T2, class T3, class T4, class T5>
        using fn = F<F<F<F<F<V, T1>, T2>, T3>, T4>, T5>;
};

template<class V, template<class...> class F> struct mp_fold_Q6
{
    template<class T1, class T2, class T3, class T4, class T5, class T6>
        using fn = F<F<F<F<F<F<V, T1>, T2>, T3>, T4>, T5>, T6>;
};

template<class V, template<class...> class F> struct mp_fold_Q7
{
    template<class T1, class T2, class T3, class T4, class T5, class T6, class T7>
        using fn = F<F<F<F<F<F<F<V, T1>, T2>, T3>, T4>, T5>, T6>, T7>;
};

template<class V, template<class...> class F> struct mp_fold_Q8
{
    template<class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8>
        using fn = F<F<F<F<F<F<F<F<V, T1>, T2>, T3>, T4>, T5>, T6>, T7>, T8>;
};

template<class V, template<class...> class F> struct mp_fold_Q9
{
    template<class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9>
        using fn = F<F<F<F<F<F<F<F<F<V, T1>, T2>, T3>, T4>, T5>, T6>, T7>, T8>, T9>;
};

//

template<template<class...> class L, class T1, class V, template<class...> class F>
struct mp_fold_impl<L<T1>, V, F>: mp_defer<mp_fold_Q1<V, F>::template fn, T1>
{
};

template<template<class...> class L, class T1, class T2, class V, template<class...> class F>
struct mp_fold_impl<L<T1, T2>, V, F>: mp_defer<mp_fold_Q2<V, F>::template fn, T1, T2>
{
};

template<template<class...> class L, class T1, class T2, class T3, class V, template<class...> class F>
struct mp_fold_impl<L<T1, T2, T3>, V, F>: mp_defer<mp_fold_Q3<V, F>::template fn, T1, T2, T3>
{
};

template<template<class...> class L, class T1, class T2, class T3, class T4, class V, template<class...> class F>
struct mp_fold_impl<L<T1, T2, T3, T4>, V, F>: mp_defer<mp_fold_Q4<V, F>::template fn, T1, T2, T3, T4>
{
};

template<template<class...> class L, class T1, class T2, class T3, class T4, class T5, class V, template<class...> class F>
struct mp_fold_impl<L<T1, T2, T3, T4, T5>, V, F>: mp_defer<mp_fold_Q5<V, F>::template fn, T1, T2, T3, T4, T5>
{
};

template<template<class...> class L, class T1, class T2, class T3, class T4, class T5, class T6, class V, template<class...> class F>
struct mp_fold_impl<L<T1, T2, T3, T4, T5, T6>, V, F>: mp_defer<mp_fold_Q6<V, F>::template fn, T1, T2, T3, T4, T5, T6>
{
};

template<template<class...> class L, class T1, class T2, class T3, class T4, class T5, class T6, class T7, class V, template<class...> class F>
struct mp_fold_impl<L<T1, T2, T3, T4, T5, T6, T7>, V, F>: mp_defer<mp_fold_Q7<V, F>::template fn, T1, T2, T3, T4, T5, T6, T7>
{
};

template<template<class...> class L, class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class V, template<class...> class F>
struct mp_fold_impl<L<T1, T2, T3, T4, T5, T6, T7, T8>, V, F>: mp_defer<mp_fold_Q8<V, F>::template fn, T1, T2, T3, T4, T5, T6, T7, T8>
{
};

template<template<class...> class L, class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9, class V, template<class...> class F>
struct mp_fold_impl<L<T1, T2, T3, T4, T5, T6, T7, T8, T9>, V, F>: mp_defer<mp_fold_Q9<V, F>::template fn, T1, T2, T3, T4, T5, T6, T7, T8, T9>
{
};

//

template<template<class...> class L, class T1, class T2, class T3, class T4, class T5, class T6, class T7, class T8, class T9, class T10, class... T, class V, template<class...> class F>
struct mp_fold_impl<L<T1, T2, T3, T4, T5, T6, T7, T8, T9, T10, T...>, V, F>
{
    using type = typename mp_fold_impl<L<T...>, F<F<F<F<F<F<F<F<F<F<V, T1>, T2>, T3>, T4>, T5>, T6>, T7>, T8>, T9>, T10>, F>::type;
};

} // namespace detail

template<class L, class V, template<class...> class F> using mp_fold = typename detail::mp_fold_impl<mp_rename<L, mp_list>, V, F>::type;
template<class L, class V, class Q> using mp_fold_q = mp_fold<L, V, Q::template fn>;

} // namespace mp11
} // namespace boost

#endif // #ifndef BOOST_MP11_DETAIL_MP_FOLD_HPP_INCLUDED
