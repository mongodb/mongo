//
// detail/config.hpp
// ~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2025 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BOOST_ASIO_DETAIL_CONFIG_HPP
#define BOOST_ASIO_DETAIL_CONFIG_HPP

#if defined(BOOST_ASIO_STANDALONE)
# define BOOST_ASIO_DISABLE_BOOST_ALIGN 1
# define BOOST_ASIO_DISABLE_BOOST_ARRAY 1
# define BOOST_ASIO_DISABLE_BOOST_ASSERT 1
# define BOOST_ASIO_DISABLE_BOOST_BIND 1
# define BOOST_ASIO_DISABLE_BOOST_CHRONO 1
# define BOOST_ASIO_DISABLE_BOOST_DATE_TIME 1
# define BOOST_ASIO_DISABLE_BOOST_LIMITS 1
# define BOOST_ASIO_DISABLE_BOOST_REGEX 1
# define BOOST_ASIO_DISABLE_BOOST_STATIC_CONSTANT 1
# define BOOST_ASIO_DISABLE_BOOST_THROW_EXCEPTION 1
# define BOOST_ASIO_DISABLE_BOOST_WORKAROUND 1
#else // defined(BOOST_ASIO_STANDALONE)
// Boost.Config library is available.
# include <boost/config.hpp>
# include <boost/version.hpp>
# define BOOST_ASIO_HAS_BOOST_CONFIG 1
#endif // defined(BOOST_ASIO_STANDALONE)

// Default to a header-only implementation. The user must specifically request
// separate compilation by defining either BOOST_ASIO_SEPARATE_COMPILATION or
// BOOST_ASIO_DYN_LINK (as a DLL/shared library implies separate compilation).
#if !defined(BOOST_ASIO_HEADER_ONLY)
# if !defined(BOOST_ASIO_SEPARATE_COMPILATION)
#  if !defined(BOOST_ASIO_DYN_LINK)
#   define BOOST_ASIO_HEADER_ONLY 1
#  endif // !defined(BOOST_ASIO_DYN_LINK)
# endif // !defined(BOOST_ASIO_SEPARATE_COMPILATION)
#endif // !defined(BOOST_ASIO_HEADER_ONLY)

#if defined(BOOST_ASIO_HEADER_ONLY)
# define BOOST_ASIO_DECL inline
#else // defined(BOOST_ASIO_HEADER_ONLY)
# if defined(_MSC_VER) || defined(__BORLANDC__) || defined(__CODEGEARC__)
// We need to import/export our code only if the user has specifically asked
// for it by defining BOOST_ASIO_DYN_LINK.
#  if defined(BOOST_ASIO_DYN_LINK)
// Export if this is our own source, otherwise import.
#   if defined(BOOST_ASIO_SOURCE)
#    define BOOST_ASIO_DECL __declspec(dllexport)
#   else // defined(BOOST_ASIO_SOURCE)
#    define BOOST_ASIO_DECL __declspec(dllimport)
#   endif // defined(BOOST_ASIO_SOURCE)
#  endif // defined(BOOST_ASIO_DYN_LINK)
# endif // defined(_MSC_VER) || defined(__BORLANDC__) || defined(__CODEGEARC__)
#endif // defined(BOOST_ASIO_HEADER_ONLY)

// If BOOST_ASIO_DECL isn't defined yet define it now.
#if !defined(BOOST_ASIO_DECL)
# define BOOST_ASIO_DECL
#endif // !defined(BOOST_ASIO_DECL)

// Helper macro for documentation.
#define BOOST_ASIO_UNSPECIFIED(e) e

// Microsoft Visual C++ detection.
#if !defined(BOOST_ASIO_MSVC)
# if defined(BOOST_ASIO_HAS_BOOST_CONFIG) && defined(BOOST_MSVC)
#  define BOOST_ASIO_MSVC BOOST_MSVC
# elif defined(_MSC_VER) && (defined(__INTELLISENSE__) \
      || (!defined(__MWERKS__) && !defined(__EDG_VERSION__)))
#  define BOOST_ASIO_MSVC _MSC_VER
# endif // defined(BOOST_ASIO_HAS_BOOST_CONFIG) && defined(BOOST_MSVC)
#endif // !defined(BOOST_ASIO_MSVC)

// Clang / libc++ detection.
#if defined(__clang__)
# if (__cplusplus >= 201103)
#  if __has_include(<__config>)
#   include <__config>
#   if defined(_LIBCPP_VERSION)
#    define BOOST_ASIO_HAS_CLANG_LIBCXX 1
#   endif // defined(_LIBCPP_VERSION)
#  endif // __has_include(<__config>)
# endif // (__cplusplus >= 201103)
#endif // defined(__clang__)

// Android platform detection.
#if defined(__ANDROID__)
# include <android/api-level.h>
#endif // defined(__ANDROID__)

// Always enabled. Retained for backwards compatibility in user code.
#if !defined(BOOST_ASIO_DISABLE_CXX11_MACROS)
# define BOOST_ASIO_HAS_MOVE 1
# define BOOST_ASIO_MOVE_ARG(type) type&&
# define BOOST_ASIO_MOVE_ARG2(type1, type2) type1, type2&&
# define BOOST_ASIO_NONDEDUCED_MOVE_ARG(type) type&
# define BOOST_ASIO_MOVE_CAST(type) static_cast<type&&>
# define BOOST_ASIO_MOVE_CAST2(type1, type2) static_cast<type1, type2&&>
# define BOOST_ASIO_MOVE_OR_LVALUE(type) static_cast<type&&>
# define BOOST_ASIO_MOVE_OR_LVALUE_ARG(type) type&&
# define BOOST_ASIO_MOVE_OR_LVALUE_TYPE(type) type
# define BOOST_ASIO_DELETED = delete
# define BOOST_ASIO_HAS_VARIADIC_TEMPLATES 1
# define BOOST_ASIO_HAS_CONSTEXPR 1
# define BOOST_ASIO_STATIC_CONSTEXPR(type, assignment) \
   static constexpr type assignment
# define BOOST_ASIO_HAS_NOEXCEPT 1
# define BOOST_ASIO_NOEXCEPT noexcept(true)
# define BOOST_ASIO_NOEXCEPT_OR_NOTHROW noexcept(true)
# define BOOST_ASIO_NOEXCEPT_IF(c) noexcept(c)
# define BOOST_ASIO_HAS_DECLTYPE 1
# define BOOST_ASIO_AUTO_RETURN_TYPE_PREFIX(t) auto
# define BOOST_ASIO_AUTO_RETURN_TYPE_PREFIX2(t0, t1) auto
# define BOOST_ASIO_AUTO_RETURN_TYPE_PREFIX3(t0, t1, t2) auto
# define BOOST_ASIO_AUTO_RETURN_TYPE_SUFFIX(expr) -> decltype expr
# define BOOST_ASIO_HAS_ALIAS_TEMPLATES 1
# define BOOST_ASIO_HAS_DEFAULT_FUNCTION_TEMPLATE_ARGUMENTS 1
# define BOOST_ASIO_HAS_ENUM_CLASS 1
# define BOOST_ASIO_HAS_REF_QUALIFIED_FUNCTIONS 1
# define BOOST_ASIO_LVALUE_REF_QUAL &
# define BOOST_ASIO_RVALUE_REF_QUAL &&
# define BOOST_ASIO_HAS_USER_DEFINED_LITERALS 1
# define BOOST_ASIO_HAS_ALIGNOF 1
# define BOOST_ASIO_ALIGNOF(T) alignof(T)
# define BOOST_ASIO_HAS_STD_ALIGN 1
# define BOOST_ASIO_HAS_STD_SYSTEM_ERROR 1
# define BOOST_ASIO_ERROR_CATEGORY_NOEXCEPT noexcept(true)
# define BOOST_ASIO_HAS_STD_ARRAY 1
# define BOOST_ASIO_HAS_STD_SHARED_PTR 1
# define BOOST_ASIO_HAS_STD_ALLOCATOR_ARG 1
# define BOOST_ASIO_HAS_STD_ATOMIC 1
# define BOOST_ASIO_HAS_STD_CHRONO 1
# define BOOST_ASIO_HAS_STD_ADDRESSOF 1
# define BOOST_ASIO_HAS_STD_FUNCTION 1
# define BOOST_ASIO_HAS_STD_REFERENCE_WRAPPER 1
# define BOOST_ASIO_HAS_STD_TYPE_TRAITS 1
# define BOOST_ASIO_HAS_NULLPTR 1
# define BOOST_ASIO_HAS_CXX11_ALLOCATORS 1
# define BOOST_ASIO_HAS_CSTDINT 1
# define BOOST_ASIO_HAS_STD_THREAD 1
# define BOOST_ASIO_HAS_STD_MUTEX_AND_CONDVAR 1
# define BOOST_ASIO_HAS_STD_CALL_ONCE 1
# define BOOST_ASIO_HAS_STD_FUTURE 1
# define BOOST_ASIO_HAS_STD_TUPLE 1
# define BOOST_ASIO_HAS_STD_IOSTREAM_MOVE 1
# define BOOST_ASIO_HAS_STD_EXCEPTION_PTR 1
# define BOOST_ASIO_HAS_STD_NESTED_EXCEPTION 1
# define BOOST_ASIO_HAS_STD_HASH 1
#endif // !defined(BOOST_ASIO_DISABLE_CXX11_MACROS)

// Support for static constexpr with default initialisation.
#if !defined(BOOST_ASIO_STATIC_CONSTEXPR_DEFAULT_INIT)
# if defined(__GNUC__)
#  if (__GNUC__ >= 8)
#   define BOOST_ASIO_STATIC_CONSTEXPR_DEFAULT_INIT(type, name) \
     static constexpr const type name{}
#  else // (__GNUC__ >= 8)
#   define BOOST_ASIO_STATIC_CONSTEXPR_DEFAULT_INIT(type, name) \
     static const type name
#  endif // (__GNUC__ >= 8)
# elif defined(BOOST_ASIO_MSVC)
#  define BOOST_ASIO_STATIC_CONSTEXPR_DEFAULT_INIT(type, name) \
    static const type name
# else // defined(BOOST_ASIO_MSVC)
#  define BOOST_ASIO_STATIC_CONSTEXPR_DEFAULT_INIT(type, name) \
    static constexpr const type name{}
# endif // defined(BOOST_ASIO_MSVC)
#endif // !defined(BOOST_ASIO_STATIC_CONSTEXPR_DEFAULT_INIT)

