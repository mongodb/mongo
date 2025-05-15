/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Tests for the Live Restore file system's fs_file_fs function.
 * [live_restore_fs_size]
 */

#include "../utils_live_restore.h"

using namespace utils;

// Wrapper for the calling the C implementation of fs_size.
static int
file_size(live_restore_test_env &env, const std::string &file_name, wt_off_t *sizep)
{
    WT_SESSION *session = (WT_SESSION *)env.session;
    WT_FILE_SYSTEM *fs = &env.lr_fs->iface;

    std::string dest_file_path = env.dest_file_path(file_name);

    return env.lr_fs->iface.fs_size(fs, session, dest_file_path.c_str(), sizep);
}

static const int DEST_FILE_SIZE = 10;
static const int SOURCE_FILE_SIZE = 100;

/*
 * Set up a live restore scenario where a file exists in some combination of the destination and
 * source directories, might have a stop file, and live restore might be in the process of
 * migrating. Then return the result of an fs_size call.
 */
static int
test_file_size(live_restore_test_env *env, HasDest has_dest, HasSource has_source,
  IsMigrating is_migrating, HasStop stop_file_exists, wt_off_t *sizep)
{
    std::string file = "file1.txt";

    std::string dest_file = env->dest_file_path(file);
    std::string source_file = env->source_file_path(file);
    std::string stop_file = env->dest_file_path(file + WTI_LIVE_RESTORE_STOP_FILE_SUFFIX);

    // Clean up any files from prior runs
    testutil_remove(dest_file.c_str());
    testutil_remove(source_file.c_str());
    testutil_remove(stop_file.c_str());

    if (has_dest == DEST)
        create_file(dest_file.c_str(), DEST_FILE_SIZE);

    if (has_source == SOURCE)
        create_file(source_file.c_str(), SOURCE_FILE_SIZE);

    if (is_migrating == MIGRATING)
        env->lr_fs->state = WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION;
    else
        env->lr_fs->state = WTI_LIVE_RESTORE_STATE_COMPLETE;

    if (stop_file_exists == STOP)
        create_file(stop_file.c_str());

    return file_size(*env, file, sizep);
}

TEST_CASE("Live Restore fs_size", "[live_restore],[live_restore_fs_size]")
{
    live_restore_test_env env;
    /*
     * There are four factors that impact how we compute fs_size:
     * - If the file exists in the destination
     * - If the file exists in the source
     * - If live restore is currently background migrating files
     * - If a stop file exists
     * We test all permutations of these arguments in order
     */
    wt_off_t size;

    // There is no file in the source or destination.
    // Regardless of migration state or stop files fs_size returns ENOENT.
    REQUIRE(test_file_size(&env, NO_DEST, NO_SOURCE, NOT_MIGRATING, NO_STOP, &size) == ENOENT);
    REQUIRE(test_file_size(&env, NO_DEST, NO_SOURCE, NOT_MIGRATING, STOP, &size) == ENOENT);
    REQUIRE(test_file_size(&env, NO_DEST, NO_SOURCE, MIGRATING, NO_STOP, &size) == ENOENT);
    REQUIRE(test_file_size(&env, NO_DEST, NO_SOURCE, MIGRATING, STOP, &size) == ENOENT);

    // When the file only exists in the source and we've finished migrating return ENOENT.
    REQUIRE(test_file_size(&env, NO_DEST, SOURCE, NOT_MIGRATING, NO_STOP, &size) == ENOENT);
    REQUIRE(test_file_size(&env, NO_DEST, SOURCE, NOT_MIGRATING, STOP, &size) == ENOENT);

    // When the file only exists in the source and migration is underway file size should be read
    // from source.
    REQUIRE(test_file_size(&env, NO_DEST, SOURCE, MIGRATING, NO_STOP, &size) == 0);
    REQUIRE(size == SOURCE_FILE_SIZE);

    // However when the file only exists in the source, migration is underway, and there is
    // a stop file return ENOENT. This indicates we've deleted the file in the destination.
    REQUIRE(test_file_size(&env, NO_DEST, SOURCE, MIGRATING, STOP, &size) == ENOENT);

    // When the file exists in the destination fs_size will return 0 and file size should be read
    // from destination, in all cases.
    REQUIRE(test_file_size(&env, DEST, NO_SOURCE, NOT_MIGRATING, NO_STOP, &size) == 0);
    REQUIRE(size == DEST_FILE_SIZE);
    REQUIRE(test_file_size(&env, DEST, NO_SOURCE, NOT_MIGRATING, STOP, &size) == 0);
    REQUIRE(size == DEST_FILE_SIZE);
    REQUIRE(test_file_size(&env, DEST, NO_SOURCE, MIGRATING, NO_STOP, &size) == 0);
    REQUIRE(size == DEST_FILE_SIZE);
    REQUIRE(test_file_size(&env, DEST, NO_SOURCE, MIGRATING, STOP, &size) == 0);
    REQUIRE(size == DEST_FILE_SIZE);
    REQUIRE(test_file_size(&env, DEST, SOURCE, NOT_MIGRATING, NO_STOP, &size) == 0);
    REQUIRE(size == DEST_FILE_SIZE);
    REQUIRE(test_file_size(&env, DEST, SOURCE, NOT_MIGRATING, STOP, &size) == 0);
    REQUIRE(size == DEST_FILE_SIZE);
    REQUIRE(test_file_size(&env, DEST, SOURCE, MIGRATING, NO_STOP, &size) == 0);
    REQUIRE(size == DEST_FILE_SIZE);
    REQUIRE(test_file_size(&env, DEST, SOURCE, MIGRATING, STOP, &size) == 0);
    REQUIRE(size == DEST_FILE_SIZE);
}
