/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Tests for the Live Restore file system's fs_file_exist function. [live_restore_file_exist]
 */

#include "../utils_live_restore.h"

using namespace utils;

// Wrapper for the calling the C implementation of fs_exists.
bool
file_exists(live_restore_test_env &env, const std::string &file_name)
{
    WT_SESSION *session = (WT_SESSION *)env.session;
    WT_FILE_SYSTEM *fs = &env.lr_fs->iface;

    std::string dest_file_path = env.dest_file_path(file_name);

    bool exists = false;
    env.lr_fs->iface.fs_exist(fs, session, dest_file_path.c_str(), &exists);
    return exists;
}

/*
 * Set up a live restore scenario where a file exists in some combination of the destination and
 * source directories, might have a stop file, and live restore might be in the process of
 * migrating. Then return the result of an fs_exist call.
 */
bool
test_file_exists(live_restore_test_env *env, HasDest has_dest, HasSource has_source,
  IsMigrating is_migrating, HasStop stop_file_exists)
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
        create_file(dest_file.c_str());

    if (has_source == SOURCE)
        create_file(source_file.c_str());

    if (is_migrating == MIGRATING)
        env->lr_fs->state = WTI_LIVE_RESTORE_STATE_BACKGROUND_MIGRATION;
    else
        env->lr_fs->state = WTI_LIVE_RESTORE_STATE_COMPLETE;

    if (stop_file_exists == STOP)
        create_file(stop_file.c_str());

    return file_exists(*env, file);
}

TEST_CASE("Live Restore fs_exist", "[live_restore],[live_restore_fs_exist]")
{
    live_restore_test_env env;

    /*
     * There are four factors that impact fs_exist:
     * - If the file exists in the destination
     * - If the file exists in the source
     * - If live restore is currently background migrating files
     * - If a stop file exists
     * We test all permutations of these arguments in order
     */

    // There is no file in the source or destination.
    // Regardless of migration state or stop files fs_exist returns false
    REQUIRE(test_file_exists(&env, NO_DEST, NO_SOURCE, NOT_MIGRATING, NO_STOP) == false);
    REQUIRE(test_file_exists(&env, NO_DEST, NO_SOURCE, NOT_MIGRATING, STOP) == false);
    REQUIRE(test_file_exists(&env, NO_DEST, NO_SOURCE, MIGRATING, NO_STOP) == false);
    REQUIRE(test_file_exists(&env, NO_DEST, NO_SOURCE, MIGRATING, STOP) == false);

    // When the file only exists in the source and we've finished migrating return false.
    // If the file existed it'd be present in the destination.
    REQUIRE(test_file_exists(&env, NO_DEST, SOURCE, NOT_MIGRATING, NO_STOP) == false);
    REQUIRE(test_file_exists(&env, NO_DEST, SOURCE, NOT_MIGRATING, STOP) == false);

    // When the file only exists in the source and migration is underway return true as
    // we haven't copied the file to the destination yet.
    REQUIRE(test_file_exists(&env, NO_DEST, SOURCE, MIGRATING, NO_STOP) == true);

    // However when the file only exists in the source, migration is underway, and there is
    // a stop file return false. This indicates we've deleted the file in the destination.
    REQUIRE(test_file_exists(&env, NO_DEST, SOURCE, MIGRATING, STOP) == false);

    // When the file exists in the destination fs_exist will return true in all cases
    REQUIRE(test_file_exists(&env, DEST, NO_SOURCE, NOT_MIGRATING, NO_STOP) == true);
    REQUIRE(test_file_exists(&env, DEST, NO_SOURCE, NOT_MIGRATING, STOP) == true);
    REQUIRE(test_file_exists(&env, DEST, NO_SOURCE, MIGRATING, NO_STOP) == true);
    REQUIRE(test_file_exists(&env, DEST, NO_SOURCE, MIGRATING, STOP) == true);
    REQUIRE(test_file_exists(&env, DEST, SOURCE, NOT_MIGRATING, NO_STOP) == true);
    REQUIRE(test_file_exists(&env, DEST, SOURCE, NOT_MIGRATING, STOP) == true);
    REQUIRE(test_file_exists(&env, DEST, SOURCE, MIGRATING, NO_STOP) == true);
    REQUIRE(test_file_exists(&env, DEST, SOURCE, MIGRATING, STOP) == true);
}
