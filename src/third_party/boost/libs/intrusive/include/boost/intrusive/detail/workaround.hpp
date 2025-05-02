//////////////////////////////////////////////////////////////////////////////
//
// (C) Copyright Ion Gaztanaga 2005-2013. Distributed under the Boost
// Software License, Version 1.0. (See accompanying file
// LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/interprocess for documentation.
//
//////////////////////////////////////////////////////////////////////////////

#ifndef BOOST_INTRUSIVE_DETAIL_WORKAROUND_HPP
#define BOOST_INTRUSIVE_DETAIL_WORKAROUND_HPP

#ifndef BOOST_CONFIG_HPP
#  include <boost/config.hpp>
#endif

#if defined(BOOST_HAS_PRAGMA_ONCE)
#  pragma once
#endif

#ifndef BOOST_CONFIG_HPP
#include <boost/config.hpp>
#endif

// MSVC-12 ICEs when variadic templates are enabled.
#if    !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES) && (!defined(BOOST_MSVC) || BOOST_MSVC >= 1900)
   #define BOOST_INTRUSIVE_VARIADIC_TEMPLATES
#endif

#if    !defined(BOOST_NO_CXX11_RVALUE_REFERENCES) && !defined(BOOST_NO_CXX11_VARIADIC_TEMPLATES)
   #define BOOST_INTRUSIVE_PERFECT_FORWARDING
#endif

//Macros for documentation purposes. For code, expands to the argument
#define BOOST_INTRUSIVE_IMPDEF(TYPE) TYPE
#define BOOST_INTRUSIVE_SEEDOC(TYPE) TYPE
#define BOOST_INTRUSIVE_DOC1ST(TYPE1, TYPE2) TYPE2
#define BOOST_INTRUSIVE_I ,
#define BOOST_INTRUSIVE_DOCIGN(T1) T1

//#define BOOST_INTRUSIVE_DISABLE_FORCEINLINE

#if defined(BOOST_INTRUSIVE_DISABLE_FORCEINLINE)
   #define BOOST_INTRUSIVE_FORCEINLINE inline
#elif defined(BOOST_INTRUSIVE_FORCEINLINE_IS_BOOST_FORCELINE)
   #define BOOST_INTRUSIVE_FORCEINLINE BOOST_FORCEINLINE
#elif defined(BOOST_MSVC) && (_MSC_VER < 1900 || defined(_DEBUG))
   //"__forceinline" and MSVC seems to have some bugs in old versions and in debug mode
   #define BOOST_INTRUSIVE_FORCEINLINE inline
#elif defined(BOOST_CLANG) || (defined(BOOST_GCC) && ((__GNUC__ <= 5) || defined(__MINGW32__)))
   //Older GCCs have problems with forceinline
   //Clang can have code bloat issues with forceinline, see
   //https://lists.boost.org/boost-users/2023/04/91445.php and
   //https://github.com/llvm/llvm-project/issues/62202
   #define BOOST_INTRUSIVE_FORCEINLINE inline
#else
   #define BOOST_INTRUSIVE_FORCEINLINE BOOST_FORCEINLINE
#endif

#if !(defined BOOST_NO_EXCEPTIONS)
#    define BOOST_INTRUSIVE_TRY { try
#    define BOOST_INTRUSIVE_CATCH(x) catch(x)
#    define BOOST_INTRUSIVE_RETHROW throw;
#    define BOOST_INTRUSIVE_CATCH_END }
#else
#    if !defined(BOOST_MSVC) || BOOST_MSVC >= 1900
#        define BOOST_INTRUSIVE_TRY { if (true)
#        define BOOST_INTRUSIVE_CATCH(x) else if (false)
#    else
// warning C4127: conditional expression is constant
#        define BOOST_INTRUSIVE_TRY { \
             __pragma(warning(push)) \
             __pragma(warning(disable: 4127)) \
             if (true) \
             __pragma(warning(pop))
#        define BOOST_INTRUSIVE_CATCH(x) else \
             __pragma(warning(push)) \
             __pragma(warning(disable: 4127)) \
             if (false) \
             __pragma(warning(pop))
#    endif
#    define BOOST_INTRUSIVE_RETHROW
#    define BOOST_INTRUSIVE_CATCH_END }
#endif

#ifndef BOOST_NO_CXX11_STATIC_ASSERT
#  ifndef BOOST_NO_CXX11_VARIADIC_MACROS
#     define BOOST_INTRUSIVE_STATIC_ASSERT( ... ) static_assert(__VA_ARGS__, #__VA_ARGS__)
#  else
#     define BOOST_INTRUSIVE_STATIC_ASSERT( B ) static_assert(B, #B)
#  endif
#else
namespace boost {
namespace intrusive {
namespace detail {

template<bool B>
struct STATIC_ASSERTION_FAILURE;

template<>
struct STATIC_ASSERTION_FAILURE<true>{};

template<unsigned> struct static_assert_test {};

}}}

#define BOOST_INTRUSIVE_STATIC_ASSERT(B) \
         typedef ::boost::intrusive::detail::static_assert_test<\
            (unsigned)sizeof(::boost::intrusive::detail::STATIC_ASSERTION_FAILURE<bool(B)>)>\
               BOOST_JOIN(boost_intrusive_static_assert_typedef_, __LINE__) BOOST_ATTRIBUTE_UNUSED

#endif   //BOOST_NO_CXX11_STATIC_ASSERT


//GCC has some false positives with some functions returning references.
//This silences this warning in selected functions
#if defined(BOOST_GCC) && (BOOST_GCC >= 140000)
#  define BOOST_INTRUSIVE_NO_DANGLING __attribute__((no_dangling))
#else
#  define BOOST_INTRUSIVE_NO_DANGLING
#endif


#endif   //#ifndef BOOST_INTRUSIVE_DETAIL_WORKAROUND_HPP
