/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [block_session_ext]: block_session.c
 * The block manager extent list consists of both extent and size type blocks. This unit test
 * suite tests aims to test all of the allocation and frees of the extent block functions.
 *
 * The block session manages an internal caching mechanism for both block and size blocks that are
 * created or discarded.
 */
#include "wt_internal.h"
#include <catch2/catch.hpp>
#include "../wrappers/mock_session.h"

void
validate_ext_block(WT_EXT *ext)
{
    REQUIRE(ext != nullptr);
    REQUIRE(ext->depth != 0);
    REQUIRE(ext->size == 0);
    REQUIRE(ext->off == 0);

    for (int i = 0; i < ext->depth; i++) {
        REQUIRE(ext->next[i + ext->depth] == nullptr);
    }
}

void
free_ext_block(WT_EXT *ext)
{
    __wt_free(nullptr, ext);
}

void
validate_ext_list(WT_BLOCK_MGR_SESSION *bms, int expected_items)
{
    REQUIRE(bms != nullptr);
    if (bms->ext_cache_cnt == 0)
        REQUIRE(bms->ext_cache == nullptr);

    REQUIRE(bms->ext_cache_cnt == expected_items);
    WT_EXT *curr = bms->ext_cache;
    for (int i = 0; i < expected_items; i++) {
        validate_ext_block(curr);
        curr = curr->next[0];
    }
    REQUIRE(curr == nullptr);
}
void
validate_and_free_ext_block(WT_EXT *ext)
{
    validate_ext_block(ext);
    free_ext_block(ext);
}

void
free_ext_list(WT_BLOCK_MGR_SESSION *bms)
{
    WT_EXT *curr = bms->ext_cache;
    while (curr != nullptr) {
        WT_EXT *tmp = curr;
        curr = curr->next[0];
        __wt_free(nullptr, tmp);
    }
}

void
validate_and_free_ext_list(WT_BLOCK_MGR_SESSION *bms, int expected_items)
{
    validate_ext_list(bms, expected_items);
    free_ext_list(bms);
}

TEST_CASE("Block session: __block_ext_alloc", "[block_session_ext]")
{
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();
    __wt_random_init(&session->getWtSessionImpl()->rnd);

    WT_EXT *ext = nullptr;
    REQUIRE(__ut_block_ext_alloc(session->getWtSessionImpl(), &ext) == 0);
    validate_and_free_ext_block(ext);
}

TEST_CASE("Block session: __block_ext_prealloc", "[block_session_ext]")
{
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();
    WT_BLOCK_MGR_SESSION *bms = session->setupBlockManagerSession();

    SECTION("Allocate zero extent blocks")
    {
        REQUIRE(__ut_block_ext_prealloc(session->getWtSessionImpl(), 0) == 0);
        validate_and_free_ext_list(bms, 0);
    }

    SECTION("Allocate one extent block")
    {
        REQUIRE(__ut_block_ext_prealloc(session->getWtSessionImpl(), 1) == 0);
        validate_and_free_ext_list(bms, 1);
    }

    SECTION("Allocate multiple extent blocks")
    {
        REQUIRE(__ut_block_ext_prealloc(session->getWtSessionImpl(), 3) == 0);
        validate_and_free_ext_list(bms, 3);
    }

    SECTION("Allocate blocks on existing cache")
    {
        REQUIRE(__ut_block_ext_prealloc(session->getWtSessionImpl(), 3) == 0);
        validate_ext_list(bms, 3);

        REQUIRE(__ut_block_ext_prealloc(session->getWtSessionImpl(), 0) == 0);
        validate_ext_list(bms, 3);

        REQUIRE(__ut_block_ext_prealloc(session->getWtSessionImpl(), 2) == 0);
        validate_ext_list(bms, 3);

        REQUIRE(__ut_block_ext_prealloc(session->getWtSessionImpl(), 5) == 0);
        validate_and_free_ext_list(bms, 5);
    }
}

