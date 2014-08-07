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
// BOOST_IS_EMPTY(T) should evaluate to true if T is an empty class type (and not a union)
// BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) should evaluate to true if "T x;" has no effect
// BOOST_HAS_TRIVIAL_COPY(T) should evaluate to true if T(t) <==> memcpy
// BOOST_HAS_TRIVIAL_MOVE_CONSTRUCTOR(T) should evaluate to true if T(boost::move(t)) <==> memcpy
// BOOST_HAS_TRIVIAL_ASSIGN(T) should evaluate to true if t = u <==> memcpy
// BOOST_HAS_TRIVIAL_MOVE_ASSIGN(T) should evaluate to true if t = boost::move(u) <==> memcpy
// BOOST_HAS_TRIVIAL_DESTRUCTOR(T) should evaluate to true if ~T() has no effect
// BOOST_HAS_NOTHROW_CONSTRUCTOR(T) should evaluate to true if "T x;" can not throw
// BOOST_HAS_NOTHROW_COPY(T) should evaluate to true if T(t) can not throw
// BOOST_HAS_NOTHROW_ASSIGN(T) should evaluate to true if t = u can not throw
// BOOST_HAS_VIRTUAL_DESTRUCTOR(T) should evaluate to true T has a virtual destructor
//
// The following can also be defined: when detected our implementation is greatly simplified.
//
// BOOST_IS_ABSTRACT(T) true if T is an abstract type
// BOOST_IS_BASE_OF(T,U) true if T is a base class of U
// BOOST_IS_CLASS(T) true if T is a class type (and not a union)
// BOOST_IS_CONVERTIBLE(T,U) true if T is convertible to U
// BOOST_IS_ENUM(T) true is T is an enum
// BOOST_IS_POLYMORPHIC(T) true if T is a polymorphic type
// BOOST_ALIGNMENT_OF(T) should evaluate to the alignment requirements of type T.

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

#if (defined(BOOST_MSVC) && defined(BOOST_MSVC_FULL_VER) && (BOOST_MSVC_FULL_VER >=140050215))\
         || (defined(BOOST_INTEL) && defined(_MSC_VER) && (_MSC_VER >= 1500))
#   include <boost/type_traits/is_same.hpp>
#   include <boost/type_traits/is_function.hpp>

#   define BOOST_IS_UNION(T) __is_union(T)
#   define BOOST_IS_POD(T) (__is_pod(T) && __has_trivial_constructor(T))
#   define BOOST_IS_EMPTY(T) __is_empty(T)
#   define BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) __has_trivial_constructor(T)
#   define BOOST_HAS_TRIVIAL_COPY(T) (__has_trivial_copy(T)|| ( ::boost::is_pod<T>::value && !::boost::is_volatile<T>::value))
#   define BOOST_HAS_TRIVIAL_ASSIGN(T) (__has_trivial_assign(T) || ( ::boost::is_pod<T>::value && ! ::boost::is_const<T>::value && !::boost::is_volatile<T>::value))
#   define BOOST_HAS_TRIVIAL_DESTRUCTOR(T) (__has_trivial_destructor(T) || ::boost::is_pod<T>::value)
#   define BOOST_HAS_NOTHROW_CONSTRUCTOR(T) (__has_nothrow_constructor(T) || ::boost::has_trivial_constructor<T>::value)
#   define BOOST_HAS_NOTHROW_COPY(T) (__has_nothrow_copy(T) || ::boost::has_trivial_copy<T>::value)
#   define BOOST_HAS_NOTHROW_ASSIGN(T) (__has_nothrow_assign(T) || ::boost::has_trivial_assign<T>::value)
#   define BOOST_HAS_VIRTUAL_DESTRUCTOR(T) __has_virtual_destructor(T)

#   define BOOST_IS_ABSTRACT(T) __is_abstract(T)
#   define BOOST_IS_BASE_OF(T,U) (__is_base_of(T,U) && !is_same<T,U>::value)
#   define BOOST_IS_CLASS(T) __is_class(T)
#   define BOOST_IS_CONVERTIBLE(T,U) ((__is_convertible_to(T,U) || (is_same<T,U>::value && !is_function<U>::value)) && !__is_abstract(U))
#   define BOOST_IS_ENUM(T) __is_enum(T)
//  This one doesn't quite always do the right thing:
//  #   define BOOST_IS_POLYMORPHIC(T) __is_polymorphic(T)
//  This one fails if the default alignment has been changed with /Zp:
//  #   define BOOST_ALIGNMENT_OF(T) __alignof(T)

