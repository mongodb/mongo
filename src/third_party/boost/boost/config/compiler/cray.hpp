//  (C) Copyright John Maddock 2011.
//  Use, modification and distribution are subject to the
//  Boost Software License, Version 1.0. (See accompanying file
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for most recent version.

//  Greenhills C compiler setup:

#define BOOST_COMPILER "Cray C version " BOOST_STRINGIZE(_RELEASE)

#if _RELEASE < 7
#  error "Boost is not configured for Cray compilers prior to version 7, please try the configure script."
#endif

//
// Check this is a recent EDG based compiler, otherwise we don't support it here:
//
#ifndef __EDG_VERSION__
#  error "Unsupported Cray compiler, please try running the configure script."
#endif

#include "boost/config/compiler/common_edg.hpp"

//
// Cray peculiarities, probably version 7 specific:
//
#undef BOOST_NO_AUTO_DECLARATIONS
#undef BOOST_NO_AUTO_MULTIDECLARATIONS
#define BOOST_HAS_NRVO
#define BOOST_NO_VARIADIC_TEMPLATES
#define BOOST_NO_UNIFIED_INITIALIZATION_SYNTAX
#define BOOST_NO_UNICODE_LITERALS
#define BOOST_NO_TWO_PHASE_NAME_LOOKUP
#define BOOST_HAS_NRVO
#define BOOST_NO_TEMPLATE_ALIASES
#define BOOST_NO_STATIC_ASSERT
#define BOOST_NO_SFINAE_EXPR
#define BOOST_NO_SCOPED_ENUMS
#define BOOST_NO_RVALUE_REFERENCES
#define BOOST_NO_RAW_LITERALS
#define BOOST_NO_NULLPTR
#define BOOST_NO_NOEXCEPT
#define BOOST_NO_LAMBDAS
#define BOOST_NO_FUNCTION_TEMPLATE_DEFAULT_ARGS
#define BOOST_NO_EXPLICIT_CONVERSION_OPERATORS
#define BOOST_NO_DELETED_FUNCTIONS
#define BOOST_NO_DEFAULTED_FUNCTIONS
#define BOOST_NO_DECLTYPE_N3276
#define BOOST_NO_DECLTYPE
#define BOOST_NO_CONSTEXPR
#define BOOST_NO_COMPLETE_VALUE_INITIALIZATION
#define BOOST_NO_CHAR32_T
#define BOOST_NO_CHAR16_T
//#define BOOST_BCB_PARTIAL_SPECIALIZATION_BUG
#define BOOST_MATH_DISABLE_STD_FPCLASSIFY
//#define BOOST_HAS_FPCLASSIFY

#define BOOST_SP_USE_PTHREADS 
#define BOOST_AC_USE_PTHREADS 

