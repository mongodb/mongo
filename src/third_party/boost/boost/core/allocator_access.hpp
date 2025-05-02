/*
Copyright 2020-2022 Glen Joseph Fernandes
(glenjofe@gmail.com)

Distributed under the Boost Software License, Version 1.0.
(http://www.boost.org/LICENSE_1_0.txt)
*/
#ifndef BOOST_CORE_ALLOCATOR_ACCESS_HPP
#define BOOST_CORE_ALLOCATOR_ACCESS_HPP

#include <boost/config.hpp>
#include <boost/core/pointer_traits.hpp>
#include <limits>
#include <new>
#if !defined(BOOST_NO_CXX11_ALLOCATOR)
#include <type_traits>
#endif
#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
#include <utility>
#endif

#if defined(BOOST_GCC_VERSION) && (BOOST_GCC_VERSION >= 40300)
#define BOOST_DETAIL_ALLOC_EMPTY(T) __is_empty(T)
#elif defined(BOOST_INTEL) && defined(_MSC_VER) && (_MSC_VER >= 1500)
#define BOOST_DETAIL_ALLOC_EMPTY(T) __is_empty(T)
#elif defined(BOOST_MSVC) && (BOOST_MSVC >= 1400)
#define BOOST_DETAIL_ALLOC_EMPTY(T) __is_empty(T)
#elif defined(BOOST_CLANG) && !defined(__CUDACC__)
#if __has_feature(is_empty)
#define BOOST_DETAIL_ALLOC_EMPTY(T) __is_empty(T)
#endif
#elif defined(__SUNPRO_CC) && (__SUNPRO_CC >= 0x5130)
#define BOOST_DETAIL_ALLOC_EMPTY(T) __oracle_is_empty(T)
#elif defined(__ghs__) && (__GHS_VERSION_NUMBER >= 600)
#define BOOST_DETAIL_ALLOC_EMPTY(T) __is_empty(T)
#elif defined(BOOST_CODEGEARC)
#define BOOST_DETAIL_ALLOC_EMPTY(T) __is_empty(T)
#endif

