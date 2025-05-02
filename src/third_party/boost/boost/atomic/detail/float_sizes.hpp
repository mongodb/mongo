/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2018-2023 Andrey Semashev
 */
/*!
 * \file   atomic/detail/float_sizes.hpp
 *
 * This header defines macros for testing buitin floating point type sizes
 */

#ifndef BOOST_ATOMIC_DETAIL_FLOAT_SIZES_HPP_INCLUDED_
#define BOOST_ATOMIC_DETAIL_FLOAT_SIZES_HPP_INCLUDED_

#include <boost/atomic/detail/config.hpp>

#ifdef BOOST_HAS_PRAGMA_ONCE
#pragma once
#endif

// Detect value sizes of the different floating point types. The value sizes may be less than the corresponding type sizes
// if the type contains padding bits. This is typical e.g. with x87 80-bit extended double types, which are often represented as 96 or 128-bit types.
// See: https://en.wikipedia.org/wiki/IEEE_754
// For Intel x87 extended double see: https://en.wikipedia.org/wiki/Extended_precision#x86_Architecture_Extended_Precision_Format
// For IBM extended double (a.k.a. double-double) see: https://en.wikipedia.org/wiki/Long_double#Implementations, https://gcc.gnu.org/wiki/Ieee128PowerPC

#if defined(__FLT_RADIX__) && defined(__FLT_MANT_DIG__) && defined(__FLT_MAX_EXP__) && \
    defined(__DBL_MANT_DIG__) && defined(__DBL_MAX_EXP__) && defined(__LDBL_MANT_DIG__) && defined(__LDBL_MAX_EXP__)

#if (__FLT_RADIX__ == 2)

#if (__FLT_MANT_DIG__ == 11) && (__FLT_MAX_EXP__ == 16) // IEEE 754 binary16
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 2
#elif (__FLT_MANT_DIG__ == 24) && (__FLT_MAX_EXP__ == 128) // IEEE 754 binary32
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 4
#elif (__FLT_MANT_DIG__ == 53) && (__FLT_MAX_EXP__ == 1024) // IEEE 754 binary64
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 8
#elif (__FLT_MANT_DIG__ == 64 || __FLT_MANT_DIG__ == 53 || __FLT_MANT_DIG__ == 24) && (__FLT_MAX_EXP__ == 16384) // x87 extended double, with full 64-bit significand or reduced to 53 or 24 bits
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 10
#elif (__FLT_MANT_DIG__ == 106) && (__FLT_MAX_EXP__ == 1024) // IBM extended double
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 16
#elif (__FLT_MANT_DIG__ == 113) && (__FLT_MAX_EXP__ == 16384) // IEEE 754 binary128
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 16
#elif (__FLT_MANT_DIG__ == 237) && (__FLT_MAX_EXP__ == 262144) // IEEE 754 binary256
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 32
#endif

#if (__DBL_MANT_DIG__ == 11) && (__DBL_MAX_EXP__ == 16) // IEEE 754 binary16
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 2
#elif (__DBL_MANT_DIG__ == 24) && (__DBL_MAX_EXP__ == 128) // IEEE 754 binary32
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 4
#elif (__DBL_MANT_DIG__ == 53) && (__DBL_MAX_EXP__ == 1024) // IEEE 754 binary64
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 8
#elif (__DBL_MANT_DIG__ == 64 || __DBL_MANT_DIG__ == 53 || __DBL_MANT_DIG__ == 24) && (__DBL_MAX_EXP__ == 16384) // x87 extended double, with full 64-bit significand or reduced to 53 or 24 bits
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 10
#elif (__DBL_MANT_DIG__ == 106) && (__DBL_MAX_EXP__ == 1024) // IBM extended double
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 16
#elif (__DBL_MANT_DIG__ == 113) && (__DBL_MAX_EXP__ == 16384) // IEEE 754 binary128
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 16
#elif (__DBL_MANT_DIG__ == 237) && (__DBL_MAX_EXP__ == 262144) // IEEE 754 binary256
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 32
#endif

