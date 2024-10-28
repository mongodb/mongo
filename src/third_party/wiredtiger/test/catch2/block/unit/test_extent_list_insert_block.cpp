/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [extent_list]: block_ext.c
 * Test extent list functions part 4.
 *
 * Test extent list insert/remove functions with block: __block_merge, __block_off_remove,
 * __block_extend, and __block_append.
 */

#include <algorithm>
#include <catch2/catch.hpp>
#include <memory>
#include <vector>

#include "test_util.h"
#include "wt_internal.h"
#include "../utils_extlist.h"
#include "../../utils.h"
#include "../../wrappers/mock_session.h"

using namespace utils;

/*!
 * To sort off_size by _off and _size.
 */
struct {
    bool
    operator()(const off_size &left, const off_size &right) const
    {
        return (left < right);
    }
} off_size_Less_off_and_size;

TEST_CASE("Extent Lists: block_merge", "[extent_list]")
{
    /* Build Mock session, this will automatically create a mock connection. */
    std::shared_ptr<mock_session> mock_session = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = mock_session->get_wt_session_impl();

    std::vector<WT_EXT **> stack(WT_SKIP_MAXDEPTH, nullptr);

    SECTION("insert/merge multiple extents and verify all extents after each insert/merge")
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
          {off_size(4096 - 64, 64), // [4,032, 4,095] Just below First, Merge with First start.
            {
              off_size(4096 - 64, 4096 + 64), // [4,032, 8,191], First'.
              off_size(3 * 4096, 4096),       // [12,288, 16,383] Second.
              off_size(5 * 4096, 4096),       // [20,480, 24,575] Third.
            }},
          {off_size(2 * 4096, 64), // [8,192, 8,255] Just above First', Merge with First' end.
            {
              off_size(4096 - 64, 4096 + 128), // [4,032, 8,255], First''.
              off_size(3 * 4096, 4096),        // [12,288, 16,383] Second.
              off_size(5 * 4096, 4096),        // [20,480, 24,575] Third.
            }},
          {off_size(2 * 4096 + 64,
             4096 - 64), // [8,256, 12,287] Just above First'', Merge First'' and Second.
            {
              off_size(4096 - 64, 3 * 4096 + 64), // [4,032, 16,383], First'''.
              off_size(5 * 4096, 4096),           // [20,480, 24,575] Third.
            }},
          {off_size(
             6 * 4096, 64), // [20,480, 12,287] Just above First''', Merge First''' and Third.
            {
              off_size(4096 - 64, 3 * 4096 + 64), // [4,032, 16,383], First'''.
              off_size(5 * 4096, 4096 + 64),      // [20,480, 24,639] Third'.
            }},
        };

        /* Setup. */
        WT_EXTLIST extlist = {};
        extlist.name = const_cast<char *>("__block_merge");

        WT_BLOCK block = {};
        block.name = "__block_merge";
        block.allocsize = 1024;
        block.size = 4096; // Description information.

        /* Test. */
        /* Insert/merge extents and verify. */
        int idx = 0;
        for (const off_size_expected &test : test_list) {
            /* Call. */
            REQUIRE(__ut_block_merge(session, &block, &extlist, test.test_off_size.off,
                      test.test_off_size.size) == 0);
            INFO("After " << idx << ". Insert/merge: " << &test.test_off_size);

            utils::extlist_print_off(extlist);

            /* Verify. */
            verify_off_extent_list(extlist, test.expected_list, true);
            ++idx;
        }

        /* Cleanup. */
        extlist_free(session, extlist);
    }
}

/*!
 * A test (off) and the expected value (expected_list) for operations that need an off to modify a
 * WT_EXTLIST.
 */
struct off_expected {
    wt_off_t off;
    std::vector<off_size> expected_list;
};

