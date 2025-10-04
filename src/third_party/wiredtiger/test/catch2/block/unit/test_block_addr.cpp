/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [block_addr]: block_addr.c
 * This file unit tests block manager functions relating to packing and unpacking address cookies.
 */

#include <catch2/catch.hpp>
#include <iostream>
#include <vector>

#include "wt_internal.h"

void
unpack_addr_cookie_and_check(const uint8_t *packed, uint32_t block_allocsize, wt_off_t pack_offset,
  uint32_t pack_size, uint32_t pack_checksum)
{
    uint64_t offset = 0, size = 0, checksum = 0;
    REQUIRE(__wt_vunpack_uint(&packed, 0, &offset) == 0);
    REQUIRE(__wt_vunpack_uint(&packed, 0, &size) == 0);
    REQUIRE(__wt_vunpack_uint(&packed, 0, &checksum) == 0);

    // Adjust the unpacked values as the block manager also does this to avoid storing large
    // offsets.
    wt_off_t unpacked_offset = (size == 0) ? static_cast<wt_off_t>(offset) :
                                             static_cast<wt_off_t>(offset + 1) * block_allocsize;
    uint32_t unpacked_size =
      (size == 0) ? static_cast<uint32_t>(size) : static_cast<uint32_t>(size) * block_allocsize;
    uint32_t unpacked_checksum = static_cast<uint32_t>(checksum);

    if (pack_size != 0) {
        CHECK(unpacked_offset == pack_offset);
        CHECK(unpacked_size == pack_size);
        CHECK(unpacked_checksum == pack_checksum);
    } else {
        CHECK(unpacked_offset == 0);
        CHECK(unpacked_size == 0);
        CHECK(unpacked_checksum == 0);
    }
}

// Test the block manager's pack function.
static void
test_pack_addr_cookie(uint8_t *pp, WT_BLOCK *block, size_t *addr_size, wt_off_t pack_offset,
  uint32_t pack_size, uint32_t pack_checksum)
{
    const uint8_t *begin = pp;
    REQUIRE(__wt_block_addr_pack(
              block, &pp, WT_TIERED_OBJECTID_NONE, pack_offset, pack_size, pack_checksum) == 0);
    *addr_size = WT_PTRDIFF(pp, begin);
    unpack_addr_cookie_and_check(begin, block->allocsize, pack_offset, pack_size, pack_checksum);
}

// Check that the unpacked values of a block's address cookie match the values we expected to pack.
static void
check_block_addr(wt_off_t offset, wt_off_t pack_offset, uint32_t size, uint32_t pack_size,
  uint32_t checksum, uint32_t pack_checksum)
{
    if (size != 0) {
        CHECK(offset == pack_offset);
        CHECK(size == pack_size);
        CHECK(checksum == pack_checksum);
    } else {
        CHECK(offset == 0);
        CHECK(size == 0);
        CHECK(checksum == 0);
    }
}

// Test the block manager's unpack function.
static void
test_unpack_addr_cookie(const uint8_t *begin, WT_BLOCK *block, size_t addr_size,
  wt_off_t pack_offset, uint32_t pack_size, uint32_t pack_checksum)
{
    uint32_t checksum, obj_id, size;
    wt_off_t offset;
    REQUIRE(__wt_block_addr_unpack(
              nullptr, block, begin, addr_size, &obj_id, &offset, &size, &checksum) == 0);
    check_block_addr(offset, pack_offset, size, pack_size, checksum, pack_checksum);
}

static void
test_pack_and_unpack_addr_cookie(
  WT_BLOCK *block, wt_off_t pack_offset, uint32_t pack_size, uint32_t pack_checksum)
{
    uint8_t p[WT_ADDR_MAX_COOKIE], *pp;
    pp = p;

    size_t addr_size;

    test_pack_addr_cookie(pp, block, &addr_size, pack_offset, pack_size, pack_checksum);
    test_unpack_addr_cookie(pp, block, addr_size, pack_offset, pack_size, pack_checksum);
}

