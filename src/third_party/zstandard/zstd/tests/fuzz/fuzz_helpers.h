/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

/**
 * Helper functions for fuzzing.
 */

#ifndef FUZZ_HELPERS_H
#define FUZZ_HELPERS_H

#include "debug.h"
#include "fuzz.h"
#include "xxhash.h"
#include "zstd.h"
#include "fuzz_data_producer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define FUZZ_QUOTE_IMPL(str) #str
#define FUZZ_QUOTE(str) FUZZ_QUOTE_IMPL(str)

/**
 * Asserts for fuzzing that are always enabled.
 */
#define FUZZ_ASSERT_MSG(cond, msg)                                             \
  ((cond) ? (void)0                                                            \
          : (fprintf(stderr, "%s: %u: Assertion: `%s' failed. %s\n", __FILE__, \
                     __LINE__, FUZZ_QUOTE(cond), (msg)),                       \
             abort()))
#define FUZZ_ASSERT(cond) FUZZ_ASSERT_MSG((cond), "");
#define FUZZ_ZASSERT(code)                                                     \
  FUZZ_ASSERT_MSG(!ZSTD_isError(code), ZSTD_getErrorName(code))

#if defined(__GNUC__)
#define FUZZ_STATIC static __inline __attribute__((unused))
#elif defined(__cplusplus) ||                                                  \
    (defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 199901L) /* C99 */)
#define FUZZ_STATIC static inline
#elif defined(_MSC_VER)
#define FUZZ_STATIC static __inline
#else
#define FUZZ_STATIC static
#endif

/**
 * malloc except return NULL for zero sized data and FUZZ_ASSERT
 * that malloc doesn't fail.
 */
void* FUZZ_malloc(size_t size);

/**
 * malloc except returns random pointer for zero sized data and FUZZ_ASSERT
 * that malloc doesn't fail.
 */
void* FUZZ_malloc_rand(size_t size,  FUZZ_dataProducer_t *producer);

/**
 * memcmp but accepts NULL.
 */
int FUZZ_memcmp(void const* lhs, void const* rhs, size_t size);

#ifdef __cplusplus
}
#endif

#endif
