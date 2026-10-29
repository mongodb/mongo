/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * The disaggregated block header is extensible: a writer may append fields, and a reader locates
 * the data using the size recorded in the header it is reading rather than the size it would write
 * itself. These tests pin that contract at the level of the offset arithmetic, so a header one
 * release older or newer than this build still resolves to the right first data byte.
 */
#include <catch2/catch.hpp>
#include <cstring>
#include <vector>

#include "../wrappers/mock_session.h"

extern "C" {
#include "wt_internal.h"
}

namespace {

/* The first data byte of a block, chosen so a misplaced read is visible rather than merely wrong.
 */
constexpr uint8_t k_data_marker = 0x5a;

/* Filler for the header region, distinct from the marker. */
constexpr uint8_t k_header_filler = 0xcd;

/*
 * The header records the page header and block header together, so a reader recovers this header's
 * own size by subtracting the page header.
 */
static uint8_t
combined(u_int block_header_size)
{
    return (uint8_t)(WT_PAGE_HEADER_SIZE + block_header_size);
}

/*
 * build_image --
 *     Lay out a block whose header claims the given combined size, with the marker at the first
 *     data position.
 */
static void
build_image(std::vector<uint8_t> &image, uint8_t combined_header_size, uint8_t magic)
{
    image.assign(WT_BLOCK_DISAGG_HEADER_MAX_COMBINED_SIZE + 16, k_header_filler);

    auto *blk = (WT_BLOCK_DISAGG_HEADER *)(image.data() + WT_PAGE_HEADER_SIZE);
    blk->magic = magic;
    blk->version = WT_BLOCK_DISAGG_VERSION;
    blk->compatible_version = WT_BLOCK_DISAGG_COMPATIBLE_VERSION;
    blk->combined_header_size = combined_header_size;

    image[combined_header_size] = k_data_marker;
}

/* Route the macro's block-manager call at the function under test. */
static u_int
stub_block_header_read(WT_BM *bm, WT_SESSION_IMPL *session, const void *dsk)
{
    return (__ut_bmd_block_header_read(bm, session, dsk));
}

struct block_header_size_fixture {
    std::shared_ptr<mock_session> mock;
    WT_SESSION_IMPL *session;
    WT_BM bm = {};

    block_header_size_fixture() : mock(mock_session::build_test_mock_session())
    {
        mock->setup_block_manager_file_operations();
        session = mock->get_wt_session_impl();

        bm.block_header_read_size = stub_block_header_read;
        S2BT(session)->bm = &bm;
    }

    ~block_header_size_fixture()
    {
        S2BT(session)->bm = nullptr;
    }
};

} // namespace

TEST_CASE_METHOD(block_header_size_fixture,
  "disagg block header size: readers use the header's own size", "[block_disagg]")
{
    std::vector<uint8_t> image;

    /*
     * A reader must resolve the data offset from the stored size for any header size it might
     * encounter: the oldest one still on disk, its own, and one a later release grew.
     */
    const u_int sizes[] = {WT_BLOCK_DISAGG_HEADER_MIN_SIZE, WT_BLOCK_DISAGG_HEADER_WRITE_SIZE,
      WT_BLOCK_DISAGG_HEADER_WRITE_SIZE + WT_BLOCK_DISAGG_HEADER_DEBUG_EXTRA_SIZE};

    for (u_int size : sizes) {
        build_image(image, combined(size), WT_BLOCK_DISAGG_MAGIC_BASE);

        REQUIRE(__ut_bmd_block_header_read(nullptr, session, image.data()) == size);

        /*
         * Independently of the reported size, the macro the read path actually uses has to land on
         * the byte the writer placed first.
         */
        const uint8_t *data =
          (const uint8_t *)WT_PAGE_HEADER_READ_BYTE(session, S2BT(session), image.data());
        REQUIRE(*data == k_data_marker);
    }
}

TEST_CASE_METHOD(block_header_size_fixture,
  "disagg block header size: a delta header resolves the same way", "[block_disagg]")
{
    std::vector<uint8_t> image;

    build_image(image, combined(WT_BLOCK_DISAGG_HEADER_WRITE_SIZE), WT_BLOCK_DISAGG_MAGIC_DELTA);
    REQUIRE(__ut_bmd_block_header_read(nullptr, session, image.data()) ==
      WT_BLOCK_DISAGG_HEADER_WRITE_SIZE);
}

