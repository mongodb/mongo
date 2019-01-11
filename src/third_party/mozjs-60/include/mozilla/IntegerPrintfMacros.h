/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Implements the C99 <inttypes.h> interface. */

#ifndef mozilla_IntegerPrintfMacros_h_
#define mozilla_IntegerPrintfMacros_h_

/*
 * These macros should not be used with the NSPR printf-like functions or their
 * users.  If you need to use NSPR's facilities, see the comment on
 * supported formats at the top of nsprpub/pr/include/prprf.h.
 */

/*
 * scanf is a footgun: if the input number exceeds the bounds of the target
 * type, behavior is undefined (in the compiler sense: that is, this code
 * could overwrite your hard drive with zeroes):
 *
 *   uint8_t u;
 *   sscanf("256", "%" SCNu8, &u); // BAD
 *
 * For this reason, *never* use the SCN* macros provided by this header!
 */

#include <inttypes.h>

/*
 * Fix up Android's broken [u]intptr_t inttype macros. Android's PRI*PTR
 * macros are defined as "ld", but sizeof(long) is 8 and sizeof(intptr_t)
 * is 4 on 32-bit Android. TestTypeTraits.cpp asserts that these new macro
 * definitions match the actual type sizes seen at compile time.
 */
#if defined(ANDROID) && !defined(__LP64__)
#  undef  PRIdPTR      /* intptr_t  */
#  define PRIdPTR "d"  /* intptr_t  */
#  undef  PRIiPTR      /* intptr_t  */
#  define PRIiPTR "i"  /* intptr_t  */
#  undef  PRIoPTR      /* uintptr_t */
#  define PRIoPTR "o"  /* uintptr_t */
#  undef  PRIuPTR      /* uintptr_t */
#  define PRIuPTR "u"  /* uintptr_t */
#  undef  PRIxPTR      /* uintptr_t */
#  define PRIxPTR "x"  /* uintptr_t */
#  undef  PRIXPTR      /* uintptr_t */
#  define PRIXPTR "X"  /* uintptr_t */
#endif

/*
 * Fix up Android's broken macros for [u]int_fastN_t. On ARM64, Android's
 * PRI*FAST16/32 macros are defined as "d", but the types themselves are defined
 * as long and unsigned long.
 */
#if defined(ANDROID) && defined(__LP64__)
#  undef  PRIdFAST16         /* int_fast16_t */
#  define PRIdFAST16 PRId64  /* int_fast16_t */
#  undef  PRIiFAST16         /* int_fast16_t */
#  define PRIiFAST16 PRIi64  /* int_fast16_t */
#  undef  PRIoFAST16         /* uint_fast16_t */
#  define PRIoFAST16 PRIo64  /* uint_fast16_t */
#  undef  PRIuFAST16         /* uint_fast16_t */
#  define PRIuFAST16 PRIu64  /* uint_fast16_t */
#  undef  PRIxFAST16         /* uint_fast16_t */
#  define PRIxFAST16 PRIx64  /* uint_fast16_t */
#  undef  PRIXFAST16         /* uint_fast16_t */
#  define PRIXFAST16 PRIX64  /* uint_fast16_t */
#  undef  PRIdFAST32         /* int_fast32_t */
#  define PRIdFAST32 PRId64  /* int_fast32_t */
#  undef  PRIiFAST32         /* int_fast32_t */
#  define PRIiFAST32 PRIi64  /* int_fast32_t */
#  undef  PRIoFAST32         /* uint_fast32_t */
#  define PRIoFAST32 PRIo64  /* uint_fast32_t */
#  undef  PRIuFAST32         /* uint_fast32_t */
#  define PRIuFAST32 PRIu64  /* uint_fast32_t */
#  undef  PRIxFAST32         /* uint_fast32_t */
#  define PRIxFAST32 PRIx64  /* uint_fast32_t */
#  undef  PRIXFAST32         /* uint_fast32_t */
#  define PRIXFAST32 PRIX64  /* uint_fast32_t */
#endif

#endif  /* mozilla_IntegerPrintfMacros_h_ */
