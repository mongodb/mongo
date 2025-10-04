/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Tests for the Live Restore file handle's fh_truncate function. [live_restore_fh_truncate]
 */

#include "../utils_live_restore.h"

using namespace utils;

static void
init_file_handle(WT_SESSION *session, WTI_LIVE_RESTORE_FS *lr_fs, const char *file_name,
  int allocsize, int file_size, WTI_LIVE_RESTORE_FILE_HANDLE **lr_fhp)
{
    testutil_check(lr_fs->iface.fs_open_file((WT_FILE_SYSTEM *)lr_fs, session, file_name,
      WT_FS_OPEN_FILE_TYPE_DATA, WT_FS_OPEN_CREATE, (WT_FILE_HANDLE **)lr_fhp));

    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = *lr_fhp;
    lr_fh->allocsize = allocsize;
    lr_fh->nbits = file_size / allocsize;
    testutil_check(__bit_alloc((WT_SESSION_IMPL *)session, lr_fh->nbits, &lr_fh->bitmap));
}

// Validate bitmap, all bits before first set bit should be clear, and all bits after it should be
// set.
static bool
validate_bitmap(WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, wt_off_t offset)
{
    uint64_t ffs = -1;
    // In this function we don't consider the case where there's no set bit in the bitmap.
    REQUIRE(__bit_ffs(lr_fh->bitmap, lr_fh->nbits, &ffs) != -1);

    auto first_set_bit = WTI_OFFSET_TO_BIT(offset);
    // Validate all bits before first set bit are clear.
    if (ffs != first_set_bit)
        return false;

    // Validate all bits after and including the first set bit are set.
    for (int current_bit = first_set_bit; current_bit < lr_fh->nbits; current_bit++)
        if (!__bit_test(lr_fh->bitmap, current_bit))
            return false;

    return true;
}

TEST_CASE("Live Restore fh_truncate", "[live_restore],[live_restore_fh_truncate]")
{
    live_restore_test_env env;
    std::string file = "file1.txt";
    wt_off_t size;
    WT_SESSION *session = (WT_SESSION *)env.session;
    WTI_LIVE_RESTORE_FS *lr_fs = env.lr_fs;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = nullptr;
    uint32_t allocsize = 4;
    wt_off_t file_size = 16;

    // Create source file and init file handle, the dest file will be created with a same size of
    // source file during fs_open_file.
    create_file(env.source_file_path(file).c_str(), file_size);
    init_file_handle(
      session, lr_fs, env.dest_file_path(file).c_str(), allocsize, file_size, &lr_fh);
    testutil_check(lr_fh->iface.fh_size((WT_FILE_HANDLE *)lr_fh, session, &size));
    REQUIRE(size == file_size);

    // Test truncating a file to its existing length does not change its length or bitmap.
    testutil_check(lr_fh->iface.fh_truncate((WT_FILE_HANDLE *)lr_fh, session, file_size));
    testutil_check(lr_fh->iface.fh_size((WT_FILE_HANDLE *)lr_fh, session, &size));
    REQUIRE(size == file_size);

    // Reduce file size, the truncated portion should have bitmap filled.
    testutil_check(
      lr_fh->iface.fh_truncate((WT_FILE_HANDLE *)lr_fh, session, file_size - allocsize * 2));
    testutil_check(lr_fh->iface.fh_size((WT_FILE_HANDLE *)lr_fh, session, &size));
    REQUIRE(size == file_size - allocsize * 2);
    REQUIRE(validate_bitmap(lr_fh, file_size - allocsize * 2));

    // Now test positive length truncate but keep the file size smaller than its original size.
    testutil_check(
      lr_fh->iface.fh_truncate((WT_FILE_HANDLE *)lr_fh, session, file_size - allocsize));
    testutil_check(lr_fh->iface.fh_size((WT_FILE_HANDLE *)lr_fh, session, &size));
    REQUIRE(size == file_size - allocsize);
    // The bitmap is unchanged.
    REQUIRE(validate_bitmap(lr_fh, file_size - allocsize * 2));

    // Test positive length file truncate that makes file size larger than its original size.
    testutil_check(
      lr_fh->iface.fh_truncate((WT_FILE_HANDLE *)lr_fh, session, file_size + allocsize * 2));
    testutil_check(lr_fh->iface.fh_size((WT_FILE_HANDLE *)lr_fh, session, &size));
    REQUIRE(size == file_size + allocsize * 2);
    REQUIRE(validate_bitmap(lr_fh, file_size - allocsize * 2));

    // Clear bitmap to simulate a file being positive truncated before any bits in its bitmap are
    // set.
    __bit_nclr(lr_fh->bitmap, 0, lr_fh->nbits);
    // Test the positive truncate.
    testutil_check(
      lr_fh->iface.fh_truncate((WT_FILE_HANDLE *)lr_fh, session, file_size + allocsize));
    testutil_check(lr_fh->iface.fh_size((WT_FILE_HANDLE *)lr_fh, session, &size));
    REQUIRE(size == file_size + allocsize);
    //  We're truncating past the end of the bitmap, so no bits are modified.
    uint64_t ffs = 0;
    REQUIRE(__bit_ffs(lr_fh->bitmap, lr_fh->nbits, &ffs) == -1);

    // Test where a truncation is partially within the bitmap range, and fh_truncate should still
    // function.
    testutil_check(
      lr_fh->iface.fh_truncate((WT_FILE_HANDLE *)lr_fh, session, file_size - allocsize));
    testutil_check(lr_fh->iface.fh_size((WT_FILE_HANDLE *)lr_fh, session, &size));
    REQUIRE(size == file_size - allocsize);
    // The truncated portion which is in the bitmap range should have bits set.
    REQUIRE(validate_bitmap(lr_fh, file_size - allocsize));

    testutil_check(lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, session));
}
