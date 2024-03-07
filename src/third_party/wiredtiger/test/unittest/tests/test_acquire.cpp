/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"

#define TEST_TYPE(type)                               \
    TEST_CASE("Test acquiring a " #type, "[acquire]") \
    {                                                 \
        type a = 5;                                   \
        type *ap = &a;                                \
        type a_result;                                \
        WT_ACQUIRE_READ(a_result, *ap);               \
        REQUIRE(a == a_result);                       \
                                                      \
        a = 10;                                       \
        WT_ACQUIRE_READ_WITH_BARRIER(a_result, *ap);  \
        REQUIRE(a == a_result);                       \
    }

/*
 * We can't test that the specific instructions we want are being output, but having written this
 * test we found that clang failed to compile for sizes that weren't uint64_t without some
 * adjustments. So by compiling this test we also check that compilation works.
 *
 * This also helps verify the type checking in the acquire read macro, and that the barrier version
 * doesn't produce different results.
 */
TEST_TYPE(uint64_t);
TEST_TYPE(uint32_t);
TEST_TYPE(uint16_t);
TEST_TYPE(uint8_t);
