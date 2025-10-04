/*
Copyright 2017-2021 Glen Joseph Fernandes
(glenjofe@gmail.com)

Distributed under the Boost Software License, Version 1.0.
(http://www.boost.org/LICENSE_1_0.txt)
*/
#ifndef BOOST_CORE_POINTER_TRAITS_HPP
#define BOOST_CORE_POINTER_TRAITS_HPP

#include <boost/config.hpp>
#include <boost/core/addressof.hpp>
#include <cstddef>

namespace boost {
namespace detail {

struct ptr_none { };

template<class>
struct ptr_valid {
    typedef void type;
};

template<class>
struct ptr_first {
    typedef ptr_none type;
};

#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
template<template<class, class...> class T, class U, class... Args>
struct ptr_first<T<U, Args...> > {
    typedef U type;
};
#else
template<template<class> class T, class U>
struct ptr_first<T<U> > {
    typedef U type;
};

template<template<class, class> class T, class U1, class U2>
struct ptr_first<T<U1, U2> > {
    typedef U1 type;
};

template<template<class, class, class> class T, class U1, class U2, class U3>
struct ptr_first<T<U1, U2, U3> > {
    typedef U1 type;
};
#endif

template<class T, class = void>
struct ptr_element {
    typedef typename ptr_first<T>::type type;
};

template<class T>
struct ptr_element<T, typename ptr_valid<typename T::element_type>::type> {
    typedef typename T::element_type type;
};

template<class, class = void>
struct ptr_difference {
    typedef std::ptrdiff_t type;
};

template<class T>
struct ptr_difference<T,
    typename ptr_valid<typename T::difference_type>::type> {
    typedef typename T::difference_type type;
};

template<class, class>
struct ptr_transform { };

#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
template<template<class, class...> class T, class U, class... Args, class V>
struct ptr_transform<T<U, Args...>, V> {
    typedef T<V, Args...> type;
};
#else
template<template<class> class T, class U, class V>
struct ptr_transform<T<U>, V> {
    typedef T<V> type;
};

template<template<class, class> class T, class U1, class U2, class V>
struct ptr_transform<T<U1, U2>, V> {
    typedef T<V, U2> type;
};

template<template<class, class, class> class T,
    class U1, class U2, class U3, class V>
struct ptr_transform<T<U1, U2, U3>, V> {
    typedef T<V, U2, U3> type;
};
#endif

template<class T, class U, class = void>
struct ptr_rebind
    : ptr_transform<T, U> { };

#if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)
template<class T, class U>
struct ptr_rebind<T, U,
    typename ptr_valid<typename T::template rebind<U> >::type> {
    typedef typename T::template rebind<U> type;
};
#else
template<class T, class U>
struct ptr_rebind<T, U,
    typename ptr_valid<typename T::template rebind<U>::other>::type> {
    typedef typename T::template rebind<U>::other type;
};
#endif

#if !defined(BOOST_NO_CXX11_DECLTYPE_N3276)
template<class T, class E>
class ptr_to_expr {
    template<class>
    struct result {
        char x, y;
    };

    static E& source();

    template<class O>
    static auto check(int) -> result<decltype(O::pointer_to(source()))>;

    template<class>
    static char check(long);

public:
    BOOST_STATIC_CONSTEXPR bool value = sizeof(check<T>(0)) > 1;
};

template<class T, class E>
struct ptr_to_expr<T*, E> {
    BOOST_STATIC_CONSTEXPR bool value = true;
};

template<class T, class E>
struct ptr_has_to {
    BOOST_STATIC_CONSTEXPR bool value = ptr_to_expr<T, E>::value;
};
#else
template<class, class>
struct ptr_has_to {
    BOOST_STATIC_CONSTEXPR bool value = true;
};
#endif

template<class T>
struct ptr_has_to<T, void> {
    BOOST_STATIC_CONSTEXPR bool value = false;
};

template<class T>
struct ptr_has_to<T, const void> {
    BOOST_STATIC_CONSTEXPR bool value = false;
};

template<class T>
struct ptr_has_to<T, volatile void> {
    BOOST_STATIC_CONSTEXPR bool value = false;
};

template<class T>
struct ptr_has_to<T, const volatile void> {
    BOOST_STATIC_CONSTEXPR bool value = false;
};

template<class T, class E, bool = ptr_has_to<T, E>::value>
struct ptr_to { };

template<class T, class E>
struct ptr_to<T, E, true> {
    static T pointer_to(E& v) {
        return T::pointer_to(v);
    }
};

template<class T>
struct ptr_to<T*, T, true> {
    static T* pointer_to(T& v) BOOST_NOEXCEPT {
        return boost::addressof(v);
    }
};

template<class T, class E>
struct ptr_traits
    : ptr_to<T, E> {
    typedef T pointer;
    typedef E element_type;
    typedef typename ptr_difference<T>::type difference_type;

    template<class U>
    struct rebind_to
        : ptr_rebind<T, U> { };

#if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)
    template<class U>
    using rebind = typename rebind_to<U>::type;
#endif
};

template<class T>
struct ptr_traits<T, ptr_none> { };

} /* detail */

template<class T>
struct pointer_traits
    : detail::ptr_traits<T, typename detail::ptr_element<T>::type> { };

template<class T>
struct pointer_traits<T*>
    : detail::ptr_to<T*, T> {
    typedef T* pointer;
    typedef T element_type;
    typedef std::ptrdiff_t difference_type;

    template<class U>
    struct rebind_to {
        typedef U* type;
    };

#if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)
    template<class U>
    using rebind = typename rebind_to<U>::type;
#endif
};

template<class T>
BOOST_CONSTEXPR inline T*
to_address(T* v) BOOST_NOEXCEPT
{
    return v;
}

#if !defined(BOOST_NO_CXX14_RETURN_TYPE_DEDUCTION)
namespace detail {

template<class T>
inline T*
ptr_address(T* v, int) BOOST_NOEXCEPT
{
    return v;
}

template<class T>
inline auto
ptr_address(const T& v, int) BOOST_NOEXCEPT
-> decltype(boost::pointer_traits<T>::to_address(v))
{
    return boost::pointer_traits<T>::to_address(v);
}

template<class T>
inline auto
ptr_address(const T& v, long) BOOST_NOEXCEPT
{
    return boost::detail::ptr_address(v.operator->(), 0);
}

} /* detail */

template<class T>
inline auto
to_address(const T& v) BOOST_NOEXCEPT
{
    return boost::detail::ptr_address(v, 0);
}
#else
template<class T>
inline typename pointer_traits<T>::element_type*
to_address(const T& v) BOOST_NOEXCEPT
{
    return boost::to_address(v.operator->());
}
#endif

} /* boost */

#endif
