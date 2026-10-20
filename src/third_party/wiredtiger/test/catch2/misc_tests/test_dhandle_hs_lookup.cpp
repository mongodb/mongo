/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Looking up the history store btree through a cached cursor must leave the session's dhandle as
 * the caller had it. The lookup holds no reference to the history store dhandle once the cursor
 * closes, so a dhandle left on the session can be freed by sweep while the session still points at
 * it.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"

TEST_CASE("session->dhandle is unchanged after a cached history store lookup", "[dhandle]")
{
    constexpr auto uri = "file:cursor_test.wt";

    connection_wrapper conn("WT_TEST.dhandle_hs_lookup");
    auto *session = conn.create_session();
    REQUIRE(session->iface.create(&session->iface, uri, "key_format=i,value_format=i") == 0);

    // Cache a history store cursor in the session, as eviction threads do.
    REQUIRE(session->dhandle == nullptr);
    REQUIRE(__wt_curhs_cache(session) == 0);

    // Look up the history store btree the way eviction does, holding the pass lock.
    auto lookup_cached_hs = [session]() {
        auto *pass_lock = &S2C(session)->evict->evict_pass_lock;
        WT_BTREE *hs_btree = nullptr;

        __wt_spin_lock(session, pass_lock);
        const int ret = __wt_curhs_get_cached(session, WT_HS_ID, &hs_btree);
        __wt_spin_unlock(session, pass_lock);

        REQUIRE(ret == 0);
        REQUIRE(hs_btree != nullptr);
    };

    SECTION("session without a dhandle")
    {
        lookup_cached_hs();
        CHECK(session->dhandle == nullptr);
    }

    SECTION("session holding another tree's dhandle")
    {
        REQUIRE(__wt_session_get_dhandle(session, uri, nullptr, nullptr, 0) == 0);
        auto *dhandle = session->dhandle;

        lookup_cached_hs();
        CHECK(session->dhandle == dhandle);

        // Release the handle acquired above, even if the lookup replaced it on the session.
        session->dhandle = dhandle;
        REQUIRE(__wt_session_release_dhandle(session) == 0);
    }
}
