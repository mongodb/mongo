/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <thread>
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

#define URI "table:test_drop_conflict"
#define CONFLICT_BACKUP_MSG "the table is currently performing backup and cannot be dropped"
#define CONFLICT_DHANDLE_MSG "another thread is currently holding the data handle of the table"
#define CONFLICT_CHECKPOINT_LOCK_MSG "another thread is currently holding the checkpoint lock"
#define CONFLICT_SCHEMA_LOCK_MSG "another thread is currently holding the schema lock"
#define CONFLICT_TABLE_LOCK_MSG "another thread is currently holding the table lock"

/*
 * This test case covers EBUSY errors resulting from drop while cursors are still open on the table.
 */
TEST_CASE("Test WT_CONFLICT_BACKUP and WT_CONFLICT_DHANDLE",
  "[sub_level_error_drop_conflict],[sub_level_error]")
{
    std::string config = "key_format=S,value_format=S";
    WT_SESSION *session = NULL;
    WT_ERROR_INFO *err_info = NULL;
    WT_CURSOR *cursor;

    SECTION("Test WT_CONFLICT_BACKUP")
    {
        connection_wrapper conn_wrapper = connection_wrapper(".", "create");
        utils::prepare_session_and_error(&conn_wrapper, &session, &err_info);
        REQUIRE(session->create(session, URI, config.c_str()) == 0);

        /* Open a backup cursor, then attempt to drop the table. */
        REQUIRE(session->open_cursor(session, "backup:", NULL, NULL, &cursor) == 0);
        REQUIRE(session->drop(session, URI, NULL) == EBUSY);
        utils::check_error_info(err_info, EBUSY, WT_CONFLICT_BACKUP, CONFLICT_BACKUP_MSG);

        /* Drop the table once the test is completed. */
        cursor->close(cursor);
        REQUIRE(session->drop(session, URI, NULL) == 0);
        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    /* This section gives us coverage in __drop_file. */
    SECTION("Test WT_CONFLICT_DHANDLE with simple table")
    {
        connection_wrapper conn_wrapper = connection_wrapper(".", "create");
        utils::prepare_session_and_error(&conn_wrapper, &session, &err_info);
        REQUIRE(session->create(session, URI, config.c_str()) == 0);

        /* Open a cursor on a table, then attempt to drop the table. */
        REQUIRE(session->open_cursor(session, URI, NULL, NULL, &cursor) == 0);
        REQUIRE(session->drop(session, URI, NULL) == EBUSY);
        utils::check_error_info(err_info, EBUSY, WT_CONFLICT_DHANDLE, CONFLICT_DHANDLE_MSG);

        /* Drop the table once the test is completed. */
        cursor->close(cursor);
        REQUIRE(session->drop(session, URI, NULL) == 0);
        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    /* This section gives us coverage in __drop_table. */
    SECTION("Test WT_CONFLICT_DHANDLE with columns")
    {
        config += ",columns=(col1,col2)";
        connection_wrapper conn_wrapper = connection_wrapper(".", "create");
        utils::prepare_session_and_error(&conn_wrapper, &session, &err_info);
        REQUIRE(session->create(session, URI, config.c_str()) == 0);

        /* Open a cursor on a table with columns, then attempt to drop the table. */
        REQUIRE(session->open_cursor(session, URI, NULL, NULL, &cursor) == 0);
        REQUIRE(session->drop(session, URI, NULL) == EBUSY);
        utils::check_error_info(err_info, EBUSY, WT_CONFLICT_DHANDLE, CONFLICT_DHANDLE_MSG);

        /* Drop the table once the test is completed. */
        cursor->close(cursor);
        REQUIRE(session->drop(session, URI, NULL) == 0);
        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    /*
     * This section gives us coverage in __drop_tiered. The dir_store extension is only supported
     * for POSIX systems, so we skip this section on Windows.
     */
#ifndef _WIN32
    SECTION("Test WT_CONFLICT_DHANDLE with tiered storage")
    {
        /* Set up the connection and session to use tiered storage. */
        const char *home = "WT_TEST";
        testutil_system("rm -rf %s && mkdir %s && mkdir %s/%s", home, home, home, "bucket");
        connection_wrapper conn_wrapper = connection_wrapper(home,
          "create,tiered_storage=(bucket=bucket,bucket_prefix=pfx-,name=dir_store),extensions=(./"
          "ext/storage_sources/dir_store/libwiredtiger_dir_store.so)");

        utils::prepare_session_and_error(&conn_wrapper, &session, &err_info);
        REQUIRE(session->create(session, URI, config.c_str()) == 0);

        /* Open a cursor on a table that uses tiered storage, then attempt to drop the table. */
        REQUIRE(session->open_cursor(session, URI, NULL, NULL, &cursor) == 0);
        REQUIRE(session->drop(session, URI, NULL) == EBUSY);
        utils::check_error_info(err_info, EBUSY, WT_CONFLICT_DHANDLE, CONFLICT_DHANDLE_MSG);

        /* Drop the table once the test is completed. */
        cursor->close(cursor);
        REQUIRE(session->drop(session, URI, NULL) == 0);
        utils::check_error_info(err_info, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }
#endif
}

/*
 * This test case covers EBUSY errors resulting from drop while a lock is held by another session.
 */
TEST_CASE("Test conflicts with checkpoint/schema/table locks", "[sub_level_error_drop_conflict]")
{
    std::string config = "key_format=S,value_format=S";
    WT_SESSION *session_a = NULL;
    WT_SESSION *session_b = NULL;
    WT_ERROR_INFO *err_info_a = NULL;
    WT_ERROR_INFO *err_info_b = NULL;

    connection_wrapper conn_wrapper = connection_wrapper(".", "create");
    utils::prepare_session_and_error(&conn_wrapper, &session_a, &err_info_a);
    utils::prepare_session_and_error(&conn_wrapper, &session_b, &err_info_b);
    REQUIRE(session_a->create(session_a, URI, config.c_str()) == 0);

    /*
     * The Windows implementation of __wt_spin_lock/__wt_try_spin_lock will still manage to take the
     * lock if it has already been taken by a different session in the same thread, resulting in a
     * successful (no conflicts) drop.
     *
     * These CHECKPOINT_LOCK/SCHEMA_LOCK sections will therefore fail on Windows - we have decided
     * to skip them in this case rather than have the tests use multithreading.
     */
#ifndef _WIN32
    SECTION("Test CONFLICT_CHECKPOINT_LOCK")
    {
        /* Attempt to drop the table while the checkpoint lock is taken by another session. */
        WT_WITH_CHECKPOINT_LOCK(((WT_SESSION_IMPL *)session_b),
          REQUIRE(session_a->drop(session_a, URI, "lock_wait=0") == EBUSY));

        utils::check_error_info(
          err_info_a, EBUSY, WT_CONFLICT_CHECKPOINT_LOCK, CONFLICT_CHECKPOINT_LOCK_MSG);
        utils::check_error_info(err_info_b, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    SECTION("Test CONFLICT_SCHEMA_LOCK")
    {
        /* Attempt to drop the table while the schema lock is taken by another session. */
        WT_WITH_SCHEMA_LOCK(((WT_SESSION_IMPL *)session_b),
          REQUIRE(session_a->drop(session_a, URI, "lock_wait=0") == EBUSY));

        utils::check_error_info(
          err_info_a, EBUSY, WT_CONFLICT_SCHEMA_LOCK, CONFLICT_SCHEMA_LOCK_MSG);
        utils::check_error_info(err_info_b, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }
#endif

    SECTION("Test CONFLICT_TABLE_LOCK")
    {
        /* Attempt to drop the table while the table write lock is taken by another session. */
        WT_WITH_TABLE_WRITE_LOCK(((WT_SESSION_IMPL *)session_b),
                                 REQUIRE(session_a->drop(session_a, URI, "lock_wait=0") == EBUSY););

        utils::check_error_info(err_info_a, EBUSY, WT_CONFLICT_TABLE_LOCK, CONFLICT_TABLE_LOCK_MSG);
        utils::check_error_info(err_info_b, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
    }

    /* Drop the table once the tests are completed. */
    REQUIRE(session_a->drop(session_a, URI, NULL) == 0);
    utils::check_error_info(err_info_a, 0, WT_NONE, WT_ERROR_INFO_SUCCESS);
}
