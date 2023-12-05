/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/bson/util/bsoncolumn.h"

#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace bsoncolumn {
namespace {

class BSONColumnBlockBasedTest : public unittest::Test {};

/**
 * Helper template to extract a value from a BSONElement.
 */
template <typename T>
void extractValueTo(T& val, BSONElement elem);

/**
 * Helper template to assert equality on the different kinds of values stored in BSONelement.
 */
template <typename T>
void assertEquals(const T& lhs, const T& rhs);

/**
 * Asserts that we can create a BSONElement from the given value.
 */
template <typename T>
void assertRoundtrip(ElementStorage& allocator, T value) {
    std::vector<BSONElement> vec;
    Collector<BSONElementMaterializer, decltype(vec)> collector{vec, allocator};
    collector.append(value);

    // Show that we can materialize the value from a primitive value
    BSONElement elem = vec.back();
    T got;
    extractValueTo(got, elem);
    assertEquals(value, got);

    // Show that we can materialize the value from a BSONElement
    collector.append<T>(elem);
    auto elem2 = vec.back();
    T got2;
    extractValueTo(got2, elem2);
    assertEquals(value, got2);
}

TEST_F(BSONColumnBlockBasedTest, BSONMaterializer) {
    ElementStorage allocator{};

    auto date = Date_t::fromMillisSinceEpoch(1701718344564);
    uint8_t binData[] = {100, 101, 102, 103, 104};

    assertRoundtrip(allocator, true);
    assertRoundtrip(allocator, false);
    assertRoundtrip(allocator, (int32_t)100);
    assertRoundtrip(allocator, (int64_t)1000);
    assertRoundtrip(allocator, Decimal128{128.25});
    assertRoundtrip(allocator, (double)32.125);
    assertRoundtrip(allocator, Timestamp{date});
    assertRoundtrip(allocator, date);
    assertRoundtrip(allocator, OID::gen());
    assertRoundtrip(allocator, StringData{"foo/bar"});
    assertRoundtrip(allocator, BSONBinData{binData, sizeof(binData), BinDataGeneral});
    assertRoundtrip(allocator, BSONCode{StringData{"x = 0"}});
}

TEST_F(BSONColumnBlockBasedTest, BSONMaterializerMissing) {
    ElementStorage allocator{};
    std::vector<BSONElement> vec;
    Collector<BSONElementMaterializer, decltype(vec)> collector{vec, allocator};
    collector.appendMissing();
    auto missing = vec.back();
    ASSERT(missing.eoo());
}

template <>
void extractValueTo<int64_t>(int64_t& val, BSONElement elem) {
    // BSONColumn uses int64_t to represent NumberLong, but BSONElement
    // uses "long long".
    long long v;
    elem.Val(v);
    val = v;
}

template <>
void extractValueTo<int32_t>(int32_t& val, BSONElement elem) {
    // BSONColumn uses int32_t to represent NumberLong, but BSONElement
    // uses "int".
    int v;
    elem.Val(v);
    val = v;
}

template <>
void extractValueTo<StringData>(StringData& val, BSONElement elem) {
    val = elem.valueStringDataSafe();
}

template <>
void extractValueTo<BSONBinData>(BSONBinData& val, BSONElement elem) {
    int len;
    const char* bytes = elem.binDataClean(len);
    val = BSONBinData{bytes, len, elem.binDataType()};
}

template <>
void extractValueTo<Timestamp>(Timestamp& val, BSONElement elem) {
    val = elem.timestamp();
}

template <>
void extractValueTo<BSONCode>(BSONCode& val, BSONElement elem) {
    auto sd = elem.valueStringData();
    val = BSONCode{sd};
}

template <typename T>
void extractValueTo(T& val, BSONElement elem) {
    elem.Val(val);
}

template <>
void assertEquals<Decimal128>(const Decimal128& lhs, const Decimal128& rhs) {
    ASSERT_EQ(lhs.toString(), rhs.toString());
}

template <>
void assertEquals<BSONBinData>(const BSONBinData& lhs, const BSONBinData& rhs) {
    ASSERT_EQ(lhs.type, rhs.type);
    ASSERT_EQ(lhs.length, rhs.length);
    auto lhsData = (const uint8_t*)lhs.data;
    auto rhsData = (const uint8_t*)rhs.data;
    for (int i = 0; i < lhs.length; ++i) {
        ASSERT_EQ(lhsData[i], rhsData[i]);
    }
}

template <>
void assertEquals<BSONCode>(const BSONCode& lhs, const BSONCode& rhs) {
    ASSERT_EQ(lhs.code, rhs.code);
}

template <typename T>
void assertEquals(const T& lhs, const T& rhs) {
    ASSERT_EQ(lhs, rhs);
}

}  // namespace
}  // namespace bsoncolumn
}  // namespace mongo
