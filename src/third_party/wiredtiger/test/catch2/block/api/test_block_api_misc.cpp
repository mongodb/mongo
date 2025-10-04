/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [block_api_misc]: block_mgr.c
 * This file unit tests the miscellaneous block manager API functions:
 *     - bm->addr_invalid
 *     - bm->addr_string
 *     - bm->block_header
 *     - bm->is_mapped
 *     - bm->size
 *     - bm->stat
 */

#include <catch2/catch.hpp>
#include <filesystem>
#include <string>

#include "wt_internal.h"
#include "../util_block.h"
#include "../utils_extlist.h"
#include "../../wrappers/mock_session.h"

const std::string ALLOCATION_SIZE = "256";
const std::string BLOCK_ALLOCATION = "best";
const std::string OS_CACHE_MAX = "0";
const std::string OS_CACHE_DIRTY_MAX = "0";
const std::string ACCESS_PATTERN = "random";
const std::string DEFAULT_FILE_NAME = "test.txt";

static void
check_bm_stats(WT_SESSION_IMPL *session, WT_BM *bm)
{
    WT_DSRC_STATS stats;
    REQUIRE(bm->stat(bm, session, &stats) == 0);
    CHECK(stats.allocation_size == bm->block->allocsize);
    CHECK(stats.block_checkpoint_size == (int64_t)bm->block->live.ckpt_size);
    CHECK(stats.block_magic == WT_BLOCK_MAGIC);
    CHECK(stats.block_major == WT_BLOCK_MAJOR_VERSION);
    CHECK(stats.block_minor == WT_BLOCK_MINOR_VERSION);
    CHECK(stats.block_reuse_bytes == (int64_t)bm->block->live.avail.bytes);
    CHECK(stats.block_size == bm->block->size);
}

static int
test_addr_invalid(WT_SESSION_IMPL *session, WT_BM *bm, wt_off_t pack_offset, uint32_t pack_size,
  uint32_t pack_checksum)
{
    // Generate an address cookie - technically, we shouldn't know about internal details of the
    // address cookie, but this allows for more rigorous testing with different inputs.
    uint8_t p[WT_ADDR_MAX_COOKIE], *pp;
    pp = p;
    REQUIRE(__wt_block_addr_pack(
              bm->block, &pp, WT_TIERED_OBJECTID_NONE, pack_offset, pack_size, pack_checksum) == 0);
    size_t addr_size = WT_PTRDIFF(pp, p);

    return (bm->addr_invalid(bm, session, p, addr_size));
}

// Test that the block manager's addr_string method produces the expected string representation.
static void
test_addr_string(WT_SESSION_IMPL *session, WT_BM *bm, wt_off_t pack_offset, uint32_t pack_size,
  uint32_t pack_checksum, std::string expected_str)
{
    // Initialize a buffer.
    WT_ITEM buf;
    WT_CLEAR(buf);

    // Generate an address cookie - technically, we shouldn't know about internal details of the
    // address cookie, but this allows for more rigorous testing with different inputs.
    uint8_t p[WT_ADDR_MAX_COOKIE], *pp;
    pp = p;
    REQUIRE(__wt_block_addr_pack(
              bm->block, &pp, WT_TIERED_OBJECTID_NONE, pack_offset, pack_size, pack_checksum) == 0);
    size_t addr_size = WT_PTRDIFF(pp, p);

    // Compare the string output of bm->addr_string against the known expected string.
    REQUIRE(bm->addr_string(bm, session, &buf, p, addr_size) == 0);
    CHECK(
      strcmp(reinterpret_cast<char *>(const_cast<void *>(buf.data)), expected_str.c_str()) == 0);

    __wt_free(nullptr, buf.data);
}

TEST_CASE("Block manager: addr invalid", "[block_api_misc]")
{
    // Build Mock session, this will automatically create a mock connection.
    std::shared_ptr<mock_session> session = mock_session::build_test_mock_session();

    WT_BM bm;
    WT_CLEAR(bm);
    // A session and block manager needs to be initialized otherwise the addr_invalid functionality
    // will crash if it attempts to check various session flags.
    auto path = std::filesystem::current_path();
    std::string file_path(path.string() + "/test.wt");
    setup_bm(session, &bm, file_path, ALLOCATION_SIZE, BLOCK_ALLOCATION, OS_CACHE_MAX,
      OS_CACHE_DIRTY_MAX, ACCESS_PATTERN);
    WT_SESSION_IMPL *s = session->get_wt_session_impl();

    SECTION("Test addr invalid with a valid address cookie containing non-zero values")
    {
        bm.block->allocsize = 1;
        bm.block->objectid = 1;
        bm.block->size = 1024;
        REQUIRE(test_addr_invalid(s, &bm, 512, 1024, 12345) == 0);
    }

    SECTION("Test addr invalid with a valid address cookie containing zero values")
    {
        bm.block->allocsize = 1;
        bm.block->objectid = 1;
        bm.block->size = 0;
        REQUIRE(test_addr_invalid(s, &bm, 0, 0, 0) == 0);
    }

    /*
     * FIXME-WT-13582: Enable once the __wt_panic functions doesn't assert anymore.
     * SECTION("Test addr invalid address with an invalid address")
     * {
     *   bm.block->allocsize = 1;
     *   bm.block->objectid = WT_TIERED_OBJECTID_NONE;
     *   bm.block->size = 1024;
     *   // Create a situation where the block is misplaced, meaning that its address is on the
     *   // available list.
     *   utils::off_size_expected test_off = {utils::off_size(512, 4096),
     *     {
     *       utils::off_size(512, 4096),
     *     }};
     *   REQUIRE(__ut_block_off_insert(s, &bm.block->live.avail, test_off.test_off_size.off,
     *             test_off.test_off_size.size) == 0);
     *
     *
     *   //Test that the block manager's addr_invalid method returns an error when checking if the
     *   //address cookie is valid.
     *   REQUIRE(test_addr_invalid(s, &bm, 512, 1024, 12345) == WT_PANIC);
     *
     * }
     */

    // Cleanup for block created during block manager initialization.
    REQUIRE(__wt_block_close(s, bm.block) == 0);
    REQUIRE(__wt_block_manager_drop(s, file_path.c_str(), false) == 0);
}

