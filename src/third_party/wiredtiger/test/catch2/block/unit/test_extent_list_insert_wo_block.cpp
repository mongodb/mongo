/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [extent_list]: block_ext.c
 * Test extent list functions part 2.
 *
 * Test insert functions without block: __block_ext_insert, and __block_off_insert.
 */

#include <algorithm>
#include <catch2/catch.hpp>
#include <memory>

#include "test_util.h"
#include "wt_internal.h"
#include "../utils_extlist.h"
#include "../../utils.h"
#include "../../wrappers/mock_session.h"

using namespace utils;

TEST_CASE("Extent Lists: block_ext_insert", "[extent_list]")
{
    /* Build Mock session, this will automatically create a mock connection. */
    std::shared_ptr<mock_session> mock_session = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = mock_session->get_wt_session_impl();

    std::vector<WT_EXT **> stack(WT_SKIP_MAXDEPTH, nullptr);

    /* Common setup. */
    WT_EXTLIST extlist = {};

    SECTION("insert into an empty list has one element")
    {
        BREAK;
        /* Test. */
        /* Insert one extent. */
        WT_EXT *first = alloc_new_ext(session, 4096, 4096);
        /* Call. */
        REQUIRE(__ut_block_ext_insert(session, &extlist, first) == 0);

        extlist_print_off(extlist);

        /* Verify. */
        REQUIRE(__ut_block_off_srch_last(&extlist.off[0], &stack[0]) == extlist.off[0]);
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

        /* Test. */
        /* Insert extents and verify. */
        int idx = 0;
        for (const off_size_expected &test : test_list) {
            WT_EXT *insert_ext = alloc_new_ext(session, test.test_off_size);
            /* Call. */
            REQUIRE(__ut_block_ext_insert(session, &extlist, insert_ext) == 0);

            INFO("After " << idx << ". Insert: " << &test.test_off_size);
            extlist_print_off(extlist);

            /* Verify. */
            verify_off_extent_list(extlist, test.expected_list, true);
            ++idx;
        }
    }

    /* Common cleanup. */
    extlist_free(session, extlist);
}

TEST_CASE("Extent Lists: block_off_insert", "[extent_list]")
{
    /* Build Mock session, this will automatically create a mock connection. */
    std::shared_ptr<mock_session> mock_session = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = mock_session->get_wt_session_impl();

    std::vector<WT_EXT **> stack(WT_SKIP_MAXDEPTH, nullptr);

    /* Common setup. */
    WT_EXTLIST extlist = {};

    SECTION("insert into an empty list has one element")
    {
        BREAK;
        /* Test. */
        /* Insert one extent. */
        /* Call. */
        REQUIRE(__ut_block_off_insert(session, &extlist, 4096, 4096) == 0);

        extlist_print_off(extlist);

        /* Verify. */
        REQUIRE(__ut_block_off_srch_last(&extlist.off[0], &stack[0]) == extlist.off[0]);
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

        /* Test. */
        /* Insert extents and verify. */
        int idx = 0;
        for (const off_size_expected &test : test_list) {
            /* Call. */
            REQUIRE(__ut_block_off_insert(
                      session, &extlist, test.test_off_size.off, test.test_off_size.size) == 0);

            INFO("After " << idx << ". Insert: " << &test.test_off_size);
            extlist_print_off(extlist);

            /* Verify. */
            verify_off_extent_list(extlist, test.expected_list, true);
            ++idx;
        }
    }

    /* Common cleanup. */
    extlist_free(session, extlist);
}
