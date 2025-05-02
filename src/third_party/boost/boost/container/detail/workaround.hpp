//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/container for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_CONTAINER_DETAIL_WORKAROUND_HPP
#define BOOST_CONTAINER_DETAIL_WORKAROUND_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#if    !defined(BOOST_NO_CXX11_RVALUE_REFERENCES) && !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)\
    && !defined(BOOST_INTERPROCESS_DISABLE_VARIADIC_TMPL)
   #define BOOST_CONTAINER_PERFECT_FORWARDING
#endif

#if !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) && defined(__GXX_EXPERIMENTAL_CXX0X__)\
    && (__GNUC__*10000 + __GNUC_MINOR__*100 + __GNUC_PATCHLEVEL__ < 40700)
   #define BOOST_CONTAINER_UNIMPLEMENTED_PACK_EXPANSION_TO_FIXED_LIST
#endif

#if defined(BOOST_GCC_VERSION)
#  if (BOOST_GCC_VERSION < 40700) || !defined(BOOST_GCC_CXX11)
#     define BOOST_CONTAINER_NO_CXX11_DELEGATING_CONSTRUCTORS
#  endif
#elif defined(BOOST_MSVC)
#  if _MSC_FULL_VER < 180020827
#     define BOOST_CONTAINER_NO_CXX11_DELEGATING_CONSTRUCTORS
#  endif
#elif defined(BOOST_CLANG)
#  if !__has_feature(cxx_delegating_constructors)
#     define BOOST_CONTAINER_NO_CXX11_DELEGATING_CONSTRUCTORS
#  endif
#endif

#if defined(BOOST_MSVC) && (_MSC_VER < 1400)
   #define BOOST_CONTAINER_TEMPLATED_CONVERSION_OPERATOR_BROKEN
#endif

#if !defined(BOOST_NO_CXX11_HDR_TUPLE) || (defined(BOOST_MSVC) && (BOOST_MSVC == 1700 || BOOST_MSVC == 1600))
#define BOOST_CONTAINER_PAIR_TEST_HAS_HEADER_TUPLE
#endif

//Macros for documentation purposes. For code, expands to the argument
#define BOOST_CONTAINER_IMPDEF(TYPE) TYPE
#define BOOST_CONTAINER_SEEDOC(TYPE) TYPE

//Macros for memset optimization. In most platforms
//memsetting pointers and floatings is safe and faster.
//
//If your platform does not offer these guarantees
//define these to value zero.
#ifndef BOOST_CONTAINER_MEMZEROED_FLOATING_POINT_IS_NOT_ZERO
#define BOOST_CONTAINER_MEMZEROED_FLOATING_POINT_IS_ZERO 1
#endif

#ifndef BOOST_CONTAINER_MEMZEROED_POINTER_IS_NOT_NULL
#define BOOST_CONTAINER_MEMZEROED_POINTER_IS_NULL
#endif

#define BOOST_CONTAINER_DOC1ST(TYPE1, TYPE2) TYPE2
#define BOOST_CONTAINER_I ,
#define BOOST_CONTAINER_DOCIGN(T) T
#define BOOST_CONTAINER_DOCONLY(T)

/*
   we need to import/export our code only if the user has specifically
   asked for it by defining either BOOST_ALL_DYN_LINK if they want all boost
   libraries to be dynamically linked, or BOOST_CONTAINER_DYN_LINK
   if they want just this one to be dynamically liked:
*/
#if defined(BOOST_ALL_DYN_LINK) || defined(BOOST_CONTAINER_DYN_LINK)

   /* export if this is our own source, otherwise import: */
   #ifdef BOOST_CONTAINER_SOURCE
   #  define BOOST_CONTAINER_DECL BOOST_SYMBOL_EXPORT
   #else
   #  define BOOST_CONTAINER_DECL BOOST_SYMBOL_IMPORT
   
   #endif  /* BOOST_CONTAINER_SOURCE */
#else
   #define BOOST_CONTAINER_DECL
#endif  /* DYN_LINK */

//#define BOOST_CONTAINER_DISABLE_FORCEINLINE

#if defined(BOOST_CONTAINER_DISABLE_FORCEINLINE)
   #define BOOST_CONTAINER_FORCEINLINE inline
#elif defined(BOOST_CONTAINER_FORCEINLINE_IS_BOOST_FORCELINE)
   #define BOOST_CONTAINER_FORCEINLINE BOOST_FORCEINLINE
#elif defined(BOOST_MSVC) && (_MSC_VER <= 1900 || defined(_DEBUG))
   //"__forceinline" and MSVC seems to have some bugs in old versions and in debug mode
   #define BOOST_CONTAINER_FORCEINLINE inline
#elif defined(BOOST_CLANG) || (defined(BOOST_GCC) && ((__GNUC__ <= 5) || defined(__MINGW32__)))
   //Older GCCs and MinGw have problems with forceinline
   //Clang can have code bloat issues with forceinline, see
   //https://lists.boost.org/boost-users/2023/04/91445.php and
   //https://github.com/llvm/llvm-project/issues/62202
   #define BOOST_CONTAINER_FORCEINLINE inline
#else
   #define BOOST_CONTAINER_FORCEINLINE BOOST_FORCEINLINE
#endif

//#define BOOST_CONTAINER_DISABLE_NOINLINE

#if defined(BOOST_CONTAINER_DISABLE_NOINLINE)
   #define BOOST_CONTAINER_NOINLINE
