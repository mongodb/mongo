/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "wt_internal.h"

#include "../utils.h"
#include "../wrappers/connection_wrapper.h"

/*
 * The pinned-timestamp lag statistics are computed as unsigned timestamp subtractions. When the
 * durable timestamp trails the oldest timestamp the subtraction must not underflow. The public stat
 * cursor clamps negative aggregates to zero, so the underflow is only observable in the raw
 * per-bucket value written by the statistics update.
 */
TEST_CASE("txn stats: pinned timestamp statistics do not underflow", "[txn][statistics]")
{
    const std::string home = "WT_TEST.txn_pinned_timestamp_stat";

    /* The connection wrapper creates the home directory and cleans it up on destruction. */
    connection_wrapper conn(home, "create,statistics=(all)");
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_CONNECTION *wt_conn = conn.get_wt_connection();
    WT_SESSION_IMPL *session = conn.create_session();

    /* The update writes to bucket zero after clearing every bucket. */
    int64_t *stat = &conn_impl->stats[0]->txn_pinned_timestamp_oldest;

    SECTION("durable timestamp unset, oldest advanced")
    {
        REQUIRE(wt_conn->set_timestamp(wt_conn, "oldest_timestamp=64,stable_timestamp=64") == 0);
        __wt_txn_stats_update(session);
        CHECK(*stat == 0);
    }

    SECTION("durable timestamp explicitly behind oldest")
    {
        REQUIRE(wt_conn->set_timestamp(wt_conn, "oldest_timestamp=64,stable_timestamp=64") == 0);
        REQUIRE(wt_conn->set_timestamp(wt_conn, "durable_timestamp=20") == 0);
        __wt_txn_stats_update(session);
        CHECK(*stat == 0);
    }

    SECTION("durable timestamp ahead of oldest reflects the real lag")
    {
        REQUIRE(wt_conn->set_timestamp(wt_conn, "oldest_timestamp=64,stable_timestamp=64") == 0);
        REQUIRE(wt_conn->set_timestamp(wt_conn, "durable_timestamp=100") == 0);
        __wt_txn_stats_update(session);
        CHECK(*stat == 0x100 - 0x64);
    }
}
