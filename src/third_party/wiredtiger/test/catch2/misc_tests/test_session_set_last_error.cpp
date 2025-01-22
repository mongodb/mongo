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
#include "../utils.h"

/*
 * [session_set_last_error]: test_session_set_last_error.cpp
 * Tests the function for storing verbose information about the last error of the session.
 */

TEST_CASE("Session set last error - test storing verbose info about the last error in the session",
  "[session_set_last_error]")
{
    WT_SESSION *session;

    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();
    REQUIRE(conn->open_session(conn, NULL, NULL, &session) == 0);

    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    WT_ERROR_INFO *err_info = &(session_impl->err_info);

    SECTION("Test with initial values")
    {
        const char *err_msg_content = WT_ERROR_INFO_EMPTY;
        REQUIRE(__wt_session_set_last_error(session_impl, 0, WT_NONE, err_msg_content) == 0);
        utils::check_error_info(err_info, 0, WT_NONE, err_msg_content);
    }

    SECTION("Test with EINVAL error")
    {
        const char *err_msg_content = "Some EINVAL error";
        REQUIRE(__wt_session_set_last_error(session_impl, EINVAL,
                  WT_BACKGROUND_COMPACT_ALREADY_RUNNING, err_msg_content) == 0);
        utils::check_error_info(
          err_info, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING, err_msg_content);
    }

    SECTION("Test with multiple errors (varying err/sub_level_err/err_msg)")
    {
        const char *err_msg_content_EINVAL = "Some EINVAL error";
        const char *err_msg_content_EBUSY = "Some EBUSY error";
        REQUIRE(__wt_session_set_last_error(session_impl, 0, WT_NONE, WT_ERROR_INFO_EMPTY) == 0);
        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_EMPTY);
        REQUIRE(__wt_session_set_last_error(session_impl, EINVAL,
                  WT_BACKGROUND_COMPACT_ALREADY_RUNNING, err_msg_content_EINVAL) == 0);
        utils::check_error_info(
          err_info, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING, err_msg_content_EINVAL);
        REQUIRE(__wt_session_set_last_error(
                  session_impl, EBUSY, WT_UNCOMMITTED_DATA, err_msg_content_EBUSY) == 0);
        utils::check_error_info(err_info, EBUSY, WT_UNCOMMITTED_DATA, err_msg_content_EBUSY);
        REQUIRE(__wt_session_set_last_error(
                  session_impl, EBUSY, WT_DIRTY_DATA, err_msg_content_EBUSY) == 0);
        utils::check_error_info(err_info, EBUSY, WT_DIRTY_DATA, err_msg_content_EBUSY);
        REQUIRE(__wt_session_set_last_error(session_impl, 0, WT_NONE, NULL) == 0);
        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test with large error message")
    {
        std::string err_msg_string(1024, 'a');
        const char *err_msg_content = err_msg_string.c_str();
        REQUIRE(__wt_session_set_last_error(session_impl, EINVAL, WT_NONE, err_msg_content) == 0);
        utils::check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);
    }
}