TEST_CASE("Block manager: addr string", "[block_api_misc]")
{
    // Build Mock session, this will automatically create a mock connection.
    std::shared_ptr<mock_session> session = mock_session::build_test_mock_session();

    WT_BM bm;
    WT_CLEAR(bm);
    auto path = std::filesystem::current_path();
    std::string file_path(path.string() + "/test.wt");
    setup_bm(session, &bm, file_path, ALLOCATION_SIZE, BLOCK_ALLOCATION, OS_CACHE_MAX,
      OS_CACHE_DIRTY_MAX, ACCESS_PATTERN);
    WT_SESSION_IMPL *s = session->get_wt_session_impl();

    SECTION("Test addr string with non-zero values")
    {
        // (512, 1024, 12345) -> (offset, size, checksum)
        test_addr_string(s, &bm, 512, 1024, 12345, "[0: 512-1536, 1024, 12345]");
    }

    SECTION("Test addr string with zero values")
    {
        // (0, 0, 0) -> (offset, size, checksum)
        test_addr_string(s, &bm, 0, 0, 0, "[0: 0-0, 0, 0]");
    }

    SECTION("Test addr string with zero size")
    {
        // (512, 0, 12345) -> (offset, size, checksum)
        test_addr_string(s, &bm, 512, 0, 12345, "[0: 0-0, 0, 0]");
    }

    REQUIRE(__wt_block_close(s, bm.block) == 0);
}

TEST_CASE("Block manager: block header", "[block_api_misc]")
{
    // Declare a block manager and set it up so that we can use its legal API methods.
    WT_BM bm;
    WT_CLEAR(bm);
    __wti_bm_method_set(&bm, false);

    SECTION("Test block header size is correct")
    {
        REQUIRE(bm.block_header(&bm) == (u_int)WT_BLOCK_HEADER_SIZE);
    }
}

TEST_CASE("Block manager: is mapped", "[block_api_misc]")
{
    // Declare a block manager and set it up so that we can use its legal API methods.
    WT_BM bm;
    WT_CLEAR(bm);
    __wti_bm_method_set(&bm, false);

    SECTION("Test block manager is mapped")
    {
        uint8_t i;
        bm.map = &i;
        REQUIRE(bm.is_mapped(&bm, nullptr) == true);
    }

    SECTION("Test block manager is not mapped")
    {
        bm.map = nullptr;
        REQUIRE(bm.is_mapped(&bm, nullptr) == false);
    }
}

TEST_CASE("Block manager: size and stat", "[block_api_misc]")
{
    // Build Mock session, this will automatically create a mock connection.
    std::shared_ptr<mock_session> session = mock_session::build_test_mock_session();

    WT_BM bm;
    WT_CLEAR(bm);
    auto path = std::filesystem::current_path();
    std::string file_path(path.string() + "/test.wt");
    setup_bm(session, &bm, file_path, ALLOCATION_SIZE, BLOCK_ALLOCATION, OS_CACHE_MAX,
      OS_CACHE_DIRTY_MAX, ACCESS_PATTERN);
    WT_SESSION_IMPL *s = session->get_wt_session_impl();

    SECTION("Test that the bm->stat method updates statistics correctly")
    {
        check_bm_stats(s, &bm);
    }

    SECTION("Test that the bm->data method updated statistics correctly after doing a write")
    {
        // Perform a write.
        WT_ITEM buf;
        WT_CLEAR(buf);
        std::string test_string("test123");
        create_write_buffer(&bm, session, test_string, &buf, 0, std::stoi(ALLOCATION_SIZE));
        uint8_t addr[WT_ADDR_MAX_COOKIE];
        size_t addr_size;
        wt_off_t bm_size, prev_size;
        REQUIRE(bm.size(&bm, s, &prev_size) == 0);
        REQUIRE(bm.write(&bm, s, &buf, NULL, addr, &addr_size, false, false) == 0);
        REQUIRE(bm.size(&bm, s, &bm_size) == 0);

        check_bm_stats(s, &bm);
        CHECK(bm_size > prev_size);
        __wt_buf_free(nullptr, &buf);
    }

    REQUIRE(__wt_block_close(s, bm.block) == 0);
}
