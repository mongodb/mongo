/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [extent_list]: block_ext.c
 * Test the front-of-file check compaction uses: __wti_block_avail_front_extent.
 *
 * The helper replaces a walk of the by-offset available list in __compact_page_skip. Both answer
 * "does the available list hold an extent of at least size bytes that starts before limit", so the
 * walk it replaces is kept here as the reference the by-size implementation is compared against.
 */

#include <catch2/catch.hpp>
#include <memory>
#include <random>
#include <vector>

#include "wt_internal.h"
#include "../utils_extlist.h"
#include "../../wrappers/mock_session.h"

using namespace utils;

namespace {

constexpr wt_off_t unit = 4096;

/*
 * Build an extent list that tracks both the by-offset and the by-size skiplists, which is how the
 * live available list is configured. The by-size skiplist is only maintained by the insert path, so
 * these lists have to be built with __ut_block_off_insert rather than by hand.
 */
void
insert_extents(WT_SESSION_IMPL *session, WT_EXTLIST *extlist, const std::vector<off_size> &extents)
{
    extlist->track_size = 1;
    for (const off_size &to_insert : extents) {
        INFO("insert off " << to_insert.off << " size " << to_insert.size);
        REQUIRE(__ut_block_off_insert(session, extlist, to_insert.off, to_insert.size) == 0);
    }
}

/*
 * The original front-of-file check from __compact_page_skip, kept here as the reference the by-size
 * implementation is compared against. It walks the by-offset list and stops as soon as it reaches
 * the limit, because that list is sorted by offset.
 */
bool
reference_avail_front_extent(WT_EXTLIST *extlist, wt_off_t size, wt_off_t limit)
{
    WT_EXT *ext;

    WT_EXT_FOREACH (ext, extlist->off) {
        if (ext->off >= limit)
            break;
        if (ext->size >= size)
            return (true);
    }

    return (false);
}

/*
 * Run the by-size front-of-file check and the original by-offset walk against the same list and
 * require that they agree. The helper asserts that the caller holds the live lock, so take it here
 * the way __compact_page_skip does.
 */
void
require_same_front_extent(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_EXTLIST *extlist, wt_off_t size, wt_off_t limit)
{
    __wt_spin_lock(session, &block->live_lock);
    bool by_size = __wti_block_avail_front_extent(session, block, extlist, size, limit);
    __wt_spin_unlock(session, &block->live_lock);

    bool reference = reference_avail_front_extent(extlist, size, limit);

    INFO("size " << size << ", limit " << limit << ": by-size " << by_size << ", reference "
                 << reference);

    REQUIRE(by_size == reference);
}

/*
 * Generate a pseudo-random, non-overlapping extent list. Gaps keep the extents non-adjacent,
 * modeling a fragmented available extent list.
 */
std::vector<off_size>
random_extents(std::mt19937 &rng, int count, wt_off_t *endp = nullptr)
{
    std::uniform_int_distribution<int> size_units(1, 20);
    std::uniform_int_distribution<int> gap_units(1, 5);
    std::vector<off_size> extents;
    wt_off_t off = unit;

    for (int i = 0; i < count; i++) {
        wt_off_t size = size_units(rng) * unit;
        extents.push_back(off_size(off, size));
        off += size + gap_units(rng) * unit;
    }

    if (endp != nullptr)
        *endp = off;

    return (extents);
}

} // namespace

