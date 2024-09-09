/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [block_file]: block_open.c
 * The block manager extent list consists of both extent blocks and size blocks. This unit test
 * suite tests aims to test all of the allocation and frees of the extent and size block functions.
 *
 * The block session manages an internal caching mechanism for both block and size blocks that are
 * created or discarded.
 */
#include <catch2/catch.hpp>
#include <iostream>
#include "wt_internal.h"
#include "../wrappers/mock_session.h"
#include "../wrappers/config_parser.h"

const std::string ALLOCATION_SIZE = "512";
const std::string BLOCK_ALLOCATION = "best";
const std::string OS_CACHE_MAX = "0";
const std::string OS_CACHE_DIRTY_MAX = "0";
const std::string ACCESS_PATTERN = "random";
const std::string DEFAULT_FILE_NAME = "test.txt";

// Added function declarations to allow default values to be set.
void validate_block_fh(WT_BLOCK *block, std::string const &name);
void validate_block_config(WT_BLOCK *block, config_parser const &cp);
void validate_block(std::shared_ptr<MockSession> session, WT_BLOCK *block, config_parser const &cp,
  int expected_ref, std::string const &name, bool readonly = false);
void validate_free_block(std::shared_ptr<MockSession> session, WT_BLOCK *block,
  config_parser const &cp, int expected_ref, std::string const &name, bool readonly = false);

void
validate_block_fh(WT_BLOCK *block, std::string const &name)
{
    REQUIRE(block->fh != nullptr);
    CHECK(std::string(block->fh->name) == name);
    CHECK(block->fh->file_type == WT_FS_OPEN_FILE_TYPE_DATA);
    CHECK(block->fh->ref == 1);
}

void
validate_block_config(WT_BLOCK *block, config_parser const &cp)
{
    CHECK(block->allocsize == std::stoi(cp.get_config_value("allocation_size")));
    uint32_t expected_block_allocation = cp.get_config_value("block_allocation") == "best" ? 0 : 1;
    CHECK(block->allocfirst == expected_block_allocation);
    CHECK(block->os_cache_max == std::stoi(cp.get_config_value("os_cache_max")));
    CHECK(block->os_cache_dirty_max == std::stoi(cp.get_config_value("os_cache_dirty_max")));
}

void
validate_block(std::shared_ptr<MockSession> session, WT_BLOCK *block, config_parser const &cp,
  int expected_ref, std::string const &name, bool readonly)
{

    REQUIRE(block != nullptr);

    // Test block immediate members.
    CHECK(std::string(block->name) == name);
    CHECK(block->objectid == WT_TIERED_OBJECTID_NONE);
    CHECK(block->compact_session_id == WT_SESSION_ID_INVALID);
    CHECK(block->ref == expected_ref);
    CHECK(block->readonly == readonly);
    CHECK(block->created_during_backup == false);
    CHECK(block->extend_len == 0);

    // Test block file handle members.
    validate_block_fh(block, name);
    CHECK(std::string(block->live_lock.name) == std::string("block manager"));
    CHECK(static_cast<bool>(block->live_lock.initialized) == true);

    // Test block configuration members.
    validate_block_config(block, cp);

    // Connection block lock should not be locked after the function completes.
    WT_CONNECTION_IMPL *conn = session->getMockConnection()->getWtConnectionImpl();
    CHECK(static_cast<bool>(conn->block_lock.initialized) == true);
    CHECK(conn->block_lock.session_id != session->getWtSessionImpl()->id);
}

void
validate_free_block(std::shared_ptr<MockSession> session, WT_BLOCK *block, config_parser const &cp,
  int expected_ref, std::string const &name, bool readonly)
{
    WT_CONNECTION_IMPL *conn = session->getMockConnection()->getWtConnectionImpl();
    if (expected_ref == 0) {
        // FIXME-WT-13467: Enable once free function adheres to WiredTiger free pattern.
        // REQUIRE(block == nullptr);

        uint64_t hash = __wt_hash_city64(name.c_str(), name.length());
        uint64_t bucket = hash & (conn->hash_size - 1);
        TAILQ_FOREACH (block, &conn->blockhash[bucket], hashq)
            REQUIRE(std::string(block->name) != name);
    } else {
        REQUIRE(block != nullptr);
        validate_block(session, block, cp, expected_ref, DEFAULT_FILE_NAME, readonly);
        block->sync_on_checkpoint = false;
    }

    // Connection block lock should not be locked after the function completes.
    REQUIRE(static_cast<bool>(conn->block_lock.initialized) == true);
    REQUIRE(conn->block_lock.session_id != session->getWtSessionImpl()->id);
}

