/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "live_restore_test_env.h"

#include <fstream>
namespace utils {

/*
 * This class sets up and tears down the testing environment for Live Restore. It spins up a normal
 * WiredTiger database and then removes all content to leave an empty destination and source folder.
 * Developers are expected to create the respective files in the these folders manually.
 */
live_restore_test_env::live_restore_test_env()
{
    // Clean up any pre-existing folders. Make sure an empty DB_SOURCE exists
    // as it need to exist to open the connection in live restore mode.
    testutil_remove(DB_DEST.c_str());
    testutil_remove(DB_TEMP_BACKUP.c_str());
    testutil_recreate_dir(DB_SOURCE.c_str());

    /*
     * We're using a connection to set up the file system and let us print WT traces, but all of our
     * tests will use empty folders where we create files manually. The issue here is
     * wiredtiger_open will create metadata and turtle files on open and think it needs to remove
     * them on close. Move these files to a temp location. We'll restore them in destructor before
     * _conn->close() is called.
     */
    static std::string cfg_string = "live_restore=(enabled=true, path=" + DB_SOURCE + ")";
    conn = std::make_unique<connection_wrapper>(DB_DEST.c_str(), cfg_string.c_str());
    testutil_copy(DB_DEST.c_str(), DB_TEMP_BACKUP.c_str());
    testutil_recreate_dir(DB_DEST.c_str());

    session = conn->create_session();
    lr_fs = (WT_LIVE_RESTORE_FS *)conn->get_wt_connection_impl()->file_system;
}

live_restore_test_env::~live_restore_test_env()
{
    // Clean up directories on close. The connections destructor will remove the final
    // destination folder.
    testutil_remove(DB_SOURCE.c_str());
    testutil_move(DB_TEMP_BACKUP.c_str(), DB_DEST.c_str());
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

} // namespace utils.