TEST_CASE("Extent Lists: front-of-file check matches the by-offset walk", "[extent_list]")
{
    std::shared_ptr<mock_session> mock_session = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = mock_session->get_wt_session_impl();

    WT_BLOCK block = {};
    REQUIRE(__wt_spin_init(session, &block.live_lock, "front extent") == 0);

    SECTION("empty list")
    {
        WT_EXTLIST extlist = {};
        extlist.track_size = 1;

        require_same_front_extent(session, &block, &extlist, unit, 100 * unit);

        extlist_free(session, extlist);
    }

    SECTION("every extent is past the limit")
    {
        /* This is the case the fast path handles: nothing starts before the limit. */
        std::vector<off_size> extents{
          off_size(100 * unit, unit),
          off_size(200 * unit, 16 * unit),
          off_size(300 * unit, 32 * unit),
        };

        WT_EXTLIST extlist = {};
        insert_extents(session, &extlist, extents);

        require_same_front_extent(session, &block, &extlist, unit, 50 * unit);
        require_same_front_extent(session, &block, &extlist, 16 * unit, 50 * unit);

        /* The limit falls exactly on the first extent, which is excluded by "off < limit". */
        require_same_front_extent(session, &block, &extlist, unit, 100 * unit);

        extlist_free(session, extlist);
    }

    SECTION("extents before the limit are all too small")
    {
        /*
         * The by-offset walk stops at the limit, so the large extents past it must not be counted
         * even though their buckets are size-sufficient.
         */
        std::vector<off_size> extents{
          off_size(unit, unit),
          off_size(3 * unit, unit),
          off_size(500 * unit, 64 * unit),
        };

        WT_EXTLIST extlist = {};
        insert_extents(session, &extlist, extents);

        /* A single allocation unit fits in front of the limit. */
        require_same_front_extent(session, &block, &extlist, unit, 100 * unit);

        /* Anything larger only exists past the limit, so the answer must be false. */
        require_same_front_extent(session, &block, &extlist, 2 * unit, 100 * unit);
        require_same_front_extent(session, &block, &extlist, 64 * unit, 100 * unit);

        extlist_free(session, extlist);
    }

    SECTION("a bucket head is past the limit but the bucket also has a front extent")
    {
        /*
         * Extents of the same size share a bucket and are sorted by offset, so the head is the
         * lowest-offset extent of that size. This checks the ordering assumption the by-size
         * implementation relies on: inserting the high offset first must not leave it as the head.
         */
        std::vector<off_size> extents{
          off_size(900 * unit, 8 * unit),
          off_size(2 * unit, 8 * unit),
        };

        WT_EXTLIST extlist = {};
        insert_extents(session, &extlist, extents);

        require_same_front_extent(session, &block, &extlist, 8 * unit, 100 * unit);

        extlist_free(session, extlist);
    }

    SECTION("an extent starting before the limit and reaching past it still counts")
    {
        /*
         * Both forms test only where an extent starts, so this qualifies in both. That is the
         * behavior of the check being replaced, preserved deliberately.
         */
        std::vector<off_size> extents{
          off_size(90 * unit, 40 * unit),
        };

        WT_EXTLIST extlist = {};
        insert_extents(session, &extlist, extents);

        require_same_front_extent(session, &block, &extlist, 40 * unit, 100 * unit);

        extlist_free(session, extlist);
    }

    SECTION("limit boundaries")
    {
        std::vector<off_size> extents{
          off_size(10 * unit, 4 * unit),
          off_size(20 * unit, 4 * unit),
        };

        WT_EXTLIST extlist = {};
        insert_extents(session, &extlist, extents);

        /* A zero limit excludes everything. */
        require_same_front_extent(session, &block, &extlist, unit, 0);

        /* Just before, exactly at, and just after the first extent's offset. */
        require_same_front_extent(session, &block, &extlist, unit, 10 * unit - 1);
        require_same_front_extent(session, &block, &extlist, unit, 10 * unit);
        require_same_front_extent(session, &block, &extlist, unit, 10 * unit + 1);

        extlist_free(session, extlist);
    }

    SECTION("a bucket head exactly at the limit does not count")
    {
        /*
         * The lowest-offset extent is in front of the limit but too small, so the answer has to
         * come from the size buckets. The only large-enough extent starts exactly at the limit,
         * which is where the region compaction is emptying begins rather than in front of it.
         * Relocating a page into it would not let the file shrink, so it must not count.
         *
         * Reaching this at all needs the lowest-offset extent to be in front of the limit and too
         * small, otherwise the checks on the head of the by-offset list answer first and the
         * buckets are never consulted.
         */
        std::vector<off_size> extents{
          off_size(unit, unit),
          off_size(100 * unit, 8 * unit),
        };

        WT_EXTLIST extlist = {};
        insert_extents(session, &extlist, extents);

        require_same_front_extent(session, &block, &extlist, 8 * unit, 100 * unit);

        /* Moving the limit one byte past that offset makes the same extent qualify. */
        require_same_front_extent(session, &block, &extlist, 8 * unit, 100 * unit + 1);

        extlist_free(session, extlist);
    }

    SECTION("randomized extent lists")
    {
        for (int iteration = 0; iteration < 20; iteration++) {
            INFO("iteration " << iteration);

            /* Seed per iteration, so a failing iteration can be replayed on its own. */
            std::mt19937 rng(1337 + iteration);

            wt_off_t end = 0;
            std::vector<off_size> extents = random_extents(rng, 100, &end);

            WT_EXTLIST extlist = {};
            insert_extents(session, &extlist, extents);

            /* Sweep the limit across the whole file for a range of request sizes. */
            for (int units = 1; units <= 21; units++)
                for (int percent = 0; percent <= 100; percent += 10)
                    require_same_front_extent(
                      session, &block, &extlist, units * unit, (end / 100) * percent);

            /*
             * The sweep above divides the file size by 100, so its limits almost never land on an
             * extent boundary. Use the offsets themselves as limits too, which is the case where an
             * extent starts exactly at the limit and must not count.
             */
            for (int units = 1; units <= 21; units++)
                for (const off_size &extent : extents)
                    require_same_front_extent(session, &block, &extlist, units * unit, extent.off);

            extlist_free(session, extlist);
        }
    }

    __wt_spin_destroy(session, &block.live_lock);
}
