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
 * [sub_level_error_live_restore_conflict]: test_sub_level_error_live_restore_conflict.cpp
 * Tests live restore sub level errors, and ensure that the correct sub level error
 * codes and messages are stored.
 */
#define CONFLICT_LIVE_RESTORE_WITH_BACKUP_MSG "backup cannot be taken when live restore is enabled"

/*
 * This test case covers EINVAL error resulting from live restore conflicts with other operations.
 */
TEST_CASE("Test WT_CONFLICT_LIVE_RESTORE",
  "[sub_level_error_live_restore_conflict],[sub_level_error],[live_restore]")
{
    WT_SESSION *session = NULL;
    WT_ERROR_INFO *err_info = NULL;
    WT_CURSOR *cursor;

    SECTION("Test WT_CONFLICT_LIVE_RESTORE while opening backup cursor")
    {
        // We need to have a real WiredTiger database in the source directory.
        // Wrap it in its own scope to close the connection.
        {
            connection_wrapper source_conn_wrapper =
              connection_wrapper("WT_LR_SOURCE", "create=true");
            source_conn_wrapper.clear_do_cleanup();
        }

        connection_wrapper conn_wrapper =
          connection_wrapper(".", "create=true,live_restore=(enabled=true, path=WT_LR_SOURCE)");
        utils::prepare_session_and_error(&conn_wrapper, &session, &err_info);

        REQUIRE(session->open_cursor(session, "backup:", NULL, NULL, &cursor) == EINVAL);
        REQUIRE(cursor == NULL);
        utils::check_error_info(
          err_info, EINVAL, WT_CONFLICT_LIVE_RESTORE, CONFLICT_LIVE_RESTORE_WITH_BACKUP_MSG);
    }
}
