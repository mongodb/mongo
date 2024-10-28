/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [block_other]: block.h
 * This file unit tests the __wt_block_header_byteswap, __wt_block_header_byteswap_copy and
 * __wt_block_eligible_for_sweep functions.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"

static void
test_block_header_byteswap_copy(WT_BLOCK_HEADER *from, WT_BLOCK_HEADER *to)
{
    WT_BLOCK_HEADER prev_from;

    // Save the original values before any potential byte re-orderings.
    prev_from.disk_size = from->disk_size;
    prev_from.checksum = from->checksum;

    __wt_block_header_byteswap_copy(from, to);

#ifdef WORDS_BIGENDIAN
    REQUIRE(to->checksum == __wt_bswap32(prev_from.checksum));
    REQUIRE(to->disk_size == __wt_bswap32(prev_from.disk_size));
#endif

    /*
     * Test that the block header we are copying from is not changed. The byte swap function is
     * allowed to swap blocks in-place, so only check this when we copy into a different block.
     */
    if (from != to) {
        REQUIRE(from->disk_size == prev_from.disk_size);
        REQUIRE(from->checksum == prev_from.checksum);
    }
}

TEST_CASE("Block manager: header byteswap copy", "[block_other]")
{
    WT_BLOCK_HEADER from, to;

    // Test the block header byteswap logic with non-zero values.
    SECTION("Test case 1")
    {
        from.disk_size = 12121;
        from.checksum = 24358;
        to.disk_size = to.checksum = 0;

        // Test using WiredTiger byte swap functions.
        test_block_header_byteswap_copy(&from, &to);

        // Test manually against known results.
#ifdef WORDS_BIGENDIAN
        // 12121 (59 2F 00 00) -> 1496252416 (00 00 2F 59).
        // 24358 (26 5F 00 00) -> 643760128 (00 00 5F 26).
        REQUIRE(to.disk_size == 1496252416);
        REQUIRE(to.checksum == 643760128);
#else
        // The byte contents are unchanged
        REQUIRE(to.disk_size == from.disk_size);
        REQUIRE(to.checksum == from.checksum);
#endif
    }

    // Test the block header byteswap logic with zero values.
    SECTION("Test case 2")
    {
        from.disk_size = from.checksum = to.disk_size = to.checksum = 0;

        // Test all zero values using WiredTiger byte swap functions.
        test_block_header_byteswap_copy(&from, &to);

        // Test manually against known results.
#ifdef WORDS_BIGENDIAN
        REQUIRE(to.disk_size == 0);
        REQUIRE(to.checksum == 0);
#else
        // The byte contents are unchanged
        REQUIRE(to.disk_size == from.disk_size);
        REQUIRE(to.checksum == from.checksum);
#endif
    }

    // Test the block header byteswap logic with non-zero values.
    SECTION("Test case 3")
    {
        from.disk_size = 28;
        from.checksum = 66666;
        to.disk_size = to.checksum = 0;

        // Test using WiredTiger byte swap functions.
        test_block_header_byteswap_copy(&from, &to);

        // Test manually against known results.
#ifdef WORDS_BIGENDIAN
        // 28 (00 00 00 1C) -> 469762048 (1C 00 00 00).
        // 66666 (00 01 04 6A) -> 1778647296 (6A 04 01 00).
        REQUIRE(to.disk_size == 469762048);
        REQUIRE(to.checksum == 1778647296);
#else
        // The byte contents are unchanged
        REQUIRE(to.disk_size == from.disk_size);
        REQUIRE(to.checksum == from.checksum);
#endif
    }

    // Test the block header byteswap logic with values containing all non-zero bytes.
    SECTION("Test case 4")
    {
        from.disk_size = 440156763;
        from.checksum = 2024418449;
        to.disk_size = to.checksum = 0;

        // Test using WiredTiger byte swap functions.
        test_block_header_byteswap_copy(&from, &to);

        // Test manually against known results.
#ifdef WORDS_BIGENDIAN
        // 440156763 (1A 3C 42 5B) -> 1531067418 (5B 42 3C 1A).
        // 2024418449 (78 AA 2C 91) -> 2435623544 (91 2C AA 78).
        REQUIRE(to.disk_size == 1531067418);
        REQUIRE(to.checksum == 2435623544);
#else
        // The byte contents are unchanged
        REQUIRE(to.disk_size == from.disk_size);
        REQUIRE(to.checksum == from.checksum);
#endif
    }
}

TEST_CASE("Block manager: block header byteswap", "[block_other]")
{
    WT_BLOCK_HEADER to;
    to.disk_size = 12121;
    to.checksum = 24358;

    /*
     * The block manager function that does the byte swap in place simply calls the same function
     * used for byte swap copy and simply passes in the same struct as both the 'to' and 'from'
     * headers.
     */
    test_block_header_byteswap_copy(&to, &to);

    // Test manually against known results.
#ifdef WORDS_BIGENDIAN
    // 12121 (59 2F 00 00) -> 1496252416 (00 00 2F 59).
    // 24358 (26 5F 00 00) -> 643760128 (00 00 5F 26).
    REQUIRE(to.disk_size == 1496252416);
    REQUIRE(to.checksum == 643760128);
#else
    // The byte contents are unchanged
    REQUIRE(to.disk_size == 12121);
    REQUIRE(to.checksum == 24358);
#endif
}

TEST_CASE("Block manager: block eligible for sweep", "[block_other]")
{
    WT_BLOCK block;
    WT_BM bm;

    SECTION("Block is local")
    {
        block.remote = false;
        block.objectid = 0;
        bm.max_flushed_objectid = 0;

        // Test that blocks that have been flushed are eligible for sweep.
        REQUIRE(__wt_block_eligible_for_sweep(&bm, &block) == true);

        // Test that blocks that haven't been flushed should not be eligible for sweep.
        block.objectid = 1;
        REQUIRE(__wt_block_eligible_for_sweep(&bm, &block) == false);
    }

    SECTION("Block is remote")
    {
        block.remote = true;
        block.objectid = 0;
        bm.max_flushed_objectid = 0;

        // Only local blocks need to be swept.
        REQUIRE(__wt_block_eligible_for_sweep(&bm, &block) == false);
    }
}
