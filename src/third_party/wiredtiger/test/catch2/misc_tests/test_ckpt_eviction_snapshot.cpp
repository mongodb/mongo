/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include <filesystem>

#include "wiredtiger.h"
#include "../wrappers/connection_wrapper.h"
#include "wt_internal.h"

/*
 * Under precise checkpoints eviction bounds itself with the snapshot the running checkpoint
 * publishes. A snapshot from an earlier checkpoint can sit entirely below the global oldest id,
 * which makes every update in the tree look invisible. A checkpoint therefore publishes once it has
 * taken its snapshot and retires before releasing it, so eviction can adopt a snapshot only while
 * the checkpoint that took it is running.
 */

static constexpr const char *k_db = "test_db_ckpt_eviction_snapshot";

/* Publish a snapshot into the inactive buffer, as a checkpoint does once it has taken one. */
static uint32_t
publish(WT_SESSION_IMPL *session, uint64_t snap_min)
{
    WT_CONNECTION_IMPL *conn = S2C(session);
    uint32_t new_idx = 1 - conn->ckpt_eviction_snap_idx;

    conn->ckpt_eviction_snap[new_idx].snap.snap_min = snap_min;
    conn->ckpt_eviction_snap[new_idx].snap.snap_max = snap_min + 100;
    conn->ckpt_eviction_snap[new_idx].snap.snapshot_count = 0;
#ifdef HAVE_DIAGNOSTIC
    conn->ckpt_eviction_snap[new_idx].gen = __wt_gen(session, WT_GEN_CHECKPOINT);
#endif
    conn->ckpt_eviction_snap_idx = new_idx;
    conn->ckpt_eviction_snap_published = true;

    return (new_idx);
}

/*
 * A published snapshot must not outlive the test: the checkpoint taken when the connection closes
 * asserts that the previous checkpoint retired its own.
 */
struct retire_on_exit {
    WT_SESSION_IMPL *session;

    ~retire_on_exit()
    {
        __ut_checkpoint_eviction_snapshot_retire(session);
    }
};

/* Check if a page being evicted adopts the published snapshot. */
static bool
adoptable(WT_SESSION_IMPL *session)
{
    return (__wt_ckpt_eviction_snap_current(session) != nullptr);
}

/*
 * Closing a precise-checkpoint connection takes a checkpoint, which requires a stable timestamp.
 * Without one the connection wrapper's destructor throws.
 */
static void
set_stable(connection_wrapper &wrapper)
{
    WT_CONNECTION *conn = wrapper.get_wt_connection();

    REQUIRE(conn->set_timestamp(conn, "stable_timestamp=1") == 0);
}

TEST_CASE("Checkpoint eviction snapshot: only a running checkpoint's snapshot is adoptable",
  "[ckpt_eviction_snapshot]")
{
    std::filesystem::remove_all(k_db);
    connection_wrapper wrapper(k_db, "create,precise_checkpoint=true");
    WT_CONNECTION_IMPL *conn = wrapper.get_wt_connection_impl();
    WT_SESSION_IMPL *session = wrapper.create_session();
    retire_on_exit guard{session};
    set_stable(wrapper);

    SECTION("no checkpoint has published yet")
    {
        REQUIRE_FALSE(conn->ckpt_eviction_snap_published);
        REQUIRE_FALSE(adoptable(session));
    }

    SECTION("the running checkpoint has published")
    {
        publish(session, 100);
        REQUIRE(adoptable(session));
    }

    SECTION("the publishing checkpoint has retired it")
    {
        publish(session, 100);
        __ut_checkpoint_eviction_snapshot_retire(session);
        REQUIRE_FALSE(adoptable(session));
    }

    SECTION("the next checkpoint has not published yet")
    {
        /*
         * The interval at the start of a checkpoint, before it takes its snapshot. The checkpoint
         * generation has moved on but nothing is published, so eviction bounds itself another way
         * rather than adopting the last checkpoint's snapshot.
         */
        publish(session, 100);
        __ut_checkpoint_eviction_snapshot_retire(session);
        REQUIRE_FALSE(adoptable(session));

        publish(session, 900);
        REQUIRE(adoptable(session));
    }
}

TEST_CASE("Checkpoint eviction snapshot: the reader sees the published buffer's contents",
  "[ckpt_eviction_snapshot]")
{
    std::filesystem::remove_all(k_db);
    connection_wrapper wrapper(k_db, "create,precise_checkpoint=true");
    WT_CONNECTION_IMPL *conn = wrapper.get_wt_connection_impl();
    WT_SESSION_IMPL *session = wrapper.create_session();
    retire_on_exit guard{session};
    set_stable(wrapper);

    uint32_t snap_idx = publish(session, 100);

    WT_TXN_SNAPSHOT *snap = __wt_ckpt_eviction_snap_current(session);
    REQUIRE(snap == &conn->ckpt_eviction_snap[snap_idx].snap);
    REQUIRE(snap->snap_min == 100);
    REQUIRE(snap->snap_max == 200);
}

TEST_CASE("Checkpoint eviction snapshot: a checkpoint never writes the buffer it published",
  "[ckpt_eviction_snapshot]")
{
    std::filesystem::remove_all(k_db);
    connection_wrapper wrapper(k_db, "create,precise_checkpoint=true");
    WT_CONNECTION_IMPL *conn = wrapper.get_wt_connection_impl();
    WT_SESSION_IMPL *session = wrapper.create_session();
    retire_on_exit guard{session};
    set_stable(wrapper);

    /*
     * Buffers must alternate: a checkpoint that reused the buffer it last published would overwrite
     * one an eviction thread may still be reading.
     */
    uint32_t first_idx = publish(session, 100);
    __ut_checkpoint_eviction_snapshot_retire(session);
    uint32_t second_idx = publish(session, 900);

    REQUIRE(second_idx != first_idx);
    REQUIRE(conn->ckpt_eviction_snap[first_idx].snap.snap_min == 100);
}

TEST_CASE(
  "Checkpoint eviction snapshot: retire leaves the buffers alone", "[ckpt_eviction_snapshot]")
{
    std::filesystem::remove_all(k_db);
    connection_wrapper wrapper(k_db, "create,precise_checkpoint=true");
    WT_CONNECTION_IMPL *conn = wrapper.get_wt_connection_impl();
    WT_SESSION_IMPL *session = wrapper.create_session();
    retire_on_exit guard{session};
    set_stable(wrapper);

    uint32_t snap_idx = publish(session, 100);

    __ut_checkpoint_eviction_snapshot_retire(session);

    /* Only the published flag is cleared; the buffer and the index naming it are untouched. */
    REQUIRE_FALSE(conn->ckpt_eviction_snap_published);
    REQUIRE(conn->ckpt_eviction_snap_idx == snap_idx);
    REQUIRE(conn->ckpt_eviction_snap[snap_idx].snap.snap_min == 100);
}
