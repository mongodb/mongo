//  Boost config.hpp configuration header file  ------------------------------//

//  Copyright (c) 2001-2003 John Maddock
//  Copyright (c) 2001 Darin Adler
//  Copyright (c) 2001 Peter Dimov
//  Copyright (c) 2002 Bill Kempf 
//  Copyright (c) 2002 Jens Maurer
//  Copyright (c) 2002-2003 David Abrahams
//  Copyright (c) 2003 Gennaro Prota
//  Copyright (c) 2003 Eric Friedman
//  Copyright (c) 2010 Eric Jourdanneau, Joel Falcou
// Distributed under the Boost Software License, Version 1.0. (See
// accompanying file LICENSE_1_0.txt or copy at
// http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org/ for most recent version.

//  Boost config.hpp policy and rationale documentation has been moved to
//  http://www.boost.org/libs/config/
//
//  This file is intended to be stable, and relatively unchanging.
//  It should contain boilerplate code only - no compiler specific
//  code unless it is unavoidable - no changes unless unavoidable.

#ifndef BOOST_CONFIG_SUFFIX_HPP
#define BOOST_CONFIG_SUFFIX_HPP

#if defined(__GNUC__) && (__GNUC__ >= 4)
//
// Some GCC-4.x versions issue warnings even when __extension__ is used,
// so use this as a workaround:
//
#pragma GCC system_header
#endif

//
// ensure that visibility macros are always defined, thus symplifying use
//
#ifndef BOOST_SYMBOL_EXPORT
# define BOOST_SYMBOL_EXPORT
#endif
#ifndef BOOST_SYMBOL_IMPORT
# define BOOST_SYMBOL_IMPORT
#endif
#ifndef BOOST_SYMBOL_VISIBLE
# define BOOST_SYMBOL_VISIBLE
#endif

//
// look for long long by looking for the appropriate macros in <limits.h>.
// Note that we use limits.h rather than climits for maximal portability,
// remember that since these just declare a bunch of macros, there should be
// no namespace issues from this.
//
#if !defined(BOOST_HAS_LONG_LONG) && !defined(BOOST_NO_LONG_LONG)                                              \
   && !defined(BOOST_MSVC) && !defined(__BORLANDC__)
# include <limits.h>
# if (defined(ULLONG_MAX) || defined(ULONG_LONG_MAX) || defined(ULONGLONG_MAX))
#   define BOOST_HAS_LONG_LONG
# else
#   define BOOST_NO_LONG_LONG
# endif
#endif

// GCC 3.x will clean up all of those nasty macro definitions that
// BOOST_NO_CTYPE_FUNCTIONS is intended to help work around, so undefine
// it under GCC 3.x.
#if defined(__GNUC__) && (__GNUC__ >= 3) && defined(BOOST_NO_CTYPE_FUNCTIONS)
#  undef BOOST_NO_CTYPE_FUNCTIONS
#endif

//
// Assume any extensions are in namespace std:: unless stated otherwise:
//
#  ifndef BOOST_STD_EXTENSION_NAMESPACE
#    define BOOST_STD_EXTENSION_NAMESPACE std
#  endif

//
// If cv-qualified specializations are not allowed, then neither are cv-void ones:
//
#  if defined(BOOST_NO_CV_SPECIALIZATIONS) \
      && !defined(BOOST_NO_CV_VOID_SPECIALIZATIONS)
#     define BOOST_NO_CV_VOID_SPECIALIZATIONS
#  endif

//
// If there is no numeric_limits template, then it can't have any compile time
// constants either!
//
#  if defined(BOOST_NO_LIMITS) \
      && !defined(BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS)
#     define BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS
#     define BOOST_NO_MS_INT64_NUMERIC_LIMITS
#     define BOOST_NO_LONG_LONG_NUMERIC_LIMITS
#  endif

//
// if there is no long long then there is no specialisation
// for numeric_limits<long long> either:
//
#if !defined(BOOST_HAS_LONG_LONG) && !defined(BOOST_NO_LONG_LONG_NUMERIC_LIMITS)
#  define BOOST_NO_LONG_LONG_NUMERIC_LIMITS
#endif

