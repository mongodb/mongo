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
 * [sub_level_error_nested_api_calls]: test_sub_level_error_nested_api_calls.cpp
 * Tests that nested API calls record errors in the session error_info struct only when necessary.
 */

using namespace utils;

static int
cursor_api_call_with_notfound(
  WT_CURSOR *cursor, WT_SESSION_IMPL *session_impl, bool set_err_info_inside)
{
    WT_DECL_RET;
    CURSOR_API_CALL(cursor, session_impl, ret, next, NULL);
    ret = WT_NOTFOUND;
    if (set_err_info_inside)
        __wt_session_set_last_error(session_impl, ret, WT_NONE, "Something was not found");
err:
    API_END_RET(session_impl, ret);
}

static int
api_call_nested_with_notfound(WT_SESSION_IMPL *session_impl, WT_CURSOR *cursor, int final_err,
  bool notfound_ok, bool set_err_info_inside)
{
    WT_DECL_RET;
    SESSION_API_CALL_NOCONF(session_impl, log_printf);

    if (notfound_ok)
        WT_ERR_NOTFOUND_OK(
          cursor_api_call_with_notfound(cursor, session_impl, set_err_info_inside), true);
    else
        WT_ERR(cursor_api_call_with_notfound(cursor, session_impl, set_err_info_inside));

    /* Simulate an error being raised later during the API call. */
    ret = final_err;
    if (final_err != 0)
        __wt_session_set_last_error(session_impl, final_err, WT_NONE, "Something was invalid");
err:
    API_END_RET(session_impl, ret);
}

TEST_CASE("API_END_RET nested - test that nested API calls only keep explicitly set errors",
  "[sub_level_error_nested_api_calls],[sub_level_error]")
{
    WT_SESSION *session;
    std::string uri = "table:cursor_test";

    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();
    REQUIRE(conn->open_session(conn, NULL, NULL, &session) == 0);

    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    WT_ERROR_INFO *err_info = &(session_impl->err_info);

    WT_CURSOR *cursor = NULL;
    REQUIRE(session->create(session, uri.c_str(), "key_format=S,value_format=S") == 0);
    REQUIRE(session->open_cursor(session, uri.c_str(), NULL, NULL, &cursor) == 0);
    REQUIRE(cursor != NULL);

    /*
     * These sections DO NOT explicitly set the err_info struct inside the nested API call. The
     * content of the err_info struct at the end of the top-level API call depends on whether the
     * ret value is discarded by WT_ERR_NOTFOUND_OK.
     */
    SECTION("Test nested API call with WT_NOTFOUND inside WT_ERR_NOTFOUND_OK()")
    {
        REQUIRE(api_call_nested_with_notfound(session_impl, cursor, 0, true, false) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test nested API call with WT_NOTFOUND inside WT_ERR_NOTFOUND_OK(), followed by EINVAL")
    {
        REQUIRE(api_call_nested_with_notfound(session_impl, cursor, EINVAL, true, false) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, "Something was invalid");
    }

    SECTION("Test nested API call with WT_NOTFOUND inside WT_ERR()")
    {
        REQUIRE(
          api_call_nested_with_notfound(session_impl, cursor, 0, false, false) == WT_NOTFOUND);
        check_error_info(err_info, WT_NOTFOUND, WT_NONE, WT_ERROR_INFO_EMPTY);
    }

    SECTION("Test nested API call with WT_NOTFOUND inside WT_ERR(), followed by EINVAL")
    {
        REQUIRE(
          api_call_nested_with_notfound(session_impl, cursor, EINVAL, false, false) == WT_NOTFOUND);
        check_error_info(err_info, WT_NOTFOUND, WT_NONE, WT_ERROR_INFO_EMPTY);
    }

    /*
     * These sections DO explicitly set the err_info struct inside the nested API call.
     */
    SECTION("Test nested API call with WT_NOTFOUND set explicitly inside WT_ERR_NOTFOUND_OK()")
    {
        REQUIRE(api_call_nested_with_notfound(session_impl, cursor, 0, true, true) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION(
      "Test nested API call with WT_NOTFOUND set explicitly inside WT_ERR_NOTFOUND_OK(), followed "
      "by EINVAL")
    {
        REQUIRE(api_call_nested_with_notfound(session_impl, cursor, EINVAL, true, true) == EINVAL);
        check_error_info(err_info, EINVAL, WT_NONE, "Something was not found");
    }

    SECTION("Test nested API call with WT_NOTFOUND set explicitly inside WT_ERR()")
    {
        REQUIRE(api_call_nested_with_notfound(session_impl, cursor, 0, false, true) == WT_NOTFOUND);
        check_error_info(err_info, WT_NOTFOUND, WT_NONE, "Something was not found");
    }

    SECTION(
      "Test nested API call with WT_NOTFOUND set explicitly inside WT_ERR(), followed by EINVAL")
    {
        REQUIRE(
          api_call_nested_with_notfound(session_impl, cursor, EINVAL, false, true) == WT_NOTFOUND);
        check_error_info(err_info, WT_NOTFOUND, WT_NONE, "Something was not found");
    }
}
