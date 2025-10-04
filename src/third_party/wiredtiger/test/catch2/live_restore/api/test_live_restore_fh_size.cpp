/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Tests for the Live Restore file handle's fh_size function. [live_restore_fh_size]
 */

#include "../utils_live_restore.h"

using namespace utils;

static void
fh_size_wrapper(live_restore_test_env &env, const std::string &file_name, wt_off_t *sizep)
{
    WT_SESSION *session = (WT_SESSION *)env.session;

    std::string dest_file_path = env.dest_file_path(file_name);

    WTI_LIVE_RESTORE_FS *lr_fs = env.lr_fs;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = nullptr;
    testutil_check(lr_fs->iface.fs_open_file((WT_FILE_SYSTEM *)lr_fs, session,
      env.dest_file_path(file_name).c_str(), WT_FS_OPEN_FILE_TYPE_DATA, WT_FS_OPEN_CREATE,
      (WT_FILE_HANDLE **)&lr_fh));
    REQUIRE(lr_fh->iface.fh_size((WT_FILE_HANDLE *)lr_fh, session, sizep) == 0);
    testutil_check(lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, session));
}

static const int DEST_FILE_SIZE = 10;
static const int SOURCE_FILE_SIZE = 100;

static void
test_fh_size(live_restore_test_env *env, HasSource has_source, IsMigrating is_migrating,
  HasStop stop_file_exists, wt_off_t *sizep)
{
    std::string file = "file1.txt";

    std::string dest_file = env->dest_file_path(file);
    std::string source_file = env->source_file_path(file);
    std::string stop_file = env->dest_file_path(file + WTI_LIVE_RESTORE_STOP_FILE_SUFFIX);

    // Clean up any files from prior runs
    testutil_remove(dest_file.c_str());
    testutil_remove(source_file.c_str());
    testutil_remove(stop_file.c_str());

    create_file(dest_file.c_str(), DEST_FILE_SIZE);

    if (has_source == SOURCE)
        create_file(source_file.c_str(), SOURCE_FILE_SIZE);

    if (is_migrating == MIGRATING)
        env->lr_fs->state = WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION;
    else
        env->lr_fs->state = WTI_LIVE_RESTORE_STATE_COMPLETE;

    if (stop_file_exists == STOP)
        create_file(stop_file.c_str());

    fh_size_wrapper(*env, file, sizep);
}

TEST_CASE("Live Restore fh_size", "[live_restore],[live_restore_fh_size]")
{
    live_restore_test_env env;
    wt_off_t size;

    /*
     * live_restore_fh_size should always return the size of dest file. We don't need to test when
     * dest file does not exist as opening the file handle will either create a dest file or return
     * ENOENT on failure.
     */
    test_fh_size(&env, NO_SOURCE, NOT_MIGRATING, NO_STOP, &size);
    REQUIRE(size == DEST_FILE_SIZE);
    test_fh_size(&env, NO_SOURCE, NOT_MIGRATING, STOP, &size);
    REQUIRE(size == DEST_FILE_SIZE);
    test_fh_size(&env, NO_SOURCE, MIGRATING, NO_STOP, &size);
    REQUIRE(size == DEST_FILE_SIZE);
    test_fh_size(&env, NO_SOURCE, MIGRATING, STOP, &size);
    REQUIRE(size == DEST_FILE_SIZE);
    test_fh_size(&env, SOURCE, NOT_MIGRATING, NO_STOP, &size);
    REQUIRE(size == DEST_FILE_SIZE);
    test_fh_size(&env, SOURCE, NOT_MIGRATING, STOP, &size);
    REQUIRE(size == DEST_FILE_SIZE);
    test_fh_size(&env, SOURCE, MIGRATING, NO_STOP, &size);
    REQUIRE(size == DEST_FILE_SIZE);
    test_fh_size(&env, SOURCE, MIGRATING, STOP, &size);
    REQUIRE(size == DEST_FILE_SIZE);
}
