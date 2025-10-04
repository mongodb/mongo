#ifndef BOOST_MP11_LIST_HPP_INCLUDED
#define BOOST_MP11_LIST_HPP_INCLUDED

//  Copyright 2015-2023 Peter Dimov.
//
//  Distributed under the Boost Software License, Version 1.0.
//
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include <boost/mp11/integral.hpp>
#include <boost/mp11/detail/mp_list.hpp>
#include <boost/mp11/detail/mp_list_v.hpp>
#include <boost/mp11/detail/mp_is_list.hpp>
#include <boost/mp11/detail/mp_is_value_list.hpp>
#include <boost/mp11/detail/mp_front.hpp>
#include <boost/mp11/detail/mp_rename.hpp>
#include <boost/mp11/detail/mp_append.hpp>
#include <boost/mp11/detail/config.hpp>
#include <type_traits>

#if defined(_MSC_VER) || defined(__GNUC__)
# pragma push_macro( "I" )
# undef I
#endif

namespace boost
{
namespace mp11
{

// mp_list<T...>
//   in detail/mp_list.hpp

// mp_list_c<T, I...>
template<class T, T... I> using mp_list_c = mp_list<std::integral_constant<T, I>...>;

// mp_list_v<A...>
//   in detail/mp_list_v.hpp

// mp_is_list<L>
//   in detail/mp_is_list.hpp

// mp_is_value_list<L>
//   in detail/mp_is_value_list.hpp

// mp_size<L>
namespace detail
{

template<class L> struct mp_size_impl
{
// An error "no type named 'type'" here means that the argument to mp_size is not a list
};

template<template<class...> class L, class... T> struct mp_size_impl<L<T...>>
{
    using type = mp_size_t<sizeof...(T)>;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto... A> struct mp_size_impl<L<A...>>
{
    using type = mp_size_t<sizeof...(A)>;
};

#endif

} // namespace detail

template<class L> using mp_size = typename detail::mp_size_impl<L>::type;

// mp_empty<L>
template<class L> using mp_empty = mp_bool< mp_size<L>::value == 0 >;

// mp_assign<L1, L2>
namespace detail
{

template<class L1, class L2> struct mp_assign_impl
{
// An error "no type named 'type'" here means that the arguments to mp_assign aren't lists
};

template<template<class...> class L1, class... T, template<class...> class L2, class... U> struct mp_assign_impl<L1<T...>, L2<U...>>
{
    using type = L1<U...>;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L1, auto... A, template<class...> class L2, class... U> struct mp_assign_impl<L1<A...>, L2<U...>>
{
    using type = L1<U::value...>;
};

template<template<class...> class L1, class... T, template<auto...> class L2, auto... B> struct mp_assign_impl<L1<T...>, L2<B...>>
{
    using type = L1<mp_value<B>...>;
};

template<template<auto...> class L1, auto... A, template<auto...> class L2, auto... B> struct mp_assign_impl<L1<A...>, L2<B...>>
{
    using type = L1<B...>;
};

#endif

} // namespace detail

template<class L1, class L2> using mp_assign = typename detail::mp_assign_impl<L1, L2>::type;

// mp_clear<L>
template<class L> using mp_clear = mp_assign<L, mp_list<>>;

// mp_front<L>
//   in detail/mp_front.hpp

// mp_pop_front<L>
namespace detail
{

template<class L> struct mp_pop_front_impl
{
// An error "no type named 'type'" here means that the argument to mp_pop_front
// is either not a list, or is an empty list
};

template<template<class...> class L, class T1, class... T> struct mp_pop_front_impl<L<T1, T...>>
{
    using type = L<T...>;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto A1, auto... A> struct mp_pop_front_impl<L<A1, A...>>
{
    using type = L<A...>;
};

#endif

} // namespace detail

template<class L> using mp_pop_front = typename detail::mp_pop_front_impl<L>::type;

// mp_first<L>
template<class L> using mp_first = mp_front<L>;

// mp_rest<L>
template<class L> using mp_rest = mp_pop_front<L>;

// mp_second<L>
namespace detail
{

template<class L> struct mp_second_impl
{
// An error "no type named 'type'" here means that the argument to mp_second
// is either not a list, or has fewer than two elements
};

template<template<class...> class L, class T1, class T2, class... T> struct mp_second_impl<L<T1, T2, T...>>
{
    using type = T2;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto A1, auto A2, auto... A> struct mp_second_impl<L<A1, A2, A...>>
{
    using type = mp_value<A2>;
};

#endif

} // namespace detail

template<class L> using mp_second = typename detail::mp_second_impl<L>::type;

// mp_third<L>
namespace detail
{

template<class L> struct mp_third_impl
{
// An error "no type named 'type'" here means that the argument to mp_third
// is either not a list, or has fewer than three elements
};

template<template<class...> class L, class T1, class T2, class T3, class... T> struct mp_third_impl<L<T1, T2, T3, T...>>
{
    using type = T3;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto A1, auto A2, auto A3, auto... A> struct mp_third_impl<L<A1, A2, A3, A...>>
{
    using type = mp_value<A3>;
};

#endif

} // namespace detail

template<class L> using mp_third = typename detail::mp_third_impl<L>::type;

// mp_push_front<L, T...>
namespace detail
{

template<class L, class... T> struct mp_push_front_impl
{
// An error "no type named 'type'" here means that the first argument to mp_push_front is not a list
};

template<template<class...> class L, class... U, class... T> struct mp_push_front_impl<L<U...>, T...>
{
    using type = L<T..., U...>;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto... A, class... T> struct mp_push_front_impl<L<A...>, T...>
{
    using type = L<T::value..., A...>;
};

#endif

} // namespace detail

template<class L, class... T> using mp_push_front = typename detail::mp_push_front_impl<L, T...>::type;

// mp_push_back<L, T...>
namespace detail
{

template<class L, class... T> struct mp_push_back_impl
{
// An error "no type named 'type'" here means that the first argument to mp_push_back is not a list
};

template<template<class...> class L, class... U, class... T> struct mp_push_back_impl<L<U...>, T...>
{
    using type = L<U..., T...>;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto... A, class... T> struct mp_push_back_impl<L<A...>, T...>
{
    using type = L<A..., T::value...>;
};

#endif

} // namespace detail

template<class L, class... T> using mp_push_back = typename detail::mp_push_back_impl<L, T...>::type;

// mp_rename<L, B>
// mp_apply<F, L>
// mp_apply_q<Q, L>
//   in detail/mp_rename.hpp

// mp_rename_v<L, B>
#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

namespace detail
{

template<class L, template<auto...> class B> struct mp_rename_v_impl
{
// An error "no type named 'type'" here means that the first argument to mp_rename_v is not a list
};

template<template<class...> class L, class... T, template<auto...> class B> struct mp_rename_v_impl<L<T...>, B>
{
    using type = B<T::value...>;
};

template<template<auto...> class L, auto... A, template<auto...> class B> struct mp_rename_v_impl<L<A...>, B>
{
    using type = B<A...>;
};

} // namespace detail

template<class L, template<auto...> class B> using mp_rename_v = typename detail::mp_rename_v_impl<L, B>::type;

#endif

// mp_replace_front<L, T>
namespace detail
{

template<class L, class T> struct mp_replace_front_impl
{
// An error "no type named 'type'" here means that the first argument to mp_replace_front
// is either not a list, or is an empty list
};

template<template<class...> class L, class U1, class... U, class T> struct mp_replace_front_impl<L<U1, U...>, T>
{
    using type = L<T, U...>;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto A1, auto... A, class T> struct mp_replace_front_impl<L<A1, A...>, T>
{
    using type = L<T::value, A...>;
};

#endif

} // namespace detail

template<class L, class T> using mp_replace_front = typename detail::mp_replace_front_impl<L, T>::type;

// mp_replace_first<L, T>
template<class L, class T> using mp_replace_first = typename detail::mp_replace_front_impl<L, T>::type;

// mp_replace_second<L, T>
namespace detail
{

template<class L, class T> struct mp_replace_second_impl
{
// An error "no type named 'type'" here means that the first argument to mp_replace_second
// is either not a list, or has fewer than two elements
};

template<template<class...> class L, class U1, class U2, class... U, class T> struct mp_replace_second_impl<L<U1, U2, U...>, T>
{
    using type = L<U1, T, U...>;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto A1, auto A2, auto... A, class T> struct mp_replace_second_impl<L<A1, A2, A...>, T>
{
    using type = L<A1, T::value, A...>;
};

#endif

} // namespace detail

template<class L, class T> using mp_replace_second = typename detail::mp_replace_second_impl<L, T>::type;

// mp_replace_third<L, T>
namespace detail
{

template<class L, class T> struct mp_replace_third_impl
{
// An error "no type named 'type'" here means that the first argument to mp_replace_third
// is either not a list, or has fewer than three elements
};

template<template<class...> class L, class U1, class U2, class U3, class... U, class T> struct mp_replace_third_impl<L<U1, U2, U3, U...>, T>
{
    using type = L<U1, U2, T, U...>;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto A1, auto A2, auto A3, auto... A, class T> struct mp_replace_third_impl<L<A1, A2, A3, A...>, T>
{
    using type = L<A1, A2, T::value, A...>;
};

#endif

} // namespace detail

template<class L, class T> using mp_replace_third = typename detail::mp_replace_third_impl<L, T>::type;

// mp_transform_front<L, F>
namespace detail
{

template<class L, template<class...> class F> struct mp_transform_front_impl
{
// An error "no type named 'type'" here means that the first argument to mp_transform_front
// is either not a list, or is an empty list
};

template<template<class...> class L, class U1, class... U, template<class...> class F> struct mp_transform_front_impl<L<U1, U...>, F>
{
    using type = L<F<U1>, U...>;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto A1, auto... A, template<class...> class F> struct mp_transform_front_impl<L<A1, A...>, F>
{
    using type = L<F<mp_value<A1>>::value, A...>;
};

#endif

} // namespace detail

template<class L, template<class...> class F> using mp_transform_front = typename detail::mp_transform_front_impl<L, F>::type;
template<class L, class Q> using mp_transform_front_q = mp_transform_front<L, Q::template fn>;

// mp_transform_first<L, F>
template<class L, template<class...> class F> using mp_transform_first = typename detail::mp_transform_front_impl<L, F>::type;
template<class L, class Q> using mp_transform_first_q = mp_transform_first<L, Q::template fn>;

// mp_transform_second<L, F>
namespace detail
{

template<class L, template<class...> class F> struct mp_transform_second_impl
{
// An error "no type named 'type'" here means that the first argument to mp_transform_second
// is either not a list, or has fewer than two elements
};

template<template<class...> class L, class U1, class U2, class... U, template<class...> class F> struct mp_transform_second_impl<L<U1, U2, U...>, F>
{
    using type = L<U1, F<U2>, U...>;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto A1, auto A2, auto... A, template<class...> class F> struct mp_transform_second_impl<L<A1, A2, A...>, F>
{
    using type = L<A1, F<mp_value<A2>>::value, A...>;
};

#endif

} // namespace detail

template<class L, template<class...> class F> using mp_transform_second = typename detail::mp_transform_second_impl<L, F>::type;
template<class L, class Q> using mp_transform_second_q = mp_transform_second<L, Q::template fn>;

// mp_transform_third<L, F>
namespace detail
{

template<class L, template<class...> class F> struct mp_transform_third_impl
{
// An error "no type named 'type'" here means that the first argument to mp_transform_third
// is either not a list, or has fewer than three elements
};

template<template<class...> class L, class U1, class U2, class U3, class... U, template<class...> class F> struct mp_transform_third_impl<L<U1, U2, U3, U...>, F>
{
    using type = L<U1, U2, F<U3>, U...>;
};

#if defined(BOOST_MP11_HAS_TEMPLATE_AUTO)

template<template<auto...> class L, auto A1, auto A2, auto A3, auto... A, template<class...> class F> struct mp_transform_third_impl<L<A1, A2, A3, A...>, F>
{
    using type = L<A1, A2, F<mp_value<A3>>::value, A...>;
};

#endif

} // namespace detail

template<class L, template<class...> class F> using mp_transform_third = typename detail::mp_transform_third_impl<L, F>::type;
template<class L, class Q> using mp_transform_third_q = mp_transform_third<L, Q::template fn>;

} // namespace mp11
} // namespace boost

#if defined(_MSC_VER) || defined(__GNUC__)
# pragma pop_macro( "I" )
#endif

#endif // #ifndef BOOST_MP11_LIST_HPP_INCLUDED
