/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/util/varint.h"

#include "mongo/base/data_builder.h"
#include "mongo/base/data_view.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
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
