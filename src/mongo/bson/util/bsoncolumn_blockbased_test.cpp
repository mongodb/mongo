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
#include "mongo/bson/util/bsoncolumnbuilder.h"

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
void assertRoundtrip(T value) {
    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
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
    auto date = Date_t::fromMillisSinceEpoch(1701718344564);
    uint8_t binData[] = {100, 101, 102, 103, 104};

    assertRoundtrip(true);
    assertRoundtrip(false);
    assertRoundtrip((int32_t)100);
    assertRoundtrip((int64_t)1000);
    assertRoundtrip(Decimal128{128.25});
    assertRoundtrip((double)32.125);
    assertRoundtrip(Timestamp{date});
    assertRoundtrip(date);
    assertRoundtrip(OID::gen());
    assertRoundtrip(StringData{"foo/bar"});
    assertRoundtrip(BSONBinData{binData, sizeof(binData), BinDataGeneral});
    assertRoundtrip(BSONCode{StringData{"x = 0"}});
}

TEST_F(BSONColumnBlockBasedTest, BSONMaterializerMissing) {
    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
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

/**
 * A simple path that traverses an object for a set of fields make up a path.
 */
struct TestPath {
    std::vector<const char*> elementsToMaterialize(BSONObj refObj) {
        BSONObj obj = refObj;
        size_t idx = 0;
        for (auto& field : _fields) {
            auto elem = obj[field];
            if (elem.eoo()) {
                return {};
            }
            if (idx == _fields.size() - 1) {
                return {elem.value()};
            }
            if (elem.type() != Object) {
                return {};
            }
            obj = elem.Obj();
            ++idx;
        }

        return {};
    }

    const std::vector<std::string> _fields;
};

TEST_F(BSONColumnBlockBasedTest, DecompressPath) {
    BSONColumnBuilder cb;
    std::vector<BSONObj> objs = {
        BSON("a" << 10 << "b" << BSON("c" << int64_t(20))),
        BSON("a" << 11 << "b" << BSON("c" << int64_t(21))),
        BSON("a" << 12 << "b" << BSON("c" << int64_t(22))),
        BSON("a" << 13 << "b" << BSON("c" << int64_t(23))),
    };
    for (auto&& o : objs) {
        cb.append(o);
    }

    BSONColumnBlockBased col{cb.finalize()};

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{
        {TestPath{{"a"}}, {}},
        {TestPath{{"b", "c"}}, {}},
    };

    // Decompress only the values of "a" to the vector.
    col.decompress<BSONElementMaterializer>(allocator, std::span(paths));

    ASSERT_EQ(paths[0].second.size(), 4);
    ASSERT_EQ(paths[0].second[0].Int(), 10);
    ASSERT_EQ(paths[0].second[1].Int(), 11);
    ASSERT_EQ(paths[0].second[2].Int(), 12);
    ASSERT_EQ(paths[0].second[3].Int(), 13);

    ASSERT_EQ(paths[1].second.size(), 4);
    ASSERT_EQ(paths[1].second[0].Long(), 20);
    ASSERT_EQ(paths[1].second[1].Long(), 21);
    ASSERT_EQ(paths[1].second[2].Long(), 22);
    ASSERT_EQ(paths[1].second[3].Long(), 23);
}

}  // namespace
}  // namespace bsoncolumn
}  // namespace mongo