// Support noexcept on function types on compilers known to allow it.
#if !defined(BOOST_ASIO_HAS_NOEXCEPT_FUNCTION_TYPE)
# if !defined(BOOST_ASIO_DISABLE_NOEXCEPT_FUNCTION_TYPE)
#  if defined(__clang__)
#   if (__cplusplus >= 202002)
#    define BOOST_ASIO_HAS_NOEXCEPT_FUNCTION_TYPE 1
#   endif // (__cplusplus >= 202002)
#  elif defined(__GNUC__)
#   if (__cplusplus >= 202002)
#    define BOOST_ASIO_HAS_NOEXCEPT_FUNCTION_TYPE 1
#   endif // (__cplusplus >= 202002)
#  elif defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1900 && _MSVC_LANG >= 202002)
#    define BOOST_ASIO_HAS_NOEXCEPT_FUNCTION_TYPE 1
#   endif // (_MSC_VER >= 1900 && _MSVC_LANG >= 202002)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_NOEXCEPT_FUNCTION_TYPE)
#endif // !defined(BOOST_ASIO_HAS_NOEXCEPT_FUNCTION_TYPE)

// Support return type deduction on compilers known to allow it.
#if !defined(BOOST_ASIO_HAS_RETURN_TYPE_DEDUCTION)
# if !defined(BOOST_ASIO_DISABLE_RETURN_TYPE_DEDUCTION)
#  if defined(__clang__)
#   if __has_feature(__cxx_return_type_deduction__)
#    define BOOST_ASIO_HAS_RETURN_TYPE_DEDUCTION 1
#   endif // __has_feature(__cxx_return_type_deduction__)
#  elif (__cplusplus >= 201402)
#   define BOOST_ASIO_HAS_RETURN_TYPE_DEDUCTION 1
#  elif defined(__cpp_return_type_deduction)
#   if (__cpp_return_type_deduction >= 201304)
#    define BOOST_ASIO_HAS_RETURN_TYPE_DEDUCTION 1
#   endif // (__cpp_return_type_deduction >= 201304)
#  elif defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1900 && _MSVC_LANG >= 201402)
#    define BOOST_ASIO_HAS_RETURN_TYPE_DEDUCTION 1
#   endif // (_MSC_VER >= 1900 && _MSVC_LANG >= 201402)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_RETURN_TYPE_DEDUCTION)
#endif // !defined(BOOST_ASIO_HAS_RETURN_TYPE_DEDUCTION)

// Support concepts on compilers known to allow them.
#if !defined(BOOST_ASIO_HAS_CONCEPTS)
# if !defined(BOOST_ASIO_DISABLE_CONCEPTS)
#  if defined(__cpp_concepts)
#   define BOOST_ASIO_HAS_CONCEPTS 1
#   if (__cpp_concepts >= 201707)
#    define BOOST_ASIO_CONCEPT concept
#   else // (__cpp_concepts >= 201707)
#    define BOOST_ASIO_CONCEPT concept bool
#   endif // (__cpp_concepts >= 201707)
#  endif // defined(__cpp_concepts)
# endif // !defined(BOOST_ASIO_DISABLE_CONCEPTS)
#endif // !defined(BOOST_ASIO_HAS_CONCEPTS)

// Support concepts on compilers known to allow them.
#if !defined(BOOST_ASIO_HAS_STD_CONCEPTS)
# if !defined(BOOST_ASIO_DISABLE_STD_CONCEPTS)
#  if defined(BOOST_ASIO_HAS_CONCEPTS)
#   if (__cpp_lib_concepts >= 202002L)
#    define BOOST_ASIO_HAS_STD_CONCEPTS 1
#   endif // (__cpp_concepts >= 202002L)
#  endif // defined(BOOST_ASIO_HAS_CONCEPTS)
# endif // !defined(BOOST_ASIO_DISABLE_STD_CONCEPTS)
#endif // !defined(BOOST_ASIO_HAS_STD_CONCEPTS)

// Support template variables on compilers known to allow it.
#if !defined(BOOST_ASIO_HAS_VARIABLE_TEMPLATES)
# if !defined(BOOST_ASIO_DISABLE_VARIABLE_TEMPLATES)
#  if defined(__clang__)
#   if (__cplusplus >= 201402)
#    if __has_feature(__cxx_variable_templates__)
#     define BOOST_ASIO_HAS_VARIABLE_TEMPLATES 1
#    endif // __has_feature(__cxx_variable_templates__)
#   endif // (__cplusplus >= 201402)
#  elif defined(__GNUC__) && !defined(__INTEL_COMPILER)
#   if (__GNUC__ >= 6)
#    if (__cplusplus >= 201402)
#     define BOOST_ASIO_HAS_VARIABLE_TEMPLATES 1
#    endif // (__cplusplus >= 201402)
#   endif // (__GNUC__ >= 6)
#  endif // defined(__GNUC__) && !defined(__INTEL_COMPILER)
#  if defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1901)
#    define BOOST_ASIO_HAS_VARIABLE_TEMPLATES 1
#   endif // (_MSC_VER >= 1901)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_VARIABLE_TEMPLATES)
#endif // !defined(BOOST_ASIO_HAS_VARIABLE_TEMPLATES)

// Support SFINAEd template variables on compilers known to allow it.
#if !defined(BOOST_ASIO_HAS_SFINAE_VARIABLE_TEMPLATES)
# if !defined(BOOST_ASIO_DISABLE_SFINAE_VARIABLE_TEMPLATES)
#  if defined(__clang__)
#   if (__cplusplus >= 201703)
#    if __has_feature(__cxx_variable_templates__)
#     define BOOST_ASIO_HAS_SFINAE_VARIABLE_TEMPLATES 1
#    endif // __has_feature(__cxx_variable_templates__)
#   endif // (__cplusplus >= 201703)
#  elif defined(__GNUC__)
#   if ((__GNUC__ == 8) && (__GNUC_MINOR__ >= 4)) || (__GNUC__ > 8)
#    if (__cplusplus >= 201402)
#     define BOOST_ASIO_HAS_SFINAE_VARIABLE_TEMPLATES 1
#    endif // (__cplusplus >= 201402)
#   endif // ((__GNUC__ == 8) && (__GNUC_MINOR__ >= 4)) || (__GNUC__ > 8)
#  endif // defined(__GNUC__)
#  if defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1901)
#    define BOOST_ASIO_HAS_SFINAE_VARIABLE_TEMPLATES 1
#   endif // (_MSC_VER >= 1901)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_SFINAE_VARIABLE_TEMPLATES)
#endif // !defined(BOOST_ASIO_HAS_SFINAE_VARIABLE_TEMPLATES)

// Support SFINAE use of constant expressions on compilers known to allow it.
#if !defined(BOOST_ASIO_HAS_CONSTANT_EXPRESSION_SFINAE)
# if !defined(BOOST_ASIO_DISABLE_CONSTANT_EXPRESSION_SFINAE)
#  if defined(__clang__)
#   if (__cplusplus >= 201402)
#    define BOOST_ASIO_HAS_CONSTANT_EXPRESSION_SFINAE 1
#   endif // (__cplusplus >= 201402)
#  elif defined(__GNUC__) && !defined(__INTEL_COMPILER)
#   if (__GNUC__ >= 7)
#    if (__cplusplus >= 201402)
#     define BOOST_ASIO_HAS_CONSTANT_EXPRESSION_SFINAE 1
#    endif // (__cplusplus >= 201402)
#   endif // (__GNUC__ >= 7)
#  endif // defined(__GNUC__) && !defined(__INTEL_COMPILER)
#  if defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1901)
#    define BOOST_ASIO_HAS_CONSTANT_EXPRESSION_SFINAE 1
#   endif // (_MSC_VER >= 1901)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_CONSTANT_EXPRESSION_SFINAE)
#endif // !defined(BOOST_ASIO_HAS_CONSTANT_EXPRESSION_SFINAE)

// Enable workarounds for lack of working expression SFINAE.
#if !defined(BOOST_ASIO_HAS_WORKING_EXPRESSION_SFINAE)
# if !defined(BOOST_ASIO_DISABLE_WORKING_EXPRESSION_SFINAE)
#  if !defined(BOOST_ASIO_MSVC) && !defined(__INTEL_COMPILER)
#   if (__cplusplus >= 201103)
#    define BOOST_ASIO_HAS_WORKING_EXPRESSION_SFINAE 1
#   endif // (__cplusplus >= 201103)
#  elif defined(BOOST_ASIO_MSVC) && (_MSC_VER >= 1929)
#   if (_MSVC_LANG >= 202000)
#    define BOOST_ASIO_HAS_WORKING_EXPRESSION_SFINAE 1
#   endif // (_MSVC_LANG >= 202000)
#  endif // defined(BOOST_ASIO_MSVC) && (_MSC_VER >= 1929)
# endif // !defined(BOOST_ASIO_DISABLE_WORKING_EXPRESSION_SFINAE)
#endif // !defined(BOOST_ASIO_HAS_WORKING_EXPRESSION_SFINAE)

// Support for capturing parameter packs in lambdas.
#if !defined(BOOST_ASIO_HAS_VARIADIC_LAMBDA_CAPTURES)
# if !defined(BOOST_ASIO_DISABLE_VARIADIC_LAMBDA_CAPTURES)
#  if defined(__GNUC__)
#   if (__GNUC__ >= 6)
#    define BOOST_ASIO_HAS_VARIADIC_LAMBDA_CAPTURES 1
#   endif // (__GNUC__ >= 6)
#  elif defined(BOOST_ASIO_MSVC)
#   if (_MSVC_LANG >= 201103)
#    define BOOST_ASIO_HAS_VARIADIC_LAMBDA_CAPTURES 1
#   endif // (_MSC_LANG >= 201103)
#  else // defined(BOOST_ASIO_MSVC)
#   if (__cplusplus >= 201103)
#    define BOOST_ASIO_HAS_VARIADIC_LAMBDA_CAPTURES 1
#   endif // (__cplusplus >= 201103)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_VARIADIC_LAMBDA_CAPTURES)
#endif // !defined(BOOST_ASIO_HAS_VARIADIC_LAMBDA_CAPTURES)

// Support for inline variables.
#if !defined(BOOST_ASIO_HAS_INLINE_VARIABLES)
# if !defined(BOOST_ASIO_DISABLE_INLINE_VARIABLES)
#  if (__cplusplus >= 201703) && (__cpp_inline_variables >= 201606)
#   define BOOST_ASIO_HAS_INLINE_VARIABLES 1
#   define BOOST_ASIO_INLINE_VARIABLE inline
#  endif // (__cplusplus >= 201703) && (__cpp_inline_variables >= 201606)
# endif // !defined(BOOST_ASIO_DISABLE_INLINE_VARIABLES)
#endif // !defined(BOOST_ASIO_HAS_INLINE_VARIABLES)
#if !defined(BOOST_ASIO_INLINE_VARIABLE)
# define BOOST_ASIO_INLINE_VARIABLE
#endif // !defined(BOOST_ASIO_INLINE_VARIABLE)