#   if defined(_MSC_VER) && (_MSC_VER >= 1700)
#       define BOOST_HAS_TRIVIAL_MOVE_CONSTRUCTOR(T) ((__has_trivial_move_constructor(T) || ::boost::is_pod<T>::value) && !::boost::is_volatile<T>::value)
#       define BOOST_HAS_TRIVIAL_MOVE_ASSIGN(T) ((__has_trivial_move_assign(T) || ::boost::is_pod<T>::value) && ! ::boost::is_const<T>::value && !::boost::is_volatile<T>::value)
#   endif

#   define BOOST_HAS_TYPE_TRAITS_INTRINSICS
#endif

#if defined(__DMC__) && (__DMC__ >= 0x848)
// For Digital Mars C++, www.digitalmars.com
#   define BOOST_IS_UNION(T) (__typeinfo(T) & 0x400)
#   define BOOST_IS_POD(T) (__typeinfo(T) & 0x800)
#   define BOOST_IS_EMPTY(T) (__typeinfo(T) & 0x1000)
#   define BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) (__typeinfo(T) & 0x10)
#   define BOOST_HAS_TRIVIAL_COPY(T) (__typeinfo(T) & 0x20)
#   define BOOST_HAS_TRIVIAL_ASSIGN(T) (__typeinfo(T) & 0x40)
#   define BOOST_HAS_TRIVIAL_DESTRUCTOR(T) (__typeinfo(T) & 0x8)
#   define BOOST_HAS_NOTHROW_CONSTRUCTOR(T) (__typeinfo(T) & 0x80)
#   define BOOST_HAS_NOTHROW_COPY(T) (__typeinfo(T) & 0x100)
#   define BOOST_HAS_NOTHROW_ASSIGN(T) (__typeinfo(T) & 0x200)
#   define BOOST_HAS_VIRTUAL_DESTRUCTOR(T) (__typeinfo(T) & 0x4)
#   define BOOST_HAS_TYPE_TRAITS_INTRINSICS
#endif

#if defined(BOOST_CLANG) && defined(__has_feature)
#   include <cstddef>
#   include <boost/type_traits/is_same.hpp>
#   include <boost/type_traits/is_reference.hpp>
#   include <boost/type_traits/is_volatile.hpp>

#   if __has_feature(is_union)
#     define BOOST_IS_UNION(T) __is_union(T)
#   endif
#   if (!defined(__GLIBCXX__) || (__GLIBCXX__ >= 20080306 && __GLIBCXX__ != 20080519)) && __has_feature(is_pod)
#     define BOOST_IS_POD(T) __is_pod(T)
#   endif
#   if (!defined(__GLIBCXX__) || (__GLIBCXX__ >= 20080306 && __GLIBCXX__ != 20080519)) && __has_feature(is_empty)
#     define BOOST_IS_EMPTY(T) __is_empty(T)
#   endif
#   if __has_feature(has_trivial_constructor)
#     define BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) __has_trivial_constructor(T)
#   endif
#   if __has_feature(has_trivial_copy)
#     define BOOST_HAS_TRIVIAL_COPY(T) (__has_trivial_copy(T) && !is_reference<T>::value && !is_volatile<T>::value)
#   endif
#   if __has_feature(has_trivial_assign)
#     define BOOST_HAS_TRIVIAL_ASSIGN(T) (__has_trivial_assign(T) && !is_volatile<T>::value)
#   endif
#   if __has_feature(has_trivial_destructor)
#     define BOOST_HAS_TRIVIAL_DESTRUCTOR(T) __has_trivial_destructor(T)
#   endif
#   if __has_feature(has_nothrow_constructor)
#     define BOOST_HAS_NOTHROW_CONSTRUCTOR(T) __has_nothrow_constructor(T)
#   endif
#   if __has_feature(has_nothrow_copy)
#     define BOOST_HAS_NOTHROW_COPY(T) (__has_nothrow_copy(T) && !is_volatile<T>::value && !is_reference<T>::value)
#   endif
#   if __has_feature(has_nothrow_assign)
#     define BOOST_HAS_NOTHROW_ASSIGN(T) (__has_nothrow_assign(T) && !is_volatile<T>::value)
#   endif
#   if __has_feature(has_virtual_destructor)
#     define BOOST_HAS_VIRTUAL_DESTRUCTOR(T) __has_virtual_destructor(T)
#   endif
#   if __has_feature(is_abstract)
#     define BOOST_IS_ABSTRACT(T) __is_abstract(T)
#   endif
#   if __has_feature(is_base_of)
#     define BOOST_IS_BASE_OF(T,U) (__is_base_of(T,U) && !is_same<T,U>::value)
#   endif
#   if __has_feature(is_class)
#     define BOOST_IS_CLASS(T) __is_class(T)
#   endif
#   if __has_feature(is_convertible_to)
#     include <boost/type_traits/is_abstract.hpp>
#     define BOOST_IS_CONVERTIBLE(T,U) (__is_convertible_to(T,U) && !::boost::is_abstract<U>::value)
#   endif
#   if __has_feature(is_enum)
#     define BOOST_IS_ENUM(T) __is_enum(T)
#   endif
#   if __has_feature(is_polymorphic)
#     define BOOST_IS_POLYMORPHIC(T) __is_polymorphic(T)
#   endif
#   if __has_feature(has_trivial_move_constructor)
#     define BOOST_HAS_TRIVIAL_MOVE_CONSTRUCTOR(T) __has_trivial_move_constructor(T)
#   endif
#   if __has_feature(has_trivial_move_assign)
#     define BOOST_HAS_TRIVIAL_MOVE_ASSIGN(T) __has_trivial_move_assign(T)
#   endif
#   define BOOST_ALIGNMENT_OF(T) __alignof(T)

