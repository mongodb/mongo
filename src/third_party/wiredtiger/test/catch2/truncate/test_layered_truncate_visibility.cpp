/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <string_view>

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "wrappers/mock_session.h"

/*
 * test_layered_truncate_visibility.cpp
 *
 * Test coverage for the follower layered-table truncate list visibility helpers.
 */

namespace {

class layered_truncate_visibility_fixture {
    WT_TXN_SHARED *txn_shared_list;
    bool fast_truncate_enabled;

public:
    std::shared_ptr<mock_session> session_wrapper;
    WT_SESSION_IMPL *session;
    WT_LAYERED_TABLE layered_table{};

    layered_truncate_visibility_fixture()
        : session_wrapper(mock_session::build_test_mock_session()),
          session(session_wrapper->get_wt_session_impl()),
          fast_truncate_enabled(__wt_process.disagg_fast_truncate_2026)
    {
        session->id = 0;

        REQUIRE(__wt_calloc(session, 1, sizeof(WT_TXN_SHARED), &txn_shared_list) == 0);
        S2C(session)->txn_global.txn_shared_list = txn_shared_list;

        REQUIRE(__wt_calloc(session, 1, sizeof(WT_TXN), &session->txn) == 0);

        layered_table.iface.name = "layered:test_layered_truncate_visibility";
        TAILQ_INIT(&layered_table.truncateqh);
        REQUIRE(__wt_rwlock_init(session, &layered_table.truncate_lock) == 0);

        __wt_process.disagg_fast_truncate_2026 = true;
    }

    ~layered_truncate_visibility_fixture()
    {
        __wt_layered_table_truncate_clear(session, &layered_table);
        __wt_rwlock_destroy(session, &layered_table.truncate_lock);
        __wt_free(session, session->txn);
        __wt_free(session, txn_shared_list);
        __wt_process.disagg_fast_truncate_2026 = fast_truncate_enabled;
    }

    /*
     * Reset the mock transaction into a snapshot reader state for visibility checks. By default,
     * snap_min/snap_max stay at 100/200 so tests can classify txn ids below 100 as already visible
     * and ids at or above 200 as still in-flight without building an explicit snapshot array, while
     * still allowing tests to override the snapshot window when needed.
     */
    void
    set_reader(uint64_t txn_id, wt_timestamp_t read_timestamp,
      WT_TXN_ISOLATION isolation = WT_ISO_SNAPSHOT, uint64_t snap_min = 100,
      uint64_t snap_max = 200)
    {
        WT_CLEAR(*session->txn);

        session->txn->isolation = isolation;
        session->txn->snapshot_data.snap_min = snap_min;
        session->txn->snapshot_data.snap_max = snap_max;
        session->txn->snapshot_data.snapshot = nullptr;
        session->txn->snapshot_data.snapshot_count = 0;
        F_SET(session->txn, WT_TXN_HAS_SNAPSHOT);

        session->txn->time_point.id = txn_id;
        F_SET(&session->txn->time_point, WT_TXN_TIME_POINT_HAS_ID);

        WT_SESSION_TXN_SHARED(session)->read_timestamp = read_timestamp;
        if (read_timestamp == WT_TS_NONE)
            F_CLR(session->txn, WT_TXN_SHARED_TS_READ);
        else
            F_SET(session->txn, WT_TXN_SHARED_TS_READ);
    }

    /*
     * Reset the mock transaction into the committer state consumed by
     * __wti_mark_committed_truncate_table_apply. Only the transaction id and commit/durable
     * timestamps matter for these tests, so the helper sets just those fields.
     */
    void
    set_committer(
      uint64_t txn_id, wt_timestamp_t commit_timestamp, wt_timestamp_t durable_timestamp)
    {
        WT_CLEAR(*session->txn);

        session->txn->time_point.id = txn_id;
        session->txn->time_point.commit_timestamp = commit_timestamp;
        session->txn->time_point.durable_timestamp = durable_timestamp;
        F_SET(&session->txn->time_point,
          WT_TXN_TIME_POINT_HAS_ID | WT_TXN_TIME_POINT_HAS_TS_COMMIT |
            WT_TXN_TIME_POINT_HAS_TS_DURABLE);
    }

