#ifndef BOOST_MP11_LAMBDA_HPP_INCLUDED
#define BOOST_MP11_LAMBDA_HPP_INCLUDED

//  Copyright 2024 Joaquin M Lopez Munoz.
//
//  Distributed under the Boost Software License, Version 1.0.
//
//  See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include <boost/mp11/detail/config.hpp>

#if BOOST_MP11_WORKAROUND(BOOST_MP11_MSVC, <= 1800)

// mp_lambda not supported due to compiler limitations

#else

#include <boost/mp11/bind.hpp>
#include <cstddef>
#include <type_traits>

#if defined(_MSC_VER) || defined(__GNUC__)
# pragma push_macro( "I" )
# undef I
#endif

namespace boost
{
namespace mp11
{
namespace detail
{

template<class T> struct lambda_impl;

} // namespace detail

// mp_lambda
template<class T> using mp_lambda = typename detail::lambda_impl<T>::type;

namespace detail
{

// base case (no placeholder replacement)
template<class T> struct lambda_impl
{
    template<class... U> using make = T;
    using type = mp_bind<make>;
};

// placeholders (behave directly as mp_bind expressions)
template<std::size_t I> struct lambda_impl<mp_arg<I>>
{
    using type = mp_arg<I>;
};

#define BOOST_MP11_SPECIALIZE_LAMBDA_IMPL(name, compound_type) \
template<class T> using lambda_make_##name = compound_type;    \
                                                               \
template<class T> struct lambda_impl<compound_type>            \
{                                                              \
    using type = mp_bind<lambda_make_##name, mp_lambda<T>>;    \
};

// [basic.type.qualifier]
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL(const, const T)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL(volatile, volatile T)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL(const_volatile, const volatile T)

// [dcl.ptr]
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL(pointer, T*)

// [dcl.ref]
// GCC < 7 fails with template<class U> using make = U&;
template<class T> struct lambda_impl<T&>
{
    template<class U> using make = typename std::add_lvalue_reference<U>::type;
    using type = mp_bind<make, mp_lambda<T>>;
};

template<class T> struct lambda_impl<T&&>
{
    template<class U> using make = typename std::add_rvalue_reference<U>::type;
    using type = mp_bind<make, mp_lambda<T>>;
};

// [dcl.array]
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL(array, T[])

#undef BOOST_MP11_SPECIALIZE_LAMBDA_IMPL

template<class T, std::size_t N> struct lambda_impl<T[N]>
{
    template<class Q> using make = Q[N];
    using type = mp_bind<make, mp_lambda<T>>;
};

// [dcl.fct], [dcl.mptr] (member functions)
#define BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(name, qualifier)                 \
template<class R, class... T> using lambda_make_fct_##name = R(T...) qualifier;          \
                                                                                         \
template<class R, class... T> struct lambda_impl<R(T...) qualifier>                      \
{                                                                                        \
    using type =  mp_bind<                                                               \
        lambda_make_fct_##name,                                                          \
        mp_lambda<R>, mp_lambda<T>...>;                                                  \
};                                                                                       \
                                                                                         \
template<class R, class... T> using lambda_make_fct_##name##_ellipsis =                  \
    R(T..., ...) qualifier;                                                              \
                                                                                         \
template<class R, class... T> struct lambda_impl<R(T..., ...) qualifier>                 \
{                                                                                        \
    using type = mp_bind<                                                                \
        lambda_make_fct_##name##_ellipsis,                                               \
        mp_lambda<R>, mp_lambda<T>...>;                                                  \
};                                                                                       \
                                                                                         \
template<class R, class C, class... T> using lambda_make_mfptr_##name =                  \
    R (C::*)(T...) qualifier;                                                            \
                                                                                         \
template<class R, class C, class... T> struct lambda_impl<R (C::*)(T...) qualifier>      \
{                                                                                        \
    using type = mp_bind<                                                                \
        lambda_make_mfptr_##name,                                                        \
        mp_lambda<R>, mp_lambda<C>, mp_lambda<T>...>;                                    \
};                                                                                       \
                                                                                         \
template<class R, class C, class... T> using lambda_make_mfptr_##name##_ellipsis =       \
    R (C::*)(T..., ...) qualifier;                                                       \
                                                                                         \
template<class R, class C, class... T> struct lambda_impl<R (C::*)(T..., ...) qualifier> \
{                                                                                        \
    using type = mp_bind<                                                                \
        lambda_make_mfptr_##name##_ellipsis,                                             \
        mp_lambda<R>, mp_lambda<C>, mp_lambda<T>...>;                                    \
};

#define BOOST_MP11_LAMBDA_EMPTY()

BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(no_qualifier, BOOST_MP11_LAMBDA_EMPTY())
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const, const)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(volatile, volatile)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const_volatile, const volatile)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(ref, &)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const_ref, const&)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(volatile_ref, volatile&)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const_volatile_ref, const volatile&)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(rvalue_ref, &&)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const_rvalue_ref, const&&)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(volatile_rvalue_ref, volatile&&)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const_volatile_rvalue_ref, const volatile&&)

#if (defined(_MSVC_LANG) &&  _MSVC_LANG >= 201703L) || __cplusplus >= 201703L

// P0012R1: exception specification as part of the type system
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(noexcept, noexcept)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const_noexcept, const noexcept)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(volatile_noexcept, volatile noexcept)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const_volatile_noexcept, const volatile noexcept)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(ref_noexcept, & noexcept)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const_ref_noexcept, const& noexcept)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(volatile_ref_noexcept, volatile& noexcept)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const_volatile_ref_noexcept, const volatile& noexcept)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(rvalue_ref_noexcept, && noexcept)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const_rvalue_ref_noexcept, const&& noexcept)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(volatile_rvalue_ref_noexcept, volatile&& noexcept)
BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR(const_volatile_rvalue_ref_noexcept, const volatile&& noexcept)

#endif // P0012R1

#undef BOOST_MP11_LAMBDA_EMPTY
#undef BOOST_MP11_SPECIALIZE_LAMBDA_IMPL_FCT_AND_MFPTR

// [dcl.mptr] (data members)
template<class T, class C> struct lambda_impl<T C::*>
{
    template<class U, class D> using make = U D::*;
    using type = mp_bind<make, mp_lambda<T>, mp_lambda<C>>;
};

// template class instantiation
template<template <class...> class C, class... Ts> struct lambda_impl<C<Ts...>>
{
    using type = mp_bind<C, mp_lambda<Ts>...>;
};

} // namespace detail
} // namespace mp11
} // namespace boost

#if defined(_MSC_VER) || defined(__GNUC__)
# pragma pop_macro( "I" )
#endif

#endif // mp_lambda supported

#endif // #ifndef BOOST_MP11_LAMBDA_HPP_INCLUDED
