//  Copyright (C) Christof Meerwald 2003
//  Copyright (C) Dan Watkins 2003
//
//  Use, modification and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  Digital Mars C++ compiler setup:
#define BOOST_COMPILER __DMC_VERSION_STRING__

#define BOOST_HAS_LONG_LONG
#define BOOST_HAS_PRAGMA_ONCE

#if (__DMC__ <= 0x833)
#define BOOST_FUNCTION_SCOPE_USING_DECLARATION_BREAKS_ADL
#define BOOST_NO_TEMPLATE_TEMPLATES
#define BOOST_NEEDS_TOKEN_PASTING_OP_FOR_TOKENS_JUXTAPOSING
#define BOOST_NO_ARRAY_TYPE_SPECIALIZATIONS
#define BOOST_NO_EXPLICIT_FUNCTION_TEMPLATE_ARGUMENTS
#endif
#if (__DMC__ <= 0x840) || !defined(BOOST_STRICT_CONFIG)
#define BOOST_NO_EXPLICIT_FUNCTION_TEMPLATE_ARGUMENTS
#define BOOST_NO_MEMBER_TEMPLATE_FRIENDS
#define BOOST_NO_OPERATORS_IN_NAMESPACE
#define BOOST_NO_UNREACHABLE_RETURN_DETECTION
#define BOOST_NO_SFINAE
#define BOOST_NO_USING_TEMPLATE
#define BOOST_FUNCTION_SCOPE_USING_DECLARATION_BREAKS_ADL
#endif

//
// has macros:
#if (__DMC__ >= 0x840)
#define BOOST_HAS_DIRENT_H
#define BOOST_HAS_STDINT_H
#define BOOST_HAS_WINTHREADS
#endif

#if (__DMC__ >= 0x847)
#define BOOST_HAS_EXPM1
#define BOOST_HAS_LOG1P
#endif

//
// Is this really the best way to detect whether the std lib is in namespace std?
//
#ifdef __cplusplus
#include <cstddef>
#endif
#if !defined(__STL_IMPORT_VENDOR_CSTD) && !defined(_STLP_IMPORT_VENDOR_CSTD)
#  define BOOST_NO_STDC_NAMESPACE
#endif


// check for exception handling support:
#if !defined(_CPPUNWIND) && !defined(BOOST_NO_EXCEPTIONS)
#  define BOOST_NO_EXCEPTIONS
#endif

//
// C++0x features
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
#define BOOST_NO_UNIFIED_INITIALIZATION_SYNTAX

#if (__DMC__ < 0x812)
#define BOOST_NO_VARIADIC_MACROS
#endif

#if __DMC__ < 0x800
#error "Compiler not supported or configured - please reconfigure"
#endif
//
// last known and checked version is ...:
#if (__DMC__ > 0x848)
#  if defined(BOOST_ASSERT_CONFIG)
#     error "Unknown compiler version - please run the configure tests and report the results"
#  endif
#endif