#else
   #define BOOST_CONTAINER_NOINLINE BOOST_NOINLINE
#endif


#if !defined(__has_feature)
#define BOOST_CONTAINER_HAS_FEATURE(feature) 0
#else
#define BOOST_CONTAINER_HAS_FEATURE(feature) __has_feature(feature)
#endif

//Detect address sanitizer
#if defined(__SANITIZE_ADDRESS__) || BOOST_CONTAINER_HAS_FEATURE(address_sanitizer)
#define BOOST_CONTAINER_ASAN
#endif


#if (BOOST_CXX_VERSION < 201703L) || !defined(__cpp_deduction_guides)
   #define BOOST_CONTAINER_NO_CXX17_CTAD
#endif

#if defined(BOOST_CONTAINER_DISABLE_ATTRIBUTE_NODISCARD)
   #define BOOST_CONTAINER_ATTRIBUTE_NODISCARD
#else
   #if   defined(BOOST_GCC) && ((BOOST_GCC < 100000) || (__cplusplus < 201703L))
      //Avoid using it in C++ < 17 and GCC < 10 because it warns in SFINAE contexts
      //(see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=89070)
      #define BOOST_CONTAINER_ATTRIBUTE_NODISCARD
   #else
      #define BOOST_CONTAINER_ATTRIBUTE_NODISCARD BOOST_ATTRIBUTE_NODISCARD
   #endif
#endif


//Configuration options:

//Define this to use std exception types instead of boost::container's own exception types
//#define BOOST_CONTAINER_USE_STD_EXCEPTIONS


namespace boost {
namespace container {

template <typename T1>
BOOST_FORCEINLINE BOOST_CXX14_CONSTEXPR void ignore(T1 const&)
{}

}} //namespace boost::container {

#if !(defined BOOST_NO_EXCEPTIONS)
#    define BOOST_CONTAINER_TRY { try
#    define BOOST_CONTAINER_CATCH(x) catch(x)
#    define BOOST_CONTAINER_RETHROW throw;
#    define BOOST_CONTAINER_CATCH_END }
#else
#    if !defined(BOOST_MSVC) || BOOST_MSVC >= 1900
#        define BOOST_CONTAINER_TRY { if (true)
#        define BOOST_CONTAINER_CATCH(x) else if (false)
#    else
// warning C4127: conditional expression is constant
#        define BOOST_CONTAINER_TRY { \
             __pragma(warning(push)) \
             __pragma(warning(disable: 4127)) \
             if (true) \
             __pragma(warning(pop))
#        define BOOST_CONTAINER_CATCH(x) else \
             __pragma(warning(push)) \
             __pragma(warning(disable: 4127)) \
             if (false) \
             __pragma(warning(pop))
#    endif
#    define BOOST_CONTAINER_RETHROW
#    define BOOST_CONTAINER_CATCH_END }
#endif

#ifndef BOOST_NO_CXX11_STATIC_ASSERT
#  ifndef BOOST_NO_CXX11_VARIADIC_MACROS
#     define BOOST_CONTAINER_STATIC_ASSERT( ... ) static_assert(__VA_ARGS__, #__VA_ARGS__)
#  else
#     define BOOST_CONTAINER_STATIC_ASSERT( B ) static_assert(B, #B)
#  endif
#else
namespace boost {
   namespace container {
      namespace dtl {

         template<bool B>
         struct STATIC_ASSERTION_FAILURE;

         template<>
         struct STATIC_ASSERTION_FAILURE<true> {};

         template<unsigned> struct static_assert_test {};

      }
   }
}

#define BOOST_CONTAINER_STATIC_ASSERT(B) \
         typedef ::boost::container::dtl::static_assert_test<\
            (unsigned)sizeof(::boost::container::dtl::STATIC_ASSERTION_FAILURE<bool(B)>)>\
               BOOST_JOIN(boost_container_static_assert_typedef_, __LINE__) BOOST_ATTRIBUTE_UNUSED

#endif

#ifndef BOOST_NO_CXX11_STATIC_ASSERT
#  ifndef BOOST_NO_CXX11_VARIADIC_MACROS
#     define BOOST_CONTAINER_STATIC_ASSERT_MSG( ... ) static_assert(__VA_ARGS__)
#  else
#     define BOOST_CONTAINER_STATIC_ASSERT_MSG( B, Msg ) static_assert( B, Msg )
#  endif
#else
#     define BOOST_CONTAINER_STATIC_ASSERT_MSG( B, Msg ) BOOST_CONTAINER_STATIC_ASSERT( B )
#endif

#if !defined(BOOST_NO_CXX17_INLINE_VARIABLES)
#  define BOOST_CONTAINER_CONSTANT_VAR BOOST_INLINE_CONSTEXPR
#else
#  define BOOST_CONTAINER_CONSTANT_VAR static BOOST_CONSTEXPR_OR_CONST
#endif

#if defined(__GNUC__) && ((__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__) >= 40600)
#define BOOST_CONTAINER_GCC_COMPATIBLE_HAS_DIAGNOSTIC_IGNORED
#elif defined(__clang__)
#define BOOST_CONTAINER_GCC_COMPATIBLE_HAS_DIAGNOSTIC_IGNORED
#endif

#endif   //#ifndef BOOST_CONTAINER_DETAIL_WORKAROUND_HPP
