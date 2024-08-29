/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [block_session_size]: block_session.c
 * The block manager extent list consists of both extent blocks and size blocks. This unit test
 * suite tests aims to test all of the allocation and frees of the size block functions.
 *
 * The block session manages an internal caching mechanism for both block and size blocks that are
 * created or discarded.
 */
#include "wt_internal.h"
#include <catch2/catch.hpp>
#include "../wrappers/mock_session.h"

void
validate_size_block(WT_SIZE *size)
{
    REQUIRE(size != nullptr);
    REQUIRE(size->depth == 0);
    REQUIRE(size->off[0] == nullptr);
    REQUIRE(size->size == 0);
}

void
free_size_block(WT_SIZE *size)
{
    __wt_free(nullptr, size);
}

void
validate_and_free_size_block(WT_SIZE *size)
{
    validate_size_block(size);
    free_size_block(size);
}

void
free_size_list(WT_BLOCK_MGR_SESSION *bms)
{
    WT_SIZE *curr = bms->sz_cache;
    while (curr != nullptr) {
        WT_SIZE *tmp = curr;
        curr = curr->next[0];
        __wt_free(nullptr, tmp);
    }
}
void
validate_size_list(WT_BLOCK_MGR_SESSION *bms, int expected_items)
{
    REQUIRE(bms != nullptr);
    if (bms->sz_cache_cnt == 0)
        REQUIRE(bms->sz_cache == nullptr);

    REQUIRE(bms->sz_cache_cnt == expected_items);
    WT_SIZE *curr = bms->sz_cache;
    for (int i = 0; i < expected_items; i++) {
        validate_size_block(curr);
        curr = curr->next[0];
    }
    REQUIRE(curr == nullptr);
}

void
validate_and_free_size_list(WT_BLOCK_MGR_SESSION *bms, int expected_items)
{
    validate_size_list(bms, expected_items);
    free_size_list(bms);
}

TEST_CASE("Block session: __block_size_alloc", "[block_session_size]")
{
    WT_SIZE *sz = nullptr;

    REQUIRE(__ut_block_size_alloc(nullptr, &sz) == 0);
    validate_and_free_size_block(sz);
}

TEST_CASE("Block session: __block_size_prealloc", "[block_session_size]")
{
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();
    WT_BLOCK_MGR_SESSION *bms = session->setupBlockManagerSession();

    SECTION("Allocate zero size blocks")
    {
        REQUIRE(__ut_block_size_prealloc(session->getWtSessionImpl(), 0) == 0);
        validate_and_free_size_list(bms, 0);
    }

    SECTION("Allocate one size block")
    {
        REQUIRE(__ut_block_size_prealloc(session->getWtSessionImpl(), 1) == 0);
        validate_and_free_size_list(bms, 1);
    }

    SECTION("Allocate multiple size blocks")
    {
        REQUIRE(__ut_block_size_prealloc(session->getWtSessionImpl(), 3) == 0);
        validate_and_free_size_list(bms, 3);
    }

    SECTION("Allocate blocks on existing cache")
    {
        REQUIRE(__ut_block_size_prealloc(session->getWtSessionImpl(), 3) == 0);
        validate_size_list(bms, 3);

        REQUIRE(__ut_block_size_prealloc(session->getWtSessionImpl(), 0) == 0);
        validate_size_list(bms, 3);

        REQUIRE(__ut_block_size_prealloc(session->getWtSessionImpl(), 2) == 0);
        validate_size_list(bms, 3);

        REQUIRE(__ut_block_size_prealloc(session->getWtSessionImpl(), 5) == 0);
        validate_and_free_size_list(bms, 5);
    }
}

TEST_CASE("Block session: __wti_block_size_alloc with NULL block manager", "[block_session_size]")
{
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();

    SECTION("Allocate with null block manager session and no size cache")
    {
        std::shared_ptr<MockSession> session_no_bm = MockSession::buildTestMockSession();
        WT_SIZE *sz = nullptr;

        REQUIRE(__wti_block_size_alloc(session_no_bm->getWtSessionImpl(), &sz) == 0);
        validate_and_free_size_block(sz);

        REQUIRE(__wti_block_size_alloc(session->getWtSessionImpl(), &sz) == 0);
        validate_and_free_size_block(sz);
    }
}

