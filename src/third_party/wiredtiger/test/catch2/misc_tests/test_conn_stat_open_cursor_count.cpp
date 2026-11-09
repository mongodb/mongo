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
 * [conn_stat]: conn_stat.c
 * Test that the connection open cursor statistic counts cursors of internal sessions.
 */
TEST_CASE("conn stats: open cursor count includes internal sessions", "[statistics][session]")
{
    const std::string home = "WT_TEST.conn_stat_open_cursor_count";

    connection_wrapper conn(home, "create,statistics=(all)");
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();
    WT_SESSION_IMPL *session = conn.create_session();

    int64_t *stat = &conn_impl->stats[0]->cursor_open_count;

    __wt_conn_stat_init(session);
    int64_t before = *stat;

    WT_SESSION_IMPL *internal_session = nullptr;
    REQUIRE(
      __wt_open_internal_session(conn_impl, "stat_internal", false, 0, 0, &internal_session) == 0);

    /* The exact cursor count is implementation-defined, so assert direction only. */
    WT_CURSOR *cursor = nullptr;
    REQUIRE(internal_session->iface.open_cursor(
              &internal_session->iface, "metadata:", nullptr, nullptr, &cursor) == 0);

    __wt_conn_stat_init(session);
    CHECK(*stat > before);

    REQUIRE(cursor->close(cursor) == 0);

    __wt_conn_stat_init(session);
    CHECK(*stat == before);

    REQUIRE(__wt_session_close_internal(internal_session) == 0);
}
