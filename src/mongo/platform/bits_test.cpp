// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/bits.h"

#include "mongo/unittest/unittest.h"

namespace mongo {

TEST(BitsTest_CountZeros, Constants) {
    ASSERT_EQUALS(countLeadingZeros64(0ull), 64);
    ASSERT_EQUALS(countTrailingZeros64(0ull), 64);

    ASSERT_EQUALS(countLeadingZeros64(0x1234ull), 64 - 13);
    ASSERT_EQUALS(countTrailingZeros64(0x1234ull), 2);

    ASSERT_EQUALS(countLeadingZeros64(0x1234ull << 32), 32 - 13);
    ASSERT_EQUALS(countTrailingZeros64(0x1234ull << 32), 2 + 32);

    ASSERT_EQUALS(countLeadingZeros64((0x1234ull << 32) | 0x1234ull), 32 - 13);
    ASSERT_EQUALS(countTrailingZeros64((0x1234ull << 32) | 0x1234ull), 2);
}

TEST(BitsTest_CountZeros, EachBit) {
    for (int i = 0; i < 64; i++) {
        unsigned long long x = 1ULL << i;
        ASSERT_EQUALS(countLeadingZeros64(x), 64 - 1 - i);
        ASSERT_EQUALS(countTrailingZeros64(x), i);
    }
}
}  // namespace mongo
