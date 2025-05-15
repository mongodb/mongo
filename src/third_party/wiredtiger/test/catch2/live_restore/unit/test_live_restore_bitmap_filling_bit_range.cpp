/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Test the live restore bitmap filling bit range functionality.
 * [live_restore_bitmap_filling_bit_range].
 */

#include "../utils_live_restore.h"
#include "../../wrappers/mock_session.h"

using namespace utils;

struct filling_data {
    filling_data(
      uint32_t allocsize, uint64_t nbits, std::vector<std::pair<uint64_t, uint64_t>> ranges)
        : allocsize(allocsize), nbits(nbits), bitmap_len(__bitstr_size(nbits)),
          bitmap(bitmap_len, 0x0), ranges(std::move(ranges))
    {
    }

    uint32_t allocsize;
    uint64_t nbits;
    uint64_t bitmap_len;
    std::vector<uint8_t> bitmap;
    // range.first represents filling offset, range.second represents filling length.
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
};

static bool
is_bit_in_range(uint64_t bit_offset, const filling_data &test)
{
    for (const auto &range : test.ranges) {
        uint64_t range_bit_start = range.first / test.allocsize;
        uint64_t range_bit_end = (range.first + range.second - 1) / test.allocsize;
        if (range_bit_start <= bit_offset && bit_offset <= range_bit_end)
            return true;
    }
    return false;
}

/*
 * Iterate through all bits in the bitmap. For each bit, check if it is as expected by verifying --
 * if the bit is set, its bit_offset must fall within one of the filling ranges.
 */
static bool
is_valid_bitmap(const filling_data &test)
{
    for (uint64_t i = 0; i < test.bitmap_len; i++) {
        for (uint64_t j = 0; j < 8; j++) {
            bool bit_set = test.bitmap[i] & (1 << j);
            bool bit_in_range = is_bit_in_range((i << 3) | j, test);
            if (bit_set != bit_in_range)
                return false;
        }
    }
    return true;
}

TEST_CASE("Test various bitmap filling bit ranges",
  "[live_restore_bitmap], [live_restore_bitmap_filling_bit_range]")
{
    std::shared_ptr<mock_session> mock_session = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = mock_session->get_wt_session_impl();

    WTI_LIVE_RESTORE_FILE_HANDLE lr_fh;
    WT_CLEAR(lr_fh);
    // We need to have a non NULL pointer here for the encoding to take place.
    lr_fh.source = reinterpret_cast<WT_FILE_HANDLE *>(0xab);

    // Filling one range that fits within a single bit slot.
    filling_data test1 = filling_data(4, 16, {{16, 4}});
    // Filling one range that spans multiple bit slots.
    filling_data test2 = filling_data(4, 16, {{16, 16}});
    // Filling one range that fits within the last bit slot and extends beyond it.
    filling_data test3 = filling_data(4, 16, {{60, 8}});
    // Filling one range that is not tracked by the bitmap.
    filling_data test4 = filling_data(4, 16, {{64, 4}});
    // Filling one range that fits the entire bitmap.
    filling_data test5 = filling_data(4, 16, {{0, 64}});
    // Filling one range that spans the entire bitmap and extends beyond the last slot.
    filling_data test6 = filling_data(4, 16, {{0, 80}});
    // Filling multiple ranges that each range fits within a bit slot.
    filling_data test7 = filling_data(4, 16, {{16, 4}, {24, 4}, {32, 4}});
    // Filling multiple ranges that overlaps with each other.
    filling_data test8 = filling_data(4, 16, {{0, 8}, {4, 12}, {12, 16}});
    // Filling with some random allocsize, nbits, and ranges.
    filling_data test9 = filling_data(8, 128, {{8, 16}, {80, 56}, {96, 120}, {136, 168}, {16, 88}});
    filling_data test10 =
      filling_data(16, 64, {{0, 80}, {128, 48}, {144, 64}, {176, 32}, {192, 16}});
    filling_data test11 = filling_data(32, 256, {{32, 160}, {480, 480}, {512, 672}, {1312, 2688}});

    std::vector<filling_data> tests = {
      test1, test2, test3, test4, test5, test6, test7, test8, test9, test10, test11};

    REQUIRE(__wt_rwlock_init(session, &lr_fh.lock) == 0);
    for (auto &test : tests) {
        __wt_writelock(session, &lr_fh.lock);
        lr_fh.allocsize = test.allocsize;
        lr_fh.bitmap = test.bitmap.data();
        lr_fh.nbits = test.nbits;

        for (const auto &range : test.ranges)
            __ut_live_restore_fh_fill_bit_range(
              &lr_fh, session, (wt_off_t)range.first, (size_t)range.second);

        REQUIRE(is_valid_bitmap(test));
        __wt_writeunlock(session, &lr_fh.lock);
    }
    __wt_rwlock_destroy(session, &lr_fh.lock);
}