TEST_CASE("Extent Lists: block_off_remove", "[extent_list]")
{
    /* Build Mock session, this will automatically create a mock connection. */
    std::shared_ptr<mock_session> mock_session = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = mock_session->get_wt_session_impl();

    std::vector<WT_EXT **> stack(WT_SKIP_MAXDEPTH, nullptr);

    SECTION("remove multiple extents and verify all extents after each remove")
    {
        BREAK;
        /* Extents to insert to setup for __ut_block_remove. */
        std::vector<off_size> insert_list{
          off_size(3 * 4096, 4096), // Second [12,288, 16,383].
          off_size(4096, 4096),     // First [4,096, 8,191].
          off_size(5 * 4096, 4096), // Third [20,480, 24,575].
        };

        /* Tests and expected values. */
        std::vector<off_expected> test_list{
          {3 * 4096, // [12,288, 16,383] Second.
            {
              off_size(4096, 4096),     // [4,096, 8,191] First.
              off_size(5 * 4096, 4096), // [20,480, 24,575] Third.
            }},
          {4096, // [4,096, 8,191] First.
            {
              off_size(5 * 4096, 4096), // [20,480, 24,575] Third.
            }},
          {5 * 4096, // [20,480, 24,575] Third.
            {}},
        };

        /* Setup. */
        WT_EXTLIST extlist = {};
        extlist.name = const_cast<char *>("__block_off_remove");

        /* Insert extents. */
        for (const off_size &to_insert : insert_list) {
            INFO("Insert: " << &to_insert);
            REQUIRE(__ut_block_off_insert(session, &extlist, to_insert.off, to_insert.size) == 0);
        }

        extlist_print_off(extlist);

        /* Verify extents. */
        std::vector<off_size> expected_order{insert_list};
        std::sort(expected_order.begin(), expected_order.end(), off_size_Less_off_and_size);
        verify_off_extent_list(extlist, expected_order);

        /* Test. */
        WT_BLOCK block = {}; // __block_off_remove used only in error checking..
        block.name = const_cast<char *>("__block_off_remove");
        int idx = 0;
        for (const off_expected &test : test_list) {
            /* For testing, half request ext returned, and half do not. */
            if ((idx % 2) == 0)
                /* Call. */
                REQUIRE(__ut_block_off_remove(session, &block, &extlist, test.off, nullptr) == 0);
            else {
                WT_EXT *ext = nullptr;
                /* Call. */
                REQUIRE(__ut_block_off_remove(session, &block, &extlist, test.off, &ext) == 0);
                REQUIRE(ext != nullptr);
                __wti_block_ext_free(session, &ext);
            }

            INFO("After " << idx << ". Remove: off " << test.off);
            extlist_print_off(extlist);

            /* Verify. */
            verify_off_extent_list(extlist, test.expected_list, true);
            ++idx;
        }

        /* Verify the result of all calls. */
        verify_empty_extent_list(&extlist.off[0], &stack[0]);

        /* Cleanup. */
        extlist_free(session, extlist);
    }
}

TEST_CASE("Extent Lists: block_append", "[extent_list]")
{
    /* Build Mock session, this will automatically create a mock connection.*/
    std::shared_ptr<mock_session> mock_session = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = mock_session->get_wt_session_impl();

    std::vector<WT_EXT **> stack(WT_SKIP_MAXDEPTH, nullptr);

    SECTION("append multiple extents and verify all extents after each append")
    {
        BREAK;
        /* Tests and expected values. */
        std::vector<off_size_expected> test_list
        {
            {off_size(4096, 2048), // First half of First [4,096, 6,143].
              {
                off_size(4096, 2048), // First [4,096, 6,143].
              }},
              {off_size(4096 + 2048, 2048), // Adjacent: Second half of First [6,144, 8,191].
                {
                  off_size(4096, 4096), // First [4,096, 8,191].
                }},
              {off_size(3 * 4096, 4096), // Not adjacent: Second [12,288, 16,383].
                {
                  off_size(4096, 4096),     // First [4,096, 8,191].
                  off_size(3 * 4096, 4096), // Second [12,288, 16,383].
                }},
              {off_size(5 * 4096, 4096), // Not adjacent: Third [20,480, 24,575].
                {
                  off_size(4096, 4096),     // First [4,096, 8,191].
                  off_size(3 * 4096, 4096), // Second [12,288, 16,383].
                  off_size(5 * 4096, 4096), // Third [20,480, 24,575].
                }},
#if 0   // Tests that are not appends and should crash.
          {off_size(4 * 4096, 4096), // Below last extent, nonoverlapping
            {
              off_size(4096, 4096),     // First [4,096, 8,191].
              off_size(3 * 4096, 3*4096), // Second' [12,288, 24,575].
            }},
#elif 0 // Tests that are not appends and should crash.
              {off_size(5 * 4096 + 1024, 4096), // Overlapping last extent
                {
                  off_size(4096, 4096),            // First [4,096, 8,191].
                  off_size(3 * 4096, 4096),        // Second [12,288, 16,383].
                  off_size(5 * 4096, 4096 + 1024), // Third' [20,480, 25,559].
                }},
#endif
        };

        /* Setup. */
        WT_EXTLIST extlist = {};
        extlist.name = const_cast<char *>("__block_append");
        /* Initial block. */
        WT_BLOCK block = {}; // Not used by __block_append.
        block.name = const_cast<char *>("__block_append");
        block.allocsize = 1024;
        block.size = 4096; // Description information.

        /* Test. */
        /* Append extents and verify. */
        int idx = 0;
        for (const off_size_expected &test : test_list) {
            /* Call. */
            REQUIRE(__ut_block_append(session, &block, &extlist, test.test_off_size.off,
                      test.test_off_size.size) == 0);

            INFO("After " << idx << ". Append: " << &test.test_off_size);
            utils::extlist_print_off(extlist);

            /* Verify. */
            verify_off_extent_list(extlist, test.expected_list, true);
            ++idx;
        }

        /* Cleanup. */
        extlist_free(session, extlist);
    }
}

