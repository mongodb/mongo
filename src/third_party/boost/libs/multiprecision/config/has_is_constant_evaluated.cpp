//  Copyright John Maddock 2013.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

// This program determines if std::is_constant_evaluated  is available to switch within a function to use compile-time constexp calculations.
// See https://en.cppreference.com/w/cpp/types/is_constant_evaluated
// and https://en.cppreference.com/w/cpp/compiler_support
// Currently only GCC 9 and Clang 9 and <cxxflags>-std=c++2a or  b2 cxxstd=2a

// https://clang.llvm.org/docs/LanguageExtensions.html#builtin-macros

// From Clang 10 onwards, __has_builtin(__X) can be used.  but also works for Clang 9

// boost\libs\multiprecision\config>b2 has_is_constant_evaluated toolset=clang-win-9.0.0 cxxstd=2a address-model=64 release  >  mp_is_const_eval_30Sep2019.log

#include <boost/config.hpp>
#include <boost/config/pragma_message.hpp>

#include <boost/multiprecision/number.hpp>

#ifdef __has_builtin
BOOST_PRAGMA_MESSAGE (" __has_builtin is defined.")

#  if __has_builtin(__builtin_is_constant_evaluated)
BOOST_PRAGMA_MESSAGE (" __has_builtin(__builtin_is_constant_evaluated), so BOOST_MP_NO_CONSTEXPR_DETECTION should NOT be defined.")
#  endif // __has_builtin(__builtin_is_constant_evaluated)

#endif // __has_builtin

#ifdef BOOST_MP_HAS_IS_CONSTANT_EVALUATED
BOOST_PRAGMA_MESSAGE ("BOOST_MP_HAS_IS_CONSTANT_EVALUATED defined.")
#else
BOOST_PRAGMA_MESSAGE ("BOOST_MP_HAS_IS_CONSTANT_EVALUATED is NOT defined, so no std::is_constant_evaluated() from std library.")
#endif

#ifdef BOOST_NO_CXX14_CONSTEXPR
BOOST_PRAGMA_MESSAGE ("BOOST_NO_CXX14_CONSTEXPR is defined.")
#endif

#ifdef BOOST_MP_NO_CONSTEXPR_DETECTION
#  error 1  "std::is_constant_evaluated is NOT available to determine if a calculation can use constexpr."
#endif
