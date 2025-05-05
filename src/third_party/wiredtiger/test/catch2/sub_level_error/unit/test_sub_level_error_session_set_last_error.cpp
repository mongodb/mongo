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
 * [sub_level_error_session_set_last_error]: test_sub_level_error_session_set_last_error.cpp
 * Tests the function for storing verbose information about the last error of the session.
 */

using namespace utils;

TEST_CASE("Test set_last_error and reset_last_error functions",
  "[sub_level_error_session_set_last_error],[sub_level_error]")
{
    WT_SESSION *session;

    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();
    REQUIRE(conn->open_session(conn, NULL, NULL, &session) == 0);

    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    WT_ERROR_INFO *err_info = &(session_impl->err_info);

    SECTION("Test with NULL session")
    {
        // Check that function can handle a NULL session without aborting.
        __wt_session_reset_last_error(NULL);
    }

    SECTION("Test with initial values")
    {
        const char *err_msg_content = WT_ERROR_INFO_SUCCESS;
        __wt_session_reset_last_error(session_impl);
        check_error_info(err_info, 0, WT_NONE, err_msg_content);
    }

    SECTION("Test with EINVAL error")
    {
        const char *err_msg_content = "Some EINVAL error";
        __wt_session_set_last_error(
          session_impl, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING, err_msg_content);
        check_error_info(err_info, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING, err_msg_content);
    }

    SECTION("Test overwriting/resetting the error message")
    {
        const char *err_msg_content = "error";
        __wt_session_set_last_error(session_impl, EINVAL, WT_NONE, err_msg_content);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);

        // The error message should not be overwritten.
        __wt_session_set_last_error(session_impl, EBUSY, WT_CONFLICT_BACKUP, "new error");
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);

        // The error message should be reset.
        __wt_session_reset_last_error(session_impl);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test with multiple errors (varying err/sub_level_err/err_msg)")
    {
        const char *err_msg_content_EINVAL = "Some EINVAL error";
        const char *err_msg_content_EBUSY = "Some EBUSY error";
        __wt_session_reset_last_error(session_impl);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
        __wt_session_set_last_error(
          session_impl, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING, err_msg_content_EINVAL);
        check_error_info(
          err_info, EINVAL, WT_BACKGROUND_COMPACT_ALREADY_RUNNING, err_msg_content_EINVAL);

        // Reset error.
        __wt_session_reset_last_error(session_impl);

        __wt_session_set_last_error(
          session_impl, EBUSY, WT_UNCOMMITTED_DATA, err_msg_content_EBUSY);
        check_error_info(err_info, EBUSY, WT_UNCOMMITTED_DATA, err_msg_content_EBUSY);

        // Reset error.
        __wt_session_reset_last_error(session_impl);

        __wt_session_set_last_error(session_impl, EBUSY, WT_DIRTY_DATA, err_msg_content_EBUSY);
        check_error_info(err_info, EBUSY, WT_DIRTY_DATA, err_msg_content_EBUSY);
        __wt_session_reset_last_error(session_impl);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test with large error message")
    {
        std::string err_msg_string(1024, 'a');
        const char *err_msg_content = err_msg_string.c_str();
        __wt_session_set_last_error(session_impl, EINVAL, WT_NONE, err_msg_content);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);
    }
}
