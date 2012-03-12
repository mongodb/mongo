// (C) Copyright Douglas Gregor 2010
//
//  Use, modification and distribution are subject to the 
//  Boost Software License, Version 1.0. (See accompanying file 
//  LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

//  See http://www.boost.org for most recent version.

// Clang compiler setup.

#if __has_feature(cxx_exceptions) && !defined(BOOST_NO_EXCEPTIONS)
#else
#  define BOOST_NO_EXCEPTIONS
#endif

#if !__has_feature(cxx_rtti)
#  define BOOST_NO_RTTI
#endif

#if defined(__int64)
#  define BOOST_HAS_MS_INT64
#endif

#define BOOST_HAS_NRVO

// Clang supports "long long" in all compilation modes.

#if !__has_feature(cxx_auto_type)
#  define BOOST_NO_AUTO_DECLARATIONS
#  define BOOST_NO_AUTO_MULTIDECLARATIONS
#endif

#if !(defined(__GXX_EXPERIMENTAL_CXX0X__) || __cplusplus >= 201103L)
#  define BOOST_NO_CHAR16_T
#  define BOOST_NO_CHAR32_T
#endif

#if !__has_feature(cxx_constexpr)
#  define BOOST_NO_CONSTEXPR
#endif

#if !__has_feature(cxx_decltype)
#  define BOOST_NO_DECLTYPE
#endif

#define BOOST_NO_DECLTYPE_N3276

#if !__has_feature(cxx_defaulted_functions)
#  define BOOST_NO_DEFAULTED_FUNCTIONS
#endif

#if !__has_feature(cxx_deleted_functions)
#  define BOOST_NO_DELETED_FUNCTIONS
#endif

#if !__has_feature(cxx_explicit_conversions)
#  define BOOST_NO_EXPLICIT_CONVERSION_OPERATORS
#endif

#if !__has_feature(cxx_default_function_template_args)
#  define BOOST_NO_FUNCTION_TEMPLATE_DEFAULT_ARGS
#endif

#if !__has_feature(cxx_generalized_initializers)
#  define BOOST_NO_INITIALIZER_LISTS
#endif

#if !__has_feature(cxx_lambdas)
#  define BOOST_NO_LAMBDAS
#endif

#if !__has_feature(cxx_noexcept)
#  define BOOST_NO_NOEXCEPT
#endif

#if !__has_feature(cxx_nullptr)
#  define BOOST_NO_NULLPTR
#endif

#if !__has_feature(cxx_raw_string_literals)
#  define BOOST_NO_RAW_LITERALS
#endif

#if !__has_feature(cxx_generalized_initializers)
#  define BOOST_NO_UNIFIED_INITIALIZATION_SYNTAX
#endif

#if !__has_feature(cxx_rvalue_references)
#  define BOOST_NO_RVALUE_REFERENCES
#endif

#if !__has_feature(cxx_strong_enums)
#  define BOOST_NO_SCOPED_ENUMS
#endif

#if !__has_feature(cxx_static_assert)
#  define BOOST_NO_STATIC_ASSERT
#endif

#if !__has_feature(cxx_alias_templates)
#  define BOOST_NO_TEMPLATE_ALIASES
#endif

#if !__has_feature(cxx_unicode_literals)
#  define BOOST_NO_UNICODE_LITERALS
#endif

#if !__has_feature(cxx_variadic_templates)
#  define BOOST_NO_VARIADIC_TEMPLATES
#endif

// Clang always supports variadic macros
// Clang always supports extern templates

#ifndef BOOST_COMPILER
#  define BOOST_COMPILER "Clang version " __clang_version__
#endif

// Macro used to identify the Clang compiler.
#define BOOST_CLANG 1

