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
#undef BOOST_NO_CXX11_AUTO_DECLARATIONS
#undef BOOST_NO_CXX11_AUTO_MULTIDECLARATIONS
#define BOOST_HAS_NRVO
#define BOOST_NO_CXX11_VARIADIC_TEMPLATES
#define BOOST_NO_CXX11_UNIFIED_INITIALIZATION_SYNTAX
#define BOOST_NO_CXX11_UNICODE_LITERALS
#define BOOST_NO_TWO_PHASE_NAME_LOOKUP
#define BOOST_HAS_NRVO
#define BOOST_NO_CXX11_TEMPLATE_ALIASES
#define BOOST_NO_CXX11_STATIC_ASSERT
#define BOOST_NO_SFINAE_EXPR
#define BOOST_NO_CXX11_SCOPED_ENUMS
#define BOOST_NO_CXX11_RVALUE_REFERENCES
#define BOOST_NO_CXX11_RANGE_BASED_FOR
#define BOOST_NO_CXX11_RAW_LITERALS
#define BOOST_NO_CXX11_NULLPTR
#define BOOST_NO_CXX11_NOEXCEPT
#define BOOST_NO_CXX11_LAMBDAS
#define BOOST_NO_CXX11_LOCAL_CLASS_TEMPLATE_PARAMETERS
#define BOOST_NO_CXX11_FUNCTION_TEMPLATE_DEFAULT_ARGS
#define BOOST_NO_CXX11_EXPLICIT_CONVERSION_OPERATORS
#define BOOST_NO_CXX11_DELETED_FUNCTIONS
#define BOOST_NO_CXX11_DEFAULTED_FUNCTIONS
#define BOOST_NO_CXX11_DECLTYPE_N3276
#define BOOST_NO_CXX11_DECLTYPE
#define BOOST_NO_CXX11_CONSTEXPR
#define BOOST_NO_CXX11_USER_DEFINED_LITERALS
#define BOOST_NO_COMPLETE_VALUE_INITIALIZATION
#define BOOST_NO_CXX11_CHAR32_T
#define BOOST_NO_CXX11_CHAR16_T
#define BOOST_NO_CXX11_ALIGNAS
//#define BOOST_BCB_PARTIAL_SPECIALIZATION_BUG
#define BOOST_MATH_DISABLE_STD_FPCLASSIFY
//#define BOOST_HAS_FPCLASSIFY

#define BOOST_SP_USE_PTHREADS
#define BOOST_AC_USE_PTHREADS

