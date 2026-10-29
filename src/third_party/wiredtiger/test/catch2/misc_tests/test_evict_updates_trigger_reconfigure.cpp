/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <atomic>
#include <thread>

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"

/*
 * Reconfiguring without naming eviction_updates_trigger must never make concurrent readers of
 * WT_EVICT::eviction_updates_trigger observe zero: a zero reading is indistinguishable from "not
 * yet defaulted" and disables the updates-trigger check in eviction.
 */
TEST_CASE("Reconfiguring eviction leaves the updates trigger visible to readers", "[evict]")
{
    /*
     * Leave eviction_updates_trigger unset so it is auto-computed from eviction_dirty_trigger: the
     * auto-computed value is never written back into the connection's saved configuration string,
     * so every reconfigure call re-derives it from scratch.
     */
    connection_wrapper conn_wrapper =
      connection_wrapper(".", "create,eviction_dirty_trigger=20,eviction_dirty_target=5");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();
    WT_EVICT *evict = conn_wrapper.get_wt_connection_impl()->evict;

    std::atomic<bool> stop{false};
    std::atomic<bool> zero_seen{false};

    std::thread reader([&]() {
        while (!stop.load()) {
            if (__wt_atomic_load_double_relaxed(&evict->eviction_updates_trigger) < DBL_EPSILON)
                zero_seen.store(true);
        }
    });

    for (int i = 0; i < 5000 && !zero_seen.load(); i++) {
        REQUIRE(conn->reconfigure(conn, "eviction_dirty_target=6") == 0);
        REQUIRE(conn->reconfigure(conn, "eviction_dirty_target=5") == 0);
    }

    stop.store(true);
    reader.join();

    REQUIRE(!zero_seen.load());
}
