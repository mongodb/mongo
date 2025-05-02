///////////////////////////////////////////////////////////////
//  Copyright 2010 - 2021 Douglas Gregor
//  Copyright 2021 Matt Borland.
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE_1_0.txt or copy at https://www.boost.org/LICENSE_1_0.txt
//
//  Used to support configuration options depending on standalone context
//  by providing either required support or disabling functionality  

#ifndef BOOST_MP_STANDALONE_CONFIG_HPP
#define BOOST_MP_STANDALONE_CONFIG_HPP

#include <climits>

// Boost.Config is dependency free so it is considered a requirement to use Boost.Multiprecision in standalone mode
#ifdef __has_include
#  if __has_include(<boost/config.hpp>)
#    include <boost/config.hpp>
#    include <boost/config/workaround.hpp>
#  else
#    error "Boost.Config is considered a requirement to use Boost.Multiprecision in standalone mode. A package is provided at https://github.com/boostorg/multiprecision/releases"
#  endif
#else
// Provides the less helpful fatal error: 'boost/config.hpp' file not found if not available
#  include <boost/config.hpp>
#  include <boost/config/workaround.hpp>
#endif

// Minimum language standard transition
 #ifdef _MSVC_LANG
 #  if _MSVC_LANG < 201402L
 #    pragma warning("The minimum language standard to use Boost.Math will be C++14 starting in July 2023 (Boost 1.82 release)");
 #  endif
 #else
 #  if __cplusplus < 201402L
 #    warning "The minimum language standard to use Boost.Math will be C++14 starting in July 2023 (Boost 1.82 release)"
 #  endif
 #endif

// If any of the most frequently used boost headers are missing assume that standalone mode is supposed to be used
#ifdef __has_include
#if !__has_include(<boost/assert.hpp>) || !__has_include(<boost/lexical_cast.hpp>) || \
    !__has_include(<boost/throw_exception.hpp>) || !__has_include(<boost/predef/other/endian.h>)
#   ifndef BOOST_MP_STANDALONE
#       define BOOST_MP_STANDALONE
#   endif
#endif
#endif

#ifndef BOOST_MP_STANDALONE

#include <boost/integer.hpp>
#include <boost/integer_traits.hpp>

// Required typedefs for interoperability with standalone mode
#if defined(BOOST_HAS_INT128) && defined(__cplusplus)
namespace boost { namespace multiprecision {
   using int128_type = boost::int128_type;
   using uint128_type = boost::uint128_type;
}}
#endif
#if defined(BOOST_HAS_FLOAT128) && defined(__cplusplus)
namespace boost { namespace multiprecision {
   using float128_type = boost::float128_type;
}}
#endif

// Boost.Math available by default
#define BOOST_MP_MATH_AVAILABLE

#else // Standalone mode

#ifdef BOOST_MATH_STANDALONE
#  define BOOST_MP_MATH_AVAILABLE
#endif

#ifndef BOOST_MP_MATH_AVAILABLE
#  define BOOST_MATH_INSTRUMENT_CODE(x)
#endif

// Prevent Macro sub
#ifndef BOOST_PREVENT_MACRO_SUBSTITUTION
#  define BOOST_PREVENT_MACRO_SUBSTITUTION
#endif

#if defined(BOOST_HAS_INT128) && defined(__cplusplus)
namespace boost { namespace multiprecision {
#  ifdef __GNUC__
   __extension__ typedef __int128 int128_type;
   __extension__ typedef unsigned __int128 uint128_type;
#  else
   typedef __int128 int128_type;
   typedef unsigned __int128 uint128_type;
#  endif
}}

#endif
// same again for __float128:
#if defined(BOOST_HAS_FLOAT128) && defined(__cplusplus)
namespace boost { namespace multiprecision {
#  ifdef __GNUC__
   __extension__ typedef __float128 float128_type;
#  else
   typedef __float128 float128_type;
#  endif
}}

#endif

#endif // BOOST_MP_STANDALONE

// Workarounds for numeric limits on old compilers
#ifdef BOOST_HAS_INT128
#  ifndef INT128_MAX
#    define INT128_MAX static_cast<boost::multiprecision::int128_type>((static_cast<boost::multiprecision::uint128_type>(1) << ((__SIZEOF_INT128__ * __CHAR_BIT__) - 1)) - 1)
#  endif
#  ifndef INT128_MIN
#    define INT128_MIN (-INT128_MAX - 1)
#  endif
#  ifndef UINT128_MAX
#    define UINT128_MAX ((2 * static_cast<boost::multiprecision::uint128_type>(INT128_MAX)) + 1)
#  endif
#endif

#define BOOST_MP_CXX14_CONSTEXPR BOOST_CXX14_CONSTEXPR
//
// Early compiler versions trip over the constexpr code:
//
#if defined(__clang__) && (__clang_major__ < 5)
#undef BOOST_MP_CXX14_CONSTEXPR
#define BOOST_MP_CXX14_CONSTEXPR
#endif
#if defined(__apple_build_version__) && (__clang_major__ < 9)
#undef BOOST_MP_CXX14_CONSTEXPR
#define BOOST_MP_CXX14_CONSTEXPR
#endif
#if defined(BOOST_GCC) && (__GNUC__ < 6)
#undef BOOST_MP_CXX14_CONSTEXPR
#define BOOST_MP_CXX14_CONSTEXPR
#endif
#if defined(BOOST_INTEL)
#undef BOOST_MP_CXX14_CONSTEXPR
#define BOOST_MP_CXX14_CONSTEXPR
#define BOOST_MP_NO_CONSTEXPR_DETECTION
#endif

#ifdef __has_attribute
#  if __has_attribute(fallthrough)
#    define BOOST_MP_FALLTHROUGH [[fallthrough]]
#  endif
#endif

#ifndef BOOST_MP_FALLTHROUGH
#  if __GNUC__ >= 7
#    define BOOST_MP_FALLTHROUGH __attribute__((fallthrough))
#  else
#    define BOOST_MP_FALLTHROUGH ((void)0)
#  endif
#endif

#endif // BOOST_MP_STANDALONE_CONFIG_HPP