// Default alignment.
#if defined(__STDCPP_DEFAULT_NEW_ALIGNMENT__)
# define BOOST_ASIO_DEFAULT_ALIGN __STDCPP_DEFAULT_NEW_ALIGNMENT__
#elif defined(__GNUC__)
# if ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 9)) || (__GNUC__ > 4)
#  define BOOST_ASIO_DEFAULT_ALIGN alignof(std::max_align_t)
# else // ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 9)) || (__GNUC__ > 4)
#  define BOOST_ASIO_DEFAULT_ALIGN alignof(max_align_t)
# endif // ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 9)) || (__GNUC__ > 4)
#else // defined(__GNUC__)
# define BOOST_ASIO_DEFAULT_ALIGN alignof(std::max_align_t)
#endif // defined(__GNUC__)

// Standard library support for aligned allocation.
#if !defined(BOOST_ASIO_HAS_STD_ALIGNED_ALLOC)
# if !defined(BOOST_ASIO_DISABLE_STD_ALIGNED_ALLOC)
#  if (__cplusplus >= 201703)
#   if defined(__clang__)
#    if defined(BOOST_ASIO_HAS_CLANG_LIBCXX)
#     if (_LIBCPP_STD_VER > 14) && defined(_LIBCPP_HAS_ALIGNED_ALLOC) \
        && !defined(_LIBCPP_MSVCRT) && !defined(__MINGW32__)
#      if defined(__ANDROID__) && (__ANDROID_API__ >= 28)
#        define BOOST_ASIO_HAS_STD_ALIGNED_ALLOC 1
#      elif defined(__APPLE__)
#       if defined(__MAC_OS_X_VERSION_MIN_REQUIRED)
#        if (__MAC_OS_X_VERSION_MIN_REQUIRED >= 101500)
#         define BOOST_ASIO_HAS_STD_ALIGNED_ALLOC 1
#        endif // (__MAC_OS_X_VERSION_MIN_REQUIRED >= 101500)
#       elif defined(__IPHONE_OS_VERSION_MIN_REQUIRED)
#        if (__IPHONE_OS_VERSION_MIN_REQUIRED >= 130000)
#         define BOOST_ASIO_HAS_STD_ALIGNED_ALLOC 1
#        endif // (__IPHONE_OS_VERSION_MIN_REQUIRED >= 130000)
#       elif defined(__TV_OS_VERSION_MIN_REQUIRED)
#        if (__TV_OS_VERSION_MIN_REQUIRED >= 130000)
#         define BOOST_ASIO_HAS_STD_ALIGNED_ALLOC 1
#        endif // (__TV_OS_VERSION_MIN_REQUIRED >= 130000)
#       elif defined(__WATCH_OS_VERSION_MIN_REQUIRED)
#        if (__WATCH_OS_VERSION_MIN_REQUIRED >= 60000)
#         define BOOST_ASIO_HAS_STD_ALIGNED_ALLOC 1
#        endif // (__WATCH_OS_VERSION_MIN_REQUIRED >= 60000)
#       endif // defined(__WATCH_OS_X_VERSION_MIN_REQUIRED)
#      else // defined(__APPLE__)
#       define BOOST_ASIO_HAS_STD_ALIGNED_ALLOC 1
#      endif // defined(__APPLE__)
#     endif // (_LIBCPP_STD_VER > 14) && defined(_LIBCPP_HAS_ALIGNED_ALLOC)
            //   && !defined(_LIBCPP_MSVCRT) && !defined(__MINGW32__)
#    elif defined(_GLIBCXX_HAVE_ALIGNED_ALLOC)
#     define BOOST_ASIO_HAS_STD_ALIGNED_ALLOC 1
#    endif // defined(_GLIBCXX_HAVE_ALIGNED_ALLOC)
#   elif defined(__GNUC__)
#    if ((__GNUC__ == 7) && (__GNUC_MINOR__ >= 4)) || (__GNUC__ > 7)
#     if defined(_GLIBCXX_HAVE_ALIGNED_ALLOC)
#      define BOOST_ASIO_HAS_STD_ALIGNED_ALLOC 1
#     endif // defined(_GLIBCXX_HAVE_ALIGNED_ALLOC)
#    endif // ((__GNUC__ == 7) && (__GNUC_MINOR__ >= 4)) || (__GNUC__ > 7)
#   endif // defined(__GNUC__)
#  endif // (__cplusplus >= 201703)
# endif // !defined(BOOST_ASIO_DISABLE_STD_ALIGNED_ALLOC)
#endif // !defined(BOOST_ASIO_HAS_STD_ALIGNED_ALLOC)

// Boost support for chrono.
#if !defined(BOOST_ASIO_HAS_BOOST_CHRONO)
# if !defined(BOOST_ASIO_DISABLE_BOOST_CHRONO)
#  if defined(BOOST_ASIO_HAS_BOOST_CONFIG) && (BOOST_VERSION >= 104700)
#   define BOOST_ASIO_HAS_BOOST_CHRONO 1
#  endif // defined(BOOST_ASIO_HAS_BOOST_CONFIG) && (BOOST_VERSION >= 104700)
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_CHRONO)
#endif // !defined(BOOST_ASIO_HAS_BOOST_CHRONO)

// Some form of chrono library is available.
#if !defined(BOOST_ASIO_HAS_CHRONO)
# if defined(BOOST_ASIO_HAS_STD_CHRONO) \
    || defined(BOOST_ASIO_HAS_BOOST_CHRONO)
#  define BOOST_ASIO_HAS_CHRONO 1
# endif // defined(BOOST_ASIO_HAS_STD_CHRONO)
        // || defined(BOOST_ASIO_HAS_BOOST_CHRONO)
#endif // !defined(BOOST_ASIO_HAS_CHRONO)

// Boost support for the DateTime library.
#if !defined(BOOST_ASIO_HAS_BOOST_DATE_TIME)
# if !defined(BOOST_ASIO_DISABLE_BOOST_DATE_TIME)
#  define BOOST_ASIO_HAS_BOOST_DATE_TIME 1
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_DATE_TIME)
#endif // !defined(BOOST_ASIO_HAS_BOOST_DATE_TIME)

// Boost support for the Context library's fibers.
#if !defined(BOOST_ASIO_HAS_BOOST_CONTEXT_FIBER)
# if !defined(BOOST_ASIO_DISABLE_BOOST_CONTEXT_FIBER)
#  if defined(__clang__)
#   if (__cplusplus >= 201103)
#    define BOOST_ASIO_HAS_BOOST_CONTEXT_FIBER 1
#   endif // (__cplusplus >= 201103)
#  elif defined(__GNUC__)
#   if ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ > 4)
#    if (__cplusplus >= 201103) || defined(__GXX_EXPERIMENTAL_CXX0X__)
#     define BOOST_ASIO_HAS_BOOST_CONTEXT_FIBER 1
#    endif // (__cplusplus >= 201103) || defined(__GXX_EXPERIMENTAL_CXX0X__)
#   endif // ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ > 4)
#  endif // defined(__GNUC__)
#  if defined(BOOST_ASIO_MSVC)
#   if (_MSVC_LANG >= 201103)
#    define BOOST_ASIO_HAS_BOOST_CONTEXT_FIBER 1
#   endif // (_MSC_LANG >= 201103)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_CONTEXT_FIBER)
#endif // !defined(BOOST_ASIO_HAS_BOOST_CONTEXT_FIBER)

// Standard library support for std::string_view.
#if !defined(BOOST_ASIO_HAS_STD_STRING_VIEW)
# if !defined(BOOST_ASIO_DISABLE_STD_STRING_VIEW)
#  if defined(__clang__)
#   if defined(BOOST_ASIO_HAS_CLANG_LIBCXX)
#    if (__cplusplus >= 201402)
#     if __has_include(<string_view>)
#      define BOOST_ASIO_HAS_STD_STRING_VIEW 1
#     endif // __has_include(<string_view>)
#    endif // (__cplusplus >= 201402)
#   else // defined(BOOST_ASIO_HAS_CLANG_LIBCXX)
#    if (__cplusplus >= 201703)
#     if __has_include(<string_view>)
#      define BOOST_ASIO_HAS_STD_STRING_VIEW 1
#     endif // __has_include(<string_view>)
#    endif // (__cplusplus >= 201703)
#   endif // defined(BOOST_ASIO_HAS_CLANG_LIBCXX)
#  elif defined(__GNUC__)
#   if (__GNUC__ >= 7)
#    if (__cplusplus >= 201703)
#     define BOOST_ASIO_HAS_STD_STRING_VIEW 1
#    endif // (__cplusplus >= 201703)
#   endif // (__GNUC__ >= 7)
#  elif defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1910 && _MSVC_LANG >= 201703)
#    define BOOST_ASIO_HAS_STD_STRING_VIEW 1
#   endif // (_MSC_VER >= 1910 && _MSVC_LANG >= 201703)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_STD_STRING_VIEW)
#endif // !defined(BOOST_ASIO_HAS_STD_STRING_VIEW)

// Standard library support for std::experimental::string_view.
#if !defined(BOOST_ASIO_HAS_STD_EXPERIMENTAL_STRING_VIEW)
# if !defined(BOOST_ASIO_DISABLE_STD_EXPERIMENTAL_STRING_VIEW)
#  if defined(__clang__)
#   if defined(BOOST_ASIO_HAS_CLANG_LIBCXX)
#    if (_LIBCPP_VERSION < 7000)
#     if (__cplusplus >= 201402)
#      if __has_include(<experimental/string_view>)
#       define BOOST_ASIO_HAS_STD_EXPERIMENTAL_STRING_VIEW 1
#      endif // __has_include(<experimental/string_view>)
#     endif // (__cplusplus >= 201402)
#    endif // (_LIBCPP_VERSION < 7000)
#   else // defined(BOOST_ASIO_HAS_CLANG_LIBCXX)
#    if (__cplusplus >= 201402)
#     if __has_include(<experimental/string_view>)
#      define BOOST_ASIO_HAS_STD_EXPERIMENTAL_STRING_VIEW 1
#     endif // __has_include(<experimental/string_view>)
#    endif // (__cplusplus >= 201402)
#   endif // // defined(BOOST_ASIO_HAS_CLANG_LIBCXX)
#  elif defined(__GNUC__)
#   if ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 9)) || (__GNUC__ > 4)
#    if (__cplusplus >= 201402)
#     define BOOST_ASIO_HAS_STD_EXPERIMENTAL_STRING_VIEW 1
#    endif // (__cplusplus >= 201402)
#   endif // ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 9)) || (__GNUC__ > 4)
#  endif // defined(__GNUC__)
# endif // !defined(BOOST_ASIO_DISABLE_STD_EXPERIMENTAL_STRING_VIEW)
#endif // !defined(BOOST_ASIO_HAS_STD_EXPERIMENTAL_STRING_VIEW)

