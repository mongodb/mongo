//  (C) Copyright John Maddock 2001 - 2002. 
//  (C) Copyright Jens Maurer 2001. 
//  (C) Copyright David Abrahams 2002. 
//  (C) Copyright Aleksey Gurtovoy 2002. 
//  (C) Copyright Markus Schoepflin 2005.
//  Use, modification and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for most recent version.

//
// Options common to all edg based compilers.
//
// This is included from within the individual compiler mini-configs.

#ifndef  __EDG_VERSION__
#  error This file requires that __EDG_VERSION__ be defined.
#endif

#if (__EDG_VERSION__ <= 238)
#   define BOOST_NO_INTEGRAL_INT64_T
#   define BOOST_NO_SFINAE
#endif

#if (__EDG_VERSION__ <= 240)
#   define BOOST_NO_VOID_RETURNS
#endif

#if (__EDG_VERSION__ <= 241) && !defined(BOOST_NO_ARGUMENT_DEPENDENT_LOOKUP)
#   define BOOST_NO_ARGUMENT_DEPENDENT_LOOKUP
#endif

#if (__EDG_VERSION__ <= 244) && !defined(BOOST_NO_TEMPLATE_TEMPLATES)
#   define BOOST_NO_TEMPLATE_TEMPLATES
#endif 

#if (__EDG_VERSION__ < 300) && !defined(BOOST_NO_IS_ABSTRACT)
#   define BOOST_NO_IS_ABSTRACT
#endif 

#if (__EDG_VERSION__ <= 303) && !defined(BOOST_FUNCTION_SCOPE_USING_DECLARATION_BREAKS_ADL)
#   define BOOST_FUNCTION_SCOPE_USING_DECLARATION_BREAKS_ADL
#endif 

// See also kai.hpp which checks a Kai-specific symbol for EH
# if !defined(__KCC) && !defined(__EXCEPTIONS) && !defined(BOOST_NO_EXCEPTIONS)
#     define BOOST_NO_EXCEPTIONS
# endif

# if !defined(__NO_LONG_LONG)
#     define BOOST_HAS_LONG_LONG
# else
#     define BOOST_NO_LONG_LONG
# endif

//
// C++0x features
//
//   See above for BOOST_NO_LONG_LONG
//
#if (__EDG_VERSION__ < 310)
#  define BOOST_NO_EXTERN_TEMPLATE
#endif
#if (__EDG_VERSION__ <= 310)
// No support for initializer lists
#  define BOOST_NO_INITIALIZER_LISTS
#endif
#if (__EDG_VERSION__ < 400)
#  define BOOST_NO_VARIADIC_MACROS
#endif

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
#define BOOST_NO_FUNCTION_TEMPLATE_DEFAULT_ARGS
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
#define BOOST_NO_UNIFIED_INITIALIZATION_SYNTAX

#ifdef c_plusplus
// EDG has "long long" in non-strict mode
// However, some libraries have insufficient "long long" support
// #define BOOST_HAS_LONG_LONG
#endif
