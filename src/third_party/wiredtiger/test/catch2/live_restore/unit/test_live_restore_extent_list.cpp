/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Tests of the Live Restore extent lists. Extent lists track "holes" in a file representing ranges
 * of data that still need to be copied from the source directory into the destination directory.
 * [live_restore_extent_list]
 */

#include "../utils_live_restore.h"

using namespace utils;

TEST_CASE("Live Restore Extent Lists: Creation", "[live_restore],[live_restore_extent_list]")
{
    /*
     * Note: this code runs once per SECTION so we're creating a brand new WT database for each
     * section. If this gets slow we can make it static and manually clear the destination and
     * source for each section.
     */
    live_restore_test_env env;

    WT_SESSION_IMPL *session = env.session;
    WT_SESSION *wt_session = reinterpret_cast<WT_SESSION *>(session);

    std::string file_name = "MY_FILE.txt";
    std::string source_file = env.source_file_path(file_name);
    std::string dest_file = env.dest_file_path(file_name);

    SECTION("Open a new unbacked file")
    {
        WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
        testutil_check(open_lr_fh(env, dest_file.c_str(), &lr_fh));

        // There's no backing file in the source directory, so no extent list to track.
        REQUIRE(extent_list_str(lr_fh) == "");
        lr_fh->iface.close(reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session);
    }

    SECTION("Open a new backed file")
    {
        create_file(source_file.c_str(), 1000);

        WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
        testutil_check(open_lr_fh(env, dest_file.c_str(), &lr_fh));

        // We've created a new file in the destination backed by a file in source.
        // We haven't read or written anything so the file is one big hole.
        REQUIRE(extent_list_in_order(lr_fh));
        REQUIRE(extent_list_str(lr_fh) == "(0-999)");
        lr_fh->iface.close(reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session);
    }

    SECTION("The extent list can't have holes beyond the end of the destination file")
    {
        // The following steps aren't a realistic scenario in Live Restore, but it gets
        // us to the desired test state.

        // Create a source file of size 110 and open the lr_fh to copy that metadata.
        // This creates a destination file also of size 110.
        create_file(source_file.c_str(), 110);
        WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
        testutil_check(open_lr_fh(env, dest_file.c_str(), &lr_fh));

        // Replace the source file with a larger file of size 200 and reopen the destination file
        // to recompute the extent list.
        testutil_remove(source_file.c_str());
        create_file(source_file.c_str(), 200);

        lr_fh->iface.close(reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session);
        testutil_check(open_lr_fh(env, dest_file.c_str(), &lr_fh));

        // The extent list is capped at the size of the destination file. It doesn't take the
        // source file size into account.
        REQUIRE(extent_list_in_order(lr_fh));
        REQUIRE(extent_list_str(lr_fh) == "(0-109)");
        lr_fh->iface.close(reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session);
    }

    // This test doesn't currently work on Macs
#ifndef __APPLE__
    SECTION("The extent list can't have holes beyond the end of the source file")
    {
        // The following steps aren't a realistic scenario in Live Restore, but it gets
        // us to the desired test state.

        // Create a source file of size 110 and open the lr_fh to copy that metadata.
        // This creates a destination file also of size 110.
        create_file(source_file.c_str(), 110);
        WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
        testutil_check(open_lr_fh(env, dest_file.c_str(), &lr_fh));

        // Now positive truncate to increase the size of the destination file to be larger than
        // the source. Using a positive truncate leaves a hole past the end of the source file.
        testutil_check(truncate(dest_file.c_str(), 200));

        lr_fh->iface.close(reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session);
        int ret = open_lr_fh(env, dest_file.c_str(), &lr_fh);
        REQUIRE(ret == EINVAL);
    }
#endif

    SECTION("Open a backed, completely copied file")
    {
        create_file(source_file.c_str(), 110);

        // Copy the file to DEST manually. This is a full copy.
        testutil_copy(source_file.c_str(), dest_file.c_str());

        // Nothing to copy and therefore no extent list.
        WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
        testutil_check(open_lr_fh(env, dest_file.c_str(), &lr_fh));
        REQUIRE(extent_list_str(lr_fh) == "");
        lr_fh->iface.close(reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session);
    }

    // This test doesn't currently work on Macs
#ifndef __APPLE__
    SECTION("Open a backed, partially copied file")
    {
        WT_LIVE_RESTORE_FILE_HANDLE *lr_fh;
        char buf[4096];

        create_file(source_file.c_str(), 32768);
        testutil_check(open_lr_fh(env, dest_file.c_str(), &lr_fh));

        // Migrate the first 4KB by reading and writing them. Live restore will read from the source
        // and write back to the destination.
        lr_fh->iface.fh_read(reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session, 0, 4096, buf);
        lr_fh->iface.fh_write(reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session, 0, 4096, buf);

        // Close the file and reopen it to generate the extent list from holes in the dest file
        lr_fh->iface.close(reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session);
        testutil_check(open_lr_fh(env, dest_file.c_str(), &lr_fh));

        // We've written 4KB to the start of the file. There should only be a hole at the end.
        REQUIRE(extent_list_in_order(lr_fh));
        REQUIRE(extent_list_str(lr_fh) == "(4096-32767)");

        // Now repeat the process to create an extent list with multiple holes.
        lr_fh->iface.fh_read(
          reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session, 8192, 4096, buf);
        lr_fh->iface.fh_write(
          reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session, 8192, 4096, buf);
        lr_fh->iface.close(reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session);
        testutil_check(open_lr_fh(env, dest_file.c_str(), &lr_fh));
        REQUIRE(extent_list_in_order(lr_fh));
        REQUIRE(extent_list_str(lr_fh) == "(4096-8191), (12288-32767)");

        lr_fh->iface.close(reinterpret_cast<WT_FILE_HANDLE *>(lr_fh), wt_session);
    }
#endif
}
