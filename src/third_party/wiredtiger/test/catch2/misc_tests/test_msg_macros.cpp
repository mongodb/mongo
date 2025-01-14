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

/*
 * [wt_msg]: test_msg_macros.cpp
 * Tests the macros for storing verbose information about the last error of the session.
 */

int
test_wt_ret_msg(WT_SESSION_IMPL *session_impl, int err, const char *err_msg_content)
{
    WT_RET_MSG(session_impl, err, "%s", err_msg_content);
}

int
test_wt_err_msg(WT_SESSION_IMPL *session_impl, int err, const char *err_msg_content)
{
    WT_DECL_RET;
    WT_ERR_MSG(session_impl, err, "%s", err_msg_content);
err:
    return (ret);
}

TEST_CASE("Test WT_RET_MSG and WT_ERR_MSG", "[message_macros]")
{
    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();

    WT_SESSION *session;
    REQUIRE(conn->open_session(conn, NULL, NULL, &session) == 0);

    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;

    SECTION("Test WT_RET_MSG with initial values")
    {
        const char *err_msg_content = "";
        REQUIRE(test_wt_ret_msg(session_impl, 0, err_msg_content) == 0);
        CHECK(session_impl->err_info.err == 0);
        CHECK(session_impl->err_info.sub_level_err == WT_NONE);
        CHECK(strcmp(session_impl->err_info.err_msg, err_msg_content) == 0);
    }

    SECTION("Test WT_ERR_MSG with initial values")
    {
        const char *err_msg_content = "";
        REQUIRE(test_wt_err_msg(session_impl, 0, err_msg_content) == 0);
        CHECK(session_impl->err_info.err == 0);
        CHECK(session_impl->err_info.sub_level_err == WT_NONE);
        CHECK(strcmp(session_impl->err_info.err_msg, err_msg_content) == 0);
    }

    SECTION("Test WT_RET_MSG with EINVAL error")
    {
        const char *err_msg_content = "Some EINVAL error";
        REQUIRE(test_wt_ret_msg(session_impl, EINVAL, err_msg_content) == EINVAL);
        CHECK(session_impl->err_info.err == EINVAL);
        CHECK(session_impl->err_info.sub_level_err == WT_NONE);
        CHECK(strcmp(session_impl->err_info.err_msg, err_msg_content) == 0);
    }

    SECTION("Test WT_ERR_MSG with EINVAL error")
    {
        const char *err_msg_content = "Some EINVAL error";
        REQUIRE(test_wt_err_msg(session_impl, EINVAL, err_msg_content) == EINVAL);
        CHECK(session_impl->err_info.err == EINVAL);
        CHECK(session_impl->err_info.sub_level_err == WT_NONE);
        CHECK(strcmp(session_impl->err_info.err_msg, err_msg_content) == 0);
    }
}