#   define BOOST_HAS_TYPE_TRAITS_INTRINSICS
#endif

#if defined(__GNUC__) && ((__GNUC__ > 4) || ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 3) && !defined(__GCCXML__))) && !defined(BOOST_CLANG)
#   include <boost/type_traits/is_same.hpp>
#   include <boost/type_traits/is_reference.hpp>
#   include <boost/type_traits/is_volatile.hpp>

#ifdef BOOST_INTEL
#  define BOOST_INTEL_TT_OPTS || is_pod<T>::value
#else
#  define BOOST_INTEL_TT_OPTS
#endif

#   define BOOST_IS_UNION(T) __is_union(T)
#   define BOOST_IS_POD(T) __is_pod(T)
#   define BOOST_IS_EMPTY(T) __is_empty(T)
#   define BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) ((__has_trivial_constructor(T) BOOST_INTEL_TT_OPTS) && ! ::boost::is_volatile<T>::value)
#   define BOOST_HAS_TRIVIAL_COPY(T) ((__has_trivial_copy(T) BOOST_INTEL_TT_OPTS) && !is_reference<T>::value && ! ::boost::is_volatile<T>::value)
#   define BOOST_HAS_TRIVIAL_ASSIGN(T) ((__has_trivial_assign(T) BOOST_INTEL_TT_OPTS) && ! ::boost::is_volatile<T>::value && ! ::boost::is_const<T>::value)
#   define BOOST_HAS_TRIVIAL_DESTRUCTOR(T) (__has_trivial_destructor(T) BOOST_INTEL_TT_OPTS)
#   define BOOST_HAS_NOTHROW_CONSTRUCTOR(T) (__has_nothrow_constructor(T) BOOST_INTEL_TT_OPTS)
#   define BOOST_HAS_NOTHROW_COPY(T) ((__has_nothrow_copy(T) BOOST_INTEL_TT_OPTS) && !is_volatile<T>::value && !is_reference<T>::value)
#   define BOOST_HAS_NOTHROW_ASSIGN(T) ((__has_nothrow_assign(T) BOOST_INTEL_TT_OPTS) && !is_volatile<T>::value && !is_const<T>::value)
#   define BOOST_HAS_VIRTUAL_DESTRUCTOR(T) __has_virtual_destructor(T)

#   define BOOST_IS_ABSTRACT(T) __is_abstract(T)
#   define BOOST_IS_BASE_OF(T,U) (__is_base_of(T,U) && !is_same<T,U>::value)
#   define BOOST_IS_CLASS(T) __is_class(T)
#   define BOOST_IS_ENUM(T) __is_enum(T)
#   define BOOST_IS_POLYMORPHIC(T) __is_polymorphic(T)
#   if (!defined(unix) && !defined(__unix__)) || defined(__LP64__)
      // GCC sometimes lies about alignment requirements
      // of type double on 32-bit unix platforms, use the
      // old implementation instead in that case:
#     define BOOST_ALIGNMENT_OF(T) __alignof__(T)
#   endif

