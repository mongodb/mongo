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
 * [sub_level_error_msg_macros]: test_sub_level_error_msg_macros.cpp
 * Tests the macros for storing verbose information about the last error of the session.
 */

int
test_wt_ret_sub(
  WT_SESSION_IMPL *session_impl, int err, int sub_level_err, const char *err_msg_content)
{
    WT_RET_SUB(session_impl, err, sub_level_err, "%s", err_msg_content);
}

int
test_wt_err_sub(
  WT_SESSION_IMPL *session_impl, int err, int sub_level_err, const char *err_msg_content)
{
    WT_DECL_RET;
    WT_ERR_SUB(session_impl, err, sub_level_err, "%s", err_msg_content);
err:
    return (ret);
}

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

using namespace utils;

TEST_CASE("Test WT_RET_SUB, WT_ERR_SUB, WT_RET_MSG, WT_ERR_MSG",
  "[sub_level_error_msg_macros],[sub_level_error]")
{
    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();

    WT_SESSION *session;
    REQUIRE(conn->open_session(conn, NULL, NULL, &session) == 0);

    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    WT_ERROR_INFO *err_info = &(session_impl->err_info);

    SECTION(
      "Test WT_RET_SUB with EINVAL error WT_BACKGROUND_COMPACT_ALREADY_RUNNING sub_level_error")
    {
        const char *err_msg_content = "Some EINVAL error";
        REQUIRE(test_wt_ret_sub(session_impl, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING,
                  err_msg_content) == EINVAL);
        check_error_info(err_info, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING, err_msg_content);
    }

    SECTION(
      "Test WT_ERR_SUB with EINVAL error WT_BACKGROUND_COMPACT_ALREADY_RUNNING sub_level_error")
    {
        const char *err_msg_content = "Some EINVAL error";
        REQUIRE(test_wt_err_sub(session_impl, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING,
                  err_msg_content) == EINVAL);
        check_error_info(err_info, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING, err_msg_content);
    }

    SECTION("Test WT_RET_MSG with EINVAL error")
    {
        const char *err_msg_content = "Some EINVAL error";
        REQUIRE(test_wt_ret_msg(session_impl, EINVAL, err_msg_content) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);
    }

    SECTION("Test WT_ERR_MSG with EINVAL error")
    {
        const char *err_msg_content = "Some EINVAL error";
        REQUIRE(test_wt_err_msg(session_impl, EINVAL, err_msg_content) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);
    }
}