static void
test_pack_and_unpack_addr_cookie_manual(
  WT_BLOCK *block, std::vector<int64_t> cookie_vals, std::vector<uint8_t> &expected_packed_vals)
{
    std::vector<uint8_t> packed(24, 0);
    uint8_t *p = packed.data();

    // Save the location where the address cookie starts as the manual checks will move the pointer.
    const uint8_t *begin = p;
    REQUIRE(__wt_block_addr_pack(block, &p, WT_TIERED_OBJECTID_NONE,
              static_cast<wt_off_t>(cookie_vals[0]), static_cast<uint32_t>(cookie_vals[1]),
              static_cast<uint32_t>(cookie_vals[2])) == 0);
    CHECK(packed[0] == expected_packed_vals[0]);
    CHECK(packed[1] == expected_packed_vals[1]);
    CHECK(packed[2] == expected_packed_vals[2]);

    unpack_addr_cookie_and_check(begin, block->allocsize, static_cast<wt_off_t>(cookie_vals[0]),
      static_cast<uint32_t>(cookie_vals[1]), static_cast<uint32_t>(cookie_vals[2]));
}

TEST_CASE("Block manager: addr pack and unpack", "[block_addr]")
{
    WT_BLOCK b;
    WT_BM bm, *bmp;

    b.allocsize = 1;
    bmp = &bm;
    bmp->block = &b;

    // Test the block manager's pack function with an address cookie containing all zero fields.
    SECTION("Pack and unpack address cookie 1")
    {
        // (0, 0, 0) -> (offset, size, checksum)
        test_pack_and_unpack_addr_cookie(bmp->block, 0, 0, 0);
    }

    /*
     * Test that packing an address cookie of size 0 just packs 0 into all the fields. The pack
     * values will differ from the expected values (which are all 0), and the function checks for
     * this.
     */
    SECTION("Pack and unpack address cookie 2")
    {
        // (1, 0, 1) -> (offset, size, checksum)
        test_pack_and_unpack_addr_cookie(bmp->block, 1, 0, 1);
    }

    // Test packing an address cookie with mostly non-zero fields.
    SECTION("Pack and unpack address cookie 3")
    {
        // (10 ,4, 12345) -> (offset, size, checksum)
        test_pack_and_unpack_addr_cookie(bmp->block, 10, 4, 12345);
    }

    // Test the block manager's packing function against hardcoded values rather than relying on
    // the integer pack function.
    SECTION("Manually pack and unpack address cookie 4")
    {
        /*
         * The block manager will modify these values due to its logic that accounts for large
         * offsets. The address cookie values that will actually be packed with an allocsize of 1:
         * {7, 7, 42}.
         */
        std::vector<int64_t> cookie_vals = {8, 7, 42};
        std::vector<uint8_t> expected_packed_vals = {0x87, 0x87, 0xaa};
        test_pack_and_unpack_addr_cookie_manual(bmp->block, cookie_vals, expected_packed_vals);
    }

    // Test that trying to pack an address cookie with negative values exhibits weird behavior.
    SECTION("Pack and unpack address cookie with negative values")
    {
        std::vector<int64_t> cookie_vals = {-6, -42, -256};
        std::vector<uint8_t> expected_packed_vals = {0x79, 0x56, 0x3f, 0x40};

        std::vector<uint8_t> packed(24, 0);
        uint8_t *p = packed.data();
        REQUIRE(__wt_block_addr_pack(bmp->block, &p, WT_TIERED_OBJECTID_NONE,
                  static_cast<wt_off_t>(cookie_vals[0]), static_cast<uint32_t>(cookie_vals[1]),
                  static_cast<uint32_t>(cookie_vals[2])) == 0);
        CHECK(packed[0] != expected_packed_vals[0]);
        CHECK(packed[1] != expected_packed_vals[1]);
        CHECK(packed[2] != expected_packed_vals[2]);
    }
}
