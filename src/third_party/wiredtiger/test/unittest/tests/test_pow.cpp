/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"

TEST_CASE("Power functions: log2_int", "[pow]")
{
    // This is mathematically wrong, but given the unsigned return type, about
    // the best that can be expected.
    REQUIRE(__wt_log2_int(0) == 0);

    REQUIRE(__wt_log2_int(1) == 0);
    REQUIRE(__wt_log2_int(2) == 1);
    REQUIRE(__wt_log2_int(4) == 2);
    REQUIRE(__wt_log2_int(8) == 3);
    REQUIRE(__wt_log2_int(16) == 4);
    REQUIRE(__wt_log2_int(32) == 5);

    REQUIRE(__wt_log2_int(0x10000000) == 28);
    REQUIRE(__wt_log2_int(0x20000000) == 29);
    REQUIRE(__wt_log2_int(0x40000000) == 30);

    REQUIRE(__wt_log2_int(0x80000000 - 1) == 30);
    REQUIRE(__wt_log2_int(0x80000000) == 31);
    REQUIRE(__wt_log2_int(0x80000000 + 1) == 31);

    REQUIRE(__wt_log2_int(0xffffffff) == 31);
}

TEST_CASE("Power functions: ispo2", "[pow]")
{
    // This is mathematically wrong, but makes sense for how it's used.
    REQUIRE(__wt_ispo2(0) == true);

    REQUIRE(__wt_ispo2(1) == true);
    REQUIRE(__wt_ispo2(2) == true);
    REQUIRE(__wt_ispo2(3) == false);
    REQUIRE(__wt_ispo2(4) == true);
    REQUIRE(__wt_ispo2(6) == false);
    REQUIRE(__wt_ispo2(8) == true);
    REQUIRE(__wt_ispo2(16) == true);
    REQUIRE(__wt_ispo2(32) == true);

    REQUIRE(__wt_ispo2(0x10000000) == true);
    REQUIRE(__wt_ispo2(0x20000000) == true);
    REQUIRE(__wt_ispo2(0x40000000) == true);

    REQUIRE(__wt_ispo2(0x80000000 - 1) == false);
    REQUIRE(__wt_ispo2(0x80000000) == true);
    REQUIRE(__wt_ispo2(0x80000000 + 1) == false);

    REQUIRE(__wt_ispo2(0xffffffff) == false);
}

TEST_CASE("Power functions: rduppo2", "[pow]")
{
    // Expected valid calls, where the 2nd param is a power of two.
    REQUIRE(__wt_rduppo2(0, 8) == 0);
    REQUIRE(__wt_rduppo2(1, 8) == 8);
    REQUIRE(__wt_rduppo2(9, 8) == 16);
    REQUIRE(__wt_rduppo2(24, 8) == 24);
    REQUIRE(__wt_rduppo2(42, 8) == 48);

    REQUIRE(__wt_rduppo2(0, 32) == 0);
    REQUIRE(__wt_rduppo2(1, 32) == 32);
    REQUIRE(__wt_rduppo2(24, 32) == 32);
    REQUIRE(__wt_rduppo2(42, 32) == 64);
    REQUIRE(__wt_rduppo2(42, 128) == 128);

    // Expected invalid calls, where the 2nd param is NOT a power of two,
    // and therefore the return value should be 0.
    REQUIRE(__wt_rduppo2(1, 7) == 0);
    REQUIRE(__wt_rduppo2(1, 42) == 0);
    REQUIRE(__wt_rduppo2(102, 42) == 0);
}
