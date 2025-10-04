/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "live_restore_test_env.h"

#include <fstream>
#include <iostream>
namespace utils {

/*
 * This class sets up and tears down the testing environment for Live Restore. Developers are
 * expected to create the respective files in the these folders manually.
 */
live_restore_test_env::live_restore_test_env()
{
    // Clean up any pre-existing folders.
    testutil_remove(DB_DEST.c_str());
    testutil_remove(DB_SOURCE.c_str());

    // Live restore requires the source directory to be a valid backup. Create one now.
    {
        static std::string non_lr_config = "create=true";
        auto backup_conn =
          std::make_unique<connection_wrapper>(DB_DEST.c_str(), non_lr_config.c_str());

        backup_conn->get_wt_connection_impl();
        WT_SESSION *session = (WT_SESSION *)backup_conn->create_session();
        WT_CURSOR *backup_cursor = nullptr;
        REQUIRE(session->open_cursor(session, "backup:", nullptr, nullptr, &backup_cursor) == 0);

        testutil_mkdir(DB_SOURCE.c_str());
        while (backup_cursor->next(backup_cursor) == 0) {
            const char *uri = nullptr;
            REQUIRE(backup_cursor->get_key(backup_cursor, &uri) == 0);
            std::string dest_file = DB_DEST + "/" + uri;
            std::string source_file = DB_SOURCE + "/" + uri;
            testutil_copy(dest_file.c_str(), source_file.c_str());
        }

        backup_cursor->close(backup_cursor);
        session->close(session, nullptr);
    }

    testutil_remove(DB_DEST.c_str());
    static std::string cfg_string = "create=true,live_restore=(enabled=true, path=" + DB_SOURCE +
      ",threads_max=0),statistics=(fast)";
    conn = std::make_unique<connection_wrapper>(DB_DEST.c_str(), cfg_string.c_str());

    session = conn->create_session();
    lr_fs = (WTI_LIVE_RESTORE_FS *)conn->get_wt_connection_impl()->file_system;
}

std::string
live_restore_test_env::dest_file_path(const std::string &file_name)
{
    return DB_DEST + "/" + file_name;
}

std::string
live_restore_test_env::source_file_path(const std::string &file_name)
{
    return DB_SOURCE + "/" + file_name;
}

std::string
live_restore_test_env::tombstone_file_path(const std::string &file_name)
{
    // Tombstone files only exist in the destination folder.
    return DB_DEST + "/" + file_name + WTI_LIVE_RESTORE_STOP_FILE_SUFFIX;
}

} // namespace utils.
