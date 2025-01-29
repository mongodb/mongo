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
#include "../../utility/test_util.h"

/*
 * [drop_conflict]: test_drop_conflict.cpp
 * Tests the drop workflows that lead to EBUSY errors, and ensure that the correct sub level error
 * codes and messages are stored.
 */

#define URI "table:test_drop_conflict"
#define CONFLICT_BACKUP_MSG "the table is currently performing backup and cannot be dropped"
#define CONFLICT_DHANDLE_MSG "another thread is currently holding the data handle of the table"

/*
 * Prepare the session and error_info structure to be used by the drop conflict tests.
 */
void
prepare_session_and_error(connection_wrapper *conn_wrapper, WT_SESSION **session,
  WT_ERROR_INFO **err_info, std::string &config)
{
    WT_CONNECTION *conn = conn_wrapper->get_wt_connection();
    REQUIRE(conn->open_session(conn, NULL, NULL, session) == 0);
    REQUIRE((*session)->create(*session, URI, config.c_str()) == 0);
    *err_info = &(((WT_SESSION_IMPL *)(*session))->err_info);
}

TEST_CASE("Test WT_CONFLICT_BACKUP and WT_CONFLICT_DHANDLE", "[drop_conflict]")
{
    std::string config = "key_format=S,value_format=S";
    WT_SESSION *session = NULL;
    WT_ERROR_INFO *err_info = NULL;

    SECTION("Test WT_CONFLICT_BACKUP")
    {
        connection_wrapper conn_wrapper = connection_wrapper(".", "create");
        prepare_session_and_error(&conn_wrapper, &session, &err_info, config);

        /* Open a backup cursor on a table, then attempt to drop the table. */
        WT_CURSOR *backup_cursor;
        REQUIRE(session->open_cursor(session, "backup:", NULL, NULL, &backup_cursor) == 0);
        REQUIRE(session->drop(session, URI, NULL) == EBUSY);
        utils::check_error_info(err_info, EBUSY, WT_CONFLICT_BACKUP, CONFLICT_BACKUP_MSG);
    }

    /* This section gives us coverage in __drop_file. */
    SECTION("Test WT_CONFLICT_DHANDLE with simple table")
    {
        connection_wrapper conn_wrapper = connection_wrapper(".", "create");
        prepare_session_and_error(&conn_wrapper, &session, &err_info, config);

        /* Open a cursor on a table, then attempt to drop the table. */
        WT_CURSOR *cursor;
        REQUIRE(session->open_cursor(session, URI, NULL, NULL, &cursor) == 0);
        REQUIRE(session->drop(session, URI, NULL) == EBUSY);
        utils::check_error_info(err_info, EBUSY, WT_CONFLICT_DHANDLE, CONFLICT_DHANDLE_MSG);
    }

    /* This section gives us coverage in __drop_table. */
    SECTION("Test WT_CONFLICT_DHANDLE with columns")
    {
        config += ",columns=(col1,col2)";
        connection_wrapper conn_wrapper = connection_wrapper(".", "create");
        prepare_session_and_error(&conn_wrapper, &session, &err_info, config);

        /* Open a cursor on a table with columns, then attempt to drop the table. */
        WT_CURSOR *cursor;
        REQUIRE(session->open_cursor(session, URI, NULL, NULL, &cursor) == 0);
        REQUIRE(session->drop(session, URI, NULL) == EBUSY);
        utils::check_error_info(err_info, EBUSY, WT_CONFLICT_DHANDLE, CONFLICT_DHANDLE_MSG);
    }

    /*
     * This section gives us coverage in __drop_tiered. The dir_store extension is only supported
     * for POSIX systems, so skip this section on Windows.
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

        prepare_session_and_error(&conn_wrapper, &session, &err_info, config);

        /* Open a cursor on a table that uses tiered storage, then attempt to drop the table. */
        WT_CURSOR *cursor;
        REQUIRE(session->open_cursor(session, URI, NULL, NULL, &cursor) == 0);
        REQUIRE(session->drop(session, URI, NULL) == EBUSY);
        utils::check_error_info(err_info, EBUSY, WT_CONFLICT_DHANDLE, CONFLICT_DHANDLE_MSG);
    }
#endif
}
