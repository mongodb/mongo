/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"

TEST_CASE("Eviction walk budget scales with cache share", "[evict_walk_budget]")
{
    struct {
        uint32_t target;
        bool clean, enabled;
        uint64_t tree, cache, min_pages;
    } cases[] = {
      {1, true, false, 100, 100, 10},
      {20, true, false, 100, 100, 200},
      {1, true, true, 0, 100, 100},
      {1, true, true, 1, 100, 109},
      {1, true, true, 20, 100, 280},
      {1, true, true, 50, 100, 550},
      {1, true, true, 100, 100, 1000},
      {1, true, true, 200, 100, 1000},
      {50, true, true, 50, 100, 550},
      {50, true, true, 55, 100, 595},
      {1, true, true, 100, 0, 10},
      {200, true, true, 20, 100, 2000},
      {1, false, true, 50, 100, 100},
      {20, false, true, 50, 100, 2000},
    };

    for (const auto &c : cases) {
        INFO("target=" << c.target << " clean=" << c.clean << " enabled=" << c.enabled
                       << " tree=" << c.tree << " cache=" << c.cache);
        REQUIRE(
          __evict_min_walk_pages(c.target, c.clean, c.enabled, c.tree, c.cache) == c.min_pages);
    }
}

TEST_CASE("Eviction walk budget is opt-in and can be reconfigured", "[evict_walk_budget]")
{
    SECTION("Default and reconfiguration")
    {
        connection_wrapper wrapper(".", "create");
        WT_CONNECTION *conn = wrapper.get_wt_connection();
        WT_CONNECTION_IMPL *conn_impl = wrapper.get_wt_connection_impl();

        REQUIRE_FALSE(
          __wt_atomic_load_bool_relaxed(&conn_impl->evict_config.cache_share_walk_budget));
        REQUIRE(conn->reconfigure(conn, "eviction=(cache_share_walk_budget=true)") == 0);
        REQUIRE(__wt_atomic_load_bool_relaxed(&conn_impl->evict_config.cache_share_walk_budget));
        REQUIRE(conn->reconfigure(conn, "eviction=(threads_min=1)") == 0);
        REQUIRE(__wt_atomic_load_bool_relaxed(&conn_impl->evict_config.cache_share_walk_budget));
        REQUIRE(conn->reconfigure(conn, "eviction=(cache_share_walk_budget=false)") == 0);
        REQUIRE_FALSE(
          __wt_atomic_load_bool_relaxed(&conn_impl->evict_config.cache_share_walk_budget));
    }

    SECTION("Enable at connection open")
    {
        connection_wrapper wrapper(".", "create,eviction=(cache_share_walk_budget=true)");
        WT_CONNECTION_IMPL *conn_impl = wrapper.get_wt_connection_impl();

        REQUIRE(__wt_atomic_load_bool_relaxed(&conn_impl->evict_config.cache_share_walk_budget));
    }
}