//
// Normalize BOOST_NO_STATIC_ASSERT and (depricated) BOOST_HAS_STATIC_ASSERT:
//
#if !defined(BOOST_NO_STATIC_ASSERT) && !defined(BOOST_HAS_STATIC_ASSERT)
#  define BOOST_HAS_STATIC_ASSERT
#endif

//
// if there is no __int64 then there is no specialisation
// for numeric_limits<__int64> either:
//
#if !defined(BOOST_HAS_MS_INT64) && !defined(BOOST_NO_MS_INT64_NUMERIC_LIMITS)
#  define BOOST_NO_MS_INT64_NUMERIC_LIMITS
#endif

//
// if member templates are supported then so is the
// VC6 subset of member templates:
//
#  if !defined(BOOST_NO_MEMBER_TEMPLATES) \
       && !defined(BOOST_MSVC6_MEMBER_TEMPLATES)
#     define BOOST_MSVC6_MEMBER_TEMPLATES
#  endif

//
// Without partial specialization, can't test for partial specialisation bugs:
//
#  if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION) \
      && !defined(BOOST_BCB_PARTIAL_SPECIALIZATION_BUG)
#     define BOOST_BCB_PARTIAL_SPECIALIZATION_BUG
#  endif

//
// Without partial specialization, we can't have array-type partial specialisations:
//
#  if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION) \
      && !defined(BOOST_NO_ARRAY_TYPE_SPECIALIZATIONS)
#     define BOOST_NO_ARRAY_TYPE_SPECIALIZATIONS
#  endif

//
// Without partial specialization, std::iterator_traits can't work:
//
#  if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION) \
      && !defined(BOOST_NO_STD_ITERATOR_TRAITS)
#     define BOOST_NO_STD_ITERATOR_TRAITS
#  endif

//
// Without partial specialization, partial 
// specialization with default args won't work either:
//
#  if defined(BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION) \
      && !defined(BOOST_NO_PARTIAL_SPECIALIZATION_IMPLICIT_DEFAULT_ARGS)
#     define BOOST_NO_PARTIAL_SPECIALIZATION_IMPLICIT_DEFAULT_ARGS
#  endif

//
// Without member template support, we can't have template constructors
// in the standard library either:
//
#  if defined(BOOST_NO_MEMBER_TEMPLATES) \
      && !defined(BOOST_MSVC6_MEMBER_TEMPLATES) \
      && !defined(BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS)
#     define BOOST_NO_TEMPLATED_ITERATOR_CONSTRUCTORS
#  endif

//
// Without member template support, we can't have a conforming
// std::allocator template either:
//
#  if defined(BOOST_NO_MEMBER_TEMPLATES) \
      && !defined(BOOST_MSVC6_MEMBER_TEMPLATES) \
      && !defined(BOOST_NO_STD_ALLOCATOR)
#     define BOOST_NO_STD_ALLOCATOR
#  endif

//
// without ADL support then using declarations will break ADL as well:
//
#if defined(BOOST_NO_ARGUMENT_DEPENDENT_LOOKUP) && !defined(BOOST_FUNCTION_SCOPE_USING_DECLARATION_BREAKS_ADL)
#  define BOOST_FUNCTION_SCOPE_USING_DECLARATION_BREAKS_ADL
#endif

//
// Without typeid support we have no dynamic RTTI either:
//
#if defined(BOOST_NO_TYPEID) && !defined(BOOST_NO_RTTI)
#  define BOOST_NO_RTTI
#endif

//
// If we have a standard allocator, then we have a partial one as well:
//
#if !defined(BOOST_NO_STD_ALLOCATOR)
#  define BOOST_HAS_PARTIAL_STD_ALLOCATOR
#endif

//
// We can't have a working std::use_facet if there is no std::locale:
//
#  if defined(BOOST_NO_STD_LOCALE) && !defined(BOOST_NO_STD_USE_FACET)
#     define BOOST_NO_STD_USE_FACET
#  endif

