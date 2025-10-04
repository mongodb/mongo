/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Tests for the Live Restore file system's fs_open_file function. [live_restore_open_file]
 */

#include "../utils_live_restore.h"

using namespace utils;

// Wrapper for fs_open_file. This will also check we get the expected return code.
static WTI_LIVE_RESTORE_FILE_HANDLE *
open_file(live_restore_test_env &env, std::string file_name, WT_FS_OPEN_FILE_TYPE file_type,
  int expect_ret = 0, int flags = 0)
{
    WT_SESSION *wt_session = reinterpret_cast<WT_SESSION *>(env.session);
    WTI_LIVE_RESTORE_FS *lr_fs = env.lr_fs;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = nullptr;

    int ret = lr_fs->iface.fs_open_file((WT_FILE_SYSTEM *)lr_fs, wt_session,
      env.dest_file_path(file_name).c_str(), file_type, flags, (WT_FILE_HANDLE **)&lr_fh);
    REQUIRE(ret == expect_ret);

    return lr_fh;
}

void
validate_lr_fh(WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, live_restore_test_env &env,
  std::string &file_name, bool is_directory = false)
{
    REQUIRE(lr_fh->destination != NULL);
    if (is_directory) {
        // directories are always created on open and have nothing to copy from source.
        REQUIRE(lr_fh->source == NULL);
        REQUIRE(lr_fh->bitmap == nullptr);
    }
    std::string dest_file_path = env.dest_file_path(file_name);
    REQUIRE(strcmp(lr_fh->destination->name, dest_file_path.c_str()) == 0);
    REQUIRE(lr_fh->back_pointer == env.lr_fs);
}

TEST_CASE("Live Restore fs_open_file", "[live_restore],[live_restore_open_file]")
{
    /*
     * Note: this code runs once per SECTION so we're creating a brand new WT database for each
     * section. If this gets slow we can make it static and manually clear the destination and
     * source for each section.
     */
    live_restore_test_env env;

    std::string file_1 = "file1.txt";
    std::string subfolder = "subfolder";
    std::string sub_dir = "sub_dir";
    int sub_dir_depth = 3;

    SECTION("fs_open - File")
    {
        WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = nullptr;

        // If the file doesn't exist return ENOENT.
        lr_fh = open_file(env, file_1, WT_FS_OPEN_FILE_TYPE_REGULAR, ENOENT);
        REQUIRE(lr_fh == nullptr);

        // However if we provide the WT_FS_OPEN_CREATE flag the file is created in the destination.
        lr_fh = open_file(env, file_1, WT_FS_OPEN_FILE_TYPE_REGULAR, 0, WT_FS_OPEN_CREATE);
        REQUIRE(testutil_exists(".", env.dest_file_path(file_1).c_str()));
        validate_lr_fh(lr_fh, env, file_1);
        testutil_remove(env.dest_file_path(file_1).c_str());
        lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)env.session);

        // If the file only exists in the destination open is successful.
        create_file(env.dest_file_path(file_1));
        lr_fh = open_file(env, file_1, WT_FS_OPEN_FILE_TYPE_REGULAR);
        validate_lr_fh(lr_fh, env, file_1);
        lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)env.session);

        // If the file only exists in the source open is successful and we create a copy in the
        // destination
        testutil_remove(env.dest_file_path(file_1).c_str());
        create_file(env.source_file_path(file_1));
        lr_fh = open_file(env, file_1, WT_FS_OPEN_FILE_TYPE_REGULAR);
        REQUIRE(testutil_exists(".", env.dest_file_path(file_1).c_str()));
        validate_lr_fh(lr_fh, env, file_1);
        lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)env.session);

        /*
         * If the file has a nested subdirectory structure, and it only exists in the source. Open
         * is successful and we create subdirectories along with a copy of the file in the
         * destination.
         */
        testutil_remove(env.dest_file_path(file_1).c_str());
        testutil_remove(env.source_file_path(file_1).c_str());
        std::string sub_dir_path = "";
        for (int i = 0; i < sub_dir_depth; i++) {
            sub_dir_path += sub_dir;
            testutil_mkdir(env.source_file_path(sub_dir_path).c_str());
            sub_dir_path += "/";
        }
        std::string file_2 = sub_dir_path + file_1;
        create_file(env.source_file_path(file_2));
        lr_fh = open_file(env, file_2, WT_FS_OPEN_FILE_TYPE_REGULAR);
        REQUIRE(testutil_exists(".", env.dest_file_path(file_2).c_str()));
        validate_lr_fh(lr_fh, env, file_2);
        lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)env.session);

        // If the file exists in both source and destination open is successful.
        testutil_remove(env.dest_file_path(file_2).c_str());
        testutil_remove(env.source_file_path(file_2).c_str());

        create_file(env.dest_file_path(file_1));
        create_file(env.source_file_path(file_1));
        lr_fh = open_file(env, file_1, WT_FS_OPEN_FILE_TYPE_REGULAR);
        validate_lr_fh(lr_fh, env, file_1);
        lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)env.session);

        // If the file is deleted in the destination open returns ENOENT even when the file is
        // present in the source.
        testutil_remove(env.dest_file_path(file_1).c_str());
        create_file(env.tombstone_file_path(file_1));
        lr_fh = open_file(env, file_1, WT_FS_OPEN_FILE_TYPE_REGULAR, ENOENT);
    }

    SECTION("fs_open - Directory")
    {
        WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = nullptr;

        // WiredTiger never creates directories. The user must do this outside of WiredTiger. This
        // means all cases where the directory doesn't exist in the destination we return ENOENT.

        // The directory doesn't exist in the source or destination. Return ENOENT.
        lr_fh = open_file(env, subfolder, WT_FS_OPEN_FILE_TYPE_DIRECTORY, ENOENT);
        REQUIRE(lr_fh == nullptr);

        // The WT_FS_OPEN_CREATE flag is provided but there is no directory in the destination.
        // Return ENOENT.
        lr_fh =
          open_file(env, subfolder, WT_FS_OPEN_FILE_TYPE_DIRECTORY, ENOENT, WT_FS_OPEN_CREATE);
        REQUIRE(lr_fh == nullptr);

        // The directory exists in the source but not the destination. Return ENOENT.
        testutil_remove(env.dest_file_path(subfolder).c_str());
        testutil_mkdir(env.source_file_path(subfolder).c_str());
        lr_fh = open_file(env, subfolder, WT_FS_OPEN_FILE_TYPE_DIRECTORY, ENOENT);
        REQUIRE(lr_fh == nullptr);

        // However, when the directory exists in the destination open will be successful.

        // The directory exists only in the destination.
        testutil_mkdir(env.dest_file_path(subfolder).c_str());
        REQUIRE(testutil_exists(".", env.dest_file_path(subfolder).c_str()));
        lr_fh = open_file(env, subfolder, WT_FS_OPEN_FILE_TYPE_DIRECTORY);
        validate_lr_fh(lr_fh, env, subfolder, true);
        lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)env.session);

        // The directory exists in both the source and destination.
        testutil_remove(env.dest_file_path(subfolder).c_str());
        testutil_remove(env.source_file_path(subfolder).c_str());

        testutil_mkdir(env.dest_file_path(subfolder).c_str());
        testutil_mkdir(env.source_file_path(subfolder).c_str());
        lr_fh = open_file(env, subfolder, WT_FS_OPEN_FILE_TYPE_DIRECTORY);
        validate_lr_fh(lr_fh, env, subfolder, true);
        lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, (WT_SESSION *)env.session);

        // We don't need to consider the presence of stop files. WiredTiger never renames or deletes
        // directories, so stop files will never exist.
    }
}
