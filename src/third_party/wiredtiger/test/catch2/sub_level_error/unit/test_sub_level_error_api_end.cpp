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
 * [sub_level_error_api_end]: test_sub_level_error_api_end.cpp
 * Tests that successful API calls are recorded as "successful" in the session error_info struct.
 */

using namespace utils;

int
api_call_with_error(
  WT_SESSION_IMPL *session_impl, int err, int sub_level_err, const char *err_msg_content)
{
    WT_DECL_RET;
    SESSION_API_CALL_NOCONF(session_impl, log_printf);

    ret = err;
    if (err != 0 && err_msg_content != NULL)
        __wt_session_set_last_error(session_impl, err, sub_level_err, err_msg_content);
err:
    API_END_RET(session_impl, ret);
}

int
api_call_with_no_error(WT_SESSION_IMPL *session_impl)
{
    return (api_call_with_error(session_impl, 0, WT_NONE, NULL));
}

int
txn_api_call_with_error(
  WT_SESSION_IMPL *session_impl, int err, int sub_level_err, const char *err_msg_content)
{
    WT_DECL_RET;
    SESSION_TXN_API_CALL(session_impl, ret, log_printf, NULL, cfg);
    WT_UNUSED(cfg);

    ret = err;
    if (err != 0 && err_msg_content != NULL)
        __wt_session_set_last_error(session_impl, err, sub_level_err, err_msg_content);
err:
    TXN_API_END(session_impl, ret, false);
    return (ret);
}

int
txn_api_call_with_no_error(WT_SESSION_IMPL *session_impl)
{
    return (txn_api_call_with_error(session_impl, 0, WT_NONE, NULL));
}

TEST_CASE("API_END_RET/TXN_API_END - test that the API call result is stored.",
  "[sub_level_error_api_end],[sub_level_error]")
{
    WT_SESSION *session;

    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();
    REQUIRE(conn->open_session(conn, NULL, NULL, &session) == 0);

    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    WT_ERROR_INFO *err_info = &(session_impl->err_info);

    SECTION("Test API_END_RET with no error")
    {
        REQUIRE(api_call_with_no_error(session_impl) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test API_END_RET with EINVAL (error code only)")
    {
        REQUIRE(api_call_with_error(session_impl, EINVAL, WT_NONE, NULL) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, WT_ERROR_INFO_EMPTY);
    }

    SECTION("Test API_END_RET with EINVAL (with message)")
    {
        const char *err_msg_content = "Some EINVAL error";
        REQUIRE(api_call_with_error(session_impl, EINVAL, WT_NONE, err_msg_content) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);
    }

    SECTION("Test API_END_RET with EINVAL (with repeated message)")
    {
        const char *err_msg_content = "Some EINVAL error";
        REQUIRE(api_call_with_error(session_impl, EINVAL, WT_NONE, err_msg_content) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);
        REQUIRE(api_call_with_error(session_impl, EINVAL, WT_NONE, err_msg_content) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);
    }

    SECTION("Test API_END_RET with EINVAL (with different messages)")
    {
        const char *err_msg_content_a = "Some EINVAL error";
        const char *err_msg_content_b = "Some other EINVAL error";
        REQUIRE(api_call_with_error(session_impl, EINVAL, WT_NONE, err_msg_content_a) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content_a);
        REQUIRE(api_call_with_error(session_impl, EINVAL, WT_NONE, err_msg_content_b) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content_b);
    }

    SECTION("Test API_END_RET with EBUSY (with different sub-level errors and messages)")
    {
        const char *err_msg_content_a = "Some EBUSY error";
        const char *err_msg_content_b = "Some other EBUSY error";
        REQUIRE(api_call_with_error(session_impl, EBUSY, WT_UNCOMMITTED_DATA, err_msg_content_a) ==
          EBUSY);
        check_error_info(err_info, EBUSY, WT_UNCOMMITTED_DATA, err_msg_content_a);
        REQUIRE(
          api_call_with_error(session_impl, EBUSY, WT_DIRTY_DATA, err_msg_content_a) == EBUSY);
        check_error_info(err_info, EBUSY, WT_DIRTY_DATA, err_msg_content_a);
        REQUIRE(
          api_call_with_error(session_impl, EBUSY, WT_DIRTY_DATA, err_msg_content_b) == EBUSY);
        check_error_info(err_info, EBUSY, WT_DIRTY_DATA, err_msg_content_b);
    }

    SECTION("Test TXN_API_END with no error")
    {
        REQUIRE(txn_api_call_with_no_error(session_impl) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test TXN_API_END with EINVAL (error code only)")
    {
        REQUIRE(txn_api_call_with_error(session_impl, EINVAL, WT_NONE, NULL) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, WT_ERROR_INFO_EMPTY);
    }

    SECTION("Test TXN_API_END with EINVAL (with message)")
    {
        const char *err_msg_content = "Some EINVAL error";
        REQUIRE(txn_api_call_with_error(session_impl, EINVAL, WT_NONE, err_msg_content) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);
    }

    SECTION("Test TXN_API_END with EINVAL (with repeated message)")
    {
        const char *err_msg_content = "Some EINVAL error";
        REQUIRE(txn_api_call_with_error(session_impl, EINVAL, WT_NONE, err_msg_content) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);
        REQUIRE(txn_api_call_with_error(session_impl, EINVAL, WT_NONE, err_msg_content) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, err_msg_content);
    }
}