/*!
 * A test (size), expected values extension offset(extension_off), block size(block_size), and
 * err(err) for __block_extend.
 */
struct block_append_test {
    wt_off_t size;
    wt_off_t extension_off;
    wt_off_t block_size;
    int err;
};

TEST_CASE("Extent Lists: block_extend", "[extent_list]")
{
    /* Build Mock session, this will automatically create a mock connection. */
    std::shared_ptr<mock_session> mock_session = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = mock_session->get_wt_session_impl();

    std::vector<WT_EXT **> stack(WT_SKIP_MAXDEPTH, nullptr);

    SECTION("fail to extend an invalid block")
    {
        BREAK;
        /* Test and expected values */
        block_append_test test = {4096, 0, 1024, EINVAL};

        /* Setup. */
        WT_EXTLIST extlist = {};
        extlist.name = const_cast<char *>("__block_extend");
        /* Invalid initial block. */
        WT_BLOCK block = {};
        block.name = const_cast<char *>("__block_extend");
        block.allocsize = 4096;
        block.size = 1024; // Description information.

        /* Test. */
        /* Fail to extend. */
        wt_off_t size_before = block.size;
        wt_off_t extension_off = 0;
        /* Call. */
        int err = __ut_block_extend(session, &block, &extlist, &extension_off, test.size);

        INFO("After extend: Before: size "
          << size_before << "; Test: size " << test.size << "; Expected: extension offset "
          << test.extension_off << ", block size " << test.block_size << ", err " << test.err
          << "; Actual: extension offset " << extension_off << ", block size " << block.size
          << ", err " << err);

        /* Verify. */
        REQUIRE(err == test.err);
        REQUIRE(extension_off == test.extension_off);
        REQUIRE(block.size == test.block_size);
    }

    SECTION("extend a block and verify it after each extension")
    {
        BREAK;
        /* Tests and expected values. */
        std::vector<block_append_test> test_list{
          {2048, 4096, 4096 + 2048, 0},     // Extend by 2,048 from 4,096 to 6,144.
          {2048, 4096 + 2048, 2 * 4096, 0}, // Extend from 6,144 to 8,192.
          {(wt_off_t)INT64_MAX - 2 * 4096 + 1024, 0, 2 * 4096, WT_ERROR}, // Block size too big.
        };

        /* Setup. */
        WT_EXTLIST extlist = {};
        extlist.name = const_cast<char *>("__block_extend");
        /* Initial valid block. */
        WT_BLOCK block = {};
        block.name = const_cast<char *>("__block_extend");
        block.allocsize = 1024;
        block.size = 4096; // Description information.

        /* Test. */
        /* Append size and verify. */
        int idx = 0;
        for (const block_append_test &test : test_list) {
            wt_off_t size_before = block.size;
            wt_off_t extension_off = 0;
            /* Call. */
            int err = __ut_block_extend(session, &block, &extlist, &extension_off, test.size);

            INFO("After " << idx << ". Extend: Before: size " << size_before << "; Test: size "
                          << test.size << "; Expected: extension offset " << test.extension_off
                          << ", block size " << test.block_size << ", err " << test.err
                          << "; Actual: extension offset " << extension_off << ", block size "
                          << block.size << ", err " << err);

            /* Verify. */
            REQUIRE(err == test.err);
            REQUIRE(extension_off == test.extension_off);
            REQUIRE(block.size == test.block_size);
            ++idx;
        }
    }
}
