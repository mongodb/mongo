/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2012 Hartmut Kaiser
 * Copyright (c) 2014-2018, 2020-2021 Andrey Semashev
 */
/*!
 * \file   atomic/detail/config.hpp
 *
 * This header defines configuraion macros for Boost.Atomic
 */

#ifndef BOOST_ATOMIC_DETAIL_CONFIG_HPP_INCLUDED_
#define BOOST_ATOMIC_DETAIL_CONFIG_HPP_INCLUDED_

#include <boost/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

#if defined(__CUDACC__)
// nvcc does not support alternatives ("q,m") in asm statement constraints
#define BOOST_ATOMIC_DETAIL_NO_ASM_CONSTRAINT_ALTERNATIVES
// nvcc does not support condition code register ("cc") clobber in asm statements
#define BOOST_ATOMIC_DETAIL_NO_ASM_CLOBBER_CC
#endif

#if !defined(BOOST_ATOMIC_DETAIL_NO_ASM_CLOBBER_CC)
#define BOOST_ATOMIC_DETAIL_ASM_CLOBBER_CC "cc"
#define BOOST_ATOMIC_DETAIL_ASM_CLOBBER_CC_COMMA "cc",
#else
#define BOOST_ATOMIC_DETAIL_ASM_CLOBBER_CC
#define BOOST_ATOMIC_DETAIL_ASM_CLOBBER_CC_COMMA
#endif

#if (defined(__i386__) || defined(__x86_64__)) && (defined(__clang__) || (defined(BOOST_GCC) && BOOST_GCC < 40500) || defined(__SUNPRO_CC))
// This macro indicates that the compiler does not support allocating eax:edx or rax:rdx register pairs ("A") in asm blocks
#define BOOST_ATOMIC_DETAIL_X86_NO_ASM_AX_DX_PAIRS
#endif

#if defined(__i386__) && (defined(__PIC__) || defined(__PIE__)) && !(defined(__clang__) || (defined(BOOST_GCC) && BOOST_GCC >= 50100))
// This macro indicates that asm blocks should preserve ebx value unchanged. Some compilers are able to maintain ebx themselves
// around the asm blocks. For those compilers we don't need to save/restore ebx in asm blocks.
#define BOOST_ATOMIC_DETAIL_X86_ASM_PRESERVE_EBX
#endif

#if defined(BOOST_NO_CXX11_HDR_TYPE_TRAITS)
#if !(defined(BOOST_LIBSTDCXX11) && BOOST_LIBSTDCXX_VERSION >= 40700) /* libstdc++ from gcc >= 4.7 in C++11 mode */
// This macro indicates that there is not even a basic <type_traits> standard header that is sufficient for most Boost.Atomic needs.
#define BOOST_ATOMIC_DETAIL_NO_CXX11_BASIC_HDR_TYPE_TRAITS
#endif
#endif // defined(BOOST_NO_CXX11_HDR_TYPE_TRAITS)

#if defined(BOOST_NO_CXX11_ALIGNAS) ||\
    (defined(BOOST_GCC) && BOOST_GCC < 40900) ||\
    (defined(BOOST_MSVC) && BOOST_MSVC < 1910 && defined(_M_IX86))
// gcc prior to 4.9 doesn't support alignas with a constant expression as an argument.
// MSVC 14.0 does support alignas, but in 32-bit mode emits "error C2719: formal parameter with requested alignment of N won't be aligned" for N > 4,
// when aligned types are used in function arguments, even though the std::max_align_t type has alignment of 8.
#define BOOST_ATOMIC_DETAIL_NO_CXX11_ALIGNAS
#endif

#if defined(BOOST_NO_CXX11_CONSTEXPR) || (defined(BOOST_GCC) && BOOST_GCC < 40800)
// This macro indicates that the compiler doesn't support constexpr constructors that initialize one member
// of an anonymous union member of the class.
#define BOOST_ATOMIC_DETAIL_NO_CXX11_CONSTEXPR_UNION_INIT
#endif

#if !defined(BOOST_ATOMIC_DETAIL_NO_CXX11_CONSTEXPR_UNION_INIT)
#define BOOST_ATOMIC_DETAIL_CONSTEXPR_UNION_INIT BOOST_CONSTEXPR
#else
#define BOOST_ATOMIC_DETAIL_CONSTEXPR_UNION_INIT
#endif

