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
 * [sub_level_error_rollback]: test_sub_level_error_rollback.cpp
 * Tests the error handling for rollback workflows.
 */

using namespace utils;

TEST_CASE("Test functions for error handling in rollback workflows",
  "[sub_level_error_rollback],[sub_level_error]")
{
    WT_SESSION *session;

    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    WT_CONNECTION *conn = conn_wrapper.get_wt_connection();
    WT_CONNECTION_IMPL *conn_impl = (WT_CONNECTION_IMPL *)conn;
    REQUIRE(conn->open_session(conn, NULL, NULL, &session) == 0);
    WT_SESSION_IMPL *session_impl = (WT_SESSION_IMPL *)session;
    WT_ERROR_INFO *err_info = &(session_impl->err_info);

    SECTION(
      "Test WT_CACHE_OVERFLOW in __wti_evict_app_assist_worker - not safe to proceed with eviction")
    {

        // If the eviction server isn't running, then the threads have not been set up yet and it's
        // not safe to evict.
        CHECK(__wt_evict_app_assist_worker_check(session_impl, false, false, true, NULL) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);

        // Set the eviction server as running.
        conn_impl->evict_server_running = true;
        // The eviction sever is running, but the application is busy and the cache is less than 100
        // percent full.
        conn_impl->cache_size = 10 * 1024 * 1024;
        conn_impl->cache->bytes_inmem = 9 * 1024 * 1024;
        CHECK(__wt_evict_app_assist_worker_check(session_impl, true, false, true, NULL) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION(
      "Test WT_CACHE_OVERFLOW in __wti_evict_app_assist_worker - conflicting sub-level error codes")
    {
        // It is possible to get WT_OLDEST_FOR_EVICTION as the sub-level rollback error from this
        // function. This should not be overwritten by WT_CACHE_OVERFLOW.

        // Set the eviction cache as stuck.
        conn_impl->evict->evict_aggressive_score = WT_EVICT_SCORE_MAX;
        F_SET(conn_impl->evict, WT_EVICT_CACHE_HARD);

        // Set transaction's update amount to 1 and ID to be equal to the oldest transaction ID.
        session_impl->txn->mod_count = 1;
        WT_SESSION_TXN_SHARED(session_impl)->id = S2C(session)->txn_global.oldest_id;
        CHECK(__wti_evict_app_assist_worker(session_impl, false, false, true) == WT_ROLLBACK);
        check_error_info(err_info, WT_ROLLBACK, WT_OLDEST_FOR_EVICTION,
          "Transaction has the oldest pinned transaction ID");

        // Reset updates to the initial value.
        session_impl->txn->mod_count = 0;
    }

    SECTION("Test WT_CACHE_OVERFLOW in __wti_evict_app_assist_worker - cache max wait")
    {
        WT_CURSOR *cursor;

        // Set the cache size and cache max wait to low values. This is required so that the thread
        // think eviction is needed and to time out the eviction thread.
        conn_impl->cache_size = 1;
        conn_impl->evict->cache_max_wait_us = 1;

        // Create a table and set a key and value to create a page to evict. This is required so
        // that the time taken doing eviction exceeds the cache max wait time.
        REQUIRE(session->create(session, "table:rollback", "key_format=S,value_format=S") == 0);
        session->open_cursor(session, "table:rollback", NULL, NULL, &cursor);
        session->begin_transaction(session, NULL);
        cursor->set_key(cursor, "key");
        cursor->set_value(cursor, "value");

        CHECK(__wti_evict_app_assist_worker(session_impl, false, false, true) == WT_ROLLBACK);
        check_error_info(err_info, WT_ROLLBACK, WT_CACHE_OVERFLOW, "Cache capacity has overflown");

        // Drop the table.
        cursor->close(cursor);
        session->drop(session, "table:rollback", NULL);
    }

    SECTION("Test WT_WRITE_CONFLICT in __txn_modify_block")
    {
        // Create a table and place a lock flag on it so session can have a set dhandle.
        REQUIRE(session->create(session, "table:rollback", "key_format=S,value_format=S") == 0);
        FLD_SET(session_impl->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST);
        REQUIRE(__wt_conn_dhandle_alloc(session_impl, "table:rollback", NULL) == 0);

        // Allocate update. The update type must not be WT_TXN_ABORTED (2), so we set it to
        // WT_UPDATE_TOMBSTONE (4).
        WT_UPDATE *upd;
        REQUIRE(__wt_upd_alloc(session_impl, NULL, WT_UPDATE_TOMBSTONE, &upd, NULL) == 0);

        // Transaction must be invisible, so we say that the session has a transaction snapshot and
        // that the transaction ID is greater than the max snap transaction ID.
        F_SET(session_impl->txn, WT_TXN_HAS_SNAPSHOT);
        session_impl->txn->snapshot_data.snap_max = 0;
        upd->txnid = 1;
        CHECK(__txn_modify_block(session_impl, NULL, upd, NULL) == WT_ROLLBACK);
        check_error_info(
          err_info, WT_ROLLBACK, WT_WRITE_CONFLICT, "Write conflict between concurrent operations");

        // Free update.
        __wt_free(session_impl, upd);

        // Clear lock flag so the table can be dropped.
        FLD_CLR(session_impl->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST);
        session->drop(session, "table:rollback", NULL);
    }

    SECTION("Test WT_OLDEST_FOR_EVICTION in __wt_txn_is_blocking - prepared transaction")
    {
        // Set transaction as prepared. This should cause an early exist so no error is returned.
        F_SET(session_impl->txn, WT_TXN_PREPARE);
        CHECK(__wt_txn_is_blocking(session_impl) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test WT_OLDEST_FOR_EVICTION in __wt_txn_is_blocking - rollback can't be handled")
    {
        // Check if there are no updates, the thread operation did not time out and the operation is
        // not running in a transaction. No error should be returned from these.

        // Set the transaction to have 1 modification.
        session_impl->txn->mod_count = 1;
        CHECK(__wt_txn_is_blocking(session_impl) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);

        // Set transaction running to true.
        F_SET(session_impl->txn, WT_TXN_RUNNING);
        CHECK(__wt_txn_is_blocking(session_impl) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);

        // Set operations timers to low value.
        session_impl->operation_start_us = session_impl->operation_timeout_us = 1;
        CHECK(__wt_txn_is_blocking(session_impl) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);

        // Set the transaction to have 0 modifications.
        session_impl->txn->mod_count = 0;
        CHECK(__wt_txn_is_blocking(session_impl) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test WT_OLDEST_FOR_EVICTION in __wt_txn_is_blocking - transaction ID")
    {
        // Set the transaction to have 1 modification.
        session_impl->txn->mod_count = 1;

        // Check if the transaction's ID or its pinned ID is equal to the oldest transaction ID.
        CHECK(__wt_txn_is_blocking(session_impl) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);

        // Set transaction's pinned ID to be equal to the oldest transaction ID.
        WT_TXN_SHARED *txn_shared = WT_SESSION_TXN_SHARED(session_impl);
        txn_shared->pinned_id = S2C(session)->txn_global.oldest_id;
        CHECK(__wt_txn_is_blocking(session_impl) == WT_ROLLBACK);
        check_error_info(err_info, WT_ROLLBACK, WT_OLDEST_FOR_EVICTION,
          "Transaction has the oldest pinned transaction ID");

        // Reset error.
        __wt_session_reset_last_error(session_impl);

        // Set transaction's ID to be equal to the oldest transaction ID.
        txn_shared->id = S2C(session)->txn_global.oldest_id;
        CHECK(__wt_txn_is_blocking(session_impl) == WT_ROLLBACK);
        check_error_info(err_info, WT_ROLLBACK, WT_OLDEST_FOR_EVICTION,
          "Transaction has the oldest pinned transaction ID");

        // Reset updates to the initial value.
        session_impl->txn->mod_count = 0;
    }

    SECTION(
      "Test WT_MODIFY_READ_UNCOMMITTED in __wt_modify_reconstruct_from_upd_list - reader with "
      "uncommitted isolation")
    {
        WT_UPDATE modify;
        WT_UPDATE base;
        WT_UPDATE_VALUE upd_value;

        // Create an aborted modify so that it doesn't need to be applied.
        modify.type = WT_UPDATE_MODIFY;
        modify.txnid = WT_TXN_ABORTED;
        modify.next = &base;

        // Create a base update.
        base.type = WT_UPDATE_STANDARD;

        CHECK(__wt_modify_reconstruct_from_upd_list(
                session_impl, NULL, &modify, &upd_value, WT_OPCTX_RECONCILATION) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);

        session_impl->txn->isolation = WT_ISO_SNAPSHOT;
        CHECK(__wt_modify_reconstruct_from_upd_list(
                session_impl, NULL, &modify, &upd_value, WT_OPCTX_TRANSACTION) == 0);
        check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);

        const char *err_msg_content =
          "Read-uncommitted readers do not support reconstructing a record with modifies.";
        session_impl->txn->isolation = WT_ISO_READ_UNCOMMITTED;
        CHECK(__wt_modify_reconstruct_from_upd_list(
                session_impl, NULL, &modify, &upd_value, WT_OPCTX_TRANSACTION) == WT_ROLLBACK);
        check_error_info(err_info, WT_ROLLBACK, WT_MODIFY_READ_UNCOMMITTED, err_msg_content);
    }
}
