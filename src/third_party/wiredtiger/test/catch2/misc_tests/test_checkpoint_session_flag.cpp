/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include <filesystem>
#include <string>

#include "wiredtiger.h"
#include "../wrappers/connection_wrapper.h"
#include "wt_internal.h"

static constexpr const char *k_db = "WT_TEST.checkpoint_session_flag";

TEST_CASE("WT_SESSION_CHECKPOINT is clear after a reserved-name checkpoint fails",
  "[checkpoint_session_flag]")
{
    std::filesystem::remove_all(k_db);
    connection_wrapper conn(k_db, "create,statistics=(all)");

    WT_SESSION *wt_session;
    REQUIRE(conn.get_wt_connection()->open_session(
              conn.get_wt_connection(), nullptr, nullptr, &wt_session) == 0);
    WT_SESSION_IMPL *session = reinterpret_cast<WT_SESSION_IMPL *>(wt_session);

    REQUIRE(wt_session->checkpoint(wt_session, "name=all") == EINVAL);
    CHECK_FALSE(F_ISSET(session, WT_SESSION_CHECKPOINT));
}

TEST_CASE(
  "WT_SESSION_CHECKPOINT is clear after an internal-prefix name fails", "[checkpoint_session_flag]")
{
    std::filesystem::remove_all(k_db);
    connection_wrapper conn(k_db, "create,statistics=(all)");

    WT_SESSION *wt_session;
    REQUIRE(conn.get_wt_connection()->open_session(
              conn.get_wt_connection(), nullptr, nullptr, &wt_session) == 0);
    WT_SESSION_IMPL *session = reinterpret_cast<WT_SESSION_IMPL *>(wt_session);

    const std::string reserved = std::string(WT_CHECKPOINT) + ".user";
    const std::string cfg = "name=" + reserved;

    REQUIRE(wt_session->checkpoint(wt_session, cfg.c_str()) == EINVAL);
    CHECK_FALSE(F_ISSET(session, WT_SESSION_CHECKPOINT));
}

TEST_CASE("Session remains usable for write transactions after checkpoint fails",
  "[checkpoint_session_flag]")
{
    std::filesystem::remove_all(k_db);
    connection_wrapper conn(k_db, "create,statistics=(all)");

    WT_SESSION *wt_session;
    REQUIRE(conn.get_wt_connection()->open_session(
              conn.get_wt_connection(), nullptr, nullptr, &wt_session) == 0);

    REQUIRE(wt_session->create(wt_session, "table:flag", "key_format=S,value_format=S") == 0);

    REQUIRE(wt_session->checkpoint(wt_session, "name=all") == EINVAL);
    REQUIRE(wt_session->begin_transaction(wt_session, nullptr) == 0);

    WT_CURSOR *cursor;
    REQUIRE(wt_session->open_cursor(wt_session, "table:flag", nullptr, nullptr, &cursor) == 0);

    cursor->set_key(cursor, "k1");
    cursor->set_value(cursor, "v1");

    REQUIRE(cursor->insert(cursor) == 0);
    REQUIRE(cursor->close(cursor) == 0);
    REQUIRE(wt_session->commit_transaction(wt_session, nullptr) == 0);
}

TEST_CASE("Checkpoint_state statistic is inactive after early-failure checkpoint",
  "[checkpoint_session_flag]")
{
    std::filesystem::remove_all(k_db);
    connection_wrapper conn(k_db, "create,statistics=(all)");
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();

    WT_SESSION *wt_session;
    REQUIRE(conn.get_wt_connection()->open_session(
              conn.get_wt_connection(), nullptr, nullptr, &wt_session) == 0);

    REQUIRE(wt_session->checkpoint(wt_session, "name=all") == EINVAL);

    CHECK(conn_impl->stats[0]->checkpoint_state == WTI_CHECKPOINT_STATE_INACTIVE);
}

TEST_CASE("Checkpoint_state is inactive when checkpoint is skipped", "[checkpoint_session_flag]")
{
    std::filesystem::remove_all(k_db);

    connection_wrapper conn(k_db, "create,statistics=(all)");
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();

    WT_SESSION *wt_session;
    REQUIRE(conn.get_wt_connection()->open_session(
              conn.get_wt_connection(), nullptr, nullptr, &wt_session) == 0);

    REQUIRE(wt_session->checkpoint(wt_session, "use_timestamp=false") == 0);

    CHECK(conn_impl->stats[0]->checkpoint_state == WTI_CHECKPOINT_STATE_INACTIVE);
}

TEST_CASE(
  "Checkpoint_state is inactive after parse_config fails with ENOTSUP", "[checkpoint_session_flag]")
{
    std::filesystem::remove_all(k_db);
    connection_wrapper conn(k_db, "create,statistics=(all)");
    WT_CONNECTION_IMPL *conn_impl = conn.get_wt_connection_impl();

    WT_SESSION *wt_session;
    REQUIRE(conn.get_wt_connection()->open_session(
              conn.get_wt_connection(), nullptr, nullptr, &wt_session) == 0);

    REQUIRE(wt_session->checkpoint(wt_session, "debug=(database_size_fix=true)") == ENOTSUP);
    CHECK(conn_impl->stats[0]->checkpoint_state == WTI_CHECKPOINT_STATE_INACTIVE);
}