#ifdef HAVE_DIAGNOSTIC
TEST_CASE_METHOD(block_header_size_fixture,
  "disagg block header size: the debug mode widens the header this build writes", "[block_disagg]")
{
    /*
     * The upgrade debug mode stands in for a future writer, so it has to widen the header and not
     * merely stamp a newer version. Nothing on the write path otherwise observes the size, so
     * without this the Python upgrade test passes whether or not the header actually grew.
     */
    S2C(session)->debug.disagg_block_header_upgrade =
      WT_CONN_DEBUG_DISAGG_BLOCK_HEADER_UPGRADE_NONE;
    REQUIRE(__wti_block_disagg_header_write_size(session) == WT_BLOCK_DISAGG_HEADER_WRITE_SIZE);

    for (auto mode : {WT_CONN_DEBUG_DISAGG_BLOCK_HEADER_UPGRADE_COMPATIBLE,
           WT_CONN_DEBUG_DISAGG_BLOCK_HEADER_UPGRADE_INCOMPATIBLE}) {
        S2C(session)->debug.disagg_block_header_upgrade = mode;
        REQUIRE(__wti_block_disagg_header_write_size(session) ==
          WT_BLOCK_DISAGG_HEADER_WRITE_SIZE + WT_BLOCK_DISAGG_HEADER_DEBUG_EXTRA_SIZE);
    }

    S2C(session)->debug.disagg_block_header_upgrade =
      WT_CONN_DEBUG_DISAGG_BLOCK_HEADER_UPGRADE_NONE;
}
#endif

TEST_CASE("disagg block header size: the bound admits growth", "[block_disagg]")
{
    /*
     * The bound is the span compression copies verbatim, which is also the span the block checksum
     * always covers. It has to leave room above what this build writes, or the format could not
     * grow at all.
     */
    REQUIRE(WT_BLOCK_DISAGG_HEADER_MAX_COMBINED_SIZE > WT_BLOCK_DISAGG_HEADER_WRITE_COMBINED_SIZE);

    /* The padded header the debug mode writes has to stay inside it. */
    REQUIRE(WT_PAGE_HEADER_SIZE + WT_BLOCK_DISAGG_HEADER_WRITE_SIZE +
        WT_BLOCK_DISAGG_HEADER_DEBUG_EXTRA_SIZE <=
      WT_BLOCK_DISAGG_HEADER_MAX_COMBINED_SIZE);
}

TEST_CASE("disagg block header size: the read path rejects sizes it cannot use", "[block_disagg]")
{
    /* A block large enough that only the header size under test decides the outcome. */
    constexpr uint32_t k_big_block = 4096;

    /* The oldest header still on disk, this build's, and a larger future one are all legal. */
    REQUIRE(
      __wt_block_disagg_header_size_valid(WT_BLOCK_DISAGG_HEADER_MIN_COMBINED_SIZE, k_big_block));
    REQUIRE(
      __wt_block_disagg_header_size_valid(WT_BLOCK_DISAGG_HEADER_WRITE_COMBINED_SIZE, k_big_block));
    REQUIRE(
      __wt_block_disagg_header_size_valid(WT_BLOCK_DISAGG_HEADER_MAX_COMBINED_SIZE, k_big_block));

    /* A header too small to hold the fields the reader has already used. */
    REQUIRE_FALSE(__wt_block_disagg_header_size_valid(
      WT_BLOCK_DISAGG_HEADER_MIN_COMBINED_SIZE - 1, k_big_block));
    REQUIRE_FALSE(__wt_block_disagg_header_size_valid(0, k_big_block));

    /* A header past the span compression leaves intact could not be read at all. */
    REQUIRE_FALSE(__wt_block_disagg_header_size_valid(
      WT_BLOCK_DISAGG_HEADER_MAX_COMBINED_SIZE + 1, k_big_block));
    REQUIRE_FALSE(__wt_block_disagg_header_size_valid(UINT8_MAX, k_big_block));

    /*
     * A header cannot extend past the block that carries it, whatever the format allows. The block
     * size is the binding limit here, not the format bound.
     */
    REQUIRE(__wt_block_disagg_header_size_valid(
      WT_BLOCK_DISAGG_HEADER_WRITE_COMBINED_SIZE, WT_BLOCK_DISAGG_HEADER_WRITE_COMBINED_SIZE));
    REQUIRE_FALSE(__wt_block_disagg_header_size_valid(
      WT_BLOCK_DISAGG_HEADER_WRITE_COMBINED_SIZE, WT_BLOCK_DISAGG_HEADER_WRITE_COMBINED_SIZE - 1));
    REQUIRE_FALSE(__wt_block_disagg_header_size_valid(WT_BLOCK_DISAGG_HEADER_MIN_COMBINED_SIZE, 0));
}
