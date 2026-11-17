/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <string>

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"

/* Turning a boolean eviction option on and then off with reconfigure must leave it off. */
TEST_CASE("Boolean eviction options can be reconfigured on and off", "[evict]")
{
    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();
    WT_CACHE_EVICTION_CONTROLS *controls =
      &conn_wrapper.get_wt_connection_impl()->cache->cache_eviction_controls;

    struct {
        const char *name;
        uint32_t flag;
    } options[] = {{"incremental_app_eviction", WT_CACHE_EVICT_INCREMENTAL_APP},
      {"prefer_scrub_eviction", WT_CACHE_PREFER_SCRUB_EVICTION},
      {"skip_update_obsolete_check", WT_CACHE_SKIP_UPDATE_OBSOLETE_CHECK},
      {"bounded_wake", WT_CACHE_BOUNDED_WAKE}};

    for (auto &option : options) {
        const std::string on = std::string("eviction=[") + option.name + "=true]";
        const std::string off = std::string("eviction=[") + option.name + "=false]";

        INFO(option.name);
        REQUIRE(conn->reconfigure(conn, on.c_str()) == 0);
        REQUIRE(F_ISSET_ATOMIC_32(controls, option.flag));
        REQUIRE(conn->reconfigure(conn, off.c_str()) == 0);
        REQUIRE(!F_ISSET_ATOMIC_32(controls, option.flag));
    }
}

/* A reconfigure that does not name a boolean eviction option must leave its current value alone. */
TEST_CASE("Unrelated reconfigure preserves boolean eviction options", "[evict]")
{
    connection_wrapper conn_wrapper =
      connection_wrapper(".", "create,eviction=[prefer_scrub_eviction=true]");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();
    WT_CACHE_EVICTION_CONTROLS *controls =
      &conn_wrapper.get_wt_connection_impl()->cache->cache_eviction_controls;

    REQUIRE(conn->reconfigure(conn, "eviction=[incremental_app_eviction=true]") == 0);
    REQUIRE(conn->reconfigure(conn, "eviction_dirty_target=6") == 0);
    REQUIRE(conn->reconfigure(conn, "eviction=[app_eviction_min_cache_fill_ratio=10]") == 0);

    REQUIRE(F_ISSET_ATOMIC_32(controls, WT_CACHE_PREFER_SCRUB_EVICTION));
    REQUIRE(F_ISSET_ATOMIC_32(controls, WT_CACHE_EVICT_INCREMENTAL_APP));
}