// Standard library has a string_view that we can use.
#if !defined(BOOST_ASIO_HAS_STRING_VIEW)
# if !defined(BOOST_ASIO_DISABLE_STRING_VIEW)
#  if defined(BOOST_ASIO_HAS_STD_STRING_VIEW)
#   define BOOST_ASIO_HAS_STRING_VIEW 1
#  elif defined(BOOST_ASIO_HAS_STD_EXPERIMENTAL_STRING_VIEW)
#   define BOOST_ASIO_HAS_STRING_VIEW 1
#  endif // defined(BOOST_ASIO_HAS_STD_EXPERIMENTAL_STRING_VIEW)
# endif // !defined(BOOST_ASIO_DISABLE_STRING_VIEW)
#endif // !defined(BOOST_ASIO_HAS_STRING_VIEW)

// Standard library has invoke_result (which supersedes result_of).
#if !defined(BOOST_ASIO_HAS_STD_INVOKE_RESULT)
# if !defined(BOOST_ASIO_DISABLE_STD_INVOKE_RESULT)
#  if defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1911 && _MSVC_LANG >= 201703)
#    define BOOST_ASIO_HAS_STD_INVOKE_RESULT 1
#   endif // (_MSC_VER >= 1911 && _MSVC_LANG >= 201703)
#  else // defined(BOOST_ASIO_MSVC)
#   if (__cplusplus >= 201703) && (__cpp_lib_is_invocable >= 201703)
#    define BOOST_ASIO_HAS_STD_INVOKE_RESULT 1
#   endif // (__cplusplus >= 201703) && (__cpp_lib_is_invocable >= 201703)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_STD_INVOKE_RESULT)
#endif // !defined(BOOST_ASIO_HAS_STD_INVOKE_RESULT)

// Standard library support for std::any.
#if !defined(BOOST_ASIO_HAS_STD_ANY)
# if !defined(BOOST_ASIO_DISABLE_STD_ANY)
#  if defined(__clang__)
#   if (__cplusplus >= 201703)
#    if __has_include(<any>)
#     define BOOST_ASIO_HAS_STD_ANY 1
#    endif // __has_include(<any>)
#   endif // (__cplusplus >= 201703)
#  elif defined(__GNUC__)
#   if (__GNUC__ >= 7)
#    if (__cplusplus >= 201703)
#     define BOOST_ASIO_HAS_STD_ANY 1
#    endif // (__cplusplus >= 201703)
#   endif // (__GNUC__ >= 7)
#  endif // defined(__GNUC__)
#  if defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1910) && (_MSVC_LANG >= 201703)
#    define BOOST_ASIO_HAS_STD_ANY 1
#   endif // (_MSC_VER >= 1910) && (_MSVC_LANG >= 201703)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_STD_ANY)
#endif // !defined(BOOST_ASIO_HAS_STD_ANY)

// Standard library support for std::variant.
#if !defined(BOOST_ASIO_HAS_STD_VARIANT)
# if !defined(BOOST_ASIO_DISABLE_STD_VARIANT)
#  if defined(__clang__)
#   if (__cplusplus >= 201703)
#    if __has_include(<variant>)
#     define BOOST_ASIO_HAS_STD_VARIANT 1
#    endif // __has_include(<variant>)
#   endif // (__cplusplus >= 201703)
#  elif defined(__GNUC__)
#   if (__GNUC__ >= 7)
#    if (__cplusplus >= 201703)
#     define BOOST_ASIO_HAS_STD_VARIANT 1
#    endif // (__cplusplus >= 201703)
#   endif // (__GNUC__ >= 7)
#  endif // defined(__GNUC__)
#  if defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1910) && (_MSVC_LANG >= 201703)
#    define BOOST_ASIO_HAS_STD_VARIANT 1
#   endif // (_MSC_VER >= 1910) && (_MSVC_LANG >= 201703)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_STD_VARIANT)
#endif // !defined(BOOST_ASIO_HAS_STD_VARIANT)

// Standard library support for std::source_location.
#if !defined(BOOST_ASIO_HAS_STD_SOURCE_LOCATION)
# if !defined(BOOST_ASIO_DISABLE_STD_SOURCE_LOCATION)
// ...
# endif // !defined(BOOST_ASIO_DISABLE_STD_SOURCE_LOCATION)
#endif // !defined(BOOST_ASIO_HAS_STD_SOURCE_LOCATION)

// Standard library support for std::experimental::source_location.
#if !defined(BOOST_ASIO_HAS_STD_EXPERIMENTAL_SOURCE_LOCATION)
# if !defined(BOOST_ASIO_DISABLE_STD_EXPERIMENTAL_SOURCE_LOCATION)
#  if defined(__GNUC__)
#   if (__cplusplus >= 201709)
#    if __has_include(<experimental/source_location>)
#     define BOOST_ASIO_HAS_STD_EXPERIMENTAL_SOURCE_LOCATION 1
#    endif // __has_include(<experimental/source_location>)
#   endif // (__cplusplus >= 201709)
#  endif // defined(__GNUC__)
# endif // !defined(BOOST_ASIO_DISABLE_STD_EXPERIMENTAL_SOURCE_LOCATION)
#endif // !defined(BOOST_ASIO_HAS_STD_EXPERIMENTAL_SOURCE_LOCATION)

// Standard library has a source_location that we can use.
#if !defined(BOOST_ASIO_HAS_SOURCE_LOCATION)
# if !defined(BOOST_ASIO_DISABLE_SOURCE_LOCATION)
#  if defined(BOOST_ASIO_HAS_STD_SOURCE_LOCATION)
#   define BOOST_ASIO_HAS_SOURCE_LOCATION 1
#  elif defined(BOOST_ASIO_HAS_STD_EXPERIMENTAL_SOURCE_LOCATION)
#   define BOOST_ASIO_HAS_SOURCE_LOCATION 1
#  endif // defined(BOOST_ASIO_HAS_STD_EXPERIMENTAL_SOURCE_LOCATION)
# endif // !defined(BOOST_ASIO_DISABLE_SOURCE_LOCATION)
#endif // !defined(BOOST_ASIO_HAS_SOURCE_LOCATION)

// Boost support for source_location and system errors.
#if !defined(BOOST_ASIO_HAS_BOOST_SOURCE_LOCATION)
# if !defined(BOOST_ASIO_DISABLE_BOOST_SOURCE_LOCATION)
#  if defined(BOOST_ASIO_HAS_BOOST_CONFIG) && (BOOST_VERSION >= 107900)
#   define BOOST_ASIO_HAS_BOOST_SOURCE_LOCATION 1
#  endif // defined(BOOST_ASIO_HAS_BOOST_CONFIG) && (BOOST_VERSION >= 107900)
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_SOURCE_LOCATION)
#endif // !defined(BOOST_ASIO_HAS_BOOST_SOURCE_LOCATION)

// Helper macros for working with Boost source locations.
#if defined(BOOST_ASIO_HAS_BOOST_SOURCE_LOCATION)
# define BOOST_ASIO_SOURCE_LOCATION_PARAM \
  , const boost::source_location& loc
# define BOOST_ASIO_SOURCE_LOCATION_DEFAULTED_PARAM \
  , const boost::source_location& loc = BOOST_CURRENT_LOCATION
# define BOOST_ASIO_SOURCE_LOCATION_ARG , loc
#else // if defined(BOOST_ASIO_HAS_BOOST_SOURCE_LOCATION)
# define BOOST_ASIO_SOURCE_LOCATION_PARAM
# define BOOST_ASIO_SOURCE_LOCATION_DEFAULTED_PARAM
# define BOOST_ASIO_SOURCE_LOCATION_ARG
#endif // if defined(BOOST_ASIO_HAS_BOOST_SOURCE_LOCATION)

// Standard library support for std::index_sequence.
#if !defined(BOOST_ASIO_HAS_STD_INDEX_SEQUENCE)
# if !defined(BOOST_ASIO_DISABLE_STD_INDEX_SEQUENCE)
#  if defined(__clang__)
#   if (__cplusplus >= 201402)
#    define BOOST_ASIO_HAS_STD_INDEX_SEQUENCE 1
#   endif // (__cplusplus >= 201402)
#  elif defined(__GNUC__)
#   if (__GNUC__ >= 7)
#    if (__cplusplus >= 201402)
#     define BOOST_ASIO_HAS_STD_INDEX_SEQUENCE 1
#    endif // (__cplusplus >= 201402)
#   endif // (__GNUC__ >= 7)
#  endif // defined(__GNUC__)
#  if defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1910) && (_MSVC_LANG >= 201402)
#    define BOOST_ASIO_HAS_STD_INDEX_SEQUENCE 1
#   endif // (_MSC_VER >= 1910) && (_MSVC_LANG >= 201402)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_STD_INDEX_SEQUENCE)
#endif // !defined(BOOST_ASIO_HAS_STD_INDEX_SEQUENCE)

// Windows App target. Windows but with a limited API.
#if !defined(BOOST_ASIO_WINDOWS_APP)
# if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0603)
#  include <winapifamily.h>
#  if (WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) \
       || WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_TV_TITLE)) \
   && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#   define BOOST_ASIO_WINDOWS_APP 1
#  endif // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP)
         // && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
# endif // defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0603)
#endif // !defined(BOOST_ASIO_WINDOWS_APP)

// Legacy WinRT target. Windows App is preferred.
#if !defined(BOOST_ASIO_WINDOWS_RUNTIME)
# if !defined(BOOST_ASIO_WINDOWS_APP)
#  if defined(__cplusplus_winrt)
#   include <winapifamily.h>
#   if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP) \
    && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#    define BOOST_ASIO_WINDOWS_RUNTIME 1
#   endif // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP)
          // && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#  endif // defined(__cplusplus_winrt)
# endif // !defined(BOOST_ASIO_WINDOWS_APP)
#endif // !defined(BOOST_ASIO_WINDOWS_RUNTIME)

// Windows target. Excludes WinRT but includes Windows App targets.
#if !defined(BOOST_ASIO_WINDOWS)
# if !defined(BOOST_ASIO_WINDOWS_RUNTIME)
#  if defined(BOOST_ASIO_HAS_BOOST_CONFIG) && defined(BOOST_WINDOWS)
#   define BOOST_ASIO_WINDOWS 1
#  elif defined(WIN32) || defined(_WIN32) || defined(__WIN32__)
#   define BOOST_ASIO_WINDOWS 1
#  elif defined(BOOST_ASIO_WINDOWS_APP)
#   define BOOST_ASIO_WINDOWS 1
#  endif // defined(BOOST_ASIO_HAS_BOOST_CONFIG) && defined(BOOST_WINDOWS)
# endif // !defined(BOOST_ASIO_WINDOWS_RUNTIME)
#endif // !defined(BOOST_ASIO_WINDOWS)

