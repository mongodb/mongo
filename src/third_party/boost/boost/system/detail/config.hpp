#ifndef BOOST_SYSTEM_DETAIL_CONFIG_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_CONFIG_HPP_INCLUDED

// Copyright 2018-2022 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/system for documentation.

#include <boost/config.hpp>
#include <boost/config/workaround.hpp>

// The macro BOOST_SYSTEM_DISABLE_THREADS can be defined on configurations
// that provide <system_error> and <atomic>, but not <mutex>, such as the
// single-threaded libstdc++.
//
// https://github.com/boostorg/system/issues/92

// BOOST_SYSTEM_NOEXCEPT
// Retained for backward compatibility

#define BOOST_SYSTEM_NOEXCEPT noexcept

// BOOST_SYSTEM_HAS_CONSTEXPR

#if !defined(BOOST_NO_CXX14_CONSTEXPR)
# define BOOST_SYSTEM_HAS_CONSTEXPR
#endif

#if BOOST_WORKAROUND(BOOST_GCC, < 60000)
# undef BOOST_SYSTEM_HAS_CONSTEXPR
#endif

#if defined(BOOST_SYSTEM_HAS_CONSTEXPR)
# define BOOST_SYSTEM_CONSTEXPR constexpr
#else
# define BOOST_SYSTEM_CONSTEXPR
#endif

// BOOST_SYSTEM_DEPRECATED

#if defined(__clang__)
# define BOOST_SYSTEM_DEPRECATED(msg) __attribute__((deprecated(msg)))
#elif defined(__GNUC__)
# if __GNUC__ * 100 + __GNUC_MINOR__ >= 405
#  define BOOST_SYSTEM_DEPRECATED(msg) __attribute__((deprecated(msg)))
# else
#  define BOOST_SYSTEM_DEPRECATED(msg) __attribute__((deprecated))
# endif
#elif defined(_MSC_VER)
#  define BOOST_SYSTEM_DEPRECATED(msg) __declspec(deprecated(msg))
#elif defined(__sun)
#  define BOOST_SYSTEM_DEPRECATED(msg) __attribute__((deprecated(msg)))
#else
# define BOOST_SYSTEM_DEPRECATED(msg)
#endif

// BOOST_SYSTEM_CLANG_6

// Android NDK r18b has Clang 7.0.2 that still needs the workaround
// https://github.com/boostorg/system/issues/100
#if defined(__clang__) && (__clang_major__ < 7 || (defined(__APPLE__) && __clang_major__ < 11) || (defined(__ANDROID__) && __clang_major__ == 7))
# define BOOST_SYSTEM_CLANG_6
#endif

//

#if defined(BOOST_LIBSTDCXX_VERSION) && BOOST_LIBSTDCXX_VERSION < 50000
# define BOOST_SYSTEM_AVOID_STD_GENERIC_CATEGORY
#endif

#if defined(__CYGWIN__) || defined(__MINGW32__) || (defined(_MSC_VER) && _MSC_VER == 1800) || (defined(BOOST_LIBSTDCXX_VERSION) && BOOST_LIBSTDCXX_VERSION < 90000)

// Under Cygwin (and MinGW!), std::system_category() is POSIX
// Under VS2013, std::system_category() isn't quite right
// Under libstdc++ before 7.4, before 8.3, before 9.1, default_error_condition
// for the system category returns a condition from the system category

# define BOOST_SYSTEM_AVOID_STD_SYSTEM_CATEGORY
#endif

#endif // BOOST_SYSTEM_DETAIL_CONFIG_HPP_INCLUDED
