
//  (C) Copyright Steve Cleary, Beman Dawes, Howard Hinnant & John Maddock 2000.
//  Use, modification and distribution are subject to the Boost Software License,
//  Version 1.0. (See accompanying file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt).
//
//  See http://www.boost.org/libs/type_traits for most recent version including documentation.

#ifndef BOOST_TT_INTRINSICS_HPP_INCLUDED
#define BOOST_TT_INTRINSICS_HPP_INCLUDED

#ifndef BOOST_TT_CONFIG_HPP_INCLUDED
#include <boost/type_traits/config.hpp>
#endif

//
// Helper macros for builtin compiler support.
// If your compiler has builtin support for any of the following
// traits concepts, then redefine the appropriate macros to pick
// up on the compiler support:
//
// (these should largely ignore cv-qualifiers)
// BOOST_IS_UNION(T) should evaluate to true if T is a union type
// BOOST_IS_POD(T) should evaluate to true if T is a POD type
// BOOST_IS_EMPTY(T) should evaluate to true if T is an empty struct or union
// BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) should evaluate to true if "T x;" has no effect
// BOOST_HAS_TRIVIAL_COPY(T) should evaluate to true if T(t) <==> memcpy
// BOOST_HAS_TRIVIAL_ASSIGN(T) should evaluate to true if t = u <==> memcpy
// BOOST_HAS_TRIVIAL_DESTRUCTOR(T) should evaluate to true if ~T() has no effect
// BOOST_HAS_NOTHROW_CONSTRUCTOR(T) should evaluate to true if "T x;" can not throw
// BOOST_HAS_NOTHROW_COPY(T) should evaluate to true if T(t) can not throw
// BOOST_HAS_NOTHROW_ASSIGN(T) should evaluate to true if t = u can not throw
// BOOST_HAS_VIRTUAL_DESTRUCTOR(T) should evaluate to true T has a virtual destructor

#ifdef BOOST_HAS_SGI_TYPE_TRAITS
    // Hook into SGI's __type_traits class, this will pick up user supplied
    // specializations as well as SGI - compiler supplied specializations.
#   include <boost/type_traits/is_same.hpp>
#   ifdef __NetBSD__
      // There are two different versions of type_traits.h on NetBSD on Spark
      // use an implicit include via algorithm instead, to make sure we get
      // the same version as the std lib:
#     include <algorithm>
#   else
#    include <type_traits.h>
#   endif
#   define BOOST_IS_POD(T) ::boost::is_same< typename ::__type_traits<T>::is_POD_type, ::__true_type>::value
#   define BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) ::boost::is_same< typename ::__type_traits<T>::has_trivial_default_constructor, ::__true_type>::value
#   define BOOST_HAS_TRIVIAL_COPY(T) ::boost::is_same< typename ::__type_traits<T>::has_trivial_copy_constructor, ::__true_type>::value
#   define BOOST_HAS_TRIVIAL_ASSIGN(T) ::boost::is_same< typename ::__type_traits<T>::has_trivial_assignment_operator, ::__true_type>::value
#   define BOOST_HAS_TRIVIAL_DESTRUCTOR(T) ::boost::is_same< typename ::__type_traits<T>::has_trivial_destructor, ::__true_type>::value

#   ifdef __sgi
#      define BOOST_HAS_TYPE_TRAITS_INTRINSICS
#   endif
#endif

#if defined(__MSL_CPP__) && (__MSL_CPP__ >= 0x8000)
    // Metrowerks compiler is acquiring intrinsic type traits support
    // post version 8.  We hook into the published interface to pick up
    // user defined specializations as well as compiler intrinsics as 
    // and when they become available:
#   include <msl_utility>
#   define BOOST_IS_UNION(T) BOOST_STD_EXTENSION_NAMESPACE::is_union<T>::value
#   define BOOST_IS_POD(T) BOOST_STD_EXTENSION_NAMESPACE::is_POD<T>::value
#   define BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) BOOST_STD_EXTENSION_NAMESPACE::has_trivial_default_ctor<T>::value
#   define BOOST_HAS_TRIVIAL_COPY(T) BOOST_STD_EXTENSION_NAMESPACE::has_trivial_copy_ctor<T>::value
#   define BOOST_HAS_TRIVIAL_ASSIGN(T) BOOST_STD_EXTENSION_NAMESPACE::has_trivial_assignment<T>::value
#   define BOOST_HAS_TRIVIAL_DESTRUCTOR(T) BOOST_STD_EXTENSION_NAMESPACE::has_trivial_dtor<T>::value
#   define BOOST_HAS_TYPE_TRAITS_INTRINSICS
#endif

#if defined(BOOST_MSVC) && defined(_MSC_FULL_VER) && (_MSC_FULL_VER >=140050215)
#   define BOOST_IS_UNION(T) __is_union(T)
#   define BOOST_IS_POD(T) (__is_pod(T) && __has_trivial_constructor(T))
#   define BOOST_IS_EMPTY(T) __is_empty(T)
#   define BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) __has_trivial_constructor(T)
#   define BOOST_HAS_TRIVIAL_COPY(T) __has_trivial_copy(T)
#   define BOOST_HAS_TRIVIAL_ASSIGN(T) __has_trivial_assign(T)
#   define BOOST_HAS_TRIVIAL_DESTRUCTOR(T) __has_trivial_destructor(T)
#   define BOOST_HAS_NOTHROW_CONSTRUCTOR(T) __has_nothrow_constructor(T)
#   define BOOST_HAS_NOTHROW_COPY(T) __has_nothrow_copy(T)
#   define BOOST_HAS_NOTHROW_ASSIGN(T) __has_nothrow_assign(T)
#   define BOOST_HAS_VIRTUAL_DESTRUCTOR(T) __has_virtual_destructor(T)
#   define BOOST_HAS_TYPE_TRAITS_INTRINSICS
#endif


#ifndef BOOST_IS_UNION
#   define BOOST_IS_UNION(T) false
#endif

#ifndef BOOST_IS_POD
#   define BOOST_IS_POD(T) false
#endif

#ifndef BOOST_IS_EMPTY
#   define BOOST_IS_EMPTY(T) false
#endif

#ifndef BOOST_HAS_TRIVIAL_CONSTRUCTOR
#   define BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) false
#endif

#ifndef BOOST_HAS_TRIVIAL_COPY
#   define BOOST_HAS_TRIVIAL_COPY(T) false
#endif

#ifndef BOOST_HAS_TRIVIAL_ASSIGN
#   define BOOST_HAS_TRIVIAL_ASSIGN(T) false
#endif

#ifndef BOOST_HAS_TRIVIAL_DESTRUCTOR
#   define BOOST_HAS_TRIVIAL_DESTRUCTOR(T) false
#endif

#ifndef BOOST_HAS_NOTHROW_CONSTRUCTOR
#   define BOOST_HAS_NOTHROW_CONSTRUCTOR(T) false
#endif

#ifndef BOOST_HAS_NOTHROW_COPY
#   define BOOST_HAS_NOTHROW_COPY(T) false
#endif

#ifndef BOOST_HAS_NOTHROW_ASSIGN
#   define BOOST_HAS_NOTHROW_ASSIGN(T) false
#endif

#ifndef BOOST_HAS_VIRTUAL_DESTRUCTOR
#   define BOOST_HAS_VIRTUAL_DESTRUCTOR(T) false
#endif

#endif // BOOST_TT_INTRINSICS_HPP_INCLUDED




