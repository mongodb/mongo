#ifndef BOOST_SYSTEM_DETAIL_CONFIG_HPP_INCLUDED
#define BOOST_SYSTEM_DETAIL_CONFIG_HPP_INCLUDED

// Copyright 2018 Peter Dimov
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
// See http://www.boost.org/libs/system for documentation.

#include <boost/config.hpp>
#include <boost/config/workaround.hpp>

// BOOST_SYSTEM_HAS_SYSTEM_ERROR

#if !defined(BOOST_NO_CXX11_HDR_SYSTEM_ERROR)
# define BOOST_SYSTEM_HAS_SYSTEM_ERROR
#endif

#if BOOST_WORKAROUND(BOOST_GCC, < 40600)
// g++ 4.4's <map> is not good enough
# undef BOOST_SYSTEM_HAS_SYSTEM_ERROR
#endif

// BOOST_SYSTEM_NOEXCEPT
// Retained for backward compatibility

#define BOOST_SYSTEM_NOEXCEPT BOOST_NOEXCEPT

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

// BOOST_SYSTEM_REQUIRE_CONST_INIT

#define BOOST_SYSTEM_REQUIRE_CONST_INIT

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::require_constant_initialization)
# undef BOOST_SYSTEM_REQUIRE_CONST_INIT
# define BOOST_SYSTEM_REQUIRE_CONST_INIT [[clang::require_constant_initialization]]
#endif
#endif

#endif // BOOST_SYSTEM_DETAIL_CONFIG_HPP_INCLUDED
