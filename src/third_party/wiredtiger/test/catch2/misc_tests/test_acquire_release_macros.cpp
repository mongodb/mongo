/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"

/* Typedefs for macros convenience. */
typedef uint64_t uint64;
typedef uint32_t uint32;
typedef uint16_t uint16;
typedef uint8_t uint8;

#define TEST_ACQUIRE_TYPE(type)                           \
    TEST_CASE("Test acquiring a " #type, "[acqrel]")      \
    {                                                     \
        type a = 5;                                       \
        type *ap = &a;                                    \
        type a_result;                                    \
        a_result = __wt_atomic_load_##type##_acquire(ap); \
        REQUIRE(a == a_result);                           \
                                                          \
        type b = 10;                                      \
        type *bp = &b;                                    \
        type b_result;                                    \
        WT_ACQUIRE_READ_WITH_BARRIER(b_result, *bp);      \
        REQUIRE(b == b_result);                           \
    }

#define TEST_RELEASE_TYPE(type, value)                 \
    TEST_CASE("Test releasing a " #type, "[acqrel]")   \
    {                                                  \
        type a;                                        \
        type *ap = &a;                                 \
        type a_set = value;                            \
        __wt_atomic_store_##type##_release(ap, a_set); \
        REQUIRE(a == a_set);                           \
                                                       \
        type b;                                        \
        type *bp = &b;                                 \
        type b_set = value;                            \
        WT_RELEASE_WRITE_WITH_BARRIER(*bp, b_set);     \
        REQUIRE(b == b_set);                           \
    }

/*
 * We can't test that the specific instructions we want are being output, but having written this
 * test we found that clang failed to compile for sizes that weren't uint64_t without some
 * adjustments. So by compiling this test we also check that compilation works.
 *
 * This also helps verify the type checking in the acquire read macro, and that the barrier version
 * doesn't produce different results.
 */
TEST_ACQUIRE_TYPE(uint64);
TEST_ACQUIRE_TYPE(uint32);
TEST_ACQUIRE_TYPE(uint16);
TEST_ACQUIRE_TYPE(uint8);

/*
 * Test each branch of the release macro. Use values that can only fit inside the type being tested
 * to make sure integer truncation doesn't occur.
 */
TEST_RELEASE_TYPE(uint64, UINT64_MAX);
TEST_RELEASE_TYPE(uint32, UINT32_MAX);
TEST_RELEASE_TYPE(uint16, UINT16_MAX);
TEST_RELEASE_TYPE(uint8, 1);

/*
 * We prevent users from releasing a value which isn't the same size as the type being released to.
 * If unchecked it could result in integer overflow, both signed and unsigned. However this creates
 * an issue when hash defined values are released, as they are assumed to be 8 bytes. We can work
 * around this by casting in the hash define.
 */
TEST_CASE("Demonstrate hash define int size workaround", "[acqrel]")
{
/* If we don't cast this value the test won't compile. */
#define TEST_VALUE ((int8_t)6)

    int8_t a;
    __wt_atomic_store_int8_release(&a, TEST_VALUE);

    REQUIRE(a == TEST_VALUE);
}