TEST_CASE("Block session: __wti_block_size_alloc with block manager", "[block_session_size]")
{
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();
    WT_BLOCK_MGR_SESSION *bms = session->setupBlockManagerSession();

    WT_SIZE *sz = nullptr;
    REQUIRE(__wti_block_size_alloc(session->getWtSessionImpl(), &sz) == 0);

    // Construct extent cache with one item.
    bms->sz_cache = sz;
    bms->sz_cache_cnt = 1;

    // Fake the cache count, the function should protect the count from becoming negative and still
    // return the cached size.
    SECTION("Fake the cache size count to 0")
    {
        bms->sz_cache_cnt = 0;
        WT_SIZE *cached_sz = nullptr;
        REQUIRE(__wti_block_size_alloc(session->getWtSessionImpl(), &cached_sz) == 0);
        // If a size is in the cache, the function should be returning the cached size.
        REQUIRE(cached_sz == sz);
        validate_and_free_size_list(bms, 0);
        validate_and_free_size_block(sz);
    }

    // Modify the cache size to a junk next, the function should clear the next reference and
    // return the cached size.
    SECTION("Modify the existing size to junk next")
    {
        // Modify extent with junk next.
        uint64_t addr = 0xdeadbeef;
        for (int i = 0; i < sz->depth; i++)
            sz->next[i + sz->depth] = reinterpret_cast<WT_SIZE *>(addr);

        WT_SIZE *cached_sz = nullptr;
        REQUIRE(__wti_block_size_alloc(session->getWtSessionImpl(), &cached_sz) == 0);
        // If a size is in the cache, the function should be returning the cached size.
        REQUIRE(cached_sz == sz);
        validate_and_free_size_block(sz);
    }

    // With two items in the cache, ensure we return the correct one.
    SECTION("Test with two sizes in the constructed cache")
    {
        WT_SIZE *sz2 = nullptr;
        // Point cache to nullptr first otherwise function will be fetching the cached size.
        bms->sz_cache = nullptr;
        REQUIRE(__wti_block_size_alloc(session->getWtSessionImpl(), &sz2) == 0);
        // Construct extent cache with two items.
        sz->next[0] = sz2;
        bms->sz_cache = sz;
        bms->sz_cache_cnt = 2;

        WT_SIZE *cached_sz = nullptr;
        REQUIRE(__wti_block_size_alloc(session->getWtSessionImpl(), &cached_sz) == 0);
        // The first size should be in the cache, the function should be returning the first
        // size.
        REQUIRE(sz == cached_sz);
        REQUIRE(sz2 != cached_sz);
        validate_and_free_size_list(bms, 1);
        validate_and_free_size_block(sz);
    }
}

TEST_CASE("Block session: __wti_block_size_free", "[block_session_size]")
{
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();
    WT_BLOCK_MGR_SESSION *bms = session->setupBlockManagerSession();

    /*
     * FIXME-WT-13451: Update __wti_block_size_free function to test that block is set to null
     * SECTION("Free with null block manager session -- needs discussion")
     * {
     *   std::shared_ptr<MockSession> session_no_bm = MockSession::buildTestMockSession();
     *   WT_SIZE *sz;
     *
     *   REQUIRE(__ut_block_size_alloc(session_no_bm->getWtSessionImpl(), &sz) == 0);
     *   REQUIRE(sz != nullptr);
     *
     *   __wti_block_size_free(session_no_bm->getWtSessionImpl(), sz);
     *
     *   REQUIRE(sz == nullptr);
     * }
     */

    SECTION("Calling free with cache")
    {
        WT_SIZE *sz = nullptr;
        REQUIRE(__ut_block_size_alloc(session->getWtSessionImpl(), &sz) == 0);

        __wti_block_size_free(session->getWtSessionImpl(), sz);

        REQUIRE(sz != nullptr);
        REQUIRE(bms->sz_cache == sz);
        validate_size_list(bms, 1);

        WT_SIZE *sz2 = nullptr;
        REQUIRE(__ut_block_size_alloc(session->getWtSessionImpl(), &sz2) == 0);
        __wti_block_size_free(session->getWtSessionImpl(), sz2);

        REQUIRE(sz != nullptr);
        REQUIRE(bms->sz_cache == sz2);
        REQUIRE(bms->sz_cache->next[0] == sz);
        validate_and_free_size_list(bms, 2);
    }
}

TEST_CASE("Block session: __block_size_discard", "[block_session_size]")
{
    std::shared_ptr<MockSession> session = MockSession::buildTestMockSession();
    WT_BLOCK_MGR_SESSION *bms = session->setupBlockManagerSession();

    WT_SIZE *sz = nullptr, *sz2 = nullptr, *sz3 = nullptr;
    REQUIRE(__wti_block_size_alloc(session->getWtSessionImpl(), &sz) == 0);
    REQUIRE(__wti_block_size_alloc(session->getWtSessionImpl(), &sz2) == 0);
    REQUIRE(__wti_block_size_alloc(session->getWtSessionImpl(), &sz3) == 0);

    // Construct size cache with three items.
    sz2->next[0] = sz3;
    sz->next[0] = sz2;
    bms->sz_cache = sz;
    bms->sz_cache_cnt = 3;

    SECTION("Discard every item in size list with 0 max items in the cache")
    {
        REQUIRE(__ut_block_size_discard(session->getWtSessionImpl(), 0) == 0);
        validate_and_free_size_list(bms, 0);
    }

    SECTION("Discard until only one item with 1 max item in size list")
    {
        REQUIRE(__ut_block_size_discard(session->getWtSessionImpl(), 1) == 0);
        validate_and_free_size_list(bms, 1);
    }

    SECTION("Discard nothing in the size list because cache already has 3 items")
    {
        REQUIRE(__ut_block_size_discard(session->getWtSessionImpl(), 3) == 0);
        validate_and_free_size_list(bms, 3);
    }

    SECTION("Fake cache count and discard every item in size list")
    {
        bms->sz_cache_cnt = 4;
        REQUIRE(__ut_block_size_discard(session->getWtSessionImpl(), 0) == WT_ERROR);
    }
}