#   define BOOST_HAS_TYPE_TRAITS_INTRINSICS
#endif

#if defined(__ghs__) && (__GHS_VERSION_NUMBER >= 600)
#   include <boost/type_traits/is_same.hpp>
#   include <boost/type_traits/is_reference.hpp>
#   include <boost/type_traits/is_volatile.hpp>

#   define BOOST_IS_UNION(T) __is_union(T)
#   define BOOST_IS_POD(T) __is_pod(T)
#   define BOOST_IS_EMPTY(T) __is_empty(T)
#   define BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) __has_trivial_constructor(T)
#   define BOOST_HAS_TRIVIAL_COPY(T) (__has_trivial_copy(T) && !is_reference<T>::value && !is_volatile<T>::value)
#   define BOOST_HAS_TRIVIAL_ASSIGN(T) (__has_trivial_assign(T) && !is_volatile<T>::value)
#   define BOOST_HAS_TRIVIAL_DESTRUCTOR(T) __has_trivial_destructor(T)
#   define BOOST_HAS_NOTHROW_CONSTRUCTOR(T) __has_nothrow_constructor(T)
#   define BOOST_HAS_NOTHROW_COPY(T) (__has_nothrow_copy(T) && !is_volatile<T>::value && !is_reference<T>::value)
#   define BOOST_HAS_NOTHROW_ASSIGN(T) (__has_nothrow_assign(T) && !is_volatile<T>::value)
#   define BOOST_HAS_VIRTUAL_DESTRUCTOR(T) __has_virtual_destructor(T)

#   define BOOST_IS_ABSTRACT(T) __is_abstract(T)
#   define BOOST_IS_BASE_OF(T,U) (__is_base_of(T,U) && !is_same<T,U>::value)
#   define BOOST_IS_CLASS(T) __is_class(T)
#   define BOOST_IS_ENUM(T) __is_enum(T)
#   define BOOST_IS_POLYMORPHIC(T) __is_polymorphic(T)
#   define BOOST_ALIGNMENT_OF(T) __alignof__(T)

#   define BOOST_HAS_TYPE_TRAITS_INTRINSICS
#endif

# if defined(__CODEGEARC__)
#   include <boost/type_traits/is_same.hpp>
#   include <boost/type_traits/is_reference.hpp>
#   include <boost/type_traits/is_volatile.hpp>
#   include <boost/type_traits/is_void.hpp>

#   define BOOST_IS_UNION(T) __is_union(T)
#   define BOOST_IS_POD(T) __is_pod(T)
#   define BOOST_IS_EMPTY(T) __is_empty(T)
#   define BOOST_HAS_TRIVIAL_CONSTRUCTOR(T) (__has_trivial_default_constructor(T))
#   define BOOST_HAS_TRIVIAL_COPY(T) (__has_trivial_copy_constructor(T) && !is_volatile<T>::value && !is_reference<T>::value)
#   define BOOST_HAS_TRIVIAL_ASSIGN(T) (__has_trivial_assign(T) && !is_volatile<T>::value)
#   define BOOST_HAS_TRIVIAL_DESTRUCTOR(T) (__has_trivial_destructor(T))
#   define BOOST_HAS_NOTHROW_CONSTRUCTOR(T) (__has_nothrow_default_constructor(T))
#   define BOOST_HAS_NOTHROW_COPY(T) (__has_nothrow_copy_constructor(T) && !is_volatile<T>::value && !is_reference<T>::value)
#   define BOOST_HAS_NOTHROW_ASSIGN(T) (__has_nothrow_assign(T) && !is_volatile<T>::value)
#   define BOOST_HAS_VIRTUAL_DESTRUCTOR(T) __has_virtual_destructor(T)

#   define BOOST_IS_ABSTRACT(T) __is_abstract(T)
#   define BOOST_IS_BASE_OF(T,U) (__is_base_of(T,U) && !is_void<T>::value && !is_void<U>::value)
#   define BOOST_IS_CLASS(T) __is_class(T)
#   define BOOST_IS_CONVERTIBLE(T,U) (__is_convertible(T,U) || is_void<U>::value)
#   define BOOST_IS_ENUM(T) __is_enum(T)
#   define BOOST_IS_POLYMORPHIC(T) __is_polymorphic(T)
#   define BOOST_ALIGNMENT_OF(T) alignof(T)

#   define BOOST_HAS_TYPE_TRAITS_INTRINSICS
#endif

#endif // BOOST_TT_INTRINSICS_HPP_INCLUDED







