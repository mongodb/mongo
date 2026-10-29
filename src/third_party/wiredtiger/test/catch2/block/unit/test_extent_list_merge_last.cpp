/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [extent_list]: block_ext.c
 * Regression test for the cached end-of-list pointer across __wti_block_extlist_merge.
 *
 * __wti_block_extlist_merge swaps two extent lists when the source is larger than the destination.
 * The cached end-of-list pointer has to move with the list it describes, otherwise the destination
 * is left pointing at an extent owned by the source list, which the caller then frees.
 */

#include <catch2/catch.hpp>
#include <memory>

#include "test_util.h"
#include "wt_internal.h"
#include "../utils_extlist.h"
#include "../../utils.h"
#include "../../wrappers/mock_session.h"

using namespace utils;

namespace {
void
insert(WT_SESSION_IMPL *session, WT_EXTLIST &el, wt_off_t off, wt_off_t size)
{
    WT_EXT *ext = alloc_new_ext(session, off, size);
    REQUIRE(ext != nullptr);
    REQUIRE(__ut_block_ext_insert(session, &el, ext) == 0);
}

bool
list_contains(const WT_EXTLIST &el, const WT_EXT *want)
{
    for (WT_EXT *ext = el.off[0]; ext != nullptr; ext = ext->next[0])
        if (ext == want)
            return (true);
    return (false);
}
} // namespace

TEST_CASE("Extent Lists: merge swap must not leave a stale last pointer", "[extent_list]")
{
    std::shared_ptr<mock_session> mock_session = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = mock_session->get_wt_session_impl();

    WT_BLOCK block = {};
    block.verify = true;

    WT_EXTLIST src = {}, dst = {};

    /*
     * Neither list tracks sizes, matching the allocation and discard lists, which are the lists
     * that use the cached end-of-list pointer when appending.
     */
    insert(session, dst, 8192, 4096);
    insert(session, src, 0, 4096);
    insert(session, src, 32768, 4096);
    REQUIRE(src.entries > dst.entries);

    REQUIRE(__wti_block_extlist_merge(session, &block, &src, &dst) == 0);

    /*
     * The source was larger, so the merge swapped the lists. Whatever the destination's cached
     * end-of-list pointer now refers to must be reachable from the destination's own skiplist: the
     * caller frees the source list.
     */
    REQUIRE(dst.entries == 3);
    REQUIRE((dst.last == nullptr || list_contains(dst, dst.last)));

    src.last = nullptr;
    extlist_free(session, src);
    extlist_free(session, dst);
}