// Windows: target OS version.
#if defined(BOOST_ASIO_WINDOWS) || defined(__CYGWIN__)
# if !defined(_WIN32_WINNT) && !defined(_WIN32_WINDOWS)
#  if defined(_MSC_VER) || (defined(__BORLANDC__) && !defined(__clang__))
#   pragma message( \
  "Please define _WIN32_WINNT or _WIN32_WINDOWS appropriately. For example:\n"\
  "- add -D_WIN32_WINNT=0x0601 to the compiler command line; or\n"\
  "- add _WIN32_WINNT=0x0601 to your project's Preprocessor Definitions.\n"\
  "Assuming _WIN32_WINNT=0x0601 (i.e. Windows 7 target).")
#  else // defined(_MSC_VER) || (defined(__BORLANDC__) && !defined(__clang__))
#   warning Please define _WIN32_WINNT or _WIN32_WINDOWS appropriately.
#   warning For example, add -D_WIN32_WINNT=0x0601 to the compiler command line.
#   warning Assuming _WIN32_WINNT=0x0601 (i.e. Windows 7 target).
#  endif // defined(_MSC_VER) || (defined(__BORLANDC__) && !defined(__clang__))
#  define _WIN32_WINNT 0x0601
# endif // !defined(_WIN32_WINNT) && !defined(_WIN32_WINDOWS)
# if defined(_MSC_VER)
#  if defined(_WIN32) && !defined(WIN32)
#   if !defined(_WINSOCK2API_)
#    define WIN32 // Needed for correct types in winsock2.h
#   else // !defined(_WINSOCK2API_)
#    error Please define the macro WIN32 in your compiler options
#   endif // !defined(_WINSOCK2API_)
#  endif // defined(_WIN32) && !defined(WIN32)
# endif // defined(_MSC_VER)
# if defined(__BORLANDC__)
#  if defined(__WIN32__) && !defined(WIN32)
#   if !defined(_WINSOCK2API_)
#    define WIN32 // Needed for correct types in winsock2.h
#   else // !defined(_WINSOCK2API_)
#    error Please define the macro WIN32 in your compiler options
#   endif // !defined(_WINSOCK2API_)
#  endif // defined(__WIN32__) && !defined(WIN32)
# endif // defined(__BORLANDC__)
# if defined(__CYGWIN__)
#  if !defined(__USE_W32_SOCKETS)
#   error You must add -D__USE_W32_SOCKETS to your compiler options.
#  endif // !defined(__USE_W32_SOCKETS)
# endif // defined(__CYGWIN__)
#endif // defined(BOOST_ASIO_WINDOWS) || defined(__CYGWIN__)

// Windows: minimise header inclusion.
#if defined(BOOST_ASIO_WINDOWS) || defined(__CYGWIN__)
# if !defined(BOOST_ASIO_NO_WIN32_LEAN_AND_MEAN)
#  if !defined(WIN32_LEAN_AND_MEAN)
#   define WIN32_LEAN_AND_MEAN
#  endif // !defined(WIN32_LEAN_AND_MEAN)
# endif // !defined(BOOST_ASIO_NO_WIN32_LEAN_AND_MEAN)
#endif // defined(BOOST_ASIO_WINDOWS) || defined(__CYGWIN__)

// Windows: suppress definition of "min" and "max" macros.
#if defined(BOOST_ASIO_WINDOWS) || defined(__CYGWIN__)
# if !defined(BOOST_ASIO_NO_NOMINMAX)
#  if !defined(NOMINMAX)
#   define NOMINMAX 1
#  endif // !defined(NOMINMAX)
# endif // !defined(BOOST_ASIO_NO_NOMINMAX)
#endif // defined(BOOST_ASIO_WINDOWS) || defined(__CYGWIN__)

// Windows: IO Completion Ports.
#if !defined(BOOST_ASIO_HAS_IOCP)
# if defined(BOOST_ASIO_WINDOWS) || defined(__CYGWIN__)
#  if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0400)
#   if !defined(UNDER_CE) && !defined(BOOST_ASIO_WINDOWS_APP)
#    if !defined(BOOST_ASIO_DISABLE_IOCP)
#     define BOOST_ASIO_HAS_IOCP 1
#    endif // !defined(BOOST_ASIO_DISABLE_IOCP)
#   endif // !defined(UNDER_CE) && !defined(BOOST_ASIO_WINDOWS_APP)
#  endif // defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0400)
# endif // defined(BOOST_ASIO_WINDOWS) || defined(__CYGWIN__)
#endif // !defined(BOOST_ASIO_HAS_IOCP)

// On POSIX (and POSIX-like) platforms we need to include unistd.h in order to
// get access to the various platform feature macros, e.g. to be able to test
// for threads support.
#if !defined(BOOST_ASIO_HAS_UNISTD_H)
# if !defined(BOOST_ASIO_HAS_BOOST_CONFIG)
#  if defined(unix) \
   || defined(__unix) \
   || defined(_XOPEN_SOURCE) \
   || defined(_POSIX_SOURCE) \
   || (defined(__MACH__) && defined(__APPLE__)) \
   || defined(__FreeBSD__) \
   || defined(__NetBSD__) \
   || defined(__OpenBSD__) \
   || defined(__linux__) \
   || defined(__HAIKU__)
#   define BOOST_ASIO_HAS_UNISTD_H 1
#  endif
# endif // !defined(BOOST_ASIO_HAS_BOOST_CONFIG)
#endif // !defined(BOOST_ASIO_HAS_UNISTD_H)
#if defined(BOOST_ASIO_HAS_UNISTD_H)
# include <unistd.h>
#endif // defined(BOOST_ASIO_HAS_UNISTD_H)

// Linux: epoll, eventfd, timerfd and io_uring.
#if defined(__linux__)
# include <linux/version.h>
# if !defined(BOOST_ASIO_HAS_EPOLL)
#  if !defined(BOOST_ASIO_DISABLE_EPOLL)
#   if LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,45)
#    define BOOST_ASIO_HAS_EPOLL 1
#   endif // LINUX_VERSION_CODE >= KERNEL_VERSION(2,5,45)
#  endif // !defined(BOOST_ASIO_DISABLE_EPOLL)
# endif // !defined(BOOST_ASIO_HAS_EPOLL)
# if !defined(BOOST_ASIO_HAS_EVENTFD)
#  if !defined(BOOST_ASIO_DISABLE_EVENTFD)
#   if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
#    define BOOST_ASIO_HAS_EVENTFD 1
#   endif // LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,22)
#  endif // !defined(BOOST_ASIO_DISABLE_EVENTFD)
# endif // !defined(BOOST_ASIO_HAS_EVENTFD)
# if !defined(BOOST_ASIO_HAS_TIMERFD)
#  if defined(BOOST_ASIO_HAS_EPOLL)
#   if (__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 8)
#    define BOOST_ASIO_HAS_TIMERFD 1
#   endif // (__GLIBC__ > 2) || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 8)
#  endif // defined(BOOST_ASIO_HAS_EPOLL)
# endif // !defined(BOOST_ASIO_HAS_TIMERFD)
# if defined(BOOST_ASIO_HAS_IO_URING)
#  if LINUX_VERSION_CODE < KERNEL_VERSION(5,10,0)
#   error Linux kernel 5.10 or later is required to support io_uring
#  endif // LINUX_VERSION_CODE < KERNEL_VERSION(5,10,0)
# endif // defined(BOOST_ASIO_HAS_IO_URING)
#endif // defined(__linux__)

// Linux: io_uring is used instead of epoll.
#if !defined(BOOST_ASIO_HAS_IO_URING_AS_DEFAULT)
# if !defined(BOOST_ASIO_HAS_EPOLL) && defined(BOOST_ASIO_HAS_IO_URING)
#  define BOOST_ASIO_HAS_IO_URING_AS_DEFAULT 1
# endif // !defined(BOOST_ASIO_HAS_EPOLL) && defined(BOOST_ASIO_HAS_IO_URING)
#endif // !defined(BOOST_ASIO_HAS_IO_URING_AS_DEFAULT)

// Mac OS X, FreeBSD, NetBSD, OpenBSD: kqueue.
#if (defined(__MACH__) && defined(__APPLE__)) \
  || defined(__FreeBSD__) \
  || defined(__NetBSD__) \
  || defined(__OpenBSD__)
# if !defined(BOOST_ASIO_HAS_KQUEUE)
#  if !defined(BOOST_ASIO_DISABLE_KQUEUE)
#   define BOOST_ASIO_HAS_KQUEUE 1
#  endif // !defined(BOOST_ASIO_DISABLE_KQUEUE)
# endif // !defined(BOOST_ASIO_HAS_KQUEUE)
#endif // (defined(__MACH__) && defined(__APPLE__))
       //   || defined(__FreeBSD__)
       //   || defined(__NetBSD__)
       //   || defined(__OpenBSD__)

// Solaris: /dev/poll.
#if defined(__sun)
# if !defined(BOOST_ASIO_HAS_DEV_POLL)
#  if !defined(BOOST_ASIO_DISABLE_DEV_POLL)
#   define BOOST_ASIO_HAS_DEV_POLL 1
#  endif // !defined(BOOST_ASIO_DISABLE_DEV_POLL)
# endif // !defined(BOOST_ASIO_HAS_DEV_POLL)
#endif // defined(__sun)

// Serial ports.
#if !defined(BOOST_ASIO_HAS_SERIAL_PORT)
# if defined(BOOST_ASIO_HAS_IOCP) \
  || !defined(BOOST_ASIO_WINDOWS) \
  && !defined(BOOST_ASIO_WINDOWS_RUNTIME) \
  && !defined(__CYGWIN__)
#  if !defined(__SYMBIAN32__)
#   if !defined(BOOST_ASIO_DISABLE_SERIAL_PORT)
#    define BOOST_ASIO_HAS_SERIAL_PORT 1
#   endif // !defined(BOOST_ASIO_DISABLE_SERIAL_PORT)
#  endif // !defined(__SYMBIAN32__)
# endif // defined(BOOST_ASIO_HAS_IOCP)
        //   || !defined(BOOST_ASIO_WINDOWS)
        //   && !defined(BOOST_ASIO_WINDOWS_RUNTIME)
        //   && !defined(__CYGWIN__)
#endif // !defined(BOOST_ASIO_HAS_SERIAL_PORT)

