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
 *
 * The lookup runs under the eviction pass lock, so it must not wait for a dhandle lock, which sweep
 * holds while waiting for the pass lock.
 */

#include <chrono>
#include <future>

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

/*
 * Look up the history store btree the way eviction does, holding the pass lock, while another
 * session holds the dhandle write lock as sweep does. On timeout, release the lock so a blocked
 * lookup can finish and the test fails.
 */
static bool
lookup_cached_hs_while_locked(
  WT_SESSION_IMPL *session, WT_SESSION_IMPL *locker, WT_DATA_HANDLE *dhandle, int *retp)
{
    auto lookup_cached_hs = [session]() {
        auto *pass_lock = &S2C(session)->evict->evict_pass_lock;
        WT_BTREE *hs_btree = nullptr;

        __wt_spin_lock(session, pass_lock);
        const int ret = __wt_curhs_get_cached(session, WT_HS_ID, &hs_btree);
        __wt_spin_unlock(session, pass_lock);
        return ret;
    };

    __wt_writelock(locker, &dhandle->rwlock);
    auto lookup = std::async(std::launch::async, lookup_cached_hs);
    const bool finished = lookup.wait_for(std::chrono::seconds(30)) == std::future_status::ready;
    __wt_writeunlock(locker, &dhandle->rwlock);
    *retp = lookup.get();
    return (finished);
}

TEST_CASE("Cached history store lookup does not wait on a write-locked dhandle", "[dhandle]")
{
    connection_wrapper conn("WT_TEST.dhandle_hs_lookup_nowait");
    auto *session = conn.create_session();
    auto *locker = conn.create_session();

    // Cache a history store cursor in the session, as eviction threads do.
    REQUIRE(__wt_curhs_cache(session) == 0);

    // Find the history store dhandle; the locker's dhandle cache keeps it referenced.
    REQUIRE(__wt_session_get_dhandle(locker, WT_HS_URI, nullptr, nullptr, 0) == 0);
    auto *hs_dhandle = locker->dhandle;
    REQUIRE(__wt_session_release_dhandle(locker) == 0);

    int ret;
    const bool finished = lookup_cached_hs_while_locked(session, locker, hs_dhandle, &ret);

    INFO("The cached history store lookup waited on the write-locked dhandle");
    REQUIRE(finished);
    CHECK(ret == WT_NOTFOUND);
}

TEST_CASE("Cached history store lookup does not wait on a stale cursor in the cursor cache sweep",
  "[dhandle]")
{
    constexpr auto uri = "file:stale.wt";

    connection_wrapper conn("WT_TEST.dhandle_hs_lookup_sweep");
    auto *session = conn.create_session();
    auto *locker = conn.create_session();
    WT_SESSION *wt_session = &session->iface;

    // Cache a cursor on another tree and a history store cursor in the session.
    REQUIRE(wt_session->create(wt_session, uri, "key_format=i,value_format=i") == 0);
    WT_CURSOR *cursor;
    REQUIRE(wt_session->open_cursor(wt_session, uri, nullptr, nullptr, &cursor) == 0);
    REQUIRE(cursor->close(cursor) == 0);
    REQUIRE(__wt_curhs_cache(session) == 0);

    // Mark the tree's dhandle outdated, so the cursor cache sweep discards its cached cursor.
    REQUIRE(__wt_session_get_dhandle(locker, uri, nullptr, nullptr, 0) == 0);
    auto *stale_dhandle = locker->dhandle;
    REQUIRE(__wt_session_release_dhandle(locker) == 0);
    int ret;
    WT_WITHOUT_DHANDLE(locker, ret = __wti_conn_dhandle_outdated(locker, uri));
    REQUIRE(ret == 0);

    // Make the history store cursor close in the lookup run a sweep starting at the stale cursor.
    session->cursor_sweep_countdown = 1;
    session->last_cursor_sweep = 0;
    session->cursor_sweep_position =
      __wt_hash_city64(uri, strlen(uri)) & (S2C(session)->hash_size - 1);

    const bool finished = lookup_cached_hs_while_locked(session, locker, stale_dhandle, &ret);

    INFO("The cursor cache sweep waited on the write-locked dhandle");
    REQUIRE(finished);
    CHECK(ret == 0);
}
