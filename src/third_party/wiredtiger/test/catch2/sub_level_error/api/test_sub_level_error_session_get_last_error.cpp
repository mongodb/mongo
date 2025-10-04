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
 * [sub_level_error_session_get_last_error]: test_sub_level_error_session_get_last_error.cpp
 * Tests the API for getting verbose information about the last error of the session.
 */

TEST_CASE("Session get last error - test getting verbose info about the last error in the session",
  "[sub_level_error_session_get_last_error],[sub_level_error]")
{
    WT_CONNECTION *conn;
    WT_SESSION *session;

    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    conn = conn_wrapper.get_wt_connection();
    REQUIRE(conn->open_session(conn, NULL, NULL, &session) == 0);

    SECTION("Test default values")
    {
        /* Prepare return arguments. */
        int err, sub_level_err;
        const char *err_msg;

        /* Call the error info API. */
        session->get_last_error(session, &err, &sub_level_err, &err_msg);

        /* Test that the API returns expected default values. */
        CHECK(err == 0);
        CHECK(sub_level_err == WT_NONE);
        CHECK(strcmp(err_msg, WT_ERROR_INFO_SUCCESS) == 0);
    }
}
