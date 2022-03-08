/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "wt_internal.h"
#include "../wrappers/mock_session.h"
#include "../wrappers/block_mods.h"

TEST_CASE("Block helper: __wt_rduppo2", "[block]")
{
    // Expected valid calls, where the 2nd param is a power of two
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
    // and therefore the return value should be 0
    REQUIRE(__wt_rduppo2(1, 7) == 0);
    REQUIRE(__wt_rduppo2(1, 42) == 0);
    REQUIRE(__wt_rduppo2(102, 42) == 0);
}

static void
test_ckpt_add_blkmod_entry(wt_off_t offset, wt_off_t len, uint64_t expectedBits)
{
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();
    BlockMods blockMods;
    blockMods.getWTBlockMods()->granularity = 1;

    REQUIRE(blockMods.getWTBlockMods()->nbits == 0);
    REQUIRE(blockMods.getWTBlockMods()->bitstring.memsize == 0);
    REQUIRE(blockMods.getWTBlockMods()->bitstring.mem == nullptr);
    REQUIRE(blockMods.getWTBlockMods()->bitstring.data == nullptr);

    int result = __ut_ckpt_add_blkmod_entry(
      session->getWtSessionImpl(), blockMods.getWTBlockMods(), offset, len);
    REQUIRE(result == 0);

    REQUIRE(blockMods.getWTBlockMods()->nbits == expectedBits);
    REQUIRE(blockMods.getWTBlockMods()->bitstring.memsize == (expectedBits / 8));
    REQUIRE(blockMods.getWTBlockMods()->bitstring.mem != nullptr);
    REQUIRE(blockMods.getWTBlockMods()->bitstring.data != nullptr);
}

TEST_CASE("Block manager: __ckpt_add_blkmod_entry", "[block]")
{
    // Use an offset greater than 128 so that we go beyond the minimum value defined by
    // WT_BLOCK_MODS_LIST_MIN
    test_ckpt_add_blkmod_entry(132, 9, 192);

    // Edge case, this should just fit in 256 bits
    test_ckpt_add_blkmod_entry(255, 1, 256);

    // This case relies on the "+ 1" introduced in WT-6366 in __ckpt_add_blkmod_entry.
    // Without it, this test would fail as it would only allocate 256 bits.
    // We expect an extra 8 bytes (64 bits to be added), 256 + 64 = 320;
    test_ckpt_add_blkmod_entry(256, 1, 320);
}