// Windows: stream handles.
#if !defined(BOOST_ASIO_HAS_WINDOWS_STREAM_HANDLE)
# if !defined(BOOST_ASIO_DISABLE_WINDOWS_STREAM_HANDLE)
#  if defined(BOOST_ASIO_HAS_IOCP)
#   define BOOST_ASIO_HAS_WINDOWS_STREAM_HANDLE 1
#  endif // defined(BOOST_ASIO_HAS_IOCP)
# endif // !defined(BOOST_ASIO_DISABLE_WINDOWS_STREAM_HANDLE)
#endif // !defined(BOOST_ASIO_HAS_WINDOWS_STREAM_HANDLE)

// Windows: random access handles.
#if !defined(BOOST_ASIO_HAS_WINDOWS_RANDOM_ACCESS_HANDLE)
# if !defined(BOOST_ASIO_DISABLE_WINDOWS_RANDOM_ACCESS_HANDLE)
#  if defined(BOOST_ASIO_HAS_IOCP)
#   define BOOST_ASIO_HAS_WINDOWS_RANDOM_ACCESS_HANDLE 1
#  endif // defined(BOOST_ASIO_HAS_IOCP)
# endif // !defined(BOOST_ASIO_DISABLE_WINDOWS_RANDOM_ACCESS_HANDLE)
#endif // !defined(BOOST_ASIO_HAS_WINDOWS_RANDOM_ACCESS_HANDLE)

// Windows: object handles.
#if !defined(BOOST_ASIO_HAS_WINDOWS_OBJECT_HANDLE)
# if !defined(BOOST_ASIO_DISABLE_WINDOWS_OBJECT_HANDLE)
#  if defined(BOOST_ASIO_WINDOWS) || defined(__CYGWIN__)
#   if !defined(UNDER_CE) && !defined(BOOST_ASIO_WINDOWS_APP)
#    define BOOST_ASIO_HAS_WINDOWS_OBJECT_HANDLE 1
#   endif // !defined(UNDER_CE) && !defined(BOOST_ASIO_WINDOWS_APP)
#  endif // defined(BOOST_ASIO_WINDOWS) || defined(__CYGWIN__)
# endif // !defined(BOOST_ASIO_DISABLE_WINDOWS_OBJECT_HANDLE)
#endif // !defined(BOOST_ASIO_HAS_WINDOWS_OBJECT_HANDLE)

// Windows: OVERLAPPED wrapper.
#if !defined(BOOST_ASIO_HAS_WINDOWS_OVERLAPPED_PTR)
# if !defined(BOOST_ASIO_DISABLE_WINDOWS_OVERLAPPED_PTR)
#  if defined(BOOST_ASIO_HAS_IOCP)
#   define BOOST_ASIO_HAS_WINDOWS_OVERLAPPED_PTR 1
#  endif // defined(BOOST_ASIO_HAS_IOCP)
# endif // !defined(BOOST_ASIO_DISABLE_WINDOWS_OVERLAPPED_PTR)
#endif // !defined(BOOST_ASIO_HAS_WINDOWS_OVERLAPPED_PTR)

// POSIX: stream-oriented file descriptors.
#if !defined(BOOST_ASIO_HAS_POSIX_STREAM_DESCRIPTOR)
# if !defined(BOOST_ASIO_DISABLE_POSIX_STREAM_DESCRIPTOR)
#  if !defined(BOOST_ASIO_WINDOWS) \
  && !defined(BOOST_ASIO_WINDOWS_RUNTIME) \
  && !defined(__CYGWIN__)
#   define BOOST_ASIO_HAS_POSIX_STREAM_DESCRIPTOR 1
#  endif // !defined(BOOST_ASIO_WINDOWS)
         //   && !defined(BOOST_ASIO_WINDOWS_RUNTIME)
         //   && !defined(__CYGWIN__)
# endif // !defined(BOOST_ASIO_DISABLE_POSIX_STREAM_DESCRIPTOR)
#endif // !defined(BOOST_ASIO_HAS_POSIX_STREAM_DESCRIPTOR)

// UNIX domain sockets.
#if !defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)
# if !defined(BOOST_ASIO_DISABLE_LOCAL_SOCKETS)
#  if !defined(BOOST_ASIO_WINDOWS_RUNTIME)
#   define BOOST_ASIO_HAS_LOCAL_SOCKETS 1
#  endif // !defined(BOOST_ASIO_WINDOWS_RUNTIME)
# endif // !defined(BOOST_ASIO_DISABLE_LOCAL_SOCKETS)
#endif // !defined(BOOST_ASIO_HAS_LOCAL_SOCKETS)

// Files.
#if !defined(BOOST_ASIO_HAS_FILE)
# if !defined(BOOST_ASIO_DISABLE_FILE)
#  if defined(BOOST_ASIO_HAS_WINDOWS_RANDOM_ACCESS_HANDLE)
#   define BOOST_ASIO_HAS_FILE 1
#  elif defined(BOOST_ASIO_HAS_IO_URING)
#   define BOOST_ASIO_HAS_FILE 1
#  endif // defined(BOOST_ASIO_HAS_IO_URING)
# endif // !defined(BOOST_ASIO_DISABLE_FILE)
#endif // !defined(BOOST_ASIO_HAS_FILE)

// Pipes.
#if !defined(BOOST_ASIO_HAS_PIPE)
# if defined(BOOST_ASIO_HAS_IOCP) \
  || !defined(BOOST_ASIO_WINDOWS) \
  && !defined(BOOST_ASIO_WINDOWS_RUNTIME) \
  && !defined(__CYGWIN__)
#  if !defined(__SYMBIAN32__)
#   if !defined(BOOST_ASIO_DISABLE_PIPE)
#    define BOOST_ASIO_HAS_PIPE 1
#   endif // !defined(BOOST_ASIO_DISABLE_PIPE)
#  endif // !defined(__SYMBIAN32__)
# endif // defined(BOOST_ASIO_HAS_IOCP)
        //   || !defined(BOOST_ASIO_WINDOWS)
        //   && !defined(BOOST_ASIO_WINDOWS_RUNTIME)
        //   && !defined(__CYGWIN__)
#endif // !defined(BOOST_ASIO_HAS_PIPE)

// Can use sigaction() instead of signal().
#if !defined(BOOST_ASIO_HAS_SIGACTION)
# if !defined(BOOST_ASIO_DISABLE_SIGACTION)
#  if !defined(BOOST_ASIO_WINDOWS) \
  && !defined(BOOST_ASIO_WINDOWS_RUNTIME) \
  && !defined(__CYGWIN__)
#   define BOOST_ASIO_HAS_SIGACTION 1
#  endif // !defined(BOOST_ASIO_WINDOWS)
         //   && !defined(BOOST_ASIO_WINDOWS_RUNTIME)
         //   && !defined(__CYGWIN__)
# endif // !defined(BOOST_ASIO_DISABLE_SIGACTION)
#endif // !defined(BOOST_ASIO_HAS_SIGACTION)

// Can use signal().
#if !defined(BOOST_ASIO_HAS_SIGNAL)
# if !defined(BOOST_ASIO_DISABLE_SIGNAL)
#  if !defined(UNDER_CE)
#   define BOOST_ASIO_HAS_SIGNAL 1
#  endif // !defined(UNDER_CE)
# endif // !defined(BOOST_ASIO_DISABLE_SIGNAL)
#endif // !defined(BOOST_ASIO_HAS_SIGNAL)

// Can use getaddrinfo() and getnameinfo().
#if !defined(BOOST_ASIO_HAS_GETADDRINFO)
# if !defined(BOOST_ASIO_DISABLE_GETADDRINFO)
#  if defined(BOOST_ASIO_WINDOWS) || defined(__CYGWIN__)
#   if defined(_WIN32_WINNT) && (_WIN32_WINNT >= 0x0501)
#    define BOOST_ASIO_HAS_GETADDRINFO 1
#   elif defined(UNDER_CE)
#    define BOOST_ASIO_HAS_GETADDRINFO 1
#   endif // defined(UNDER_CE)
#  elif defined(__MACH__) && defined(__APPLE__)
#   if defined(__MAC_OS_X_VERSION_MIN_REQUIRED)
#    if (__MAC_OS_X_VERSION_MIN_REQUIRED >= 1050)
#     define BOOST_ASIO_HAS_GETADDRINFO 1
#    endif // (__MAC_OS_X_VERSION_MIN_REQUIRED >= 1050)
#   else // defined(__MAC_OS_X_VERSION_MIN_REQUIRED)
#    define BOOST_ASIO_HAS_GETADDRINFO 1
#   endif // defined(__MAC_OS_X_VERSION_MIN_REQUIRED)
#  else // defined(__MACH__) && defined(__APPLE__)
#   define BOOST_ASIO_HAS_GETADDRINFO 1
#  endif // defined(__MACH__) && defined(__APPLE__)
# endif // !defined(BOOST_ASIO_DISABLE_GETADDRINFO)
#endif // !defined(BOOST_ASIO_HAS_GETADDRINFO)

// Whether standard iostreams are disabled.
#if !defined(BOOST_ASIO_NO_IOSTREAM)
# if defined(BOOST_ASIO_HAS_BOOST_CONFIG) && defined(BOOST_NO_IOSTREAM)
#  define BOOST_ASIO_NO_IOSTREAM 1
# endif // !defined(BOOST_NO_IOSTREAM)
#endif // !defined(BOOST_ASIO_NO_IOSTREAM)

// Whether exception handling is disabled.
#if !defined(BOOST_ASIO_NO_EXCEPTIONS)
# if defined(BOOST_ASIO_HAS_BOOST_CONFIG) && defined(BOOST_NO_EXCEPTIONS)
#  define BOOST_ASIO_NO_EXCEPTIONS 1
# endif // !defined(BOOST_NO_EXCEPTIONS)
#endif // !defined(BOOST_ASIO_NO_EXCEPTIONS)

// Whether the typeid operator is supported.
#if !defined(BOOST_ASIO_NO_TYPEID)
# if defined(BOOST_ASIO_HAS_BOOST_CONFIG) && defined(BOOST_NO_TYPEID)
#  define BOOST_ASIO_NO_TYPEID 1
# endif // !defined(BOOST_NO_TYPEID)
#endif // !defined(BOOST_ASIO_NO_TYPEID)

// Threads.
#if !defined(BOOST_ASIO_HAS_THREADS)
# if !defined(BOOST_ASIO_DISABLE_THREADS)
#  if defined(BOOST_ASIO_HAS_BOOST_CONFIG) && defined(BOOST_HAS_THREADS)
#   define BOOST_ASIO_HAS_THREADS 1
#  elif defined(__GNUC__) && !defined(__MINGW32__) \
     && !defined(linux) && !defined(__linux) && !defined(__linux__)
