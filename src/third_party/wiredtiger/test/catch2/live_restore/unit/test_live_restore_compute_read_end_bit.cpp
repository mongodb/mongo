/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Test the live restore __live_restore_compute_read_end_bit functionality.
 * [live_restore_compute_read_end_bit].
 */

#include "../utils_live_restore.h"

using namespace utils;

struct compute_read_end_bit_test {
    compute_read_end_bit_test(uint32_t allocsize, uint64_t nbits, wt_off_t read_size,
      wt_off_t file_size, uint64_t first_clear_bit, uint64_t clear_len)
        : allocsize(allocsize), nbits(nbits), buf_size(WT_MAX(allocsize, read_size)),
          file_size(file_size), first_clear_bit(first_clear_bit)
    {
        // Initialize bitmap with bits in [first_clear_bit, first_clear_bit + clear_len - 1] are
        // unset where the rest are set.
        auto bitmap_len = (nbits + 7) >> 3;
        bitmap = (uint8_t *)malloc(bitmap_len * sizeof(uint8_t));
        memset(bitmap, 0xFF, bitmap_len * sizeof(uint8_t));
        __bit_nclr(bitmap, first_clear_bit, first_clear_bit + clear_len - 1);
    }

    ~compute_read_end_bit_test()
    {
        bitmap = nullptr;
    }
    uint32_t allocsize;
    uint64_t nbits;
    wt_off_t buf_size;
    wt_off_t file_size;
    uint64_t first_clear_bit;
    uint8_t *bitmap;
};

static bool
is_valid_end_bit(const uint64_t expected_end_bit, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh,
  wt_off_t buf_size, uint64_t first_clear_bit, wt_off_t file_size)
{
    auto max_read =
      std::min({WTI_BITMAP_END(lr_fh), WTI_BIT_TO_OFFSET(first_clear_bit) + buf_size, file_size});
    auto max_read_bit = WTI_OFFSET_TO_BIT(max_read - 1);

    uint64_t end_bit = first_clear_bit;
    for (uint64_t current_bit = first_clear_bit + 1; current_bit <= max_read_bit; current_bit++) {
        if (current_bit >= lr_fh->nbits)
            break;
        if (__bit_test(lr_fh->bitmap, current_bit))
            break;
        end_bit = current_bit;
    }

    return end_bit == expected_end_bit;
}

TEST_CASE("Test various live restore compute read end bit",
  "[live_restore], [live_restore_compute_read_end_bit]")
{
    live_restore_test_env env;
    WT_SESSION *session = reinterpret_cast<WT_SESSION *>(env.session);
    WTI_LIVE_RESTORE_FS *lr_fs = env.lr_fs;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = nullptr;

    auto dest_file = env.dest_file_path("test_table.wt");
    uint32_t allocsize = 4;
    uint64_t nbits = 16;
    wt_off_t read_size = (wt_off_t)allocsize * 4;
    wt_off_t file_size = (wt_off_t)nbits * allocsize;

    // The first sequence of clear bits has length 1.
    auto test1 = compute_read_end_bit_test(allocsize, nbits, read_size, file_size, 0, 1);
    // The first sequence of clear bits has multiple clear bits.
    auto test2 = compute_read_end_bit_test(allocsize, nbits, read_size, file_size, 0, 4);
    // The first sequence of clear bits is longer than the maximum we can read.
    auto test3 = compute_read_end_bit_test(allocsize, nbits, read_size, file_size, 4, 8);
    // The first sequence of clear bits has clear bits till the end of the bitmap.
    auto test4 = compute_read_end_bit_test(allocsize, nbits, read_size, file_size, 4, nbits - 4);
    // Simulate "buf_size == allocsize" by making read_size smaller than allocsize.
    auto test5 = compute_read_end_bit_test(allocsize, nbits, allocsize / 2, file_size, 4, 4);
    // Simulate file_size larger than bitmap length(When we have inserted new data to the file).
    auto test6 = compute_read_end_bit_test(allocsize, nbits, read_size, file_size + 12, 4, 4);
    // Simulate file_size smaller than bitmap length(When we have truncated the file).
    auto test7 = compute_read_end_bit_test(allocsize, nbits, read_size, file_size - 12, 4, 4);
    std::vector<compute_read_end_bit_test> tests = {
      test1, test2, test3, test4, test5, test6, test7};

    uint64_t end_bit;
    for (auto &test : tests) {
        create_file(dest_file, test.file_size);
        testutil_check(
          lr_fs->iface.fs_open_file((WT_FILE_SYSTEM *)lr_fs, session, dest_file.c_str(),
            WT_FS_OPEN_FILE_TYPE_DATA, WT_FS_OPEN_CREATE, (WT_FILE_HANDLE **)&lr_fh));

        lr_fh->allocsize = test.allocsize;
        lr_fh->bitmap = test.bitmap;
        lr_fh->nbits = test.nbits;
        testutil_check(__ut_live_restore_compute_read_end_bit(
          (WT_SESSION_IMPL *)session, lr_fh, test.buf_size, test.first_clear_bit, &end_bit));
        REQUIRE(
          is_valid_end_bit(end_bit, lr_fh, test.buf_size, test.first_clear_bit, test.file_size));

        testutil_check(lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, session));
        testutil_remove(dest_file.c_str());
    }
}
