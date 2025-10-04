/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "wiredtiger.h"
#include "wt_internal.h"

TEST_CASE("Error: simple macros and inline functions - WT_TRET", "[error]")
{
    WT_DECL_RET;

    SECTION("ret = 0")
    {
        WT_TRET(0);
        REQUIRE(ret == 0);
    }

    SECTION("ret = 0, try to set to WT_PANIC")
    {
        WT_TRET(WT_PANIC);
        REQUIRE(ret == WT_PANIC);
    }

    SECTION("ret = 0, try to set to WT_RUN_RECOVERY")
    {
        WT_TRET(WT_RUN_RECOVERY);
        REQUIRE(ret == WT_RUN_RECOVERY);
    }

    SECTION("ret = WT_ERROR, try to set to WT_PANIC")
    {
        ret = WT_ERROR;
        WT_TRET(WT_PANIC);
        REQUIRE(ret == WT_PANIC);
    }

    SECTION("ret = WT_ERROR, try to set to WT_RUN_RECOVERY but will stay unchanged")
    {
        ret = WT_ERROR;
        WT_TRET(WT_RUN_RECOVERY);
        REQUIRE(ret == WT_ERROR);
    }

    SECTION("ret = WT_PANIC, try to set to WT_RUN_RECOVERY but will stay unchanged")
    {
        ret = WT_PANIC;
        WT_TRET(WT_RUN_RECOVERY);
        REQUIRE(ret == WT_PANIC);
    }

    SECTION("ret = WT_DUPLICATE_KEY, try to set to WT_RUN_RECOVERY")
    {
        ret = WT_DUPLICATE_KEY;
        WT_TRET(WT_RUN_RECOVERY);
        REQUIRE(ret == WT_RUN_RECOVERY);
    }

    SECTION("ret = WT_NOTFOUND, try to set to WT_RUN_RECOVERY")
    {
        ret = WT_NOTFOUND;
        WT_TRET(WT_RUN_RECOVERY);
        REQUIRE(ret == WT_RUN_RECOVERY);
    }

    SECTION("ret = WT_RESTART, try to set to WT_RUN_RECOVERY")
    {
        ret = WT_RESTART;
        WT_TRET(WT_RUN_RECOVERY);
        REQUIRE(ret == WT_RUN_RECOVERY);
    }
}

TEST_CASE("Error: simple macros and inline functions - WT_TRET_ERROR_OK", "[error]")
{
    WT_DECL_RET;

    SECTION("ret = 0")
    {
        WT_TRET_ERROR_OK(0, WT_CACHE_FULL);
        REQUIRE(ret == 0);
    }

    SECTION("ret = 0, WT_CACHE_FULL ok")
    {
        WT_TRET_ERROR_OK(WT_CACHE_FULL, WT_CACHE_FULL);
        REQUIRE(ret == 0);
    }

    SECTION("ret = 0, try to set to WT_PANIC")
    {
        WT_TRET_ERROR_OK(WT_PANIC, WT_CACHE_FULL);
        REQUIRE(ret == WT_PANIC);
    }

    SECTION("ret = 0, try to set to WT_RUN_RECOVERY")
    {
        WT_TRET_ERROR_OK(WT_RUN_RECOVERY, WT_CACHE_FULL);
        REQUIRE(ret == WT_RUN_RECOVERY);
    }

    SECTION("ret = WT_ERROR, try to set to WT_PANIC")
    {
        ret = WT_ERROR;
        WT_TRET_ERROR_OK(WT_PANIC, WT_CACHE_FULL);
        REQUIRE(ret == WT_PANIC);
    }

    SECTION("ret = WT_ERROR, try to set to WT_RUN_RECOVERY but will stay unchanged")
    {
        ret = WT_ERROR;
        WT_TRET_ERROR_OK(WT_RUN_RECOVERY, WT_CACHE_FULL);
        REQUIRE(ret == WT_ERROR);
    }

    SECTION("ret = WT_PANIC, try to set to WT_RUN_RECOVERY but will stay unchanged")
    {
        ret = WT_PANIC;
        WT_TRET_ERROR_OK(WT_RUN_RECOVERY, WT_CACHE_FULL);
        REQUIRE(ret == WT_PANIC);
    }

    SECTION("ret = WT_DUPLICATE_KEY, try to set to WT_RUN_RECOVERY")
    {
        ret = WT_DUPLICATE_KEY;
        WT_TRET_ERROR_OK(WT_RUN_RECOVERY, WT_CACHE_FULL);
        REQUIRE(ret == WT_RUN_RECOVERY);
    }

    SECTION("ret = WT_NOTFOUND, try to set to WT_RUN_RECOVERY")
    {
        ret = WT_NOTFOUND;
        WT_TRET_ERROR_OK(WT_RUN_RECOVERY, WT_CACHE_FULL);
        REQUIRE(ret == WT_RUN_RECOVERY);
    }

    SECTION("ret = WT_RESTART, try to set to WT_RUN_RECOVERY")
    {
        ret = WT_RESTART;
        WT_TRET_ERROR_OK(WT_RUN_RECOVERY, WT_CACHE_FULL);
        REQUIRE(ret == WT_RUN_RECOVERY);
    }
}