#   define BOOST_ASIO_HAS_THREADS 1
#  elif defined(_MT) || defined(__MT__)
#   define BOOST_ASIO_HAS_THREADS 1
#  elif defined(_REENTRANT)
#   define BOOST_ASIO_HAS_THREADS 1
#  elif defined(__APPLE__)
#   define BOOST_ASIO_HAS_THREADS 1
#  elif defined(__HAIKU__)
#   define BOOST_ASIO_HAS_THREADS 1
#  elif defined(_POSIX_THREADS) && (_POSIX_THREADS + 0 >= 0)
#   define BOOST_ASIO_HAS_THREADS 1
#  elif defined(_PTHREADS)
#   define BOOST_ASIO_HAS_THREADS 1
#  endif // defined(BOOST_ASIO_HAS_BOOST_CONFIG) && defined(BOOST_HAS_THREADS)
# endif // !defined(BOOST_ASIO_DISABLE_THREADS)
#endif // !defined(BOOST_ASIO_HAS_THREADS)

// POSIX threads.
#if !defined(BOOST_ASIO_HAS_PTHREADS)
# if defined(BOOST_ASIO_HAS_THREADS)
#  if defined(BOOST_ASIO_HAS_BOOST_CONFIG) && defined(BOOST_HAS_PTHREADS)
#   define BOOST_ASIO_HAS_PTHREADS 1
#  elif defined(_POSIX_THREADS) && (_POSIX_THREADS + 0 >= 0)
#   define BOOST_ASIO_HAS_PTHREADS 1
#  elif defined(__HAIKU__)
#   define BOOST_ASIO_HAS_PTHREADS 1
#  endif // defined(BOOST_ASIO_HAS_BOOST_CONFIG) && defined(BOOST_HAS_PTHREADS)
# endif // defined(BOOST_ASIO_HAS_THREADS)
#endif // !defined(BOOST_ASIO_HAS_PTHREADS)

// Helper to prevent macro expansion.
#define BOOST_ASIO_PREVENT_MACRO_SUBSTITUTION

// Helper to define in-class constants.
#if !defined(BOOST_ASIO_STATIC_CONSTANT)
# if !defined(BOOST_ASIO_DISABLE_BOOST_STATIC_CONSTANT)
#  define BOOST_ASIO_STATIC_CONSTANT(type, assignment) \
    BOOST_STATIC_CONSTANT(type, assignment)
# else // !defined(BOOST_ASIO_DISABLE_BOOST_STATIC_CONSTANT)
#  define BOOST_ASIO_STATIC_CONSTANT(type, assignment) \
    static const type assignment
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_STATIC_CONSTANT)
#endif // !defined(BOOST_ASIO_STATIC_CONSTANT)

// Boost align library.
#if !defined(BOOST_ASIO_HAS_BOOST_ALIGN)
# if !defined(BOOST_ASIO_DISABLE_BOOST_ALIGN)
#  if defined(BOOST_ASIO_HAS_BOOST_CONFIG) && (BOOST_VERSION >= 105600)
#   define BOOST_ASIO_HAS_BOOST_ALIGN 1
#  endif // defined(BOOST_ASIO_HAS_BOOST_CONFIG) && (BOOST_VERSION >= 105600)
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_ALIGN)
#endif // !defined(BOOST_ASIO_HAS_BOOST_ALIGN)

// Boost array library.
#if !defined(BOOST_ASIO_HAS_BOOST_ARRAY)
# if !defined(BOOST_ASIO_DISABLE_BOOST_ARRAY)
#  define BOOST_ASIO_HAS_BOOST_ARRAY 1
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_ARRAY)
#endif // !defined(BOOST_ASIO_HAS_BOOST_ARRAY)

// Boost assert macro.
#if !defined(BOOST_ASIO_HAS_BOOST_ASSERT)
# if !defined(BOOST_ASIO_DISABLE_BOOST_ASSERT)
#  define BOOST_ASIO_HAS_BOOST_ASSERT 1
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_ASSERT)
#endif // !defined(BOOST_ASIO_HAS_BOOST_ASSERT)

// Boost limits header.
#if !defined(BOOST_ASIO_HAS_BOOST_LIMITS)
# if !defined(BOOST_ASIO_DISABLE_BOOST_LIMITS)
#  define BOOST_ASIO_HAS_BOOST_LIMITS 1
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_LIMITS)
#endif // !defined(BOOST_ASIO_HAS_BOOST_LIMITS)

// Boost throw_exception function.
#if !defined(BOOST_ASIO_HAS_BOOST_THROW_EXCEPTION)
# if !defined(BOOST_ASIO_DISABLE_BOOST_THROW_EXCEPTION)
#  define BOOST_ASIO_HAS_BOOST_THROW_EXCEPTION 1
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_THROW_EXCEPTION)
#endif // !defined(BOOST_ASIO_HAS_BOOST_THROW_EXCEPTION)

// Boost regex library.
#if !defined(BOOST_ASIO_HAS_BOOST_REGEX)
# if !defined(BOOST_ASIO_DISABLE_BOOST_REGEX)
#  define BOOST_ASIO_HAS_BOOST_REGEX 1
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_REGEX)
#endif // !defined(BOOST_ASIO_HAS_BOOST_REGEX)

// Boost bind function.
#if !defined(BOOST_ASIO_HAS_BOOST_BIND)
# if !defined(BOOST_ASIO_DISABLE_BOOST_BIND)
#  define BOOST_ASIO_HAS_BOOST_BIND 1
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_BIND)
#endif // !defined(BOOST_ASIO_HAS_BOOST_BIND)

// Boost's BOOST_WORKAROUND macro.
#if !defined(BOOST_ASIO_HAS_BOOST_WORKAROUND)
# if !defined(BOOST_ASIO_DISABLE_BOOST_WORKAROUND)
#  define BOOST_ASIO_HAS_BOOST_WORKAROUND 1
# endif // !defined(BOOST_ASIO_DISABLE_BOOST_WORKAROUND)
#endif // !defined(BOOST_ASIO_HAS_BOOST_WORKAROUND)

// Microsoft Visual C++'s secure C runtime library.
#if !defined(BOOST_ASIO_HAS_SECURE_RTL)
# if !defined(BOOST_ASIO_DISABLE_SECURE_RTL)
#  if defined(BOOST_ASIO_MSVC) \
    && (BOOST_ASIO_MSVC >= 1400) \
    && !defined(UNDER_CE)
#   define BOOST_ASIO_HAS_SECURE_RTL 1
#  endif // defined(BOOST_ASIO_MSVC)
         // && (BOOST_ASIO_MSVC >= 1400)
         // && !defined(UNDER_CE)
# endif // !defined(BOOST_ASIO_DISABLE_SECURE_RTL)
#endif // !defined(BOOST_ASIO_HAS_SECURE_RTL)

// Handler hooking. Disabled for ancient Borland C++ and gcc compilers.
#if !defined(BOOST_ASIO_HAS_HANDLER_HOOKS)
# if !defined(BOOST_ASIO_DISABLE_HANDLER_HOOKS)
#  if defined(__GNUC__)
#   if (__GNUC__ >= 3)
#    define BOOST_ASIO_HAS_HANDLER_HOOKS 1
#   endif // (__GNUC__ >= 3)
#  elif !defined(__BORLANDC__) || defined(__clang__)
#   define BOOST_ASIO_HAS_HANDLER_HOOKS 1
#  endif // !defined(__BORLANDC__) || defined(__clang__)
# endif // !defined(BOOST_ASIO_DISABLE_HANDLER_HOOKS)
#endif // !defined(BOOST_ASIO_HAS_HANDLER_HOOKS)

// Support for the __thread keyword extension, or equivalent.
#if !defined(BOOST_ASIO_DISABLE_THREAD_KEYWORD_EXTENSION)
# if defined(__linux__)
#  if defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#   if ((__GNUC__ == 3) && (__GNUC_MINOR__ >= 3)) || (__GNUC__ > 3)
#    if !defined(__INTEL_COMPILER) && !defined(__ICL) \
       && !(defined(__clang__) && defined(__ANDROID__))
#     define BOOST_ASIO_HAS_THREAD_KEYWORD_EXTENSION 1
#     define BOOST_ASIO_THREAD_KEYWORD __thread
#    elif defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 1100)
#     define BOOST_ASIO_HAS_THREAD_KEYWORD_EXTENSION 1
#    endif // defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 1100)
           // && !(defined(__clang__) && defined(__ANDROID__))
#   endif // ((__GNUC__ == 3) && (__GNUC_MINOR__ >= 3)) || (__GNUC__ > 3)
#  endif // defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
# endif // defined(__linux__)
# if defined(BOOST_ASIO_MSVC) && defined(BOOST_ASIO_WINDOWS_RUNTIME)
#  if (_MSC_VER >= 1700)
#   define BOOST_ASIO_HAS_THREAD_KEYWORD_EXTENSION 1
#   define BOOST_ASIO_THREAD_KEYWORD __declspec(thread)
#  endif // (_MSC_VER >= 1700)
# endif // defined(BOOST_ASIO_MSVC) && defined(BOOST_ASIO_WINDOWS_RUNTIME)
# if defined(__APPLE__)
#  if defined(__clang__)
#   if defined(__apple_build_version__)
#    define BOOST_ASIO_HAS_THREAD_KEYWORD_EXTENSION 1
#    define BOOST_ASIO_THREAD_KEYWORD __thread
#   endif // defined(__apple_build_version__)
#  endif // defined(__clang__)
# endif // defined(__APPLE__)
# if !defined(BOOST_ASIO_HAS_THREAD_KEYWORD_EXTENSION)
#  if defined(BOOST_ASIO_HAS_BOOST_CONFIG)
#   if !defined(BOOST_NO_CXX11_THREAD_LOCAL)
#    define BOOST_ASIO_HAS_THREAD_KEYWORD_EXTENSION 1
#    define BOOST_ASIO_THREAD_KEYWORD thread_local
#   endif // !defined(BOOST_NO_CXX11_THREAD_LOCAL)
#  endif // defined(BOOST_ASIO_HAS_BOOST_CONFIG)
# endif // !defined(BOOST_ASIO_HAS_THREAD_KEYWORD_EXTENSION)
#endif // !defined(BOOST_ASIO_DISABLE_THREAD_KEYWORD_EXTENSION)
#if !defined(BOOST_ASIO_THREAD_KEYWORD)
# define BOOST_ASIO_THREAD_KEYWORD __thread
#endif // !defined(BOOST_ASIO_THREAD_KEYWORD)

// Support for POSIX ssize_t typedef.
#if !defined(BOOST_ASIO_DISABLE_SSIZE_T)
# if defined(__linux__) \
   || (defined(__MACH__) && defined(__APPLE__))