//
// We can't have a std::messages facet if there is no std::locale:
//
#  if defined(BOOST_NO_STD_LOCALE) && !defined(BOOST_NO_STD_MESSAGES)
#     define BOOST_NO_STD_MESSAGES
#  endif

//
// We can't have a working std::wstreambuf if there is no std::locale:
//
#  if defined(BOOST_NO_STD_LOCALE) && !defined(BOOST_NO_STD_WSTREAMBUF)
#     define BOOST_NO_STD_WSTREAMBUF
#  endif

//
// We can't have a <cwctype> if there is no <cwchar>:
//
#  if defined(BOOST_NO_CWCHAR) && !defined(BOOST_NO_CWCTYPE)
#     define BOOST_NO_CWCTYPE
#  endif

//
// We can't have a swprintf if there is no <cwchar>:
//
#  if defined(BOOST_NO_CWCHAR) && !defined(BOOST_NO_SWPRINTF)
#     define BOOST_NO_SWPRINTF
#  endif

//
// If Win32 support is turned off, then we must turn off
// threading support also, unless there is some other
// thread API enabled:
//
#if defined(BOOST_DISABLE_WIN32) && defined(_WIN32) \
   && !defined(BOOST_DISABLE_THREADS) && !defined(BOOST_HAS_PTHREADS)
#  define BOOST_DISABLE_THREADS
#endif

//
// Turn on threading support if the compiler thinks that it's in
// multithreaded mode.  We put this here because there are only a
// limited number of macros that identify this (if there's any missing
// from here then add to the appropriate compiler section):
//
#if (defined(__MT__) || defined(_MT) || defined(_REENTRANT) \
    || defined(_PTHREADS) || defined(__APPLE__) || defined(__DragonFly__)) \
    && !defined(BOOST_HAS_THREADS)
#  define BOOST_HAS_THREADS
#endif

//
// Turn threading support off if BOOST_DISABLE_THREADS is defined:
//
#if defined(BOOST_DISABLE_THREADS) && defined(BOOST_HAS_THREADS)
#  undef BOOST_HAS_THREADS
#endif

//
// Turn threading support off if we don't recognise the threading API:
//
#if defined(BOOST_HAS_THREADS) && !defined(BOOST_HAS_PTHREADS)\
      && !defined(BOOST_HAS_WINTHREADS) && !defined(BOOST_HAS_BETHREADS)\
      && !defined(BOOST_HAS_MPTASKS)
#  undef BOOST_HAS_THREADS
#endif

//
// Turn threading detail macros off if we don't (want to) use threading
//
#ifndef BOOST_HAS_THREADS
#  undef BOOST_HAS_PTHREADS
#  undef BOOST_HAS_PTHREAD_MUTEXATTR_SETTYPE
#  undef BOOST_HAS_PTHREAD_YIELD
#  undef BOOST_HAS_PTHREAD_DELAY_NP
#  undef BOOST_HAS_WINTHREADS
#  undef BOOST_HAS_BETHREADS
#  undef BOOST_HAS_MPTASKS
#endif

//
// If the compiler claims to be C99 conformant, then it had better
// have a <stdint.h>:
//
#  if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901)
#     define BOOST_HAS_STDINT_H
#     ifndef BOOST_HAS_LOG1P
#        define BOOST_HAS_LOG1P
#     endif
#     ifndef BOOST_HAS_EXPM1
#        define BOOST_HAS_EXPM1
#     endif
#  endif

//
// Define BOOST_NO_SLIST and BOOST_NO_HASH if required.
// Note that this is for backwards compatibility only.
//
#  if !defined(BOOST_HAS_SLIST) && !defined(BOOST_NO_SLIST)
#     define BOOST_NO_SLIST
#  endif

#  if !defined(BOOST_HAS_HASH) && !defined(BOOST_NO_HASH)
#     define BOOST_NO_HASH
#  endif

//
// Set BOOST_SLIST_HEADER if not set already:
//
#if defined(BOOST_HAS_SLIST) && !defined(BOOST_SLIST_HEADER)
#  define BOOST_SLIST_HEADER <slist>
#endif

