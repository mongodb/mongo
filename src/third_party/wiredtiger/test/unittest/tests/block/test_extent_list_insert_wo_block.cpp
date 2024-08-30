/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * block_ext.c: [extent_list2] Test extent list functions part 2. (More to come.)
 *
 * Test insert functions without block: __block_ext_insert, and __block_off_insert.
 */

#include <algorithm>
#include <catch2/catch.hpp>
#include <memory>

#include "test_util.h"
#include "../utils.h"
#include "../utils_extlist.h"
#include "../wrappers/mock_session.h"
#include "wt_internal.h"

using namespace utils;

TEST_CASE("Extent Lists: block_ext_insert", "[extent_list2]")
{
    /* Build Mock session, this will automatically create a mock connection. */
    std::shared_ptr<MockSession> mock_session = MockSession::buildTestMockSession();
    WT_SESSION_IMPL *session = mock_session->getWtSessionImpl();

    std::vector<WT_EXT **> stack(WT_SKIP_MAXDEPTH, nullptr);

    SECTION("insert into an empty list has one element")
    {
        BREAK;
        /* Setup. */
        WT_EXTLIST extlist = {};

        /* Test. */
        /* Insert one extent. */
        WT_EXT *first = alloc_new_ext(session, 4096, 4096);
        /* Call. */
        REQUIRE(__ut_block_ext_insert(session, &extlist, first) == 0);

        extlist_print_off(extlist);

        /* Verify. */
        REQUIRE(__ut_block_off_srch_last(&extlist.off[0], &stack[0]) == extlist.off[0]);

        /* Cleanup. */
        extlist_free(session, extlist);
    }

    SECTION("insert multiple extents and retrieve in correct order")
    {
        BREAK;
        /* Tests and expected values. */
        std::vector<off_size_expected> test_list{
          {off_size(3 * 4096, 4096), // [12,288, 16,383] Second.
            {
              off_size(3 * 4096, 4096), // [12,288, 16,383].
            }},
          {off_size(4096, 4096), // [4,096, 8,191] First.
            {
              off_size(4096, 4096),     // [4,096, 8,191].
              off_size(3 * 4096, 4096), // [12,288, 16,383].
            }},
          {off_size(5 * 4096, 4096), // [20,480, 24,575] Third.
            {
              off_size(4096, 4096),     // [4,096, 8,191].
              off_size(3 * 4096, 4096), // [12,288, 16,383].
              off_size(5 * 4096, 4096), // [20,480, 24,575].
            }},
        };

        /* Setup. */
        WT_EXTLIST extlist = {};

        /* Test. */
        /* Insert extents and verify. */
        int idx = 0;
        for (const off_size_expected &test : test_list) {
            WT_EXT *insert_ext = alloc_new_ext(session, test._off_size);
            /* Call. */
            REQUIRE(__ut_block_ext_insert(session, &extlist, insert_ext) == 0);

            INFO("After " << idx << ". Insert: {off " << std::showbase << test._off_size._off
                          << ", size " << test._off_size._size << ", end " << test._off_size.end()
                          << '}');
            extlist_print_off(extlist);

            /* Verify. */
            verify_off_extent_list(extlist, test._expected_list, true);
            ++idx;
        }

        /* Cleanup. */
        extlist_free(session, extlist);
    }
}

TEST_CASE("Extent Lists: block_off_insert", "[extent_list2]")
{
    /* Build Mock session, this will automatically create a mock connection. */
    std::shared_ptr<MockSession> mock_session = MockSession::buildTestMockSession();
    WT_SESSION_IMPL *session = mock_session->getWtSessionImpl();

    std::vector<WT_EXT **> stack(WT_SKIP_MAXDEPTH, nullptr);

    SECTION("insert into an empty list has one element")
    {
        BREAK;
        /* Setup. */
        WT_EXTLIST extlist = {};

        /* Test. */
        /* Insert one extent. */
        /* Call. */
        REQUIRE(__ut_block_off_insert(session, &extlist, 4096, 4096) == 0);

        extlist_print_off(extlist);

        /* Verify. */
        REQUIRE(__ut_block_off_srch_last(&extlist.off[0], &stack[0]) == extlist.off[0]);

        /* Cleanup. */
        extlist_free(session, extlist);
    }

    SECTION("insert multiple extents and retrieve in correct order")
    {
        BREAK;
        /* Tests and expected values. */
        std::vector<off_size_expected> test_list{
          {off_size(3 * 4096, 4096), // [12,288, 16,383] Second.
            {
              off_size(3 * 4096, 4096), // [12,288, 16,383].
            }},
          {off_size(4096, 4096), // [4,096, 8,191] First.
            {
              off_size(4096, 4096),     // [4,096, 8,191].
              off_size(3 * 4096, 4096), // [12,288, 16,383].
            }},
          {off_size(5 * 4096, 4096), // [20,480, 24,575] Third.
            {
              off_size(4096, 4096),     // [4,096, 8,191].
              off_size(3 * 4096, 4096), // [12,288, 16,383].
              off_size(5 * 4096, 4096), // [20,480, 24,575].
            }},
        };

        /* Setup. */
        WT_EXTLIST extlist = {};

        /* Test. */
        /* Insert extents and verify. */
        int idx = 0;
        for (const off_size_expected &test : test_list) {
            /* Call. */
            REQUIRE(__ut_block_off_insert(
                      session, &extlist, test._off_size._off, test._off_size._size) == 0);

            INFO("After " << idx << ". Insert: {off " << std::showbase << test._off_size._off
                          << ", size " << test._off_size._size << ", end " << test._off_size.end()
                          << '}');
            extlist_print_off(extlist);

            /* Verify. */
            verify_off_extent_list(extlist, test._expected_list, true);
            ++idx;
        }

        /* Cleanup. */
        extlist_free(session, extlist);
    }
}