// Enable pointer/reference casts between storage and value when possible.
// Note: Despite that MSVC does not employ strict aliasing rules for optimizations
// and does not require an explicit markup for types that may alias, we still don't
// enable the optimization for this compiler because at least MSVC-8 and 9 are known
// to generate broken code sometimes when casts are used.
#define BOOST_ATOMIC_DETAIL_MAY_ALIAS BOOST_MAY_ALIAS
#if !defined(BOOST_NO_MAY_ALIAS)
#define BOOST_ATOMIC_DETAIL_STORAGE_TYPE_MAY_ALIAS
#endif

#if defined(__GCC_ASM_FLAG_OUTPUTS__)
// The compiler supports output values in flag registers.
// See: https://gcc.gnu.org/onlinedocs/gcc/Extended-Asm.html, Section 6.44.3.
#define BOOST_ATOMIC_DETAIL_ASM_HAS_FLAG_OUTPUTS
#endif

#if defined(BOOST_INTEL) || (defined(BOOST_GCC) && BOOST_GCC < 40700) ||\
    (defined(BOOST_CLANG) && !defined(__apple_build_version__) && (__clang_major__ * 100 + __clang_minor__) < 302) ||\
    (defined(__clang__) && defined(__apple_build_version__) && (__clang_major__ * 100 + __clang_minor__) < 402)
// Intel compiler (at least 18.0 update 1) breaks if noexcept specification is used in defaulted function declarations:
// error: the default constructor of "boost::atomics::atomic<T>" cannot be referenced -- it is a deleted function
// GCC 4.6 doesn't seem to support that either. Clang 3.1 deduces wrong noexcept for the defaulted function and fails as well.
#define BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_DECL
#define BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_IMPL BOOST_NOEXCEPT
#else
#define BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_DECL BOOST_NOEXCEPT
#define BOOST_ATOMIC_DETAIL_DEF_NOEXCEPT_IMPL
#endif

#if defined(__has_builtin)
#if __has_builtin(__builtin_constant_p)
#define BOOST_ATOMIC_DETAIL_IS_CONSTANT(x) __builtin_constant_p(x)
#endif
#if __has_builtin(__builtin_clear_padding)
#define BOOST_ATOMIC_DETAIL_CLEAR_PADDING(x) __builtin_clear_padding(x)
#elif __has_builtin(__builtin_zero_non_value_bits)
#define BOOST_ATOMIC_DETAIL_CLEAR_PADDING(x) __builtin_zero_non_value_bits(x)
#endif
#endif

#if !defined(BOOST_ATOMIC_DETAIL_IS_CONSTANT) && defined(__GNUC__)
#define BOOST_ATOMIC_DETAIL_IS_CONSTANT(x) __builtin_constant_p(x)
#endif

#if !defined(BOOST_ATOMIC_DETAIL_IS_CONSTANT)
#define BOOST_ATOMIC_DETAIL_IS_CONSTANT(x) false
#endif

#if !defined(BOOST_ATOMIC_DETAIL_CLEAR_PADDING) && defined(BOOST_MSVC) && BOOST_MSVC >= 1927
// Note that as of MSVC 19.29 this intrinsic does not clear padding in unions:
// https://developercommunity.visualstudio.com/t/__builtin_zero_non_value_bits-does-not-c/1551510
#define BOOST_ATOMIC_DETAIL_CLEAR_PADDING(x) __builtin_zero_non_value_bits(x)
#endif

#if !defined(BOOST_ATOMIC_DETAIL_CLEAR_PADDING)
#define BOOST_ATOMIC_NO_CLEAR_PADDING
#define BOOST_ATOMIC_DETAIL_CLEAR_PADDING(x)
#endif

#if (defined(__BYTE_ORDER__) && defined(__FLOAT_WORD_ORDER__) && __BYTE_ORDER__ == __FLOAT_WORD_ORDER__) ||\
    defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)
// This macro indicates that integer and floating point endianness is the same
#define BOOST_ATOMIC_DETAIL_INT_FP_ENDIAN_MATCH
#endif

#endif // BOOST_ATOMIC_DETAIL_CONFIG_HPP_INCLUDED_
