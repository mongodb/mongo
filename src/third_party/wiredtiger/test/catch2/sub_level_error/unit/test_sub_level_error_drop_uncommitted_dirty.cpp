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
#include "../../../utility/test_util.h"

/*
 * [sub_level_error_drop_conflict]: test_sub_level_error_drop_conflict.cpp
 * Tests the drop workflows that lead to EBUSY errors, and ensure that the correct sub level error
 * codes and messages are stored.
 */

#define URI "table:test_drop_uncommitted_dirty"
#define FILE_URI "file:test_drop_uncommitted_dirty.wt"
#define UNCOMMITTED_DATA_MSG "the table has uncommitted data and cannot be dropped yet"
#define DIRTY_DATA_MSG "the table has dirty data and can not be dropped yet"

/*
 * This test case covers EBUSY errors resulting from drop before committing/checkpointing changes.
 */
TEST_CASE("Test WT_UNCOMMITTED_DATA and WT_DIRTY_DATA",
  "[sub_level_error_drop_uncommitted_dirty],[sub_level_error]")
{
    std::string config = "key_format=S,value_format=S";
    WT_SESSION *session = NULL;
    WT_ERROR_INFO *err_info = NULL;

    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    utils::prepare_session_and_error(&conn_wrapper, &session, &err_info);
    REQUIRE(session->create(session, URI, config.c_str()) == 0);

    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    REQUIRE(__wt_session_get_dhandle(session_impl, FILE_URI, NULL, NULL, 0) == 0);

    SECTION("Test WT_UNCOMMITTED_DATA is not thrown")
    {
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, false) == 0);
        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test WT_UNCOMMITTED_DATA is not thrown (with visibility check only)")
    {
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, true) == 0);
        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test WT_UNCOMMITTED_DATA is not thrown (with uncommitted txn only)")
    {
        // The oldest ID is not pinned by a transaction. Set the value to a high number such
        // that it can never catch up.
        S2BT(session_impl)->max_upd_txn = 100;
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, false) == 0);
        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test WT_UNCOMMITTED_DATA is thrown (with both visibility check and uncommitted txn)")
    {
        // The oldest ID is not pinned by a transaction. Set the value to a high number such
        // that it can never catch up.
        S2BT(session_impl)->max_upd_txn = 100;
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, true) == EBUSY);
        utils::check_error_info(err_info, EBUSY, WT_UNCOMMITTED_DATA, UNCOMMITTED_DATA_MSG);
    }

    SECTION("Test WT_DIRTY_DATA is not thrown (btree is unmodified, is not bulk, is not metadata)")
    {
        FLD_SET(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, false) == 0);
        FLD_CLR(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);

        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test WT_DIRTY_DATA is not thrown (btree is unmodified, is not bulk, is metadata)")
    {
        F_SET(session_impl->dhandle, WT_DHANDLE_IS_METADATA);

        FLD_SET(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, false) == 0);
        FLD_CLR(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);

        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test WT_DIRTY_DATA is not thrown (btree is unmodified, is bulk, is not metadata)")
    {
        F_SET(S2BT(session_impl), WT_BTREE_BULK);

        FLD_SET(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, false) == 0);
        FLD_CLR(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);

        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test WT_DIRTY_DATA is not thrown (btree is unmodified, is bulk, is metadata)")
    {
        F_SET(S2BT(session_impl), WT_BTREE_BULK);
        F_SET(session_impl->dhandle, WT_DHANDLE_IS_METADATA);

        FLD_SET(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, false) == 0);
        FLD_CLR(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);

        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test WT_DIRTY_DATA is thrown (btree is modified, is not bulk, is not metadata)")
    {
        S2BT(session_impl)->modified = true;

        FLD_SET(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, false) == EBUSY);
        FLD_CLR(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);

        utils::check_error_info(err_info, EBUSY, WT_DIRTY_DATA, DIRTY_DATA_MSG);
    }

    SECTION("Test WT_DIRTY_DATA is not thrown (btree is modified, is not bulk, is metadata)")
    {
        S2BT(session_impl)->modified = true;
        F_SET(session_impl->dhandle, WT_DHANDLE_IS_METADATA);

        FLD_SET(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, false) == 0);
        FLD_CLR(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);

        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test WT_DIRTY_DATA is not thrown (btree is modified, is bulk, is not metadata)")
    {
        S2BT(session_impl)->modified = true;
        F_SET(S2BT(session_impl), WT_BTREE_BULK);

        FLD_SET(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, false) == 0);
        FLD_CLR(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);

        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test WT_DIRTY_DATA is not thrown (btree is modified, is bulk, is metadata)")
    {
        S2BT(session_impl)->modified = true;
        F_SET(S2BT(session_impl), WT_BTREE_BULK);
        F_SET(session_impl->dhandle, WT_DHANDLE_IS_METADATA);

        FLD_SET(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);
        REQUIRE(__wt_conn_dhandle_close(session_impl, false, false, false) == 0);
        FLD_CLR(session_impl->lock_flags, WT_SESSION_LOCKED_SCHEMA);

        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    /* Drop the table once the tests are completed. */
    S2BT(session_impl)->max_upd_txn = 1;
    S2BT(session_impl)->modified = false;
    F_CLR(S2BT(session_impl), WT_BTREE_BULK);
    F_CLR(session_impl->dhandle, WT_DHANDLE_IS_METADATA);
    REQUIRE(__wt_session_release_dhandle(session_impl) == 0);
    REQUIRE(session->drop(session, URI, NULL) == 0);
}
