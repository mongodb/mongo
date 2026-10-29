/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"

TEST_CASE("Checking if a btree has eviction enabled", "[btree]")
{
    WT_BTREE btree{};

    SECTION("eviction is enabled")
    {
        const bool enabled = __wt_btree_eviction_enabled(&btree);

        CHECK(enabled);
    }

    SECTION("btree is marked as WT_BTREE_NO_EVICT")
    {
        F_SET(&btree, WT_BTREE_NO_EVICT);

        const bool enabled = __wt_btree_eviction_enabled(&btree);

        CHECK_FALSE(enabled);
    }

    SECTION("eviction is disabled")
    {
        btree.evict_disabled = 1;

        const bool enabled = __wt_btree_eviction_enabled(&btree);

        CHECK_FALSE(enabled);
    }

    SECTION("eviction is disabled and the btree is marked as WT_BTREE_NO_EVICT")
    {
        F_SET(&btree, WT_BTREE_NO_EVICT);
        btree.evict_disabled = 1;

        const bool enabled = __wt_btree_eviction_enabled(&btree);

        CHECK_FALSE(enabled);
    }
}