//
// Set BOOST_HASH_SET_HEADER if not set already:
//
#if defined(BOOST_HAS_HASH) && !defined(BOOST_HASH_SET_HEADER)
#  define BOOST_HASH_SET_HEADER <hash_set>
#endif

//
// Set BOOST_HASH_MAP_HEADER if not set already:
//
#if defined(BOOST_HAS_HASH) && !defined(BOOST_HASH_MAP_HEADER)
#  define BOOST_HASH_MAP_HEADER <hash_map>
#endif

//
// Set BOOST_NO_INITIALIZER_LISTS if there is no library support.
//

#if defined(BOOST_NO_0X_HDR_INITIALIZER_LIST) && !defined(BOOST_NO_INITIALIZER_LISTS)
#  define BOOST_NO_INITIALIZER_LISTS
#endif
#if defined(BOOST_NO_INITIALIZER_LISTS) && !defined(BOOST_NO_0X_HDR_INITIALIZER_LIST)
#  define BOOST_NO_0X_HDR_INITIALIZER_LIST
#endif

//
// Set BOOST_HAS_RVALUE_REFS when BOOST_NO_RVALUE_REFERENCES is not defined
//
#if !defined(BOOST_NO_RVALUE_REFERENCES) && !defined(BOOST_HAS_RVALUE_REFS)
#define BOOST_HAS_RVALUE_REFS
#endif

//
// Set BOOST_HAS_VARIADIC_TMPL when BOOST_NO_VARIADIC_TEMPLATES is not defined
//
#if !defined(BOOST_NO_VARIADIC_TEMPLATES) && !defined(BOOST_HAS_VARIADIC_TMPL)
#define BOOST_HAS_VARIADIC_TMPL
#endif

//
// Set BOOST_NO_DECLTYPE_N3276 when BOOST_NO_DECLTYPE is defined
//
#if !defined(BOOST_NO_DECLTYPE_N3276) && defined(BOOST_NO_DECLTYPE)
#define BOOST_NO_DECLTYPE_N3276
#endif

//  BOOST_HAS_ABI_HEADERS
//  This macro gets set if we have headers that fix the ABI,
//  and prevent ODR violations when linking to external libraries:
#if defined(BOOST_ABI_PREFIX) && defined(BOOST_ABI_SUFFIX) && !defined(BOOST_HAS_ABI_HEADERS)
#  define BOOST_HAS_ABI_HEADERS
#endif

#if defined(BOOST_HAS_ABI_HEADERS) && defined(BOOST_DISABLE_ABI_HEADERS)
#  undef BOOST_HAS_ABI_HEADERS
#endif

//  BOOST_NO_STDC_NAMESPACE workaround  --------------------------------------//
//  Because std::size_t usage is so common, even in boost headers which do not
//  otherwise use the C library, the <cstddef> workaround is included here so
//  that ugly workaround code need not appear in many other boost headers.
//  NOTE WELL: This is a workaround for non-conforming compilers; <cstddef>
//  must still be #included in the usual places so that <cstddef> inclusion
//  works as expected with standard conforming compilers.  The resulting
//  double inclusion of <cstddef> is harmless.

# if defined(BOOST_NO_STDC_NAMESPACE) && defined(__cplusplus)
#   include <cstddef>
    namespace std { using ::ptrdiff_t; using ::size_t; }
# endif

//  Workaround for the unfortunate min/max macros defined by some platform headers

#define BOOST_PREVENT_MACRO_SUBSTITUTION

#ifndef BOOST_USING_STD_MIN
#  define BOOST_USING_STD_MIN() using std::min
#endif

#ifndef BOOST_USING_STD_MAX
#  define BOOST_USING_STD_MAX() using std::max
#endif

//  BOOST_NO_STD_MIN_MAX workaround  -----------------------------------------//

#  if defined(BOOST_NO_STD_MIN_MAX) && defined(__cplusplus)

