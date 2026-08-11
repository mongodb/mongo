/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef _WIN32

#include <string>
#include <vector>

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "wt_internal.h"
#include "../utils.h"
#include "../wrappers/connection_wrapper.h"
#include "layered_disagg_utils.h"
#include "../../utility/test_util.h"

/*
 * test_layered_snapshot_pickup.cpp
 *
 * On a disaggregated follower, a transaction at snapshot isolation with no read timestamp must not
 * observe data committed after its snapshot was established, even when the follower picks up a new
 * checkpoint in the middle of the transaction. A correct implementation may either keep the read on
 * the pre-pickup view or refuse it with WT_ROLLBACK; both outcomes are accepted.
 *
 * The leader and the follower run as two connections in one process sharing the page log store;
 * writes are applied on both sides, mirroring replication into the follower's ingest table.
 */

static const std::string TABLE_URI = "table:test_layered_snap";
static const std::string LEADER_HOME = "WT_TEST.layered_snap_leader";
static const std::string FOLLOWER_HOME = "WT_TEST.layered_snap_follower";

/*
 * put --
 *     Commit a set of key/value pairs in a single transaction at the given timestamp.
 */
static void
put(WT_SESSION *session, const std::vector<std::pair<const char *, const char *>> &kv,
  const char *commit_ts)
{
    WT_CURSOR *cursor;
    char cfg[64];

    REQUIRE(session->open_cursor(session, TABLE_URI.c_str(), nullptr, nullptr, &cursor) == 0);
    REQUIRE(session->begin_transaction(session, nullptr) == 0);
    for (const auto &pair : kv) {
        cursor->set_key(cursor, pair.first);
        cursor->set_value(cursor, pair.second);
        REQUIRE(cursor->insert(cursor) == 0);
    }
    testutil_snprintf(cfg, sizeof(cfg), "commit_timestamp=%s", commit_ts);
    REQUIRE(session->commit_transaction(session, cfg) == 0);
    REQUIRE(cursor->close(cursor) == 0);
}

TEST_CASE(
  "Layered follower: snapshot isolation across a checkpoint pickup", "[layered_snapshot_pickup]")
{
    /* Fresh homes with the page log store shared between leader and follower. */
    testutil_system("rm -rf %s %s && mkdir -p %s/kv_home %s && ln -s ../%s/kv_home %s/kv_home",
      LEADER_HOME.c_str(), FOLLOWER_HOME.c_str(), LEADER_HOME.c_str(), FOLLOWER_HOME.c_str(),
      LEADER_HOME.c_str(), FOLLOWER_HOME.c_str());

    connection_wrapper conn_leader_wrap(LEADER_HOME, layered_disagg_build_cfg("leader").c_str());
    WT_CONNECTION *conn_leader = conn_leader_wrap.get_wt_connection();
    WT_SESSION *session_leader = (WT_SESSION *)conn_leader_wrap.create_session();

    const char *table_cfg = "key_format=S,value_format=S,block_manager=disagg,type=layered";
    REQUIRE(session_leader->create(session_leader, TABLE_URI.c_str(), table_cfg) == 0);
    REQUIRE(conn_leader->set_timestamp(conn_leader, "oldest_timestamp=1") == 0);

    /* Baseline data sealed into the first checkpoint. */
    put(session_leader, {{"key_updated", "old value"}}, "10");
    layered_disagg_leader_checkpoint(conn_leader, session_leader, 0x10);

    connection_wrapper conn_follow_wrap(
      FOLLOWER_HOME, layered_disagg_build_cfg("follower").c_str());
    WT_CONNECTION *conn_follow = conn_follow_wrap.get_wt_connection();
    WT_SESSION *session_follow = (WT_SESSION *)conn_follow_wrap.create_session();
    REQUIRE(session_follow->create(session_follow, TABLE_URI.c_str(), table_cfg) == 0);

    /* Replicate the baseline into the follower's ingest and adopt the checkpoint. */
    put(session_follow, {{"key_updated", "old value"}}, "10");
    layered_disagg_pickup_latest_checkpoint(conn_follow, session_follow);

    /*
     * Begin the racing transaction: snapshot isolation, no read timestamp, snapshot established by
     * a read before the next pickup.
     */
    WT_CURSOR *cursor_before;
    REQUIRE(session_follow->begin_transaction(session_follow, nullptr) == 0);
    REQUIRE(session_follow->open_cursor(
              session_follow, TABLE_URI.c_str(), nullptr, nullptr, &cursor_before) == 0);
    cursor_before->set_key(cursor_before, "key_updated");
    REQUIRE(cursor_before->search(cursor_before) == 0);

    /*
     * A writer commits an insert and an update in one transaction after the snapshot: on the
     * leader, and replicated into the follower's ingest. The leader seals it into a new checkpoint
     * and the follower picks it up mid-transaction.
     */
    const std::vector<std::pair<const char *, const char *>> writes = {
      {"key_inserted", "new value"}, {"key_updated", "new value 2"}};
    put(session_leader, writes, "20");
    WT_SESSION *session_replay = (WT_SESSION *)conn_follow_wrap.create_session();
    put(session_replay, writes, "20");
    layered_disagg_leader_checkpoint(conn_leader, session_leader, 0x20);
    layered_disagg_pickup_latest_checkpoint(conn_follow, session_replay);

    /*
     * A new cursor inside the old transaction must not observe any part of the post-snapshot writer
     * transaction.
     */
    WT_CURSOR *cursor_after;
    REQUIRE(session_follow->open_cursor(
              session_follow, TABLE_URI.c_str(), nullptr, nullptr, &cursor_after) == 0);

    cursor_after->set_key(cursor_after, "key_inserted");
    int ret = cursor_after->search(cursor_after);
    CHECK((ret == WT_NOTFOUND || ret == WT_ROLLBACK));

    if (ret != WT_ROLLBACK) {
        cursor_after->set_key(cursor_after, "key_updated");
        ret = cursor_after->search(cursor_after);
        if (ret == 0) {
            const char *value;
            REQUIRE(cursor_after->get_value(cursor_after, &value) == 0);
            CHECK(std::string(value) == "old value");
        } else
            CHECK(ret == WT_ROLLBACK);
    }

    REQUIRE(cursor_after->close(cursor_after) == 0);
    REQUIRE(cursor_before->close(cursor_before) == 0);
    REQUIRE(session_follow->rollback_transaction(session_follow, nullptr) == 0);

    /* Outside the racing transaction the picked-up content must be visible. */
    WT_CURSOR *cursor_check;
    REQUIRE(session_replay->begin_transaction(session_replay, nullptr) == 0);
    REQUIRE(session_replay->open_cursor(
              session_replay, TABLE_URI.c_str(), nullptr, nullptr, &cursor_check) == 0);
    cursor_check->set_key(cursor_check, "key_inserted");
    REQUIRE(cursor_check->search(cursor_check) == 0);
    REQUIRE(cursor_check->close(cursor_check) == 0);
    REQUIRE(session_replay->rollback_transaction(session_replay, nullptr) == 0);
}

#endif /* !_WIN32 */
