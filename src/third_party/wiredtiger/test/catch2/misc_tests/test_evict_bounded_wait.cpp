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

TEST_CASE("Eviction bounded wait remaining time", "[evict]")
{
    REQUIRE(
      __evict_bounded_wait_remaining_us(0, WTI_EVICT_BOUNDED_WAIT_US) == WTI_EVICT_BOUNDED_WAIT_US);
    REQUIRE(__evict_bounded_wait_remaining_us(
              WTI_EVICT_BOUNDED_WAIT_US - 1, WTI_EVICT_BOUNDED_WAIT_US) == 1);
    REQUIRE(
      __evict_bounded_wait_remaining_us(WTI_EVICT_BOUNDED_WAIT_US, WTI_EVICT_BOUNDED_WAIT_US) == 0);
    REQUIRE(__evict_bounded_wait_remaining_us(
              WTI_EVICT_BOUNDED_WAIT_US + 1, WTI_EVICT_BOUNDED_WAIT_US) == 0);
}

TEST_CASE("Eviction bounded wait limit falls back without an operation timeout", "[evict]")
{
    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    WT_SESSION *session;
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();
    REQUIRE(conn->open_session(conn, NULL, NULL, &session) == 0);
    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;

    /* No operation timeout configured: the caller gets the default cap. */
    session_impl->operation_timeout_us = session_impl->operation_start_us = 0;
    REQUIRE(__evict_bounded_wait_limit_us(session_impl) == WTI_EVICT_BOUNDED_WAIT_US);

    /* An operation timeout that has already elapsed leaves nothing to wait for. */
    session_impl->operation_timeout_us = 1;
    session_impl->operation_start_us = __wt_clock(session_impl);
    __wt_sleep(0, 10 * WT_THOUSAND);
    REQUIRE(__evict_bounded_wait_limit_us(session_impl) == 0);
}