    WT_TRUNCATE *
    add_truncate(uint64_t txn_id, wt_timestamp_t start_ts, wt_timestamp_t durable_ts,
      const char *start, const char *stop)
    {
        WT_TRUNCATE *entry = nullptr;

        REQUIRE(__wt_calloc_one(session, &entry) == 0);
        entry->layered_table = &layered_table;
        WT_DHANDLE_ACQUIRE(&layered_table.iface);
        entry->txn_id = txn_id;
        entry->start_ts = start_ts;
        entry->durable_ts = durable_ts;
        entry->committed = (start_ts != WT_TS_NONE || durable_ts != WT_TS_NONE);

        REQUIRE(__wt_buf_set(session, &entry->start_key, start, strlen(start)) == 0);
        REQUIRE(__wt_buf_set(session, &entry->stop_key, stop, strlen(stop)) == 0);

        TAILQ_INSERT_TAIL(&layered_table.truncateqh, entry, q);
        return entry;
    }

    int
    truncate_visible(std::string_view key, WT_TRUNCATE **matched = nullptr)
    {
        WT_ITEM item{};
        item.data = key.data();
        item.size = key.size();
        return (__wt_truncate_delete_visible_check(session, &layered_table, &item, matched));
    }
};

} // namespace

TEST_CASE_METHOD(
  layered_truncate_visibility_fixture, "own uncommitted truncate is visible", "[layered][truncate]")
{
    const uint64_t txn_id = 50;
    const wt_timestamp_t start_ts = WT_TS_NONE;
    const wt_timestamp_t durable_ts = WT_TS_NONE;
    const char *start = "0100";
    const char *stop = "0700";

    add_truncate(txn_id, start_ts, durable_ts, start, stop);

    WT_TRUNCATE *matched = nullptr;
    const char *key = "0150";
    set_reader(txn_id, WT_TS_NONE);
    REQUIRE(truncate_visible(key, &matched) == 0);
    REQUIRE(matched != nullptr);

    const std::string_view matched_start{
      static_cast<const char *>(matched->start_key.data), matched->start_key.size};
    REQUIRE(matched_start == start);
}

TEST_CASE_METHOD(layered_truncate_visibility_fixture,
  "other transaction uncommitted truncate is invisible", "[layered][truncate]")
{
    uint64_t txn_id = 250;
    const wt_timestamp_t start_ts = WT_TS_NONE;
    const wt_timestamp_t durable_ts = WT_TS_NONE;
    const char *start = "0100";
    const char *stop = "0700";

    add_truncate(txn_id, start_ts, durable_ts, start, stop);

    txn_id = 60;
    const wt_timestamp_t read_timestamp = WT_TS_NONE;

    set_reader(txn_id, read_timestamp);

    const char *key = "0150";
    REQUIRE(truncate_visible(key) == WT_NOTFOUND);
}

TEST_CASE_METHOD(layered_truncate_visibility_fixture, "read timestamp gates committed truncate",
  "[layered][truncate]")
{
    const uint64_t txn_id = 10;
    const wt_timestamp_t start_ts = 30;
    const wt_timestamp_t durable_ts = 30;
    const char *start = "0100";
    const char *stop = "0700";

    add_truncate(txn_id, start_ts, durable_ts, start, stop);

    SECTION("older read timestamp still sees the key")
    {
        const uint64_t txn_id = 60;
        const wt_timestamp_t read_timestamp = 20;

        set_reader(txn_id, read_timestamp);

        const char *key = "0150";
        REQUIRE(truncate_visible(key) == WT_NOTFOUND);
    }

    SECTION("equal or newer read timestamp sees the truncate")
    {
        const uint64_t txn_id = 60;
        wt_timestamp_t read_timestamp = 30;

        set_reader(txn_id, read_timestamp);

        const char *key = "0150";
        REQUIRE(truncate_visible(key) == 0);

        read_timestamp = 40;
        set_reader(txn_id, read_timestamp);
        REQUIRE(truncate_visible(key) == 0);
    }
}

