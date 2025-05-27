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

#include "mongo/base/data_range_cursor.h"

#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <string>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {
// ConstDataRange::operator==() requires that the pointers
// refer to the same memory addresses.
// So just promote to a string that we can do direct comparisons on.
std::string toString(ConstDataRange cdr) {
    return std::string(cdr.data(), cdr.length());
}

// The ASSERT macro can't handle template specialization,
// so work out the value external to the macro call.
template <typename T>
bool isConstDataRange(const T& val) {
    return std::is_same_v<T, ConstDataRange>;
}

template <typename T>
bool isDataRange(const T& val) {
    return std::is_same_v<T, DataRange>;
}
}  // namespace

TEST(DataRangeCursor, ConstDataRangeCursor) {
    char buf[14];

    DataView(buf).write<uint16_t>(1);
    DataView(buf).write<LittleEndian<uint32_t>>(2, sizeof(uint16_t));
    DataView(buf).write<BigEndian<uint64_t>>(3, sizeof(uint16_t) + sizeof(uint32_t));

    ConstDataRangeCursor cdrc(buf, buf + sizeof(buf));
    ConstDataRangeCursor backup(cdrc);

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>());
    ASSERT_EQUALS(static_cast<uint32_t>(2), cdrc.readAndAdvance<LittleEndian<uint32_t>>());
    ASSERT_EQUALS(static_cast<uint64_t>(3), cdrc.readAndAdvance<BigEndian<uint64_t>>());
    ASSERT_NOT_OK(cdrc.readAndAdvanceNoThrow<char>());

    // test skip()
    cdrc = backup;
    ASSERT_OK(cdrc.skipNoThrow<uint32_t>());
    ASSERT_OK(cdrc.advanceNoThrow(10));
    ASSERT_NOT_OK(cdrc.readAndAdvanceNoThrow<char>());
}

TEST(DataRangeCursor, ConstDataRangeCursorType) {
    char buf[] = "foo";

    ConstDataRangeCursor cdrc(buf, buf + sizeof(buf));

    ConstDataRangeCursor out(nullptr, nullptr);

    ASSERT_OK(cdrc.readIntoNoThrow(&out));
    ASSERT_EQUALS(buf, out.data());
}

TEST(DataRangeCursor, DataRangeCursor) {
    char buf[100] = {0};

    DataRangeCursor dc(buf, buf + 14);

    ASSERT_OK(dc.writeAndAdvanceNoThrow<uint16_t>(1));
    ASSERT_OK(dc.writeAndAdvanceNoThrow<LittleEndian<uint32_t>>(2));
    ASSERT_OK(dc.writeAndAdvanceNoThrow<BigEndian<uint64_t>>(3));
    ASSERT_NOT_OK(dc.writeAndAdvanceNoThrow<char>(1));

    ConstDataRangeCursor cdrc(buf, buf + sizeof(buf));

    ASSERT_EQUALS(static_cast<uint16_t>(1), cdrc.readAndAdvance<uint16_t>());
    ASSERT_EQUALS(static_cast<uint32_t>(2), cdrc.readAndAdvance<LittleEndian<uint32_t>>());
    ASSERT_EQUALS(static_cast<uint64_t>(3), cdrc.readAndAdvance<BigEndian<uint64_t>>());
    ASSERT_EQUALS(static_cast<char>(0), cdrc.readAndAdvance<char>());
}

TEST(DataRangeCursor, DataRangeCursorType) {
    char buf[] = "foo";
    char buf2[] = "barZ";

    DataRangeCursor drc(buf, buf + sizeof(buf) + -1);

    DataRangeCursor out(nullptr, nullptr);

    ASSERT_OK(drc.readIntoNoThrow(&out));
    ASSERT_EQUALS(buf, out.data());

    drc = DataRangeCursor(buf2, buf2 + sizeof(buf2) + -1);
    ASSERT_OK(drc.writeNoThrow(out));

    ASSERT_EQUALS(std::string("fooZ"), buf2);
}

TEST(DataRangeCursor, sliceAndAdvance) {
    std::string buffer = "Hello World";
    ConstDataRangeCursor bufferCDRC(buffer.c_str(), buffer.size());

    // Split by position in range [0..length)
    auto helloCDR = bufferCDRC.sliceAndAdvance(5);
    ASSERT_EQ(toString(helloCDR), "Hello");
    ASSERT_EQ(toString(bufferCDRC), " World");

    // Split by pointer
    auto spaceCDR = bufferCDRC.sliceAndAdvance(bufferCDRC.data() + 1);
    ASSERT_EQ(toString(spaceCDR), " ");
    ASSERT_EQ(toString(bufferCDRC), "World");

    // Get DataRange from a DataRangeCursor if original was non-const.
    DataRangeCursor mutableDRC(const_cast<char*>(buffer.c_str()), buffer.size());
    auto mutableByLen = mutableDRC.sliceAndAdvance(1);
    ASSERT_TRUE(isDataRange(mutableByLen));

    // Get ConstDataRange from a DataRangeCursor if original was const.
    const DataRange nonmutableDRC = mutableDRC;
    auto nonmutableCDR = nonmutableDRC.slice(2);
    ASSERT_TRUE(isConstDataRange(nonmutableCDR));
}

TEST(DataRange, sliceAndAdvanceThrow) {
    std::string buffer("Hello World");
    ConstDataRangeCursor bufferCDRC(buffer.c_str(), buffer.size());

    // Split point is out of range.
    ASSERT_THROWS(bufferCDRC.sliceAndAdvance(bufferCDRC.length() + 1), AssertionException);
    ASSERT_THROWS(bufferCDRC.sliceAndAdvance(bufferCDRC.data() + bufferCDRC.length() + 1),
                  AssertionException);
    ASSERT_THROWS(bufferCDRC.sliceAndAdvance(bufferCDRC.data() - 1), AssertionException);
}

}  // namespace mongo
