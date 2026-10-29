/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#ifndef _WIN32

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch.hpp>

#include "wiredtiger.h"
#include "wt_internal.h"
#include "../utils.h"
#include "../wrappers/connection_wrapper.h"
#include "layered_disagg_utils.h"
#include "../../utility/test_util.h"

/*
 * test_layered_snapshot_stress.cpp
 *
 * Concurrency stress for snapshot reads on a disaggregated follower racing checkpoint pickups.
 * Reader threads at snapshot isolation without a read timestamp continuously open fresh cursors,
 * binding stable content, while the main thread streams checkpoints through the deferral queue.
 * Every reader transaction must observe one atomic version across all keys, repeatably, and with
 * deferral enabled must never be refused. Any torn, drifting, or vanishing read fails the test.
 */

static const std::string TABLE_URI = "table:test_layered_snap_stress";
static const std::string LEADER_HOME = "WT_TEST.layered_snap_stress_leader";
static const std::string FOLLOWER_HOME = "WT_TEST.layered_snap_stress_follower";

static constexpr int NKEYS = 10;
static constexpr int NREADERS = 4;
static constexpr int NVERSIONS = 60;

/*
 * put_version --
 *     Move every key to the given version in one transaction: readers must never see a mix.
 */
static void
put_version(WT_SESSION *session, int version, uint32_t ts)
{
    WT_CURSOR *cursor;
    char cfg[64], key[64], value[64];

    REQUIRE(session->open_cursor(session, TABLE_URI.c_str(), nullptr, nullptr, &cursor) == 0);
    REQUIRE(session->begin_transaction(session, nullptr) == 0);
    for (int i = 0; i < NKEYS; i++) {
        testutil_snprintf(key, sizeof(key), "key_%d", i);
        testutil_snprintf(value, sizeof(value), "v_%d", version);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        REQUIRE(cursor->insert(cursor) == 0);
    }
    testutil_snprintf(cfg, sizeof(cfg), "commit_timestamp=%x", ts);
    REQUIRE(session->commit_transaction(session, cfg) == 0);
    REQUIRE(cursor->close(cursor) == 0);
}

/*
 * reader --
 *     Read all keys twice per transaction through freshly opened cursors, checking that every value
 *     belongs to one version. Catch2 assertions cannot run off the main thread, so failures are
 *     recorded and checked after the join.
 */
static void
reader(WT_CONNECTION *conn, std::atomic<bool> *stop, std::mutex *errors_mutex,
  std::vector<std::string> *errors, std::atomic<uint64_t> *transactions)
{
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int ret;
    char key[64];
    const char *value;
    std::string error, seen;

    if (conn->open_session(conn, nullptr, nullptr, &session) != 0) {
        std::lock_guard<std::mutex> lock(*errors_mutex);
        errors->push_back("reader failed to open a session");
        stop->store(true);
        return;
    }

    while (!stop->load() && error.empty()) {
        if ((ret = session->begin_transaction(session, nullptr)) != 0) {
            error = "begin_transaction failed: " + std::to_string(ret);
            break;
        }
        seen.clear();
        for (int pass = 0; pass < 2 && error.empty(); pass++) {
            if ((ret = session->open_cursor(
                   session, TABLE_URI.c_str(), nullptr, nullptr, &cursor)) != 0) {
                error = "open_cursor failed: " + std::to_string(ret);
                break;
            }
            for (int i = 0; i < NKEYS; i++) {
                testutil_snprintf(key, sizeof(key), "key_%d", i);
                cursor->set_key(cursor, key);
                if ((ret = cursor->search(cursor)) != 0) {
                    error = "search failed or was refused: " + std::to_string(ret);
                    break;
                }
                if (cursor->get_value(cursor, &value) != 0) {
                    error = "get_value failed";
                    break;
                }
                if (seen.empty())
                    seen = value;
                else if (seen != value) {
                    error = "torn or drifting read: " + std::string(value) + " != " + seen;
                    break;
                }
            }
            (void)cursor->close(cursor);
        }
        (void)session->rollback_transaction(session, nullptr);
        if (error.empty())
            transactions->fetch_add(1);
    }

    if (!error.empty()) {
        std::lock_guard<std::mutex> lock(*errors_mutex);
        errors->push_back(error);
        stop->store(true);
    }
    (void)session->close(session, nullptr);
}

TEST_CASE("Layered follower: snapshot readers racing checkpoint pickups",
  "[layered_snapshot_pickup][layered_snapshot_stress]")
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

    /* Baseline version sealed into the first checkpoint. */
    put_version(session_leader, 0, 0x10);
    layered_disagg_leader_checkpoint(conn_leader, session_leader, 0x10);

    connection_wrapper conn_follow_wrap(
      FOLLOWER_HOME, layered_disagg_build_cfg("follower").c_str());
    WT_CONNECTION *conn_follow = conn_follow_wrap.get_wt_connection();
    WT_SESSION *session_follow = (WT_SESSION *)conn_follow_wrap.create_session();
    REQUIRE(session_follow->create(session_follow, TABLE_URI.c_str(), table_cfg) == 0);

    put_version(session_follow, 0, 0x10);
    layered_disagg_pickup_latest_checkpoint(conn_follow, session_follow);

    std::atomic<bool> stop(false);
    std::atomic<uint64_t> transactions(0);
    std::mutex errors_mutex;
    std::vector<std::string> errors;
    std::vector<std::thread> readers;
    for (int i = 0; i < NREADERS; i++)
        readers.emplace_back(reader, conn_follow, &stop, &errors_mutex, &errors, &transactions);

    /* Stream versions and checkpoints through the deferral queue while the readers run. */
    for (int version = 1; version < NVERSIONS && !stop.load(); version++) {
        uint32_t ts = (uint32_t)(0x10 + version);
        put_version(session_leader, version, ts);
        layered_disagg_leader_checkpoint(conn_leader, session_leader, ts);
        put_version(session_follow, version, ts);
        layered_disagg_pickup_latest_checkpoint(conn_follow, session_follow);
    }

    stop.store(true);
    for (auto &t : readers)
        t.join();

    {
        std::lock_guard<std::mutex> lock(errors_mutex);
        REQUIRE(errors == std::vector<std::string>());
    }

    /* The intended path must have run: readers finished transactions and deferred an adoption. */
    REQUIRE(transactions.load() > 0);
    {
        WT_CURSOR *stat_cursor;
        WT_SESSION *stat_session;
        int64_t deferred;

        REQUIRE(conn_follow->open_session(conn_follow, nullptr, nullptr, &stat_session) == 0);
        REQUIRE(stat_session->open_cursor(
                  stat_session, "statistics:", nullptr, nullptr, &stat_cursor) == 0);
        stat_cursor->set_key(stat_cursor, WT_STAT_CONN_DISAGG_CHECKPOINT_DEFER);
        REQUIRE(stat_cursor->search(stat_cursor) == 0);
        const char *desc, *pvalue;
        REQUIRE(stat_cursor->get_value(stat_cursor, &desc, &pvalue, &deferred) == 0);
        REQUIRE(deferred > 0);
        REQUIRE(stat_cursor->close(stat_cursor) == 0);
        REQUIRE(stat_session->close(stat_session, nullptr) == 0);
    }
}

#endif /* !_WIN32 */