TEST_CASE_METHOD(layered_truncate_visibility_fixture, "overlapping ranges honor visible timestamps",
  "[layered][truncate]")
{
    uint64_t txn_id = 10;
    wt_timestamp_t start_ts = 20;
    wt_timestamp_t durable_ts = 20;
    const char *start = "0100";
    const char *stop = "0400";

    add_truncate(txn_id, start_ts, durable_ts, start, stop);

    txn_id = 11;
    start_ts = 40;
    durable_ts = 40;
    start = "0300";
    stop = "0700";

    add_truncate(txn_id, start_ts, durable_ts, start, stop);

    SECTION("only the older truncate is visible")
    {
        const uint64_t txn_id = 60;
        const wt_timestamp_t read_timestamp = 30;

        set_reader(txn_id, read_timestamp);

        const char *key = "0350";
        REQUIRE(truncate_visible(key) == 0);

        key = "0500";
        REQUIRE(truncate_visible(key) == WT_NOTFOUND);
    }

    SECTION("both truncates are visible")
    {
        const uint64_t txn_id = 60;
        const wt_timestamp_t read_timestamp = 40;

        set_reader(txn_id, read_timestamp);

        const char *key = "0350";
        REQUIRE(truncate_visible(key) == 0);

        key = "0500";
        REQUIRE(truncate_visible(key) == 0);
    }
}

TEST_CASE_METHOD(layered_truncate_visibility_fixture, "truncate commit stamps the truncate entry",
  "[layered][truncate]")
{
    uint64_t txn_id = 250;
    const wt_timestamp_t start_ts = WT_TS_NONE;
    const wt_timestamp_t durable_ts = WT_TS_NONE;
    const char *start = "0100";
    const char *stop = "0200";
    const char *key = "0125";

    /* Truncate at txn 250: uncommitted. */
    WT_TRUNCATE *committed = add_truncate(txn_id, start_ts, durable_ts, start, stop);

    txn_id = 60;
    wt_timestamp_t read_timestamp = 30;

    /* Reader txn 60 sees no truncate yet: txn 250 is still uncommitted. */
    set_reader(txn_id, read_timestamp);
    REQUIRE(truncate_visible(key) == WT_NOTFOUND);

    txn_id = 250;
    const wt_timestamp_t commit_timestamp = 30;
    const wt_timestamp_t durable_timestamp = 40;

    /* Set txn 250's commit state so the entry can be stamped. */
    set_committer(txn_id, commit_timestamp, durable_timestamp);

    WT_TXN_OP op{};
    op.u.follower_truncate.t = committed;

    /* Publish the entry: commit_ts=30, durable_ts=40, committed. */
    __wti_mark_committed_truncate_table_apply(session, &layered_table, &op);
    REQUIRE(committed->txn_id == txn_id);
    REQUIRE(committed->start_ts == commit_timestamp);
    REQUIRE(committed->durable_ts == durable_timestamp);
    REQUIRE(committed->committed);

    txn_id = 60;
    read_timestamp = 30;

    /* Read ts 30 is below durable_ts=40, so the published truncate stays hidden. */
    set_reader(txn_id, read_timestamp);
    REQUIRE(truncate_visible(key) == WT_NOTFOUND);

    read_timestamp = 40;

    /* Read ts 40 reaches durability, but txn 250 is still in-flight to this snapshot (100-200). */
    set_reader(txn_id, read_timestamp);
    REQUIRE(truncate_visible(key) == WT_NOTFOUND);

    txn_id = 300;

    /* With snap_min=300, txn 250 is visible and its published truncate matches the key. */
    set_reader(txn_id, read_timestamp, WT_ISO_SNAPSHOT, 300, 400);
    WT_TRUNCATE *matched = nullptr;
    REQUIRE(truncate_visible(key, &matched) == 0);
    REQUIRE(matched == committed);
}

