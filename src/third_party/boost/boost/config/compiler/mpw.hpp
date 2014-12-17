//  (C) Copyright John Maddock 2001 - 2002. 
//  (C) Copyright Aleksey Gurtovoy 2002. 
//  Use, modification and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for most recent version.

//  MPW C++ compilers setup:

#   if    defined(__SC__)
#     define BOOST_COMPILER "MPW SCpp version " BOOST_STRINGIZE(__SC__)
#   elif defined(__MRC__)
#     define BOOST_COMPILER "MPW MrCpp version " BOOST_STRINGIZE(__MRC__)
#   else
#     error "Using MPW compiler configuration by mistake.  Please update."
#   endif

//
// MPW 8.90:
//
#if (MPW_CPLUS <= 0x890) || !defined(BOOST_STRICT_CONFIG)
#  define BOOST_NO_CV_SPECIALIZATIONS
#  define BOOST_NO_DEPENDENT_NESTED_DERIVATIONS
#  define BOOST_NO_DEPENDENT_TYPES_IN_TEMPLATE_VALUE_PARAMETERS
#  define BOOST_NO_INCLASS_MEMBER_INITIALIZATION
#  define BOOST_NO_INTRINSIC_WCHAR_T
#  define BOOST_NO_TEMPLATE_PARTIAL_SPECIALIZATION
#  define BOOST_NO_USING_TEMPLATE

#  define BOOST_NO_CWCHAR
#  define BOOST_NO_LIMITS_COMPILE_TIME_CONSTANTS

#  define BOOST_NO_STD_ALLOCATOR /* actually a bug with const reference overloading */

#endif

//
// C++0x features
//
//   See boost\config\suffix.hpp for BOOST_NO_LONG_LONG
//
#define BOOST_NO_AUTO_DECLARATIONS
#define BOOST_NO_AUTO_MULTIDECLARATIONS
#define BOOST_NO_CHAR16_T
#define BOOST_NO_CHAR32_T
#define BOOST_NO_CONSTEXPR
#define BOOST_NO_DECLTYPE
#define BOOST_NO_DECLTYPE_N3276
#define BOOST_NO_DEFAULTED_FUNCTIONS
#define BOOST_NO_DELETED_FUNCTIONS
#define BOOST_NO_EXPLICIT_CONVERSION_OPERATORS
#define BOOST_NO_EXTERN_TEMPLATE
#define BOOST_NO_FUNCTION_TEMPLATE_DEFAULT_ARGS
#define BOOST_NO_INITIALIZER_LISTS
#define BOOST_NO_LAMBDAS
#define BOOST_NO_NOEXCEPT
#define BOOST_NO_NULLPTR
#define BOOST_NO_RAW_LITERALS
#define BOOST_NO_RVALUE_REFERENCES
#define BOOST_NO_SCOPED_ENUMS
#define BOOST_NO_SFINAE_EXPR
#define BOOST_NO_STATIC_ASSERT
#define BOOST_NO_TEMPLATE_ALIASES
#define BOOST_NO_UNICODE_LITERALS
#define BOOST_NO_VARIADIC_TEMPLATES
#define BOOST_NO_VARIADIC_MACROS
#define BOOST_NO_UNIFIED_INITIALIZATION_SYNTAX

//
// versions check:
// we don't support MPW prior to version 8.9:
#if MPW_CPLUS < 0x890
#  error "Compiler not supported or configured - please reconfigure"
#endif
//
// last known and checked version is 0x890:
#if (MPW_CPLUS > 0x890)
#  if defined(BOOST_ASSERT_CONFIG)
#     error "Unknown compiler version - please run the configure tests and report the results"
#  endif
#endif


