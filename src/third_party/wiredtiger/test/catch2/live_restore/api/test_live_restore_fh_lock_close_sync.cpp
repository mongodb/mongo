/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [live_restore_fh_lock_close_sync]: live_restore_fs.c
 * Test file handle lock, sync, and close API functions.
 * These functions are simple often call directly into the underlying file system layer, so they
 * require minimal testing.
 */

#include "../utils_live_restore.h"
#include "../../wrappers/mock_session.h"

using namespace utils;

TEST_CASE(
  "Live Restore fh_lock fh_close fh_sync", "[live_restore],[live_restore_fh_lock_close_sync]")
{
    live_restore_test_env env;

    std::string file_name = "file";
    WT_FILE_HANDLE *fh = nullptr;
    create_file(env.dest_file_path(file_name));
    create_file(env.source_file_path(file_name));

    WT_SESSION *wt_session = reinterpret_cast<WT_SESSION *>(env.session);
    REQUIRE(env.lr_fs->iface.fs_open_file((WT_FILE_SYSTEM *)env.lr_fs, wt_session,
              env.dest_file_path(file_name).c_str(), WT_FS_OPEN_FILE_TYPE_DATA, 0, &fh) == 0);

    // If we test this any deeper then we'd be testing the posix implementation.
    REQUIRE(fh->fh_lock(fh, wt_session, true) == 0);
    REQUIRE(fh->fh_lock(fh, wt_session, false) == 0);
    // Re-entrant locking is allowed.
    REQUIRE(fh->fh_lock(fh, wt_session, true) == 0);
    // Call sync with no writes.
    REQUIRE(fh->fh_sync(fh, wt_session) == 0);

    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = (WTI_LIVE_RESTORE_FILE_HANDLE *)fh;
    uint32_t write_size = 4096;
    lr_fh->allocsize = write_size;
    std::string aaa(write_size, 'A');
    // Do a write.
    REQUIRE(fh->fh_write(fh, wt_session, 0, write_size, aaa.c_str()) == 0);
    // Sync the write.
    REQUIRE(fh->fh_sync(fh, wt_session) == 0);

    // Close the file handle.
    REQUIRE(fh->close(fh, wt_session) == 0);
    // Calling close again would be an interesting test but it actually aborts.
}
