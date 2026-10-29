// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/varint.h"

#include "mongo/base/data_builder.h"
#include "mongo/base/data_view.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"

namespace mongo {

// Test integer packing and unpacking
void TestInt(std::uint64_t i) {
    char buf[11];

    DataView dvWrite(&buf[0]);

    dvWrite.write(i);

    ConstDataView cdvRead(&buf[0]);

    std::uint64_t d = cdvRead.read<std::uint64_t>();

    ASSERT_EQUALS(i, d);
}

// Test various integer combinations compress and uncompress correctly
TEST(VarIntTest, TestIntCompression) {
    // Check numbers with leading 1
    for (int i = 0; i < 63; i++) {
        TestInt(i);
        TestInt(i - 1);
    }

    // Check numbers composed of repeating hex numbers
    for (int i = 0; i < 15; i++) {
        std::uint64_t v = 0;
        for (int j = 0; j < 15; j++) {
            v = v << 4 | i;
            TestInt(v);
        }
    }
}

// Test data builder can write a lot of zeros
TEST(VarIntTest, TestDataBuilder) {
    DataBuilder db(1);

    // DataBuilder grows by 2x, and we reserve 10 bytes
    // lcm(2**x, 10) == 16
    for (int i = 0; i < 16; i++) {
        auto s1 = db.writeAndAdvance(VarInt(0));
        ASSERT_OK(s1);
    };
}

}  // namespace mongo
