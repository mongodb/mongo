/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Unit tests for __verify_compare_page_id_lists.
 *
 * The merge-compares two sorted arrays returning WT_ERROR on any mismatch.
 */

#include <catch2/catch.hpp>
#include <vector>

#include "wt_internal.h"
#include "wrappers/mock_session.h"

static int
run_compare(
  std::shared_ptr<mock_session> ms, std::vector<uint64_t> btree_ids, std::vector<uint64_t> pali_ids)
{
    /* btree_ids is passed by value. */
    return (__ut_verify_compare_page_id_lists(ms->get_wt_session_impl(), btree_ids.data(),
      btree_ids.size(), pali_ids.data(), pali_ids.size()));
}

TEST_CASE("verify_compare_page_id_lists merge loop", "[verify][disagg]")
{
    auto ms = mock_session::build_test_mock_session();

    SECTION("no mismatches")
    {
        CHECK(run_compare(ms, {1, 2, 3}, {1, 2, 3}) == 0);
        CHECK(run_compare(ms, {}, {}) == 0);
        CHECK(run_compare(ms, {42}, {42}) == 0);
    }

    SECTION("PALI exhausted first")
    {
        CHECK(run_compare(ms, {1, 2, 3}, {1, 2}) == WT_ERROR);
    }

    SECTION("btree exhausted first")
    {
        CHECK(run_compare(ms, {1, 2}, {1, 2, 3}) == WT_ERROR);
    }

    SECTION("interleaved mismatches")
    {
        CHECK(run_compare(ms, {1, 2, 3}, {1, 3, 4}) == WT_ERROR);
    }

    SECTION("all entries mismatched")
    {
        CHECK(run_compare(ms, {1, 4}, {2, 3}) == WT_ERROR);
    }

    SECTION("empty PALI")
    {
        CHECK(run_compare(ms, {1, 2, 3}, {}) == WT_ERROR);
    }

    SECTION("empty btree")
    {
        CHECK(run_compare(ms, {}, {1, 2, 3}) == WT_ERROR);
    }

    SECTION("multiple trailing mismatches on each side")
    {
        CHECK(run_compare(ms, {1, 2, 3, 4}, {1, 2, 5, 6}) == WT_ERROR);
    }
}
