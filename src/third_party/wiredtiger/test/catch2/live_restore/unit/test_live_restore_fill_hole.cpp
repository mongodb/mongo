/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Test the live restore __live_restore_fill_hole functionality.
 * [live_restore_fill_hole].
 */

#include "../utils_live_restore.h"

using namespace utils;

struct fill_hole_test {
    fill_hole_test(uint32_t allocsize, uint64_t nbits, wt_off_t read_size, uint64_t first_clear_bit,
      uint64_t clear_len, uint64_t expect_filled_len, bool expect_finished)
        : allocsize(allocsize), nbits(nbits), buf_size(WT_MAX(allocsize, read_size)),
          first_clear_bit(first_clear_bit), clear_len(clear_len),
          expect_filled_len(expect_filled_len), expect_finished(expect_finished)
    {
        // Initialize bitmap with bits in [first_clear_bit, first_clear_bit + clear_len - 1] are
        // unset where the rest are set.
        auto bitmap_len = (nbits + 7) >> 3;
        bitmap = (uint8_t *)malloc(bitmap_len * sizeof(uint8_t));
        memset(bitmap, 0xFF, bitmap_len * sizeof(uint8_t));
        if (clear_len > 0)
            __bit_nclr(bitmap, first_clear_bit, first_clear_bit + clear_len - 1);
    }

    ~fill_hole_test()
    {
        bitmap = nullptr;
    }
    uint32_t allocsize;
    uint64_t nbits;
    wt_off_t buf_size;
    uint64_t first_clear_bit;
    uint64_t clear_len;
    uint8_t *bitmap;
    uint64_t expect_filled_len;
    bool expect_finished;
};

/*
 * Verify live restore fill hole, we should have set bits from first_clear_bit with a length of
 * expect_filled_len, where the rest(with length of clear_len - expect_filled_len) should remain as
 * unset bits.
 */
static bool
is_valid_fill(WT_SESSION *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, uint64_t first_clear_bit,
  uint64_t clear_len, uint64_t expect_filled_len, char dummy_char, char source_char)
{
    std::vector<char> buf(lr_fh->allocsize * clear_len);
    testutil_check(lr_fh->destination->fh_read(lr_fh->destination, session,
      WTI_BIT_TO_OFFSET(first_clear_bit), WTI_BIT_TO_OFFSET(clear_len), buf.data()));

    // Verify set bits.
    for (auto i = 0; i < expect_filled_len; i++) {
        if (!__bit_test(lr_fh->bitmap, first_clear_bit + i))
            return false;
        for (auto j = 0; j < lr_fh->allocsize; j++)
            if (buf[WTI_BIT_TO_OFFSET(i) + j] != source_char)
                return false;
    }
    // Verify unset bits.
    for (auto i = expect_filled_len; i < clear_len; i++) {
        if (__bit_test(lr_fh->bitmap, first_clear_bit + i))
            return false;
        for (auto j = 0; j < lr_fh->allocsize; j++)
            if (buf[WTI_BIT_TO_OFFSET(i) + j] != dummy_char)
                return false;
    }
    return true;
}

static void
generate_bitmap(uint64_t bitmap_value, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, uint8_t *bitmap_old)
{
    __bit_nclr(lr_fh->bitmap, 0, lr_fh->nbits - 1);
    __bit_nclr(bitmap_old, 0, lr_fh->nbits - 1);
    for (auto i = 0; i < lr_fh->nbits; i++) {
        if (bitmap_value & 1) {
            __bit_set(lr_fh->bitmap, i);
            __bit_set(bitmap_old, i);
        }
        bitmap_value >>= 1;
    }
}