namespace std {
  template <class _Tp>
  inline const _Tp& min BOOST_PREVENT_MACRO_SUBSTITUTION (const _Tp& __a, const _Tp& __b) {
    return __b < __a ? __b : __a;
  }
  template <class _Tp>
  inline const _Tp& max BOOST_PREVENT_MACRO_SUBSTITUTION (const _Tp& __a, const _Tp& __b) {
    return  __a < __b ? __b : __a;
  }
}

#  endif

// BOOST_STATIC_CONSTANT workaround --------------------------------------- //
// On compilers which don't allow in-class initialization of static integral
// constant members, we must use enums as a workaround if we want the constants
// to be available at compile-time. This macro gives us a convenient way to
// declare such constants.

#  ifdef BOOST_NO_INCLASS_MEMBER_INITIALIZATION
#       define BOOST_STATIC_CONSTANT(type, assignment) enum { assignment }
#  else
#     define BOOST_STATIC_CONSTANT(type, assignment) static const type assignment
#  endif

// BOOST_USE_FACET / HAS_FACET workaround ----------------------------------//
// When the standard library does not have a conforming std::use_facet there
// are various workarounds available, but they differ from library to library.
// The same problem occurs with has_facet.
// These macros provide a consistent way to access a locale's facets.
// Usage:
//    replace
//       std::use_facet<Type>(loc);
//    with
//       BOOST_USE_FACET(Type, loc);
//    Note do not add a std:: prefix to the front of BOOST_USE_FACET!
//  Use for BOOST_HAS_FACET is analogous.

#if defined(BOOST_NO_STD_USE_FACET)
#  ifdef BOOST_HAS_TWO_ARG_USE_FACET
#     define BOOST_USE_FACET(Type, loc) std::use_facet(loc, static_cast<Type*>(0))
#     define BOOST_HAS_FACET(Type, loc) std::has_facet(loc, static_cast<Type*>(0))
#  elif defined(BOOST_HAS_MACRO_USE_FACET)
#     define BOOST_USE_FACET(Type, loc) std::_USE(loc, Type)
#     define BOOST_HAS_FACET(Type, loc) std::_HAS(loc, Type)
#  elif defined(BOOST_HAS_STLP_USE_FACET)
#     define BOOST_USE_FACET(Type, loc) (*std::_Use_facet<Type >(loc))
#     define BOOST_HAS_FACET(Type, loc) std::has_facet< Type >(loc)
#  endif
#else
#  define BOOST_USE_FACET(Type, loc) std::use_facet< Type >(loc)
#  define BOOST_HAS_FACET(Type, loc) std::has_facet< Type >(loc)
#endif

// BOOST_NESTED_TEMPLATE workaround ------------------------------------------//
// Member templates are supported by some compilers even though they can't use
// the A::template member<U> syntax, as a workaround replace:
//
// typedef typename A::template rebind<U> binder;
//
// with:
//
// typedef typename A::BOOST_NESTED_TEMPLATE rebind<U> binder;

#ifndef BOOST_NO_MEMBER_TEMPLATE_KEYWORD
#  define BOOST_NESTED_TEMPLATE template
#else
#  define BOOST_NESTED_TEMPLATE
#endif

// BOOST_UNREACHABLE_RETURN(x) workaround -------------------------------------//
// Normally evaluates to nothing, unless BOOST_NO_UNREACHABLE_RETURN_DETECTION
// is defined, in which case it evaluates to return x; Use when you have a return
// statement that can never be reached.

#ifdef BOOST_NO_UNREACHABLE_RETURN_DETECTION
#  define BOOST_UNREACHABLE_RETURN(x) return x;
#else
#  define BOOST_UNREACHABLE_RETURN(x)
#endif

// BOOST_DEDUCED_TYPENAME workaround ------------------------------------------//
//
// Some compilers don't support the use of `typename' for dependent
// types in deduced contexts, e.g.
//
//     template <class T> void f(T, typename T::type);
//                                  ^^^^^^^^
// Replace these declarations with:
//
//     template <class T> void f(T, BOOST_DEDUCED_TYPENAME T::type);