TEST_CASE("Block session: __wti_block_ext_alloc", "[block_session_ext]")
{
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();

    SECTION("Allocate with null block manager session and no extent cache")
    {
        std::shared_ptr<MockSession> session_test_bm = MockSession::buildTestMockSession();

        WT_EXT *ext;
        REQUIRE(__wti_block_ext_alloc(session_test_bm->getWtSessionImpl(), &ext) == 0);
        validate_and_free_ext_block(ext);

        REQUIRE(__wti_block_ext_alloc(session->getWtSessionImpl(), &ext) == 0);
        validate_and_free_ext_block(ext);
    }

    WT_BLOCK_MGR_SESSION *bms = session->setupBlockManagerSession();
    SECTION("Allocate with fake zero cache extent count")
    {
        WT_EXT *ext;

        REQUIRE(__wti_block_ext_alloc(session->getWtSessionImpl(), &ext) == 0);
        // Construct extent cache with one item.
        bms->ext_cache = ext;
        bms->ext_cache_cnt = 0;

        WT_EXT *cached_ext;
        REQUIRE(__wti_block_ext_alloc(session->getWtSessionImpl(), &cached_ext) == 0);
        REQUIRE(cached_ext == ext);
        validate_and_free_ext_list(bms, 0);
        validate_and_free_ext_block(ext);
    }

    SECTION("Allocate with one extent in cache")
    {
        WT_EXT *ext;

        REQUIRE(__wti_block_ext_alloc(session->getWtSessionImpl(), &ext) == 0);
        // Construct extent cache with one item with junk next.
        uint64_t addr = 0xdeadbeef;
        for (int i = 0; i < ext->depth; i++)
            ext->next[i + ext->depth] = reinterpret_cast<WT_EXT *>(addr);
        bms->ext_cache = ext;
        bms->ext_cache_cnt = 1;

        WT_EXT *cached_ext;
        REQUIRE(__wti_block_ext_alloc(session->getWtSessionImpl(), &cached_ext) == 0);
        REQUIRE(cached_ext == ext);
        validate_and_free_ext_block(ext);
    }

    SECTION("Allocate with two extents in cache ")
    {
        WT_EXT *ext, *ext2;
        REQUIRE(__wti_block_ext_alloc(session->getWtSessionImpl(), &ext) == 0);
        REQUIRE(__wti_block_ext_alloc(session->getWtSessionImpl(), &ext2) == 0);

        // Construct extent cache with two items.
        ext->next[0] = ext2;
        bms->ext_cache = ext;
        bms->ext_cache_cnt = 2;

        WT_EXT *cached_ext;
        REQUIRE(__wti_block_ext_alloc(session->getWtSessionImpl(), &cached_ext) == 0);
        REQUIRE(ext == cached_ext);
        REQUIRE(ext2 != cached_ext);
        validate_and_free_ext_list(bms, 1);
        validate_and_free_ext_block(cached_ext);
    }
}

TEST_CASE("Block session: __wti_block_ext_free", "[block_session_ext]")
{
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();
    WT_BLOCK_MGR_SESSION *bms = session->setupBlockManagerSession();

    /*
     * FIXME-WT-13451: Update __wti_block_ext_free function to test that block is set to null
     * SECTION("Free with null block manager session")
     * {
     *    std::shared_ptr<MockSession> session_no_bm = MockSession::buildTestMockSession();
     *    WT_EXT *ext;
     *
     *    REQUIRE(__ut_block_ext_alloc(session_no_bm->getWtSessionImpl(), &ext) == 0);
     *    REQUIRE(ext != nullptr);
     *
     *    __wti_block_ext_free(session_no_bm->getWtSessionImpl(), ext);
     *    REQUIRE(ext == nullptr);
     * }
     */

    SECTION("Calling free with cache")
    {
        WT_EXT *ext;
        REQUIRE(__ut_block_ext_alloc(session->getWtSessionImpl(), &ext) == 0);

        __wti_block_ext_free(session->getWtSessionImpl(), ext);

        REQUIRE(ext != nullptr);
        REQUIRE(bms->ext_cache == ext);
        validate_ext_list(bms, 1);

        WT_EXT *ext2;
        REQUIRE(__ut_block_ext_alloc(session->getWtSessionImpl(), &ext2) == 0);
        __wti_block_ext_free(session->getWtSessionImpl(), ext2);

        REQUIRE(ext != nullptr);
        REQUIRE(bms->ext_cache == ext2);
        REQUIRE(bms->ext_cache->next[0] == ext);
        validate_and_free_ext_list(bms, 2);
    }
}

TEST_CASE("Block session: __block_ext_discard", "[block_session_ext]")
{
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();
    WT_BLOCK_MGR_SESSION *bms = session->setupBlockManagerSession();

    WT_EXT *ext, *ext2, *ext3;
    REQUIRE(__wti_block_ext_alloc(session->getWtSessionImpl(), &ext) == 0);
    REQUIRE(__wti_block_ext_alloc(session->getWtSessionImpl(), &ext2) == 0);
    REQUIRE(__wti_block_ext_alloc(session->getWtSessionImpl(), &ext3) == 0);

    // Construct extent cache with three items.
    ext2->next[0] = ext3;
    ext->next[0] = ext2;
    bms->ext_cache = ext;
    bms->ext_cache_cnt = 3;
    SECTION("Discard every item in extent list with 0 max items in the cache")
    {
        REQUIRE(__ut_block_ext_discard(session->getWtSessionImpl(), 0) == 0);
        validate_and_free_ext_list(bms, 0);
    }

    SECTION("Discard until only one item with 1 max item in extent list")
    {
        REQUIRE(__ut_block_ext_discard(session->getWtSessionImpl(), 1) == 0);

        validate_and_free_ext_list(bms, 1);
    }

    SECTION("Discard nothing in the extent list because cache already has 3 items")
    {
        REQUIRE(__ut_block_ext_discard(session->getWtSessionImpl(), 3) == 0);
        validate_and_free_ext_list(bms, 3);
    }

    SECTION("Fake cache count and discard everything in extent list with 0 max items in cache")
    {
        bms->ext_cache_cnt = 4;
        REQUIRE(__ut_block_ext_discard(session->getWtSessionImpl(), 0) == WT_ERROR);
    }
}
