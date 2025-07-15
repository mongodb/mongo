/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "wt_internal.h"
#include "../../wrappers/connection_wrapper.h"
#include "../utils_sub_level_error.h"

/*
 * [sub_level_error_compact]: test_sub_level_error_compact.cpp
 * Tests the error handling for compact workflows.
 */

using namespace utils;

TEST_CASE("Test functions for error handling in compaction workflows",
  "[sub_level_error_compact],[sub_level_error]")
{
    WT_SESSION *session;

    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();
    REQUIRE(conn->open_session(conn, NULL, NULL, &session) == 0);
    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    WT_CONNECTION_IMPL *conn_impl = (WT_CONNECTION_IMPL *)conn;
    WT_ERROR_INFO *err_info = &(session_impl->err_info);

    SECTION("Test __wt_background_compact_signal - in-memory or readonly database")
    {
        // Set database as in-memory and readonly.
        F_SET(conn_impl, WT_CONN_IN_MEMORY | WT_CONN_READONLY);
        CHECK(__wt_background_compact_signal(session_impl, NULL) == ENOTSUP);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
        // Clear in-memory and read only flag as connection close requires them to be cleared.
        F_CLR(conn_impl, WT_CONN_IN_MEMORY | WT_CONN_READONLY);
    }

    SECTION("Test __wt_background_compact_signal - changes in config string")
    {
        // New background compaction config string doesn't contain background key.
        CHECK(__wt_background_compact_signal(session_impl, "") == WT_NOTFOUND);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);

        // Set new background compaction config string to false.
        CHECK(__wt_background_compact_signal(session_impl, "background=false") == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);

        // Set new background compaction config string to true.
        CHECK(__wt_background_compact_signal(session_impl, "background=true") == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test __wt_background_compact_signal - background_compact configuration")
    {
        // Set current background compaction running to true and expect the new background
        // configuration to match the already set configuration.
        conn_impl->background_compact.running = true;
        conn_impl->background_compact.config =
          "dryrun=false,exclude=,free_space_target=20MB,run_once=false,timeout=1200";

        CHECK(__wt_background_compact_signal(session_impl, "background=true") == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);

        // Expect the new configuration not match the already set configuration. Expect an error.
        conn_impl->background_compact.config = "";

        CHECK(__wt_background_compact_signal(session_impl, "background=true") == EINVAL);
        check_error_info(err_info, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING,
          "Cannot reconfigure background compaction while it's already running.");

        // Cannot have background compaction ran without a configuration, and the config string will
        // be freed, but the string is owned by the test rather than the session so it is reset.
        conn_impl->background_compact.running = false;
        conn_impl->background_compact.config = NULL;
    }
}