#if (__LDBL_MANT_DIG__ == 11) && (__LDBL_MAX_EXP__ == 16) // IEEE 754 binary16
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 2
#elif (__LDBL_MANT_DIG__ == 24) && (__LDBL_MAX_EXP__ == 128) // IEEE 754 binary32
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 4
#elif (__LDBL_MANT_DIG__ == 53) && (__LDBL_MAX_EXP__ == 1024) // IEEE 754 binary64
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 8
#elif (__LDBL_MANT_DIG__ == 64 || __LDBL_MANT_DIG__ == 53 || __LDBL_MANT_DIG__ == 24) && (__LDBL_MAX_EXP__ == 16384) // x87 extended double, with full 64-bit significand or reduced to 53 or 24 bits
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 10
#elif (__LDBL_MANT_DIG__ == 106) && (__LDBL_MAX_EXP__ == 1024) // IBM extended double
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 16
#elif (__LDBL_MANT_DIG__ == 113) && (__LDBL_MAX_EXP__ == 16384) // IEEE 754 binary128
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 16
#elif (__LDBL_MANT_DIG__ == 237) && (__LDBL_MAX_EXP__ == 262144) // IEEE 754 binary256
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 32
#endif

#elif (__FLT_RADIX__ == 10)

#if (__FLT_MANT_DIG__ == 7) && (__FLT_MAX_EXP__ == 97) // IEEE 754 decimal32
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 4
#elif (__FLT_MANT_DIG__ == 16) && (__FLT_MAX_EXP__ == 385) // IEEE 754 decimal64
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 8
#elif (__FLT_MANT_DIG__ == 34) && (__FLT_MAX_EXP__ == 6145) // IEEE 754 decimal128
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 16
#endif

#if (__DBL_MANT_DIG__ == 7) && (__DBL_MAX_EXP__ == 97) // IEEE 754 decimal32
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 4
#elif (__DBL_MANT_DIG__ == 16) && (__DBL_MAX_EXP__ == 385) // IEEE 754 decimal64
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 8
#elif (__DBL_MANT_DIG__ == 34) && (__DBL_MAX_EXP__ == 6145) // IEEE 754 decimal128
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 16
#endif

#if (__LDBL_MANT_DIG__ == 7) && (__LDBL_MAX_EXP__ == 97) // IEEE 754 decimal32
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 4
#elif (__LDBL_MANT_DIG__ == 16) && (__LDBL_MAX_EXP__ == 385) // IEEE 754 decimal64
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 8
#elif (__LDBL_MANT_DIG__ == 34) && (__LDBL_MAX_EXP__ == 6145) // IEEE 754 decimal128
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 16
#endif

#endif

#else // defined(__FLT_RADIX__) ...

#include <cfloat>

#if (FLT_RADIX == 2)

#if (FLT_MANT_DIG == 11) && (FLT_MAX_EXP == 16) // IEEE 754 binary16
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 2
#elif (FLT_MANT_DIG == 24) && (FLT_MAX_EXP == 128) // IEEE 754 binary32
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 4
#elif (FLT_MANT_DIG == 53) && (FLT_MAX_EXP == 1024) // IEEE 754 binary64
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 8
#elif (FLT_MANT_DIG == 64 || FLT_MANT_DIG == 53 || FLT_MANT_DIG == 24) && (FLT_MAX_EXP == 16384) // x87 extended double, with full 64-bit significand or reduced to 53 or 24 bits
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 10
#elif (FLT_MANT_DIG == 106) && (FLT_MAX_EXP == 1024) // IBM extended double
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 16
#elif (FLT_MANT_DIG == 113) && (FLT_MAX_EXP == 16384) // IEEE 754 binary128
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 16
#elif (FLT_MANT_DIG == 237) && (FLT_MAX_EXP == 262144) // IEEE 754 binary256
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 32
#endif

#if (DBL_MANT_DIG == 11) && (DBL_MAX_EXP == 16) // IEEE 754 binary16
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 2
#elif (DBL_MANT_DIG == 24) && (DBL_MAX_EXP == 128) // IEEE 754 binary32
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 4
#elif (DBL_MANT_DIG == 53) && (DBL_MAX_EXP == 1024) // IEEE 754 binary64
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 8
#elif (DBL_MANT_DIG == 64 || DBL_MANT_DIG == 53 || DBL_MANT_DIG == 24) && (DBL_MAX_EXP == 16384) // x87 extended double, with full 64-bit significand or reduced to 53 or 24 bits
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 10
#elif (DBL_MANT_DIG == 106) && (DBL_MAX_EXP == 1024) // IBM extended double
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 16
#elif (DBL_MANT_DIG == 113) && (DBL_MAX_EXP == 16384) // IEEE 754 binary128
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 16
#elif (DBL_MANT_DIG == 237) && (DBL_MAX_EXP == 262144) // IEEE 754 binary256
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 32
#endif

