/*
Copyright 2021 Glen Joseph Fernandes
(glenjofe@gmail.com)

Distributed under the Boost Software License, Version 1.0.
(http://www.boost.org/LICENSE_1_0.txt)
*/
#ifndef BOOST_CORE_ALLOCATOR_TRAITS_HPP
#define BOOST_CORE_ALLOCATOR_TRAITS_HPP

#include <boost/core/allocator_access.hpp>

namespace boost {

template<class A>
struct allocator_traits {
    typedef A allocator_type;

    typedef typename allocator_value_type<A>::type value_type;

    typedef typename allocator_pointer<A>::type pointer;

    typedef typename allocator_const_pointer<A>::type const_pointer;

    typedef typename allocator_void_pointer<A>::type void_pointer;

    typedef typename allocator_const_void_pointer<A>::type const_void_pointer;

    typedef typename allocator_difference_type<A>::type difference_type;

    typedef typename allocator_size_type<A>::type size_type;

    typedef typename allocator_propagate_on_container_copy_assignment<A>::type
        propagate_on_container_copy_assignment;

    typedef typename allocator_propagate_on_container_move_assignment<A>::type
        propagate_on_container_move_assignment;

    typedef typename allocator_propagate_on_container_swap<A>::type
        propagate_on_container_swap;

    typedef typename allocator_is_always_equal<A>::type is_always_equal;

#if !defined(BOOST_NO_CXX11_TEMPLATE_ALIASES)
    template<class T>
    using rebind_traits = allocator_traits<typename
        allocator_rebind<A, T>::type>;
#else
    template<class T>
    struct rebind_traits
        : allocator_traits<typename allocator_rebind<A, T>::type> { };
#endif

    static pointer allocate(A& a, size_type n) {
        return boost::allocator_allocate(a, n);
    }

    static pointer allocate(A& a, size_type n, const_void_pointer h) {
        return boost::allocator_allocate(a, n, h);
    }

    static void deallocate(A& a, pointer p, size_type n) {
        return boost::allocator_deallocate(a, p, n);
    }

    template<class T>
    static void construct(A& a, T* p) {
        boost::allocator_construct(a, p);
    }

#if !defined(BOOST_NO_CXX11_RVALUE_REFERENCES)
#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
    template<class T, class V, class... Args>
    static void construct(A& a, T* p, V&& v, Args&&... args) {
        boost::allocator_construct(a, p, std::forward<V>(v),
            std::forward<Args>(args)...);
    }
#else
    template<class T, class V>
    static void construct(A& a, T* p, V&& v) {
        boost::allocator_construct(a, p, std::forward<V>(v));
    }
#endif
#else
    template<class T, class V>
    static void construct(A& a, T* p, const V& v) {
        boost::allocator_construct(a, p, v);
    }

    template<class T, class V>
    static void construct(A& a, T* p, V& v) {
        boost::allocator_construct(a, p, v);
    }
#endif

    template<class T>
    static void destroy(A& a, T* p) {
        boost::allocator_destroy(a, p);
    }

    static size_type max_size(const A& a) BOOST_NOEXCEPT {
        return boost::allocator_max_size(a);
    }

    static A select_on_container_copy_construction(const A& a) {
        return boost::allocator_select_on_container_copy_construction(a);
    }
};

} /* boost */

#endif