#  define BOOST_ASIO_HAS_SSIZE_T 1
# endif // defined(__linux__)
        //   || (defined(__MACH__) && defined(__APPLE__))
#endif // !defined(BOOST_ASIO_DISABLE_SSIZE_T)

// Helper macros to manage transition away from error_code return values.
#if defined(BOOST_ASIO_NO_DEPRECATED)
# define BOOST_ASIO_SYNC_OP_VOID void
# define BOOST_ASIO_SYNC_OP_VOID_RETURN(e) return
#else // defined(BOOST_ASIO_NO_DEPRECATED)
# define BOOST_ASIO_SYNC_OP_VOID boost::system::error_code
# define BOOST_ASIO_SYNC_OP_VOID_RETURN(e) return e
#endif // defined(BOOST_ASIO_NO_DEPRECATED)

// Newer gcc, clang need special treatment to suppress unused typedef warnings.
#if defined(__clang__)
# if defined(__apple_build_version__)
#  if (__clang_major__ >= 7)
#   define BOOST_ASIO_UNUSED_TYPEDEF __attribute__((__unused__))
#  endif // (__clang_major__ >= 7)
# elif ((__clang_major__ == 3) && (__clang_minor__ >= 6)) \
    || (__clang_major__ > 3)
#  define BOOST_ASIO_UNUSED_TYPEDEF __attribute__((__unused__))
# endif // ((__clang_major__ == 3) && (__clang_minor__ >= 6))
        //   || (__clang_major__ > 3)
#elif defined(__GNUC__)
# if ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ > 4)
#  define BOOST_ASIO_UNUSED_TYPEDEF __attribute__((__unused__))
# endif // ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 8)) || (__GNUC__ > 4)
#endif // defined(__GNUC__)
#if !defined(BOOST_ASIO_UNUSED_TYPEDEF)
# define BOOST_ASIO_UNUSED_TYPEDEF
#endif // !defined(BOOST_ASIO_UNUSED_TYPEDEF)

// Some versions of gcc generate spurious warnings about unused variables.
#if defined(__GNUC__)
# if (__GNUC__ >= 4)
#  define BOOST_ASIO_UNUSED_VARIABLE __attribute__((__unused__))
# endif // (__GNUC__ >= 4)
#endif // defined(__GNUC__)
#if !defined(BOOST_ASIO_UNUSED_VARIABLE)
# define BOOST_ASIO_UNUSED_VARIABLE
#endif // !defined(BOOST_ASIO_UNUSED_VARIABLE)

// Helper macro to tell the optimiser what may be assumed to be true.
#if defined(BOOST_ASIO_MSVC)
# define BOOST_ASIO_ASSUME(expr) __assume(expr)
#elif defined(__clang__)
# if __has_builtin(__builtin_assume)
#  define BOOST_ASIO_ASSUME(expr) __builtin_assume(expr)
# endif // __has_builtin(__builtin_assume)
#elif defined(__GNUC__)
# if ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 5)) || (__GNUC__ > 4)
#  define BOOST_ASIO_ASSUME(expr) if (expr) {} else { __builtin_unreachable(); }
# endif // ((__GNUC__ == 4) && (__GNUC_MINOR__ >= 5)) || (__GNUC__ > 4)
#endif // defined(__GNUC__)
#if !defined(BOOST_ASIO_ASSUME)
# define BOOST_ASIO_ASSUME(expr) (void)0
#endif // !defined(BOOST_ASIO_ASSUME)

// Support the co_await keyword on compilers known to allow it.
#if !defined(BOOST_ASIO_HAS_CO_AWAIT)
# if !defined(BOOST_ASIO_DISABLE_CO_AWAIT)
#  if (__cplusplus >= 202002) \
     && (__cpp_impl_coroutine >= 201902) && (__cpp_lib_coroutine >= 201902)
#   define BOOST_ASIO_HAS_CO_AWAIT 1
#  elif defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1928) && (_MSVC_LANG >= 201705) && !defined(__clang__)
#    define BOOST_ASIO_HAS_CO_AWAIT 1
#   elif (_MSC_FULL_VER >= 190023506)
#    if defined(_RESUMABLE_FUNCTIONS_SUPPORTED)
#     define BOOST_ASIO_HAS_CO_AWAIT 1
#    endif // defined(_RESUMABLE_FUNCTIONS_SUPPORTED)
#   endif // (_MSC_FULL_VER >= 190023506)
#  elif defined(__clang__)
#   if (__clang_major__ >= 14)
#    if (__cplusplus >= 202002) && (__cpp_impl_coroutine >= 201902)
#     if __has_include(<coroutine>)
#      define BOOST_ASIO_HAS_CO_AWAIT 1
#     endif // __has_include(<coroutine>)
#    elif (__cplusplus >= 201703) && (__cpp_coroutines >= 201703)
#     if __has_include(<experimental/coroutine>)
#      define BOOST_ASIO_HAS_CO_AWAIT 1
#     endif // __has_include(<experimental/coroutine>)
#    endif // (__cplusplus >= 201703) && (__cpp_coroutines >= 201703)
#   else // (__clang_major__ >= 14)
#    if (__cplusplus >= 201703) && (__cpp_coroutines >= 201703)
#     if __has_include(<experimental/coroutine>)
#      define BOOST_ASIO_HAS_CO_AWAIT 1
#     endif // __has_include(<experimental/coroutine>)
#    endif // (__cplusplus >= 201703) && (__cpp_coroutines >= 201703)
#   endif // (__clang_major__ >= 14)
#  elif defined(__GNUC__)
#   if (__cplusplus >= 201709) && (__cpp_impl_coroutine >= 201902)
#    if __has_include(<coroutine>)
#     define BOOST_ASIO_HAS_CO_AWAIT 1
#    endif // __has_include(<coroutine>)
#   endif // (__cplusplus >= 201709) && (__cpp_impl_coroutine >= 201902)
#  endif // defined(__GNUC__)
# endif // !defined(BOOST_ASIO_DISABLE_CO_AWAIT)
#endif // !defined(BOOST_ASIO_HAS_CO_AWAIT)

// Standard library support for coroutines.
#if !defined(BOOST_ASIO_HAS_STD_COROUTINE)
# if !defined(BOOST_ASIO_DISABLE_STD_COROUTINE)
#  if defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1928) && (_MSVC_LANG >= 201705)
#    define BOOST_ASIO_HAS_STD_COROUTINE 1
#   endif // (_MSC_VER >= 1928) && (_MSVC_LANG >= 201705)
#  elif defined(__clang__)
#   if (__clang_major__ >= 14)
#    if (__cplusplus >= 202002) && (__cpp_impl_coroutine >= 201902)
#     if __has_include(<coroutine>)
#      define BOOST_ASIO_HAS_STD_COROUTINE 1
#     endif // __has_include(<coroutine>)
#    endif // (__cplusplus >= 202002) && (__cpp_impl_coroutine >= 201902)
#   endif // (__clang_major__ >= 14)
#  elif defined(__GNUC__)
#   if (__cplusplus >= 201709) && (__cpp_impl_coroutine >= 201902)
#    if __has_include(<coroutine>)
#     define BOOST_ASIO_HAS_STD_COROUTINE 1
#    endif // __has_include(<coroutine>)
#   endif // (__cplusplus >= 201709) && (__cpp_impl_coroutine >= 201902)
#  endif // defined(__GNUC__)
# endif // !defined(BOOST_ASIO_DISABLE_STD_COROUTINE)
#endif // !defined(BOOST_ASIO_HAS_STD_COROUTINE)

// Compiler support for the the [[nodiscard]] attribute.
#if !defined(BOOST_ASIO_NODISCARD)
# if defined(__has_cpp_attribute)
#  if __has_cpp_attribute(nodiscard)
#   if (__cplusplus >= 201703)
#    define BOOST_ASIO_NODISCARD [[nodiscard]]
#   endif // (__cplusplus >= 201703)
#  endif // __has_cpp_attribute(nodiscard)
# endif // defined(__has_cpp_attribute)
#endif // !defined(BOOST_ASIO_NODISCARD)
#if !defined(BOOST_ASIO_NODISCARD)
# define BOOST_ASIO_NODISCARD
#endif // !defined(BOOST_ASIO_NODISCARD)

// Kernel support for MSG_NOSIGNAL.
#if !defined(BOOST_ASIO_HAS_MSG_NOSIGNAL)
# if defined(__linux__)
#  define BOOST_ASIO_HAS_MSG_NOSIGNAL 1
# elif defined(_POSIX_VERSION)
#  if (_POSIX_VERSION >= 200809L)
#   define BOOST_ASIO_HAS_MSG_NOSIGNAL 1
#  endif // _POSIX_VERSION >= 200809L
# endif // defined(_POSIX_VERSION)
#endif // !defined(BOOST_ASIO_HAS_MSG_NOSIGNAL)

// Standard library support for std::to_address.
#if !defined(BOOST_ASIO_HAS_STD_TO_ADDRESS)
# if !defined(BOOST_ASIO_DISABLE_STD_TO_ADDRESS)
#  if defined(__clang__)
#   if (__cplusplus >= 202002)
#    define BOOST_ASIO_HAS_STD_TO_ADDRESS 1
#   endif // (__cplusplus >= 202002)
#  elif defined(__GNUC__)
#   if (__GNUC__ >= 8)
#    if (__cplusplus >= 202002)
#     define BOOST_ASIO_HAS_STD_TO_ADDRESS 1
#    endif // (__cplusplus >= 202002)
#   endif // (__GNUC__ >= 8)
#  endif // defined(__GNUC__)
#  if defined(BOOST_ASIO_MSVC)
#   if (_MSC_VER >= 1922) && (_MSVC_LANG >= 202002)
#    define BOOST_ASIO_HAS_STD_TO_ADDRESS 1
#   endif // (_MSC_VER >= 1922) && (_MSVC_LANG >= 202002)
#  endif // defined(BOOST_ASIO_MSVC)
# endif // !defined(BOOST_ASIO_DISABLE_STD_TO_ADDRESS)
#endif // !defined(BOOST_ASIO_HAS_STD_TO_ADDRESS)

// Standard library support for snprintf.
#if !defined(BOOST_ASIO_HAS_SNPRINTF)
# if !defined(BOOST_ASIO_DISABLE_SNPRINTF)
#  if defined(__APPLE__)
#   define BOOST_ASIO_HAS_SNPRINTF 1
#  endif // defined(__APPLE__)
# endif // !defined(BOOST_ASIO_DISABLE_SNPRINTF)
#endif // !defined(BOOST_ASIO_HAS_SNPRINTF)

#endif // BOOST_ASIO_DETAIL_CONFIG_HPP
