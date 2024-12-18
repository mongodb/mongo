/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>
#include "wt_internal.h"
#include "../wrappers/mock_session.h"

/*
 * [session_get_last_error]: test_session_get_last_error.cpp
 * Tests the API for getting verbose information about the last error of the session.
 */

static const char *home;

TEST_CASE("Session get last error - test getting verbose info about the last error in the session",
  "[session_get_last_error]")
{
    WT_CONNECTION *conn;
    WT_SESSION *session;

    /* Open a connection to the database, creating it if necessary. */
    REQUIRE(wiredtiger_open(home, NULL, "create", &conn) == 0);
    REQUIRE(conn->open_session(conn, NULL, NULL, &session) == 0);

    SECTION("Test API placeholder")
    {
        /* Prepare return arguments. */
        int err, sub_level_err;
        const char *err_msg;

        /* Call the placeholder API. */
        session->get_last_error(session, &err, &sub_level_err, &err_msg);

        /* Test that the API returns expected placeholder values. */
        CHECK(err == 0);
        CHECK(sub_level_err == 0);
        CHECK(strcmp(err_msg, "") == 0);
    }
}