// Verify if bitmap == -1 and all unset bits are migrated from source where set bits remain
// unchanged.
static bool
verify_fill_complete(WT_SESSION *session, WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh, uint8_t *bitmap_old,
  char dummy_char, char source_char)
{
    uint64_t first_clear_bit;
    if (__bit_ffc(lr_fh->bitmap, lr_fh->nbits, &first_clear_bit) != -1)
        return false;

    auto file_size = (size_t)(lr_fh->nbits * lr_fh->allocsize);
    std::vector<char> buf(file_size);
    testutil_check(
      lr_fh->iface.fh_read((WT_FILE_HANDLE *)lr_fh, session, 0, file_size, buf.data()));

    // Check dest file, if bits are unset before migration they should be migrated and a source_char
    // is expected, otherwise they should remain unchanged as dummy_char.
    for (auto i = 0; i < lr_fh->nbits; i++) {
        auto expected_char = __bit_test(bitmap_old, i) ? dummy_char : source_char;
        for (auto j = 0; j < lr_fh->allocsize; j++)
            if (buf[WTI_BIT_TO_OFFSET(i) + j] != expected_char)
                return false;
    }
    return true;
}

TEST_CASE("Test various live restore fill hole", "[live_restore], [live_restore_fill_hole]")
{
    auto file_name = "test_table.wt";
    uint32_t allocsize = 4;
    uint64_t nbits = 16;
    wt_off_t read_size = (wt_off_t)allocsize * 4;
    wt_off_t file_size = (wt_off_t)nbits * allocsize;
    auto dummy_char = '0';
    auto source_char = '1';

    live_restore_test_env env;
    WT_SESSION *session = reinterpret_cast<WT_SESSION *>(env.session);
    WTI_LIVE_RESTORE_FS *lr_fs = env.lr_fs;
    WTI_LIVE_RESTORE_FILE_HANDLE *lr_fh = nullptr;

    SECTION("Live restore fill hole single call")
    {
        // The first sequence of clear bits has length 1.
        auto test1 = fill_hole_test(allocsize, nbits, read_size, 0, 1, 1, false);
        // The first sequence has multiple clear bits.
        auto test2 = fill_hole_test(allocsize, nbits, read_size, 0, 4, 4, false);
        // The first sequence of clear bits is longer than the buf_size(the maximum we can read).
        auto test3 = fill_hole_test(allocsize, nbits, read_size, 4, 8, 4, false);
        // The first sequence has clear bits till the end of the bitmap.
        auto test4 = fill_hole_test(allocsize, nbits, read_size, 4, nbits - 4, 4, false);
        // Simulate "buf_size == allocsize" by making read_size smaller than allocsize.
        auto test5 = fill_hole_test(allocsize, nbits, allocsize / 2, 4, 4, 1, false);
        // The file has been fully migrated.
        auto test6 = fill_hole_test(allocsize, nbits, read_size, 0, 0, 0, true);
        std::vector<fill_hole_test> tests = {test1, test2, test3, test4, test5, test6};

        bool finished = false;
        std::vector<char> buf(read_size);
        for (auto &test : tests) {
            create_file(env.source_file_path(file_name), file_size, source_char);
            create_file(env.dest_file_path(file_name), file_size, dummy_char);
            testutil_check(lr_fs->iface.fs_open_file((WT_FILE_SYSTEM *)lr_fs, session,
              env.dest_file_path(file_name).c_str(), WT_FS_OPEN_FILE_TYPE_DATA, WT_FS_OPEN_CREATE,
              (WT_FILE_HANDLE **)&lr_fh));

            lr_fh->allocsize = test.allocsize;
            lr_fh->bitmap = test.bitmap;
            lr_fh->nbits = test.nbits;

            wt_off_t read_offset = WTI_BIT_TO_OFFSET(lr_fh->nbits);
            __wt_writelock((WT_SESSION_IMPL *)session, &lr_fh->lock);
            testutil_check(__ut_live_restore_fill_hole(
              lr_fh, session, buf.data(), test.buf_size, &read_offset, &finished));
            __wt_writeunlock((WT_SESSION_IMPL *)session, &lr_fh->lock);
            REQUIRE(finished == test.expect_finished);
            if (!finished)
                REQUIRE(WTI_OFFSET_TO_BIT(read_offset) == test.first_clear_bit);

            /*
             * Only verify hole filled when clear_len is greater than 0, as 0 means bitmap == -1,
             * which represents a test for dest file being fully written before
             * __live_restore_fill_hole is called, thus no hole will be filled.
             */
            if (test.clear_len > 0)
                REQUIRE(is_valid_fill(session, lr_fh, test.first_clear_bit, test.clear_len,
                  test.expect_filled_len, dummy_char, source_char));
            testutil_check(lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, session));
            testutil_remove(env.source_file_path(file_name).c_str());
            testutil_remove(env.dest_file_path(file_name).c_str());
        }
    }

    // The background migration thread keeps calling _live_restore_fill_hole until all holes are
    // filled, we
    SECTION("Live restore fill hole multiple calls")
    {
        /*
         * Try multiple runs of the fill hole function until migration is complete. In each run, use
         * a value from 0 to 65535 (as `nbits` is defined as 16 here) to represent the bitmap in
         * decimal. This value will be used to generate the bitmap for the test.
         * -- the first run simulates a common case where bitmap is 0 representing no data written
         *    to the dest file before migration starts.
         * -- the second run simulates bitmap is -1 representing all data has been written to the
         *    dest file before migration starts.
         * -- the rest runs use some random bitmaps.
         */
        std::vector<uint64_t> bitmap_values = {0, (1 << 16) - 1, 25570, 60512, 61138, 5312, 54072,
          53673, 12818, 25220, 43891, 24512, 4554, 31056, 33855, 47176, 16133, 38798, 5838, 47037,
          52768, 53470, 20027, 1178, 25770, 24647, 1252, 54659, 37263, 2932, 32675, 45793, 9074,
          18155, 19660, 22559, 27085, 52890, 25813, 30261, 16726, 52872, 44574, 41308, 36030, 58776,
          39381, 5055, 38016, 10280};
        uint8_t *bitmap_old = nullptr;
        testutil_check(__bit_alloc((WT_SESSION_IMPL *)session, nbits, &bitmap_old));
        std::vector<char> buf(read_size);
        for (auto bitmap_value : bitmap_values) {
            testutil_assert(nbits < 64);
            testutil_assert(bitmap_value < ((uint64_t)1 << nbits));
            create_file(env.source_file_path(file_name), file_size, source_char);
            create_file(env.dest_file_path(file_name), file_size, dummy_char);
            testutil_check(lr_fs->iface.fs_open_file((WT_FILE_SYSTEM *)lr_fs, session,
              env.dest_file_path(file_name).c_str(), WT_FS_OPEN_FILE_TYPE_DATA, WT_FS_OPEN_CREATE,
              (WT_FILE_HANDLE **)&lr_fh));

            lr_fh->allocsize = allocsize;
            testutil_check(__bit_alloc((WT_SESSION_IMPL *)session, nbits, &lr_fh->bitmap));
            lr_fh->nbits = nbits;
            // Generate bitmap for the run.
            generate_bitmap(bitmap_value, lr_fh, bitmap_old);

            wt_off_t read_offset;
            bool finished = false;
            // Run fill hole until finished.
            __wt_writelock((WT_SESSION_IMPL *)session, &lr_fh->lock);
            while (!finished)
                testutil_check(__ut_live_restore_fill_hole(
                  lr_fh, session, buf.data(), read_size, &read_offset, &finished));
            __wt_writeunlock((WT_SESSION_IMPL *)session, &lr_fh->lock);

            // Verify if filling hole completes.
            REQUIRE(verify_fill_complete(session, lr_fh, bitmap_old, dummy_char, source_char));

            testutil_check(lr_fh->iface.close((WT_FILE_HANDLE *)lr_fh, session));
            testutil_remove(env.source_file_path(file_name).c_str());
            testutil_remove(env.dest_file_path(file_name).c_str());
        }

        __wt_free((WT_SESSION_IMPL *)session, bitmap_old);
    }
}
