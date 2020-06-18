// MPark.Variant
//
// Copyright Michael Park, 2015-2017
//
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file LICENSE.md or copy at http://boost.org/LICENSE_1_0.txt)

#ifndef MPARK_CONFIG_HPP
#define MPARK_CONFIG_HPP

// MSVC 2015 Update 3.
#if __cplusplus < 201103L && (!defined(_MSC_VER) || _MSC_FULL_VER < 190024210)
#error "MPark.Variant requires C++11 support."
#endif

#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

#ifndef __has_builtin
#define __has_builtin(x) 0
#endif

#ifndef __has_include
#define __has_include(x) 0
#endif

#ifndef __has_feature
#define __has_feature(x) 0
#endif

#if __has_attribute(always_inline) || defined(__GNUC__)
#define MPARK_ALWAYS_INLINE __attribute__((__always_inline__)) inline
#elif defined(_MSC_VER)
#define MPARK_ALWAYS_INLINE __forceinline
#else
#define MPARK_ALWAYS_INLINE inline
#endif

#if __has_builtin(__builtin_addressof) || \
    (defined(__GNUC__) && __GNUC__ >= 7) || defined(_MSC_VER)
#define MPARK_BUILTIN_ADDRESSOF
#endif

#if __has_builtin(__builtin_unreachable) || defined(__GNUC__)
#define MPARK_BUILTIN_UNREACHABLE __builtin_unreachable()
#elif defined(_MSC_VER)
#define MPARK_BUILTIN_UNREACHABLE __assume(false)
#else
#define MPARK_BUILTIN_UNREACHABLE
#endif

#if __has_builtin(__type_pack_element)
#define MPARK_TYPE_PACK_ELEMENT
#endif

#if defined(__cpp_constexpr) && __cpp_constexpr >= 200704 && \
    !(defined(__GNUC__) && __GNUC__ == 4 && __GNUC_MINOR__ == 9)
#define MPARK_CPP11_CONSTEXPR
#endif

#if defined(__cpp_constexpr) && __cpp_constexpr >= 201304
#define MPARK_CPP14_CONSTEXPR
#endif

#if __has_feature(cxx_exceptions) || defined(__cpp_exceptions) || \
    (defined(_MSC_VER) && defined(_CPPUNWIND))
#define MPARK_EXCEPTIONS
#endif

#if defined(__cpp_generic_lambdas) || defined(_MSC_VER)
#define MPARK_GENERIC_LAMBDAS
#endif

#if defined(__cpp_lib_integer_sequence)
#define MPARK_INTEGER_SEQUENCE
#endif

#if defined(__cpp_return_type_deduction) || defined(_MSC_VER)
#define MPARK_RETURN_TYPE_DEDUCTION
#endif

#if defined(__cpp_lib_transparent_operators) || defined(_MSC_VER)
#define MPARK_TRANSPARENT_OPERATORS
#endif

#if defined(__cpp_variable_templates) || defined(_MSC_VER)
#define MPARK_VARIABLE_TEMPLATES
#endif

#if !defined(__GLIBCXX__) || __has_include(<codecvt>)  // >= libstdc++-5
#define MPARK_TRIVIALITY_TYPE_TRAITS
#define MPARK_INCOMPLETE_TYPE_TRAITS
#endif

#endif  // MPARK_CONFIG_HPP