TEST_CASE("Block: __wt_block_open and __wti_bm_close_block", "[block_file]")
{
    /* Build Mock session, this will automatically create a mock connection. */
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();
    config_parser cp({{"allocation_size", ALLOCATION_SIZE}, {"block_allocation", BLOCK_ALLOCATION},
      {"os_cache_max", OS_CACHE_MAX}, {"os_cache_dirty_max", OS_CACHE_DIRTY_MAX},
      {"access_pattern_hint", ACCESS_PATTERN}});

    REQUIRE((session->getMockConnection()->setupBlockManager(session->getWtSessionImpl())) == 0);

    SECTION("Test block open and block close with default configuration")
    {
        WT_BLOCK *block = nullptr;
        REQUIRE(
          (__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
            WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false, 0, &block)) == 0);
        validate_block(session, block, cp, 1, DEFAULT_FILE_NAME);

        // Test already made item in hashmap.
        WT_BLOCK *block2 = nullptr;
        REQUIRE(
          (__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
            WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false, 0, &block2)) == 0);
        validate_block(session, block2, cp, 2, DEFAULT_FILE_NAME);

        // Test block close, frees the block correctly.
        REQUIRE(__wti_bm_close_block(session->getWtSessionImpl(), block2) == 0);
        validate_free_block(session, block2, cp, 1, DEFAULT_FILE_NAME);

        REQUIRE(__wti_bm_close_block(session->getWtSessionImpl(), block) == 0);
        validate_free_block(session, block, cp, 0, DEFAULT_FILE_NAME);
    }

    SECTION("Test the configuration of allocation size")
    {
        // Test that argument allocation size should be priority over configuration string.
        WT_BLOCK *block = nullptr;
        REQUIRE((__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
                  WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false, 1024,
                  &block)) == 0);
        // Changing configuration here for validation purposes.
        cp.insert_config("allocation_size", "1024");
        validate_block(session, block, cp, 1, DEFAULT_FILE_NAME);

        REQUIRE(__wti_bm_close_block(session->getWtSessionImpl(), block) == 0);
        validate_free_block(session, block, cp, 0, DEFAULT_FILE_NAME);

        // Test that no allocation size in configuration should fail.
        REQUIRE(cp.erase_config("allocation_size"));
        REQUIRE((__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
                  WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false, 0,
                  &block)) == WT_NOTFOUND);
        REQUIRE(block == nullptr);
    }

    SECTION("Test block_allocation configuration")
    {
        // Test that block allocation is configured to first.
        cp.insert_config("block_allocation", "first");
        WT_BLOCK *block = nullptr;
        REQUIRE(
          (__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
            WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false, 0, &block)) == 0);
        validate_block(session, block, cp, 1, DEFAULT_FILE_NAME);

        REQUIRE(__wti_bm_close_block(session->getWtSessionImpl(), block) == 0);
        validate_free_block(session, block, cp, 0, DEFAULT_FILE_NAME);

        // If block allocation is set to garbage, it should default back to "best".
        cp.insert_config("block_allocation", "garbage");
        REQUIRE((__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
                  WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false, 512,
                  &block)) == 0);
        // Changing configuration here for validation purposes.
        cp.insert_config("block_allocation", "best");
        validate_block(session, block, cp, 1, DEFAULT_FILE_NAME);

        REQUIRE(__wti_bm_close_block(session->getWtSessionImpl(), block) == 0);
        validate_free_block(session, block, cp, 0, DEFAULT_FILE_NAME);

        // Test when block allocation is not configured.
        REQUIRE(cp.erase_config("block_allocation"));
        REQUIRE((__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
                  WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false, 0,
                  &block)) == WT_NOTFOUND);
        REQUIRE(block == nullptr);
    }

    SECTION("Test os_cache_max and os_cache_dirty_max configuration")
    {
        // Test when os_cache_max is not configured.
        WT_BLOCK *block = nullptr;
        REQUIRE(cp.erase_config("os_cache_max"));
        REQUIRE((__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
                  WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false, 0,
                  &block)) == WT_NOTFOUND);
        REQUIRE(block == nullptr);

        // Test when os_cache_max is configured to 512.
        cp.insert_config("os_cache_max", "512");
        REQUIRE(
          (__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
            WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false, 0, &block)) == 0);
        validate_block(session, block, cp, 1, DEFAULT_FILE_NAME);

        REQUIRE(__wti_bm_close_block(session->getWtSessionImpl(), block) == 0);
        validate_free_block(session, block, cp, 0, DEFAULT_FILE_NAME);

        // Test when os_cache_dirty_max is not configured.
        REQUIRE(cp.erase_config("os_cache_dirty_max"));
        REQUIRE((__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
                  WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false, 0,
                  &block)) == WT_NOTFOUND);
        REQUIRE(block == nullptr);

        // Test when os_cache_dirty_max is configured to 512.
        cp.insert_config("os_cache_dirty_max", "512");
        REQUIRE(
          (__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
            WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, false, false, 0, &block)) == 0);
        validate_block(session, block, cp, 1, DEFAULT_FILE_NAME);

        REQUIRE(__wti_bm_close_block(session->getWtSessionImpl(), block) == 0);
        validate_free_block(session, block, cp, 0, DEFAULT_FILE_NAME);
    }

    SECTION("Test block open with read only configuration")
    {
        WT_BLOCK *block = nullptr;
        REQUIRE(
          (__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
            WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, true, false, 0, &block)) == 0);
        validate_block(session, block, cp, 1, DEFAULT_FILE_NAME, true);

        REQUIRE(__wti_bm_close_block(session->getWtSessionImpl(), block) == 0);
        validate_free_block(session, block, cp, 0, DEFAULT_FILE_NAME);
    }
    /*
     * FIXME-WT-13504: Enable once segmentation fault is fixed.
     * SECTION("Test block close with nullptr")
     * {
     *     REQUIRE(__wti_bm_close_block(session->getWtSessionImpl(), nullptr) == 0);
     * }
     */

    SECTION("Test block close with block sync")
    {
        WT_BLOCK *block = nullptr;
        REQUIRE(
          (__wt_block_open(session->getWtSessionImpl(), DEFAULT_FILE_NAME.c_str(),
            WT_TIERED_OBJECTID_NONE, cp.get_config_array(), false, true, false, 0, &block)) == 0);
        validate_block(session, block, cp, 1, DEFAULT_FILE_NAME, true);
        block->sync_on_checkpoint = true;

        REQUIRE(__wti_bm_close_block(session->getWtSessionImpl(), block) == 0);
        validate_free_block(session, block, cp, 0, DEFAULT_FILE_NAME);
    }
}
