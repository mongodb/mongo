/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Tests of the Live Restore file systems, directory list functions. These functions report which
 * files exist in the unified live restore directory, hiding whether they're in the destination,
 * source, or both backing directories. [live_restore_directory_list]
 */

#include "../utils_live_restore.h"
#include <iostream>
#include <set>

using namespace utils;

using lr_files = std::set<std::string>;

const std::string NO_DIR = "NO_DIRECTORY";

/*
 * Run directory_list and return the files found as a set for easy comparisons. Optionally specify a
 * subdirectory too look in or a prefix all results must match. An expected return code can also be
 * provided. If we expect an error and it fires we'll still return an empty list of files.
 */
static lr_files
directory_list(live_restore_test_env &env, const std::string &directory = NO_DIR,
  const std::string &prefix = "", int expect_ret = 0)
{

    WT_SESSION *wt_session = reinterpret_cast<WT_SESSION *>(env.session);
    WTI_LIVE_RESTORE_FS *lr_fs = env.lr_fs;

    char **dirlist = NULL;
    uint32_t count;
    std::string list_dir = directory;
    if (list_dir == NO_DIR) {
        list_dir = env.DB_DEST;
    }

    int ret = lr_fs->iface.fs_directory_list(
      (WT_FILE_SYSTEM *)lr_fs, wt_session, list_dir.c_str(), prefix.c_str(), &dirlist, &count);
    REQUIRE(ret == expect_ret);

    std::set<std::string> found_files{};
    for (int i = 0; i < count; i++) {
        std::string found_file(dirlist[i]);
        found_files.insert(found_file);
    }

    lr_fs->iface.fs_directory_list_free((WT_FILE_SYSTEM *)lr_fs, wt_session, dirlist, count);
    return found_files;
}

/* Return true if the file list contains the check files, removing the metadata files manually. */
static bool
file_list_equals(lr_files list, lr_files check)
{
    list.erase(WT_METAFILE);
    list.erase(WT_METADATA_TURTLE);
    list.erase(WT_METADATA_TURTLE_SET);
    list.erase(WT_WIREDTIGER);
    list.erase(WT_BASECONFIG);
    list.erase(WT_SINGLETHREAD);
    list.erase(WT_HS_FILE);

    if (list != check) {
        std::cout << "Mismatch between list and check!" << std::endl;
        std::cout << "List: ";
        for (const auto &file : list)
            std::cout << file << " ";
        std::cout << std::endl;

        std::cout << "Check: ";
        for (const auto &file : check)
            std::cout << file << " ";
        std::cout << std::endl;
    }

    return list == check;
}

static lr_files
directory_list_subfolder(
  live_restore_test_env &env, const std::string &directory, int expect_ret = 0)
{
    return directory_list(env, directory, "", expect_ret);
}

static lr_files
directory_list_prefix(live_restore_test_env &env, const std::string &prefix, int expect_ret = 0)
{
    return directory_list(env, env.DB_DEST, prefix, expect_ret);
}