TEST_CASE_METHOD(layered_truncate_visibility_fixture,
  "truncate commits that overlap honor visibility", "[layered][truncate]")
{
    uint64_t txn_id = 250;
    const wt_timestamp_t start_ts = WT_TS_NONE;
    const wt_timestamp_t durable_ts = WT_TS_NONE;
    const char *start = "0100";
    const char *stop = "0200";

    /* Truncate at txn 250: uncommitted. */
    WT_TRUNCATE *committed = add_truncate(txn_id, start_ts, durable_ts, start, stop);

    const uint64_t overlapping_txn_id = 10;
    const wt_timestamp_t overlapping_start_ts = 40;
    const wt_timestamp_t overlapping_durable_ts = 40;

    /* Truncate at txn 10: already committed, over 0150-0300. */
    WT_TRUNCATE *overlapping = add_truncate(
      overlapping_txn_id, overlapping_start_ts, overlapping_durable_ts, "0150", "0300");

    txn_id = 60;
    wt_timestamp_t read_timestamp = 30;
    const char *overlap_key = "0175";
    const char *overlap_only_key = "0250";

    /* At read ts 30, the published overlap is too new and txn 250 is still uncommitted. */
    set_reader(txn_id, read_timestamp);
    REQUIRE(truncate_visible(overlap_key) == WT_NOTFOUND);
    REQUIRE(truncate_visible(overlap_only_key) == WT_NOTFOUND);

    txn_id = 250;
    const wt_timestamp_t commit_timestamp = 30;
    const wt_timestamp_t durable_timestamp = 40;

    /* Set txn 250's commit state so the entry can be stamped. */
    set_committer(txn_id, commit_timestamp, durable_timestamp);

    WT_TXN_OP op{};
    op.u.follower_truncate.t = committed;

    /* Publish txn 250's entry; both truncates are now durable at ts 40. */
    __wti_mark_committed_truncate_table_apply(session, &layered_table, &op);

    txn_id = 60;
    read_timestamp = 40;
    const char *target_only_key = "0125";

    /* At read ts 40, txn 250 is still in-flight, so only the older visible overlap can match. */
    set_reader(txn_id, read_timestamp);
    REQUIRE(truncate_visible(target_only_key) == WT_NOTFOUND);

    WT_TRUNCATE *matched = nullptr;
    REQUIRE(truncate_visible(overlap_key, &matched) == 0);
    REQUIRE(matched == overlapping);
    REQUIRE(truncate_visible(overlap_only_key) == 0);

    txn_id = 300;

    /* With snap_min 300, txn 250 becomes visible and wins on the shared key range. */
    set_reader(txn_id, read_timestamp, WT_ISO_SNAPSHOT, 300, 400);
    REQUIRE(truncate_visible(target_only_key) == 0);

    matched = nullptr;
    REQUIRE(truncate_visible(overlap_key, &matched) == 0);
    REQUIRE(matched == committed);
    REQUIRE(truncate_visible(overlap_only_key) == 0);
}

TEST_CASE_METHOD(layered_truncate_visibility_fixture,
  "truncate rollback removes only the targeted entry", "[layered][truncate]")
{
    uint64_t txn_id = 50;
    const wt_timestamp_t start_ts = WT_TS_NONE;
    const wt_timestamp_t durable_ts = WT_TS_NONE;
    const char *start = "0100";
    const char *stop = "0200";

    WT_TRUNCATE *rolled_back = add_truncate(txn_id, start_ts, durable_ts, start, stop);

    txn_id = 51;
    start = "0150";
    stop = "0400";

    WT_TRUNCATE *surviving = add_truncate(txn_id, start_ts, durable_ts, start, stop);

    WT_TXN_OP op{};
    op.u.follower_truncate.t = rolled_back;

    __wti_layered_table_truncate_rollback_apply(session, &layered_table, &op);
    REQUIRE(op.u.follower_truncate.t == nullptr);
    REQUIRE(TAILQ_FIRST(&layered_table.truncateqh) == surviving);
    REQUIRE(TAILQ_NEXT(surviving, q) == nullptr);

    const char *key = "0125";
    set_reader(50, WT_TS_NONE);
    REQUIRE(truncate_visible(key) == WT_NOTFOUND);

    WT_TRUNCATE *matched = nullptr;
    key = "0175";
    REQUIRE(truncate_visible(key, &matched) == 0);
    REQUIRE(matched == surviving);

    key = "0350";
    REQUIRE(truncate_visible(key) == 0);
}