#if defined(_LIBCPP_SUPPRESS_DEPRECATED_PUSH)
_LIBCPP_SUPPRESS_DEPRECATED_PUSH
#endif
#if defined(_STL_DISABLE_DEPRECATED_WARNING)
_STL_DISABLE_DEPRECATED_WARNING
#endif
#if defined(__clang__) && defined(__has_warning)
# if __has_warning("-Wdeprecated-declarations")
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wdeprecated-declarations"
# endif
#elif defined(_MSC_VER)
# pragma warning(push)
# pragma warning(disable: 4996)
#elif defined(BOOST_GCC) && BOOST_GCC >= 40600
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace boost {

template<class A>
struct allocator_value_type {
    typedef typename A::value_type type;
};

namespace detail {

template<class A, class = void>
struct alloc_ptr {
    typedef typename boost::allocator_value_type<A>::type* type;
};

template<class>
struct alloc_void {
    typedef void type;
};

template<class A>
struct alloc_ptr<A,
    typename alloc_void<typename A::pointer>::type> {
    typedef typename A::pointer type;
};

} /* detail */

template<class A>
struct allocator_pointer {
    typedef typename detail::alloc_ptr<A>::type type;
};

namespace detail {

template<class A, class = void>
struct alloc_const_ptr {
    typedef typename boost::pointer_traits<typename
        boost::allocator_pointer<A>::type>::template rebind_to<const typename
            boost::allocator_value_type<A>::type>::type type;
};

template<class A>
struct alloc_const_ptr<A,
    typename alloc_void<typename A::const_pointer>::type> {
    typedef typename A::const_pointer type;
};

} /* detail */

template<class A>
struct allocator_const_pointer {
    typedef typename detail::alloc_const_ptr<A>::type type;
};

namespace detail {

template<class, class>
struct alloc_to { };

#if defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
template<template<class> class A, class T, class U>
struct alloc_to<A<U>, T> {
    typedef A<T> type;
};

template<template<class, class> class A, class T, class U, class V>
struct alloc_to<A<U, V>, T> {
    typedef A<T, V> type;
};

template<template<class, class, class> class A, class T, class U, class V1,
    class V2>
struct alloc_to<A<U, V1, V2>, T> {
    typedef A<T, V1, V2> type;
};
#else
template<template<class, class...> class A, class T, class U, class... V>
struct alloc_to<A<U, V...>, T> {
    typedef A<T, V...> type;
};
#endif

template<class A, class T, class = void>
struct alloc_rebind {
    typedef typename alloc_to<A, T>::type type;
};

template<class A, class T>
struct alloc_rebind<A, T,
    typename alloc_void<typename A::template rebind<T>::other>::type> {
    typedef typename A::template rebind<T>::other type;
};

} /* detail */

template<class A, class T>
struct allocator_rebind {
    typedef typename detail::alloc_rebind<A, T>::type type;
};

namespace detail {

template<class A, class = void>
struct alloc_void_ptr {
     typedef typename boost::pointer_traits<typename
        boost::allocator_pointer<A>::type>::template
            rebind_to<void>::type type;
};

template<class A>
struct alloc_void_ptr<A,
    typename alloc_void<typename A::void_pointer>::type> {
    typedef typename A::void_pointer type;
};

} /* detail */

template<class A>
struct allocator_void_pointer {
    typedef typename detail::alloc_void_ptr<A>::type type;
};

namespace detail {

template<class A, class = void>
struct alloc_const_void_ptr {
     typedef typename boost::pointer_traits<typename
        boost::allocator_pointer<A>::type>::template
            rebind_to<const void>::type type;
};

template<class A>
struct alloc_const_void_ptr<A,
    typename alloc_void<typename A::const_void_pointer>::type> {
    typedef typename A::const_void_pointer type;
};

} /* detail */

template<class A>
struct allocator_const_void_pointer {
    typedef typename detail::alloc_const_void_ptr<A>::type type;
};

namespace detail {

template<class A, class = void>
struct alloc_diff_type {
    typedef typename boost::pointer_traits<typename
        boost::allocator_pointer<A>::type>::difference_type type;
};

template<class A>
struct alloc_diff_type<A,
    typename alloc_void<typename A::difference_type>::type> {
    typedef typename A::difference_type type;
};

} /* detail */

template<class A>
struct allocator_difference_type {
    typedef typename detail::alloc_diff_type<A>::type type;
};

namespace detail {

#if defined(BOOST_NO_CXX11_ALLOCATOR)
template<class A, class = void>
struct alloc_size_type {
    typedef std::size_t type;
};
#else
template<class A, class = void>
struct alloc_size_type {
    typedef typename std::make_unsigned<typename
        boost::allocator_difference_type<A>::type>::type type;
};
#endif

template<class A>
struct alloc_size_type<A,
    typename alloc_void<typename A::size_type>::type> {
    typedef typename A::size_type type;
};

} /* detail */

template<class A>
struct allocator_size_type {
    typedef typename detail::alloc_size_type<A>::type type;
};

namespace detail {

#if defined(BOOST_NO_CXX11_ALLOCATOR)
template<bool V>
struct alloc_bool {
    typedef bool value_type;
    typedef alloc_bool type;

    static const bool value = V;

    operator bool() const BOOST_NOEXCEPT {
        return V;
    }

    bool operator()() const BOOST_NOEXCEPT {
        return V;
    }
};

template<bool V>
const bool alloc_bool<V>::value;

typedef alloc_bool<false> alloc_false;
#else
typedef std::false_type alloc_false;
#endif

template<class A, class = void>
struct alloc_pocca {
    typedef alloc_false type;
};

template<class A>
struct alloc_pocca<A,
    typename alloc_void<typename
        A::propagate_on_container_copy_assignment>::type> {
    typedef typename A::propagate_on_container_copy_assignment type;
};

} /* detail */

template<class A, class = void>
struct allocator_propagate_on_container_copy_assignment {
    typedef typename detail::alloc_pocca<A>::type type;
};

namespace detail {

template<class A, class = void>
struct alloc_pocma {
    typedef alloc_false type;
};

template<class A>
struct alloc_pocma<A,
    typename alloc_void<typename
        A::propagate_on_container_move_assignment>::type> {
    typedef typename A::propagate_on_container_move_assignment type;
};

} /* detail */

template<class A>
struct allocator_propagate_on_container_move_assignment {
    typedef typename detail::alloc_pocma<A>::type type;
};

namespace detail {

template<class A, class = void>
struct alloc_pocs {
    typedef alloc_false type;
};

template<class A>
struct alloc_pocs<A,
    typename alloc_void<typename A::propagate_on_container_swap>::type> {
    typedef typename A::propagate_on_container_swap type;
};

} /* detail */

template<class A>
struct allocator_propagate_on_container_swap {
    typedef typename detail::alloc_pocs<A>::type type;
};

namespace detail {

#if !defined(BOOST_NO_CXX11_ALLOCATOR)
template<class A, class = void>
struct alloc_equal {
    typedef typename std::is_empty<A>::type type;
};
#elif defined(BOOST_DETAIL_ALLOC_EMPTY)
template<class A, class = void>
struct alloc_equal {
    typedef alloc_bool<BOOST_DETAIL_ALLOC_EMPTY(A)> type;
};
#else
template<class A, class = void>
struct alloc_equal {
    typedef alloc_false type;
};
#endif

template<class A>
struct alloc_equal<A,
    typename alloc_void<typename A::is_always_equal>::type> {
    typedef typename A::is_always_equal type;
};

} /* detail */

template<class A>
struct allocator_is_always_equal {
    typedef typename detail::alloc_equal<A>::type type;
};

template<class A>
inline typename allocator_pointer<A>::type
allocator_allocate(A& a, typename allocator_size_type<A>::type n)
{
    return a.allocate(n);
}

template<class A>
inline void
allocator_deallocate(A& a, typename allocator_pointer<A>::type p,
    typename allocator_size_type<A>::type n)
{
    a.deallocate(p, n);
}

#if defined(BOOST_NO_CXX11_ALLOCATOR)
template<class A>
inline typename allocator_pointer<A>::type
allocator_allocate(A& a, typename allocator_size_type<A>::type n,
    typename allocator_const_void_pointer<A>::type h)
{
    return a.allocate(n, h);
}
#else
namespace detail {

template<class>
struct alloc_no {
    char x, y;
};

template<class A>
class alloc_has_allocate {
    template<class O>
    static auto check(int)
    -> alloc_no<decltype(std::declval<O&>().allocate(std::declval<typename
        boost::allocator_size_type<A>::type>(), std::declval<typename
            boost::allocator_const_void_pointer<A>::type>()))>;

    template<class>
    static char check(long);

public:
    BOOST_STATIC_CONSTEXPR bool value = sizeof(check<A>(0)) > 1;
};

} /* detail */

template<class A>
inline typename std::enable_if<detail::alloc_has_allocate<A>::value,
    typename allocator_pointer<A>::type>::type
allocator_allocate(A& a, typename allocator_size_type<A>::type n,
    typename allocator_const_void_pointer<A>::type h)
{
    return a.allocate(n, h);
}

template<class A>
inline typename std::enable_if<!detail::alloc_has_allocate<A>::value,
    typename allocator_pointer<A>::type>::type
allocator_allocate(A& a, typename allocator_size_type<A>::type n,
    typename allocator_const_void_pointer<A>::type)
{
    return a.allocate(n);
}
#endif

namespace detail {

#if defined(BOOST_NO_CXX11_ALLOCATOR)
template<class A, class = void>
struct alloc_has_construct {
    BOOST_STATIC_CONSTEXPR bool value = false;
};

template<class A>
struct alloc_has_construct<A,
    typename alloc_void<typename A::_default_construct_destroy>::type> {
    BOOST_STATIC_CONSTEXPR bool value = true;
};
#else
template<class A, class T, class... Args>
class alloc_has_construct {
    template<class O>
    static auto check(int)
    -> alloc_no<decltype(std::declval<O&>().construct(std::declval<T*>(),
        std::declval<Args&&>()...))>;

    template<class>
    static char check(long);

public:
    BOOST_STATIC_CONSTEXPR bool value = sizeof(check<A>(0)) > 1;
};
#endif

template<bool, class = void>
struct alloc_if { };

template<class T>
struct alloc_if<true, T> {
    typedef T type;
};

} /* detail */

#if defined(BOOST_NO_CXX11_ALLOCATOR)
template<class A, class T>
inline typename detail::alloc_if<detail::alloc_has_construct<A>::value>::type
allocator_construct(A& a, T* p)
{
    a.construct(p);
}

template<class A, class T>
inline typename detail::alloc_if<!detail::alloc_has_construct<A>::value>::type
allocator_construct(A&, T* p)
{
    ::new((void*)p) T();
}

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
template<class A, class T, class V, class... Args>
inline void
allocator_construct(A&, T* p, V&& v, Args&&... args)
{
    ::new((void*)p) T(std::forward<V>(v), std::forward<Args>(args)...);
}
#else
template<class A, class T, class V>
inline void
allocator_construct(A&, T* p, V&& v)
{
    ::new((void*)p) T(std::forward<V>(v));
}
#endif
#else
template<class A, class T, class V>
inline void
allocator_construct(A&, T* p, const V& v)
{
    ::new((void*)p) T(v);
}

template<class A, class T, class V>
inline void
allocator_construct(A&, T* p, V& v)
{
    ::new((void*)p) T(v);
}
#endif
#else
template<class A, class T, class... Args>
inline typename std::enable_if<detail::alloc_has_construct<A, T,
    Args...>::value>::type
allocator_construct(A& a, T* p, Args&&... args)
{
    a.construct(p, std::forward<Args>(args)...);
}

template<class A, class T, class... Args>
inline typename std::enable_if<!detail::alloc_has_construct<A, T,
    Args...>::value>::type
allocator_construct(A&, T* p, Args&&... args)
{
    ::new((void*)p) T(std::forward<Args>(args)...);
}
#endif

namespace detail {

#if defined(BOOST_NO_CXX11_ALLOCATOR)
template<class A, class, class = void>
struct alloc_has_destroy {
    BOOST_STATIC_CONSTEXPR bool value = false;
};

template<class A, class T>
struct alloc_has_destroy<A, T,
    typename alloc_void<typename A::_default_construct_destroy>::type> {
    BOOST_STATIC_CONSTEXPR bool value = true;
};
#else
template<class A, class T>
class alloc_has_destroy {
    template<class O>
    static auto check(int)
    -> alloc_no<decltype(std::declval<O&>().destroy(std::declval<T*>()))>;

    template<class>
    static char check(long);

public:
    BOOST_STATIC_CONSTEXPR bool value = sizeof(check<A>(0)) > 1;
};
#endif

} /* detail */

template<class A, class T>
inline typename detail::alloc_if<detail::alloc_has_destroy<A, T>::value>::type
allocator_destroy(A& a, T* p)
{
    a.destroy(p);
}

template<class A, class T>
inline typename detail::alloc_if<!detail::alloc_has_destroy<A, T>::value>::type
allocator_destroy(A&, T* p)
{
    p->~T();
    (void)p;
}

namespace detail {

#if defined(BOOST_NO_CXX11_ALLOCATOR)
template<class T, T>
struct alloc_no {
    char x, y;
};

template<class A>
class alloc_has_max_size {
    template<class O>
    static alloc_no<typename boost::allocator_size_type<O>::type(O::*)(),
        &O::max_size> check(int);

    template<class O>
    static alloc_no<typename boost::allocator_size_type<O>::type(O::*)() const,
        &O::max_size> check(int);

    template<class O>
    static alloc_no<typename boost::allocator_size_type<O>::type(*)(),
        &O::max_size> check(int);

    template<class>
    static char check(long);

public:
    BOOST_STATIC_CONSTEXPR bool value = sizeof(check<A>(0)) > 1;
};
#else
template<class A>
class alloc_has_max_size {
    template<class O>
    static auto check(int)
    -> alloc_no<decltype(std::declval<const O&>().max_size())>;

    template<class>
    static char check(long);

public:
    BOOST_STATIC_CONSTEXPR bool value = sizeof(check<A>(0)) > 1;
};
#endif

} /* detail */

template<class A>
inline typename detail::alloc_if<detail::alloc_has_max_size<A>::value,
    typename allocator_size_type<A>::type>::type
allocator_max_size(const A& a) BOOST_NOEXCEPT
{
    return a.max_size();
}

template<class A>
inline typename detail::alloc_if<!detail::alloc_has_max_size<A>::value,
    typename allocator_size_type<A>::type>::type
allocator_max_size(const A&) BOOST_NOEXCEPT
{
    return (std::numeric_limits<typename
        allocator_size_type<A>::type>::max)() /
            sizeof(typename allocator_value_type<A>::type);
}

namespace detail {

#if defined(BOOST_NO_CXX11_ALLOCATOR)
template<class A>
class alloc_has_soccc {
    template<class O>
    static alloc_no<O(O::*)(), &O::select_on_container_copy_construction>
    check(int);

    template<class O>
    static alloc_no<O(O::*)() const, &O::select_on_container_copy_construction>
    check(int);

    template<class O>
    static alloc_no<O(*)(), &O::select_on_container_copy_construction>
    check(int);

    template<class>
    static char check(long);

public:
    BOOST_STATIC_CONSTEXPR bool value = sizeof(check<A>(0)) > 1;
};
#else
template<class A>
class alloc_has_soccc {
    template<class O>
    static auto check(int) -> alloc_no<decltype(std::declval<const
        O&>().select_on_container_copy_construction())>;

    template<class>
    static char check(long);

public:
    BOOST_STATIC_CONSTEXPR bool value = sizeof(check<A>(0)) > 1;
};
#endif

} /* detail */

template<class A>
inline typename detail::alloc_if<detail::alloc_has_soccc<A>::value, A>::type
allocator_select_on_container_copy_construction(const A& a)
{
    return a.select_on_container_copy_construction();
}

template<class A>
inline typename detail::alloc_if<!detail::alloc_has_soccc<A>::value, A>::type
allocator_select_on_container_copy_construction(const A& a)
{
    return a;
}

template<class A, class T>
inline void
allocator_destroy_n(A& a, T* p, std::size_t n)
{
    while (n > 0) {
        boost::allocator_destroy(a, p + --n);
    }
}

namespace detail {

template<class A, class T>
class alloc_destroyer {
public:
    alloc_destroyer(A& a, T* p) BOOST_NOEXCEPT
        : a_(a), p_(p), n_(0) { }

    ~alloc_destroyer() {
        boost::allocator_destroy_n(a_, p_, n_);
    }

    std::size_t& size() BOOST_NOEXCEPT {
        return n_;
    }

private:
    alloc_destroyer(const alloc_destroyer&);
    alloc_destroyer& operator=(const alloc_destroyer&);

    A& a_;
    T* p_;
    std::size_t n_;
};

} /* detail */

template<class A, class T>
inline void
allocator_construct_n(A& a, T* p, std::size_t n)
{
    detail::alloc_destroyer<A, T> d(a, p);
    for (std::size_t& i = d.size(); i < n; ++i) {
        boost::allocator_construct(a, p + i);
    }
    d.size() = 0;
}

template<class A, class T>
inline void
allocator_construct_n(A& a, T* p, std::size_t n, const T* l, std::size_t m)
{
    detail::alloc_destroyer<A, T> d(a, p);
    for (std::size_t& i = d.size(); i < n; ++i) {
        boost::allocator_construct(a, p + i, l[i % m]);
    }
    d.size() = 0;
}

template<class A, class T, class I>
inline void
allocator_construct_n(A& a, T* p, std::size_t n, I b)
{
    detail::alloc_destroyer<A, T> d(a, p);
    for (std::size_t& i = d.size(); i < n; void(++i), void(++b)) {
        boost::allocator_construct(a, p + i, *b);
    }
    d.size() = 0;
}

#if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)
template<class A>
using allocator_value_type_t = typename allocator_value_type<A>::type;

template<class A>
using allocator_pointer_t = typename allocator_pointer<A>::type;

template<class A>
using allocator_const_pointer_t = typename allocator_const_pointer<A>::type;

template<class A>
using allocator_void_pointer_t = typename allocator_void_pointer<A>::type;

template<class A>
using allocator_const_void_pointer_t =
    typename allocator_const_void_pointer<A>::type;

template<class A>
using allocator_difference_type_t =
    typename allocator_difference_type<A>::type;

template<class A>
using allocator_size_type_t = typename allocator_size_type<A>::type;

template<class A>
using allocator_propagate_on_container_copy_assignment_t =
    typename allocator_propagate_on_container_copy_assignment<A>::type;

template<class A>
using allocator_propagate_on_container_move_assignment_t =
    typename allocator_propagate_on_container_move_assignment<A>::type;

template<class A>
using allocator_propagate_on_container_swap_t =
    typename allocator_propagate_on_container_swap<A>::type;

template<class A>
using allocator_is_always_equal_t =
    typename allocator_is_always_equal<A>::type;

template<class A, class T>
using allocator_rebind_t = typename allocator_rebind<A, T>::type;
#endif

} /* boost */

#if defined(__clang__) && defined(__has_warning)
# if __has_warning("-Wdeprecated-declarations")
#  pragma clang diagnostic pop
# endif
#elif defined(_MSC_VER)
# pragma warning(pop)
#elif defined(BOOST_GCC) && BOOST_GCC >= 40600
# pragma GCC diagnostic pop
#endif  
#if defined(_STL_RESTORE_DEPRECATED_WARNING)
_STL_RESTORE_DEPRECATED_WARNING
#endif
#if defined(_LIBCPP_SUPPRESS_DEPRECATED_POP)
_LIBCPP_SUPPRESS_DEPRECATED_POP
#endif

#endif