TEST_CASE("Live Restore Directory List", "[live_restore],[live_restore_directory_list]")
{
    /*
     * Note: this code runs once per SECTION so we're creating a brand new WT database for each
     * section. If this gets slow we can make it static and manually clear the destination and
     * source for each section.
     */
    live_restore_test_env env;

    std::string file_1 = "file1.txt";
    std::string file_2 = "file2.txt";
    std::string file_3 = "file3.txt";
    std::string file_4 = "file4.txt";

    std::string subfolder = "subfolder";
    std::string subfolder_dest_path = env.DB_DEST + "/" + subfolder;
    std::string subfolder_source_path = env.DB_SOURCE + "/" + subfolder;

    SECTION("Directory list - Test when files only exist in the destination")
    {
        // Start with an empty directory.
        REQUIRE(file_list_equals(directory_list(env), lr_files{}));

        // Progressively add files
        create_file(env.dest_file_path(file_1).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1}));

        create_file(env.dest_file_path(file_2).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_2}));

        create_file(env.dest_file_path(file_3).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_2, file_3}));

        // And then delete them
        testutil_remove(env.dest_file_path(file_2).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_3}));

        testutil_remove(env.dest_file_path(file_1).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_3}));

        testutil_remove(env.dest_file_path(file_3).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{}));
    }

    SECTION("Directory list - Test when files only exist in the source")
    {
        // Start with an empty directory.
        REQUIRE(file_list_equals(directory_list(env), lr_files{}));

        // Progressively add files
        create_file(env.source_file_path(file_1).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1}));

        create_file(env.source_file_path(file_2).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_2}));

        create_file(env.source_file_path(file_3).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_2, file_3}));

        // And then delete them
        testutil_remove(env.source_file_path(file_2).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_3}));

        testutil_remove(env.source_file_path(file_1).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_3}));

        testutil_remove(env.source_file_path(file_3).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{}));
    }

    SECTION("Directory list - Test when files exist in both source and destination")
    {
        // Start with an empty directory.
        REQUIRE(file_list_equals(directory_list(env), lr_files{}));

        // Progressively add files
        create_file(env.dest_file_path(file_1).c_str());
        create_file(env.source_file_path(file_1).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1}));

        create_file(env.dest_file_path(file_2).c_str());
        create_file(env.source_file_path(file_2).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_2}));

        create_file(env.dest_file_path(file_3).c_str());
        create_file(env.source_file_path(file_3).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_2, file_3}));

        // And then delete them
        testutil_remove(env.dest_file_path(file_2).c_str());
        testutil_remove(env.source_file_path(file_2).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_3}));

        testutil_remove(env.dest_file_path(file_1).c_str());
        testutil_remove(env.source_file_path(file_1).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_3}));

        testutil_remove(env.dest_file_path(file_3).c_str());
        testutil_remove(env.source_file_path(file_3).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{}));
    }

    SECTION("Directory list - Test when files exist either in source or destination, but not both")
    {
        // Add one file to the source.
        create_file(env.source_file_path(file_1).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1}));

        // And now the destination.
        create_file(env.dest_file_path(file_2).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_2}));
    }

    SECTION(
      "Directory list - Test a file isn't reported when there's a tombstone in the destination")
    {
        // Add some files to the source.
        create_file(env.source_file_path(file_1).c_str());
        create_file(env.source_file_path(file_2).c_str());
        create_file(env.source_file_path(file_3).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_2, file_3}));

        // Now progressively add tombstones. The files are no longer reported.
        create_file(env.tombstone_file_path(file_2).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_1, file_3}));

        create_file(env.tombstone_file_path(file_1).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{file_3}));

        create_file(env.tombstone_file_path(file_3).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{}));

        // Now add the tombstone before the file to confirm it isn't reported.
        create_file(env.tombstone_file_path(file_4).c_str());
        create_file(env.source_file_path(file_4).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{}));
    }

    SECTION("Directory list - Test directory list reports subfolders")
    {
        // Only in the destination
        testutil_mkdir(subfolder_dest_path.c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{subfolder}));

        // And then deleted
        testutil_remove(subfolder_dest_path.c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{}));

        // Only in the source
        testutil_mkdir(subfolder_source_path.c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{subfolder}));

        // Now in both
        testutil_mkdir(subfolder_dest_path.c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{subfolder}));

        // Check that we *don't* report the contents, just the subfolder itself
        std::string sub_file_1 = subfolder + "/" + file_1;
        create_file(env.dest_file_path(sub_file_1).c_str());
        REQUIRE(file_list_equals(directory_list(env), lr_files{subfolder}));
    }

    SECTION(
      "Directory list - Test ENOENT is returned when listing the contents of a subfolder that "
      "doesn't exist")
    {
        // When the subfolder doesn't exist expect a ENOENT will be returned.
        directory_list_subfolder(env, subfolder_dest_path, ENOENT);

        // But if the subfolder exists in either backing directory we'll return successfully.
        testutil_mkdir(subfolder_dest_path.c_str());
        REQUIRE(file_list_equals(directory_list(env, subfolder_dest_path), lr_files{}));

        testutil_remove(subfolder_dest_path.c_str());
        testutil_mkdir(subfolder_source_path.c_str());
        REQUIRE(file_list_equals(directory_list(env, subfolder_dest_path), lr_files{}));

        testutil_mkdir(subfolder_dest_path.c_str());
        REQUIRE(file_list_equals(directory_list(env, subfolder_dest_path), lr_files{}));
    }

    SECTION("Directory list - Test multi-level subdirectories")
    {
        std::string sub_subfolder_dest_path = env.DB_DEST + "/" + subfolder + "/sub_subfolder";
        std::string sub_subfolder_file_1_path = sub_subfolder_dest_path + "/" + file_1;
        testutil_mkdir(subfolder_dest_path.c_str());
        testutil_mkdir(sub_subfolder_dest_path.c_str());
        create_file(sub_subfolder_file_1_path.c_str());

        REQUIRE(file_list_equals(directory_list(env, sub_subfolder_dest_path), lr_files{file_1}));
    }

    SECTION("Directory list - Test reporting contents of a subdirectory")
    {
        testutil_mkdir(subfolder_dest_path.c_str());
        testutil_mkdir(subfolder_source_path.c_str());

        // To keep this test short we'll just test on file in each backing directory.
        // We've tested other behaviors above
        std::string sub_file_1 = subfolder + "/" + file_1;
        create_file(env.dest_file_path(sub_file_1).c_str());

        std::string sub_file_2 = subfolder + "/" + file_2;
        create_file(env.source_file_path(sub_file_2).c_str());

        // Note we're returning file_1 here, not sub_file_1. Since we're reporting the
        // contents of the subfolder the file names are relative to that folder.
        REQUIRE(
          file_list_equals(directory_list(env, subfolder_dest_path), lr_files{file_1, file_2}));
    }

    SECTION("Directory list - Test only files matching the specified prefix are returned")
    {

        create_file(env.dest_file_path(file_1).c_str());
        create_file(env.source_file_path(file_2).c_str());
        testutil_mkdir(subfolder_dest_path.c_str());

        // Report all files and folders when prefix is empty
        REQUIRE(
          file_list_equals(directory_list_prefix(env, ""), lr_files{file_1, file_2, subfolder}));

        // Now only report the files
        REQUIRE(file_list_equals(directory_list_prefix(env, "file"), lr_files{file_1, file_2}));

        // Only the folder
        REQUIRE(file_list_equals(directory_list_prefix(env, "sub"), lr_files{subfolder}));

        // Only file_1. The prefix is the entire file name
        REQUIRE(file_list_equals(directory_list_prefix(env, file_1), lr_files{file_1}));

        // A prefix longer than any files or folders in the directory
        REQUIRE(file_list_equals(directory_list_prefix(env, std::string(10000, 'A')), lr_files{}));

        // The prefix is actually a suffix
        REQUIRE(file_list_equals(directory_list_prefix(env, "_1.txt"), lr_files{}));

        // The prefix matches a file's full name but then has additional characters
        REQUIRE(file_list_equals(directory_list_prefix(env, "file_1.txt.txt.txt"), lr_files{}));
    }

    SECTION("Directory list - Test that temporary files aren't returned")
    {
        create_file(env.dest_file_path(file_1).c_str());
        create_file(env.source_file_path(file_2).c_str());
        create_file(env.dest_file_path(file_1 + ".lr_tmp").c_str());
        REQUIRE(file_list_equals(directory_list_prefix(env, ""), lr_files{file_1, file_2}));

        // Test that a prefixed search doesn't return temporary files.
        create_file(env.dest_file_path("WiredTigerLog.lr_tmp"));
        REQUIRE(file_list_equals(directory_list_prefix(env, "WiredTigerLog"), lr_files{}));

        // A temporary file string without the "." will be returned.
        create_file(env.dest_file_path("lr_tmp"));
        REQUIRE(
          file_list_equals(directory_list_prefix(env, ""), lr_files{file_1, file_2, "lr_tmp"}));
    }
}