#if (LDBL_MANT_DIG == 11) && (LDBL_MAX_EXP == 16) // IEEE 754 binary16
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 2
#elif (LDBL_MANT_DIG == 24) && (LDBL_MAX_EXP == 128) // IEEE 754 binary32
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 4
#elif (LDBL_MANT_DIG == 53) && (LDBL_MAX_EXP == 1024) // IEEE 754 binary64
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 8
#elif (LDBL_MANT_DIG == 64 || LDBL_MANT_DIG == 53 || LDBL_MANT_DIG == 24) && (LDBL_MAX_EXP == 16384) // x87 extended double, with full 64-bit significand or reduced to 53 or 24 bits
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 10
#elif (LDBL_MANT_DIG == 106) && (LDBL_MAX_EXP == 1024) // IBM extended double
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 16
#elif (LDBL_MANT_DIG == 113) && (LDBL_MAX_EXP == 16384) // IEEE 754 binary128
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 16
#elif (LDBL_MANT_DIG == 237) && (LDBL_MAX_EXP == 262144) // IEEE 754 binary256
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 32
#endif

#elif (FLT_RADIX == 10)

#if (FLT_MANT_DIG == 7) && (FLT_MAX_EXP == 97) // IEEE 754 decimal32
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 4
#elif (FLT_MANT_DIG == 16) && (FLT_MAX_EXP == 385) // IEEE 754 decimal64
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 8
#elif (FLT_MANT_DIG == 34) && (FLT_MAX_EXP == 6145) // IEEE 754 decimal128
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE 16
#endif

#if (DBL_MANT_DIG == 7) && (DBL_MAX_EXP == 97) // IEEE 754 decimal32
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 4
#elif (DBL_MANT_DIG == 16) && (DBL_MAX_EXP == 385) // IEEE 754 decimal64
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 8
#elif (DBL_MANT_DIG == 34) && (DBL_MAX_EXP == 6145) // IEEE 754 decimal128
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE 16
#endif

#if (LDBL_MANT_DIG == 7) && (LDBL_MAX_EXP == 97) // IEEE 754 decimal32
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 4
#elif (LDBL_MANT_DIG == 16) && (LDBL_MAX_EXP == 385) // IEEE 754 decimal64
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 8
#elif (LDBL_MANT_DIG == 34) && (LDBL_MAX_EXP == 6145) // IEEE 754 decimal128
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE 16
#endif

#endif

#endif // defined(__FLT_RADIX__) ...

// GCC and compatible compilers define internal macros with builtin type traits
#if defined(__SIZEOF_FLOAT__)
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT __SIZEOF_FLOAT__
#endif
#if defined(__SIZEOF_DOUBLE__)
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE __SIZEOF_DOUBLE__
#endif
#if defined(__SIZEOF_LONG_DOUBLE__)
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE __SIZEOF_LONG_DOUBLE__
#endif

#if !defined(BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT) || !defined(BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE) || !defined(BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE)

#define BOOST_ATOMIC_DETAIL_ALIGN_SIZE_TO_POWER_OF_2(x)\
    ((x) == 1u ? 1u : ((x) == 2u ? 2u : ((x) <= 4u ? 4u : ((x) <= 8u ? 8u : ((x) <= 16u ? 16u : ((x) <= 32u ? 32u : (x)))))))

// Make our best guess. These sizes may not be accurate, but they are good enough to estimate the size of the storage required to hold these types.
#if !defined(BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT) && defined(BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE)
#define BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT BOOST_ATOMIC_DETAIL_ALIGN_SIZE_TO_POWER_OF_2(BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE)
#endif
#if !defined(BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE) && defined(BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE)
#define BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE BOOST_ATOMIC_DETAIL_ALIGN_SIZE_TO_POWER_OF_2(BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE)
#endif
#if !defined(BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE) && defined(BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE)
#define BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE BOOST_ATOMIC_DETAIL_ALIGN_SIZE_TO_POWER_OF_2(BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE)
#endif

#endif // !defined(BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT) || !defined(BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE) || !defined(BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE)

#if !defined(BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT_VALUE) || !defined(BOOST_ATOMIC_DETAIL_SIZEOF_FLOAT) ||\
    !defined(BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE_VALUE) || !defined(BOOST_ATOMIC_DETAIL_SIZEOF_DOUBLE) ||\
    !defined(BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE_VALUE) || !defined(BOOST_ATOMIC_DETAIL_SIZEOF_LONG_DOUBLE)
#error Boost.Atomic: Failed to determine builtin floating point type sizes, the target platform is not supported. Please, report to the developers (patches are welcome).
#endif

#endif // BOOST_ATOMIC_DETAIL_FLOAT_SIZES_HPP_INCLUDED_
