/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * [live_restore_fs_remove_rename]: live_restore_fs.c
 * Test the remove and rename file system APIs.
 */

#include "../utils_live_restore.h"

using namespace utils;

bool
stop_file_exists(std::string file_name)
{
    return (testutil_exists(nullptr, (file_name + WTI_LIVE_RESTORE_STOP_FILE_SUFFIX).c_str()));
}

TEST_CASE("Live Restore fs_remove", "[live_restore],[live_restore_remove]")
{
    live_restore_test_env env;

    WTI_LIVE_RESTORE_FS *lr_fs = env.lr_fs;
    WT_FILE_SYSTEM *fs = &lr_fs->iface;
    WT_SESSION *wt_session = reinterpret_cast<WT_SESSION *>(env.session);

    // Remove a file that only exists in the destination.
    std::string dest_filename = env.dest_file_path("file");
    create_file(dest_filename);
    REQUIRE(fs->fs_remove(fs, wt_session, dest_filename.c_str(), 0) == 0);
    REQUIRE(stop_file_exists(dest_filename));

    // Removing a file that doesn't exist fails, we check the underlying file system behavior here
    // too, ensuring they match.
    REQUIRE(lr_fs->os_file_system->fs_remove(
              lr_fs->os_file_system, wt_session, dest_filename.c_str(), 0) == ENOENT);
    REQUIRE(fs->fs_remove(fs, wt_session, dest_filename.c_str(), 0) == ENOENT);

    // Removing a file that exists in the source but has a stop file in the destination fails.
    std::string source_filename = env.source_file_path("file");
    create_file(source_filename);
    REQUIRE(fs->fs_remove(fs, wt_session, dest_filename.c_str(), 0) == ENOENT);

    // Removing a file that exists in the source and has no associated file in the destination
    // succeeds and creates a new stop file in the destination.
    std::string source_filename2 = env.source_file_path("file2");
    std::string dest_filename2 = env.dest_file_path("file2");
    create_file(source_filename2);
    REQUIRE(fs->fs_remove(fs, wt_session, dest_filename2.c_str(), 0) == 0);
    // Ensure we didn't remove the source file.
    REQUIRE(testutil_exists(nullptr, source_filename2.c_str()));
    REQUIRE(stop_file_exists(dest_filename2));
    // Ensure we didn't create a stop file in the source.
    REQUIRE(!stop_file_exists(source_filename2));

    // We can recreate a file with the same name as the one we removed earlier and remove it again.
    create_file(dest_filename2);
    REQUIRE(fs->fs_remove(fs, wt_session, dest_filename2.c_str(), 0) == 0);
    REQUIRE(stop_file_exists(dest_filename2));

    // Removing a file once the live restore state is complete won't generate a tombstone.
    lr_fs->state = WTI_LIVE_RESTORE_STATE_COMPLETE;
    std::string dest_filename3 = env.dest_file_path("file3");
    create_file(dest_filename3);
    REQUIRE(fs->fs_remove(fs, wt_session, dest_filename3.c_str(), 0) == 0);
    REQUIRE(!testutil_exists(nullptr, dest_filename3.c_str()));
    REQUIRE(!stop_file_exists(dest_filename3));

    // Removing a file that exists only in source once the live restore completes will fail.
    std::string source_filename3 = env.source_file_path("file3");
    create_file(source_filename3);
    REQUIRE(fs->fs_remove(fs, wt_session, dest_filename3.c_str(), 0) == ENOENT);

    // Renaming a file that exists in both the source and the destination doesn't remove the source
    // file.
    lr_fs->state = WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION;
    std::string source_filename4 = env.source_file_path("file4");
    std::string dest_filename4 = env.dest_file_path("file4");
    create_file(source_filename4);
    create_file(dest_filename4);
    REQUIRE(fs->fs_remove(fs, wt_session, dest_filename4.c_str(), 0) == 0);
    REQUIRE(stop_file_exists(dest_filename4));
    REQUIRE(testutil_exists(nullptr, source_filename4.c_str()));
}

TEST_CASE("Live Restore fs_rename", "[live_restore],[live_restore_rename]")
{
    live_restore_test_env env;

    WTI_LIVE_RESTORE_FS *lr_fs = env.lr_fs;
    WT_FILE_SYSTEM *fs = &lr_fs->iface;
    WT_SESSION *wt_session = reinterpret_cast<WT_SESSION *>(env.session);

    // Rename a file that only exists in the destination. We create stop files for both the old and
    // the new file names.
    std::string dest_filename = env.dest_file_path("file");
    std::string dest_rename = env.dest_file_path("file_rename");
    create_file(dest_filename);
    REQUIRE(fs->fs_rename(fs, wt_session, dest_filename.c_str(), dest_rename.c_str(), 0) == 0);
    REQUIRE(stop_file_exists(dest_filename));
    REQUIRE(stop_file_exists(dest_rename));
    REQUIRE(!testutil_exists(nullptr, dest_filename.c_str()));
    REQUIRE(testutil_exists(nullptr, dest_rename.c_str()));

    // Renaming a file that doesn't exist fails.
    REQUIRE(lr_fs->os_file_system->fs_rename(lr_fs->os_file_system, wt_session,
              dest_filename.c_str(), dest_rename.c_str(), 0) == ENOENT);
    REQUIRE(fs->fs_rename(fs, wt_session, dest_filename.c_str(), dest_rename.c_str(), 0) == ENOENT);

    // Renaming a file that only exists in the source fails as we require that the file exists in
    // the destination.
    std::string source_filename = env.source_file_path("file2");
    std::string dest_filename2 = env.dest_file_path("file2");
    create_file(source_filename);
    // Note: We need to pass dest filename here as WiredTiger would only refer to files as existing
    // in the home directory.
    REQUIRE(
      fs->fs_rename(fs, wt_session, dest_filename2.c_str(), dest_rename.c_str(), 0) == EINVAL);

    // Renaming over the top of an existing file succeeds.
    REQUIRE(testutil_exists(nullptr, dest_rename.c_str()));
    create_file(dest_filename2);
    REQUIRE(fs->fs_rename(fs, wt_session, dest_filename2.c_str(), dest_rename.c_str(), 0) == 0);
    REQUIRE(!testutil_exists(nullptr, dest_filename.c_str()));
    REQUIRE(testutil_exists(nullptr, dest_rename.c_str()));

    // Renaming a file once the live restore has completed won't generate tombstones.
    lr_fs->state = WTI_LIVE_RESTORE_STATE_COMPLETE;
    std::string dest_filename3 = env.dest_file_path("file3");
    std::string dest_rename3 = env.dest_file_path("file3_rename");
    create_file(dest_filename3);
    REQUIRE(fs->fs_rename(fs, wt_session, dest_filename3.c_str(), dest_rename3.c_str(), 0) == 0);
    REQUIRE(!testutil_exists(nullptr, dest_filename3.c_str()));
    REQUIRE(testutil_exists(nullptr, dest_rename3.c_str()));
    REQUIRE(!stop_file_exists(dest_filename3));
    REQUIRE(!stop_file_exists(dest_rename3));
}