#ifndef BOOST_NO_DEDUCED_TYPENAME
#  define BOOST_DEDUCED_TYPENAME typename
#else
#  define BOOST_DEDUCED_TYPENAME
#endif

#ifndef BOOST_NO_TYPENAME_WITH_CTOR
#  define BOOST_CTOR_TYPENAME typename
#else
#  define BOOST_CTOR_TYPENAME
#endif

// long long workaround ------------------------------------------//
// On gcc (and maybe other compilers?) long long is alway supported
// but it's use may generate either warnings (with -ansi), or errors
// (with -pedantic -ansi) unless it's use is prefixed by __extension__
//
#if defined(BOOST_HAS_LONG_LONG) && defined(__cplusplus)
namespace boost{
#  ifdef __GNUC__
   __extension__ typedef long long long_long_type;
   __extension__ typedef unsigned long long ulong_long_type;
#  else
   typedef long long long_long_type;
   typedef unsigned long long ulong_long_type;
#  endif
}
#endif

// BOOST_[APPEND_]EXPLICIT_TEMPLATE_[NON_]TYPE macros --------------------------//
//
// Some compilers have problems with function templates whose template
// parameters don't appear in the function parameter list (basically
// they just link one instantiation of the template in the final
// executable). These macros provide a uniform way to cope with the
// problem with no effects on the calling syntax.

// Example:
//
//  #include <iostream>
//  #include <ostream>
//  #include <typeinfo>
//
//  template <int n>
//  void f() { std::cout << n << ' '; }
//
//  template <typename T>
//  void g() { std::cout << typeid(T).name() << ' '; }
//
//  int main() {
//    f<1>();
//    f<2>();
//
//    g<int>();
//    g<double>();
//  }
//
// With VC++ 6.0 the output is:
//
//   2 2 double double
//
// To fix it, write
//
//   template <int n>
//   void f(BOOST_EXPLICIT_TEMPLATE_NON_TYPE(int, n)) { ... }
//
//   template <typename T>
//   void g(BOOST_EXPLICIT_TEMPLATE_TYPE(T)) { ... }
//


#if defined(BOOST_NO_EXPLICIT_FUNCTION_TEMPLATE_ARGUMENTS) && defined(__cplusplus)

#  include "boost/type.hpp"
#  include "boost/non_type.hpp"

#  define BOOST_EXPLICIT_TEMPLATE_TYPE(t)              boost::type<t>* = 0
#  define BOOST_EXPLICIT_TEMPLATE_TYPE_SPEC(t)         boost::type<t>*
#  define BOOST_EXPLICIT_TEMPLATE_NON_TYPE(t, v)       boost::non_type<t, v>* = 0
#  define BOOST_EXPLICIT_TEMPLATE_NON_TYPE_SPEC(t, v)  boost::non_type<t, v>*

#  define BOOST_APPEND_EXPLICIT_TEMPLATE_TYPE(t)        \
             , BOOST_EXPLICIT_TEMPLATE_TYPE(t)
#  define BOOST_APPEND_EXPLICIT_TEMPLATE_TYPE_SPEC(t)   \
             , BOOST_EXPLICIT_TEMPLATE_TYPE_SPEC(t)
#  define BOOST_APPEND_EXPLICIT_TEMPLATE_NON_TYPE(t, v) \
             , BOOST_EXPLICIT_TEMPLATE_NON_TYPE(t, v)
#  define BOOST_APPEND_EXPLICIT_TEMPLATE_NON_TYPE_SPEC(t, v)    \
             , BOOST_EXPLICIT_TEMPLATE_NON_TYPE_SPEC(t, v)

#else

// no workaround needed: expand to nothing

#  define BOOST_EXPLICIT_TEMPLATE_TYPE(t)
#  define BOOST_EXPLICIT_TEMPLATE_TYPE_SPEC(t)
#  define BOOST_EXPLICIT_TEMPLATE_NON_TYPE(t, v)
#  define BOOST_EXPLICIT_TEMPLATE_NON_TYPE_SPEC(t, v)

#  define BOOST_APPEND_EXPLICIT_TEMPLATE_TYPE(t)
#  define BOOST_APPEND_EXPLICIT_TEMPLATE_TYPE_SPEC(t)
#  define BOOST_APPEND_EXPLICIT_TEMPLATE_NON_TYPE(t, v)
#  define BOOST_APPEND_EXPLICIT_TEMPLATE_NON_TYPE_SPEC(t, v)


#endif // defined BOOST_NO_EXPLICIT_FUNCTION_TEMPLATE_ARGUMENTS

// When BOOST_NO_STD_TYPEINFO is defined, we can just import
// the global definition into std namespace:
#if defined(BOOST_NO_STD_TYPEINFO) && defined(__cplusplus)
#include <typeinfo>
namespace std{ using ::type_info; }
#endif

// ---------------------------------------------------------------------------//

//
// Helper macro BOOST_STRINGIZE:
// Converts the parameter X to a string after macro replacement
// on X has been performed.
//
#define BOOST_STRINGIZE(X) BOOST_DO_STRINGIZE(X)
#define BOOST_DO_STRINGIZE(X) #X

//
// Helper macro BOOST_JOIN:
// The following piece of macro magic joins the two
// arguments together, even when one of the arguments is
// itself a macro (see 16.3.1 in C++ standard).  The key
// is that macro expansion of macro arguments does not
// occur in BOOST_DO_JOIN2 but does in BOOST_DO_JOIN.
//
#define BOOST_JOIN( X, Y ) BOOST_DO_JOIN( X, Y )
#define BOOST_DO_JOIN( X, Y ) BOOST_DO_JOIN2(X,Y)
#define BOOST_DO_JOIN2( X, Y ) X##Y

//
// Helper macros BOOST_NOEXCEPT, BOOST_NOEXCEPT_IF, BOOST_NOEXCEPT_EXPR
// These aid the transition to C++11 while still supporting C++03 compilers
//
#ifdef BOOST_NO_NOEXCEPT
#  define BOOST_NOEXCEPT
#  define BOOST_NOEXCEPT_IF(Predicate)
#  define BOOST_NOEXCEPT_EXPR(Expression) false
#else
#  define BOOST_NOEXCEPT noexcept
#  define BOOST_NOEXCEPT_IF(Predicate) noexcept((Predicate))
#  define BOOST_NOEXCEPT_EXPR(Expression) noexcept((Expression))
#endif

//
// Set some default values for compiler/library/platform names.
// These are for debugging config setup only:
//
#  ifndef BOOST_COMPILER
#     define BOOST_COMPILER "Unknown ISO C++ Compiler"
#  endif
#  ifndef BOOST_STDLIB
#     define BOOST_STDLIB "Unknown ISO standard library"
#  endif
#  ifndef BOOST_PLATFORM
#     if defined(unix) || defined(__unix) || defined(_XOPEN_SOURCE) \
         || defined(_POSIX_SOURCE)
#        define BOOST_PLATFORM "Generic Unix"
#     else
#        define BOOST_PLATFORM "Unknown"
#     endif
#  endif

//
// Set some default values GPU support
//
#  ifndef BOOST_GPU_ENABLED
#  define BOOST_GPU_ENABLED 
#  endif

//
// constexpr workarounds
// 
#if defined(BOOST_NO_CONSTEXPR)
#define BOOST_CONSTEXPR
#define BOOST_CONSTEXPR_OR_CONST const
#else
#define BOOST_CONSTEXPR constexpr
#define BOOST_CONSTEXPR_OR_CONST constexpr
#endif

#define BOOST_STATIC_CONSTEXPR  static BOOST_CONSTEXPR_OR_CONST

// BOOST_FORCEINLINE ---------------------------------------------//
// Macro to use in place of 'inline' to force a function to be inline
#if !defined(BOOST_FORCEINLINE)
#  if defined(_MSC_VER)
#    define BOOST_FORCEINLINE __forceinline
#  elif defined(__GNUC__) && __GNUC__ > 3
#    define BOOST_FORCEINLINE inline __attribute__ ((always_inline))
#  else
#    define BOOST_FORCEINLINE inline
#  endif
#endif

#endif

