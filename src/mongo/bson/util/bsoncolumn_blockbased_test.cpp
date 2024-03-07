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

#include "mongo/bson/json.h"
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/bson/util/bsoncolumnbuilder.h"

#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace bsoncolumn {
namespace {

struct BSONColumnBlockBasedTest : public unittest::Test {
    BSONColumnBlockBased bsonColumnFromObjs(std::vector<BSONObj> objs) {
        for (auto& o : objs) {
            _columnBuilder.append(o);
        }

        return BSONColumnBlockBased{_columnBuilder.finalize()};
    }

private:
    BSONColumnBuilder _columnBuilder;
};

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

TEST_F(BSONColumnBlockBasedTest, BSONMaterializerBSONElement) {
    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<BSONElement> vec;
    Collector<BSONElementMaterializer, decltype(vec)> collector{vec, allocator};

    // Not all types are compressed in BSONColumn. Values of these types are just stored as
    // uncompressed BSONElements. "Code with scope" is an example of this.
    BSONCodeWScope codeWScope{"print(`${x}`)", BSON("x" << 10)};
    auto obj = BSON("" << codeWScope);
    auto bsonElem = obj.firstElement();

    // Test with copying.
    collector.append<BSONElement>(bsonElem);
    auto elem = vec.back();
    ASSERT(bsonElem.binaryEqual(elem));
    // Since we are making a copy and storing it in the ElementStorage, the address of the data
    // should not be the same.
    ASSERT_NOT_EQUALS(elem.value(), bsonElem.value());

    // Test without copying.
    collector.appendPreallocated(bsonElem);
    elem = vec.back();
    ASSERT(bsonElem.binaryEqual(elem));
    // Assert that we did not make a copy, because the address of the data is the same.
    ASSERT_EQ(elem.value(), bsonElem.value());
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
 * A simple path that traverses an object for a set of fields that make up a path.
 */
struct TestPath {
    std::vector<const char*> elementsToMaterialize(BSONObj refObj) {
        if (_fields.empty()) {
            return {refObj.objdata()};
        }

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

TEST_F(BSONColumnBlockBasedTest, DecompressScalars) {
    auto col = bsonColumnFromObjs({
        BSON("a" << 10 << "b" << BSON("c" << int64_t(20))),
        BSON("a" << 11 << "b" << BSON("c" << int64_t(21))),
        BSON("a" << 12 << "b" << BSON("c" << int64_t(22))),
        BSON("a" << 13 << "b" << BSON("c" << int64_t(23))),
    });

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{
        {TestPath{{"a"}}, {}},
        {TestPath{{"b", "c"}}, {}},
    };

    // Decompress both scalar fields to vectors. Both paths can use the fast implementation to
    // decompress the data.
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

TEST_F(BSONColumnBlockBasedTest, DecompressSomeScalars) {
    // Create a BSONColumn that has different deltas in the object fields. This ensures that the
    // number of deltas per simple8b block will be different for each field to encourage
    // interleaved-ness of the data.
    const int kN = 5000;
    std::vector<BSONObj> objs;
    for (int i = 0; i < kN; ++i) {
        objs.push_back(BSON("a" << i << "b" << (i * 1000) << "c" << (i * 100000)));
    }
    auto col = bsonColumnFromObjs(std::move(objs));

    // Select a and c, but omit b to show that we can skip over parts of the data as needed.
    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{
        {TestPath{{"a"}}, {}},
        {TestPath{{"c"}}, {}},
    };

    // Decompress both scalar fields to vectors. The fast path will be used.
    col.decompress<BSONElementMaterializer>(allocator, std::span(paths));

    ASSERT_EQ(paths[0].second.size(), kN);
    for (size_t i = 0; i < kN; ++i) {
        ASSERT_EQ(paths[0].second[i].Int(), i);
    }

    ASSERT_EQ(paths[1].second.size(), kN);
    for (size_t i = 0; i < kN; ++i) {
        ASSERT_EQ(paths[1].second[i].Int(), i * 100000);
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressObjects) {
    auto col = bsonColumnFromObjs({
        fromjson("{a: 10}"),
        fromjson("{a: 11}"),
        fromjson("{a: 12}"),
        fromjson("{a: 13}"),
    });

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{{TestPath{}, {}}};

    // Decompress complete objects to the vector. The fast path won't be used here, since we are
    // decompressing objects.
    col.decompress<BSONElementMaterializer>(allocator, std::span(paths));

    ASSERT_EQ(paths[0].second.size(), 4);
    ASSERT_EQ(paths[0].second[0].type(), Object);
    ASSERT_BSONOBJ_EQ(paths[0].second[0].Obj(), fromjson("{a: 10}"));
    ASSERT_BSONOBJ_EQ(paths[0].second[1].Obj(), fromjson("{a: 11}"));
    ASSERT_BSONOBJ_EQ(paths[0].second[2].Obj(), fromjson("{a: 12}"));
    ASSERT_BSONOBJ_EQ(paths[0].second[3].Obj(), fromjson("{a: 13}"));
}

TEST_F(BSONColumnBlockBasedTest, DecompressNestedObjects) {
    auto col = bsonColumnFromObjs({
        fromjson("{a: 10, b: {c: 30}}"),
        fromjson("{a: 11, b: {c: 31}}"),
        fromjson("{a: 12, b: {c: 32}}"),
        fromjson("{a: 13, b: {c: 33}}"),
    });

    {
        boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{{TestPath{}, {}}};

        // Decompress complete objects to the vector.
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));

        ASSERT_EQ(paths[0].second.size(), 4);
        ASSERT_EQ(paths[0].second[0].type(), Object);
        ASSERT_BSONOBJ_EQ(paths[0].second[0].Obj(), fromjson("{a: 10, b: {c: 30}}"));
        ASSERT_BSONOBJ_EQ(paths[0].second[1].Obj(), fromjson("{a: 11, b: {c: 31}}"));
        ASSERT_BSONOBJ_EQ(paths[0].second[2].Obj(), fromjson("{a: 12, b: {c: 32}}"));
        ASSERT_BSONOBJ_EQ(paths[0].second[3].Obj(), fromjson("{a: 13, b: {c: 33}}"));
    }
    {
        boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{{TestPath{{"b"}}, {}}};

        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));

        ASSERT_EQ(paths[0].second.size(), 4);
        ASSERT_EQ(paths[0].second[0].type(), Object);
        ASSERT_BSONOBJ_EQ(paths[0].second[0].Obj(), fromjson("{c: 30}"));
        ASSERT_BSONOBJ_EQ(paths[0].second[1].Obj(), fromjson("{c: 31}"));
        ASSERT_BSONOBJ_EQ(paths[0].second[2].Obj(), fromjson("{c: 32}"));
        ASSERT_BSONOBJ_EQ(paths[0].second[3].Obj(), fromjson("{c: 33}"));
    }
    {
        boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{{TestPath{{"a"}}, {}},
                                                                         {TestPath{{"b"}}, {}}};

        // We will use the fast implementation to decompress "a" since it is scalar. We will fall
        // back to the general implementation for "b".
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));

        ASSERT_EQ(paths[0].second.size(), 4);
        ASSERT_EQ(paths[0].second[0].type(), NumberInt);
        ASSERT_EQ(paths[0].second[0].Int(), 10);
        ASSERT_EQ(paths[0].second[1].Int(), 11);
        ASSERT_EQ(paths[0].second[2].Int(), 12);
        ASSERT_EQ(paths[0].second[3].Int(), 13);

        ASSERT_EQ(paths[1].second.size(), 4);
        ASSERT_EQ(paths[1].second[0].type(), Object);
        ASSERT_BSONOBJ_EQ(paths[1].second[0].Obj(), fromjson("{c: 30}"));
        ASSERT_BSONOBJ_EQ(paths[1].second[1].Obj(), fromjson("{c: 31}"));
        ASSERT_BSONOBJ_EQ(paths[1].second[2].Obj(), fromjson("{c: 32}"));
        ASSERT_BSONOBJ_EQ(paths[1].second[3].Obj(), fromjson("{c: 33}"));
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressSiblingObjects) {
    auto col = bsonColumnFromObjs({
        fromjson("{a: {aa: 100}, b: {c: 30}}"),
        fromjson("{a: {aa: 101}, b: {c: 31}}"),
        fromjson("{a: {aa: 102}, b: {c: 32}}"),
        fromjson("{a: {aa: 103}, b: {c: 33}}"),
    });

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{{TestPath{{"a"}}, {}},
                                                                     {TestPath{{"b"}}, {}}};

    col.decompress<BSONElementMaterializer>(allocator, std::span(paths));

    ASSERT_EQ(paths[0].second.size(), 4);
    ASSERT_EQ(paths[0].second[0].type(), Object);
    ASSERT_BSONOBJ_EQ(paths[0].second[0].Obj(), fromjson("{aa: 100}"));
    ASSERT_BSONOBJ_EQ(paths[0].second[1].Obj(), fromjson("{aa: 101}"));
    ASSERT_BSONOBJ_EQ(paths[0].second[2].Obj(), fromjson("{aa: 102}"));
    ASSERT_BSONOBJ_EQ(paths[0].second[3].Obj(), fromjson("{aa: 103}"));

    ASSERT_EQ(paths[1].second.size(), 4);
    ASSERT_EQ(paths[1].second[0].type(), Object);
    ASSERT_BSONOBJ_EQ(paths[1].second[0].Obj(), fromjson("{c: 30}"));
    ASSERT_BSONOBJ_EQ(paths[1].second[1].Obj(), fromjson("{c: 31}"));
    ASSERT_BSONOBJ_EQ(paths[1].second[2].Obj(), fromjson("{c: 32}"));
    ASSERT_BSONOBJ_EQ(paths[1].second[3].Obj(), fromjson("{c: 33}"));
}

/**
 * A path that is equivalent to
 *     Get("a") / Traverse / Get("b") / Id
 */
struct TestArrayPath {
    std::vector<const char*> elementsToMaterialize(BSONObj refObj) {
        auto a = refObj["a"];
        if (a.type() == Array) {
            std::vector<const char*> addrs;
            for (auto&& elem : a.Array()) {
                if (elem.type() == Object) {
                    auto b = elem.Obj()["b"];
                    if (!b.eoo()) {
                        addrs.push_back(b.value());
                    }
                }
            }

            return addrs;
        } else if (a.type() == Object) {
            auto b = a.Obj()["b"];
            if (!b.eoo()) {
                return {b.value()};
            }
        }
        return {};
    }
};

template <typename T>
auto decompressArraysAndFullColumns(const std::vector<T> values,
                                    boost::intrusive_ptr<ElementStorage>& allocator) {
    std::vector<BSONObj> objs;
    for (std::size_t i = 0; i < values.size(); i += 2) {
        auto bsonObj =
            BSON("a" << BSON_ARRAY(BSON("b" << values[i]) << BSON("b" << values[i + 1])));
        objs.emplace_back(bsonObj);
    }

    BSONColumnBuilder cb;
    for (auto& o : objs) {
        cb.append(o);
    }

    auto col = BSONColumnBlockBased{cb.finalize()};

    auto mockRefObj = BSON("a" << BSON_ARRAY(BSON("b" << values[0]) << BSON("b" << values[1])));

    // Test that we can decompress the whole column.
    TestPath testPath{};
    ASSERT_EQ(testPath.elementsToMaterialize(mockRefObj).size(), 1);
    std::vector<std::pair<TestPath, std::vector<BSONElement>>> testPaths{{testPath, {}}};

    // This is decompressing the whole column, in which there are scalars within objects.
    col.decompress<BSONElementMaterializer>(allocator, std::span(testPaths));

    ASSERT_EQ(testPaths[0].second.size(), objs.size());
    ASSERT_EQ(testPaths[0].second[0].type(), Object);

    for (size_t i = 0; i < testPaths[0].second.size(); ++i) {
        ASSERT_BSONOBJ_EQ(testPaths[0].second[i].Obj(), objs[i]);
    }

    // Test that we can decompress multiple elements within arrays.
    // Create a path that will get the "b" fields of both array elements.
    TestArrayPath arrayPath;
    ASSERT_EQ(arrayPath.elementsToMaterialize(mockRefObj).size(), 2);

    std::vector<std::pair<TestArrayPath, std::vector<BSONElement>>> arrayPaths{{arrayPath, {}}};

    // This is decompressing scalars, but since a single path is accessing two fields that need to
    // be interleaved in the output, we still need to use the slow path.
    col.decompress<BSONElementMaterializer>(allocator, std::span(arrayPaths));

    ASSERT_EQ(arrayPaths[0].second.size(), values.size());

    return arrayPaths[0].second;
}

TEST_F(BSONColumnBlockBasedTest, DecompressArraysWithIntegers) {
    const std::vector<int> integers = {0, 10, 20, 30, 40, 50, 60, 70};

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<BSONElement> result = decompressArraysAndFullColumns(integers, allocator);

    ASSERT_EQ(result[0].type(), NumberInt);
    for (size_t i = 0; i < result.size(); ++i) {
        ASSERT_EQ(result[i].Int(), integers[i]);
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressWithDecimals) {
    const std::vector<Decimal128> decimals = {Decimal128(1023.75),
                                              Decimal128(1024.75),
                                              Decimal128(1025.75),
                                              Decimal128(1026.75),
                                              Decimal128(1027.75),
                                              Decimal128(1028.75)};

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    const std::vector<BSONElement> result = decompressArraysAndFullColumns(decimals, allocator);

    ASSERT_EQ(result[0].type(), NumberDecimal);
    for (size_t i = 0; i < result.size(); ++i) {
        ASSERT_EQ(result[i].Decimal(), decimals[i]);
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressWithOID) {
    std::vector<OID> oids = {OID("112233445566778899AABBCC"),
                             OID("112233445566778899AABBCB"),
                             OID("112233445566778899AABBAA"),
                             OID("112233445566778899AABBAB"),
                             OID("112233445566778899AABBAC"),
                             OID("112233445566778899AABBBC")};

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    const std::vector<BSONElement> result = decompressArraysAndFullColumns(oids, allocator);

    ASSERT_EQ(result[0].type(), jstOID);
    for (size_t i = 0; i < result.size(); ++i) {
        ASSERT_EQ(result[i].OID(), oids[i]);
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressWithStrings) {
    std::vector<StringData> strs = {StringData("hello_world0"),
                                    StringData("hello_world1"),
                                    StringData("hello_world2"),
                                    StringData("hello_world3"),
                                    StringData("hello_world4"),
                                    StringData("hello_world5")};

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    const std::vector<BSONElement> result = decompressArraysAndFullColumns(strs, allocator);

    ASSERT_EQ(result[0].type(), String);
    for (size_t i = 0; i < result.size(); ++i) {
        ASSERT_EQ(result[i].String(), strs[i]);
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressWithDate) {
    std::vector<Date_t> dates = {Date_t::fromMillisSinceEpoch(1702334800770),
                                 Date_t::fromMillisSinceEpoch(1702334800771),
                                 Date_t::fromMillisSinceEpoch(1702334800772),
                                 Date_t::fromMillisSinceEpoch(1702334800773),
                                 Date_t::fromMillisSinceEpoch(1702334800774),
                                 Date_t::fromMillisSinceEpoch(1702334800775)};

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    const std::vector<BSONElement> result = decompressArraysAndFullColumns(dates, allocator);

    ASSERT_EQ(result[0].type(), Date);

    for (size_t i = 0; i < result.size(); ++i) {
        ASSERT_EQ(result[i].Date(), dates[i]);
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressWithTimestamp) {
    // Test for Timestamp
    std::vector<Timestamp> timestamps = {Timestamp(Date_t::fromMillisSinceEpoch(1702334800770)),
                                         Timestamp(Date_t::fromMillisSinceEpoch(1702334800771)),
                                         Timestamp(Date_t::fromMillisSinceEpoch(1702334800772)),
                                         Timestamp(Date_t::fromMillisSinceEpoch(1702334800773)),
                                         Timestamp(Date_t::fromMillisSinceEpoch(1702334800774)),
                                         Timestamp(Date_t::fromMillisSinceEpoch(1702334800775))};

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    const std::vector<BSONElement> result = decompressArraysAndFullColumns(timestamps, allocator);

    ASSERT_EQ(result[0].type(), bsonTimestamp);
    for (size_t i = 0; i < result.size(); ++i) {
        ASSERT_EQ(result[i].timestamp(), timestamps[i]);
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressWithCode) {
    std::vector<BSONCode> codes = {BSONCode(StringData{"x = 0"}),
                                   BSONCode(StringData{"x = 1"}),
                                   BSONCode(StringData{"x = 2"}),
                                   BSONCode(StringData{"x = 3"}),
                                   BSONCode(StringData{"x = 4"}),
                                   BSONCode(StringData{"x = 5"})};

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    const std::vector<BSONElement> result = decompressArraysAndFullColumns(codes, allocator);

    for (size_t i = 0; i < result.size(); ++i) {
        BSONCode res;
        extractValueTo<BSONCode>(res, result[i]);
        assertEquals<BSONCode>(res, codes[i]);
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressWithBinData) {
    uint8_t binData0[] = {100, 101, 102, 103};
    uint8_t binData1[] = {101, 102, 103, 104};
    uint8_t binData2[] = {102, 103, 104, 105};
    uint8_t binData3[] = {103, 104, 105, 106};
    uint8_t binData4[] = {104, 105, 106, 107};
    uint8_t binData5[] = {105, 106, 107, 108};

    std::vector<BSONBinData> bsonBinDatas = {
        BSONBinData(binData0, sizeof(binData0), BinDataGeneral),
        BSONBinData(binData1, sizeof(binData1), BinDataGeneral),
        BSONBinData(binData2, sizeof(binData2), BinDataGeneral),
        BSONBinData(binData3, sizeof(binData3), BinDataGeneral),
        BSONBinData(binData4, sizeof(binData4), BinDataGeneral),
        BSONBinData(binData5, sizeof(binData5), BinDataGeneral)};

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    const std::vector<BSONElement> result = decompressArraysAndFullColumns(bsonBinDatas, allocator);

    for (size_t i = 0; i < result.size(); ++i) {
        BSONBinData res;
        extractValueTo<BSONBinData>(res, result[i]);
        assertEquals<BSONBinData>(res, bsonBinDatas[i]);
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressMissingArrays) {
    auto col = bsonColumnFromObjs({
        fromjson("{a: [{b:  0}]}"),
        fromjson("{a: [{b: 20}, {b: 30}]}"),
        fromjson("{a: [{b: 40}]}"),
        fromjson("{a: [{b: 60}, {b: 70}]}"),
    });

    // Create a path that will get the "b" fields of both array elements.
    TestArrayPath path;
    auto mockRefObj = fromjson("{a: [{b: 0}, {b: 10}]}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 2);

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<TestArrayPath, std::vector<BSONElement>>> paths{{path, {}}};

    // This is decompressing scalars, but since a single path is accessing two fields that need to
    // be interleaved in the output, we still need to use the slow path.
    col.decompress<BSONElementMaterializer>(allocator, std::span(paths));

    // TODO(SERVER-87339): This is not the right answer for Traverse paths ,which is what this test
    // is simulating. We should be getting only 6 elements back, and no missing/EOO elements.
    ASSERT_EQ(paths[0].second.size(), 8);
    ASSERT_EQ(paths[0].second[0].type(), NumberInt);
    for (int i = 0; i < 8; ++i) {
        if (i == 1 || i == 5) {
            ASSERT(paths[0].second[i].eoo());
        } else {
            ASSERT_EQ(paths[0].second[i].Int(), i * 10);
        }
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressMissingScalar) {
    // Create a BSONColumn where even elements have a "b" field, and odd elements have "c." This
    // will use a single section of interleaved mode, with three scalar fields. "b" and "c" will
    // have missing values in the delta blocks.
    const int nObjs = 10;
    std::vector<BSONObj> objs;
    for (int i = 0; i < nObjs; ++i) {
        auto fld2 = i % 2 ? "c" : "b";
        objs.push_back(BSON("a" << (i * 10) << fld2 << (i * 100)));
    }
    auto col = bsonColumnFromObjs(std::move(objs));

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();

    {
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{{TestPath{{}}, {}}};
        // This takes the general path since we are getting whole documents.
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));
        ASSERT_EQ(paths[0].second.size(), nObjs);
        for (int i = 0; i < nObjs; ++i) {
            auto fld2 = i % 2 ? "c" : "b";
            ASSERT_BSONOBJ_EQ(paths[0].second[i].Obj(), BSON("a" << (i * 10) << fld2 << (i * 100)));
        }
    }
    {
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{{TestPath{{"a"}}, {}},
                                                                         {TestPath{{"b"}}, {}}};
        // This takes the fast path because each path maps to a single scalar stream.
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));
        ASSERT_EQ(paths[0].second.size(), nObjs);
        for (int i = 0; i < nObjs; ++i) {
            ASSERT_EQ(paths[0].second[i].Int(), i * 10);
        }

        ASSERT_EQ(paths[1].second.size(), nObjs);
        for (int i = 0; i < nObjs; ++i) {
            if (i % 2) {
                ASSERT(paths[1].second[i].eoo());
            } else {
                ASSERT_EQ(paths[1].second[i].Int(), i * 100);
            }
        }
    }
    {
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{{TestPath{{"c"}}, {}}};
        // This takes the fast path because each path maps to a single scalar stream.
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));
        ASSERT_EQ(paths[0].second.size(), nObjs);
        for (int i = 0; i < nObjs; ++i) {
            if (i % 2) {
                ASSERT_EQ(paths[0].second[i].Int(), i * 100);
            } else {
                ASSERT(paths[0].second[i].eoo());
            }
        }
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressMissingObject) {
    const int nObjs = 10;
    std::vector<BSONObj> objs;
    for (int i = 0; i < nObjs; ++i) {
        if (i % 2) {
            objs.push_back(BSON("a" << (i * 10)));
        } else {
            objs.push_back(BSON("a" << (i * 10) << "b" << BSON("bb" << (i * 100))));
        }
    }
    auto col = bsonColumnFromObjs(std::move(objs));

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();

    {
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{{TestPath{{}}, {}}};
        // this takes the general path because we are materializing objects.
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));
        ASSERT_EQ(paths[0].second.size(), nObjs);
        for (int i = 0; i < nObjs; ++i) {
            if (i % 2) {
                ASSERT_BSONOBJ_EQ(paths[0].second[i].Obj(), BSON("a" << (i * 10)));
            } else {
                ASSERT_BSONOBJ_EQ(paths[0].second[i].Obj(),
                                  BSON("a" << (i * 10) << "b" << BSON("bb" << (i * 100))));
            }
        }
    }
    {
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{{TestPath{{"b"}}, {}}};
        // this takes the general path because we are materializing objects.
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));
        ASSERT_EQ(paths[0].second.size(), nObjs);
        for (int i = 0; i < nObjs; ++i) {
            if (i % 2) {
                ASSERT(paths[0].second[i].eoo());
            } else {
                ASSERT_BSONOBJ_EQ(paths[0].second[i].Obj(), BSON("bb" << (i * 100)));
            }
        }
    }
    {
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{
            {TestPath{{"b", "bb"}}, {}}};
        // this takes the fast path because we are materializing a scalar field.
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));
        ASSERT_EQ(paths[0].second.size(), nObjs);
        for (int i = 0; i < nObjs; ++i) {
            if (i % 2) {
                ASSERT(paths[0].second[i].eoo());
            } else {
                ASSERT_EQ(paths[0].second[i].Int(), i * 100);
            }
        }
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressMissingNestedObject) {
    const int nObjs = 10;
    std::vector<BSONObj> objs;
    for (int i = 0; i < nObjs; ++i) {
        if (i % 2) {
            objs.push_back(BSON("a" << (i * 10)));
        } else {
            objs.push_back(BSON("a" << (i * 10) << "b" << BSON("bb" << BSON("bbb" << (i * 100)))));
        }
    }

    auto col = bsonColumnFromObjs(std::move(objs));
    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();

    {
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{
            {TestPath{{"b", "bb"}}, {}},        // general
            {TestPath{{"b", "bb", "bbb"}}, {}}  // fast
        };
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));

        ASSERT_EQ(paths[0].second.size(), nObjs);
        for (int i = 0; i < nObjs; ++i) {
            if (i % 2) {
                ASSERT(paths[0].second[i].eoo());
            } else {
                ASSERT_BSONOBJ_EQ(paths[0].second[i].Obj(), BSON("bbb" << (i * 100)));
            }
        }

        ASSERT_EQ(paths[1].second.size(), nObjs);
        for (int i = 0; i < nObjs; ++i) {
            if (i % 2) {
                ASSERT(paths[1].second[i].eoo());
            } else {
                ASSERT_EQ(paths[1].second[i].Int(), i * 100);
            }
        }
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressWithEmptyInReference) {
    const int nObjs = 10;
    std::vector<BSONObj> objs;
    for (int i = 0; i < nObjs; ++i) {
        std::stringstream ss;
        ss << "{a: " << (i * 10) << ", b: {}}";
        objs.push_back(fromjson(ss.str()));
    }

    auto col = bsonColumnFromObjs(std::move(objs));
    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();

    {
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{
            {TestPath{{"b"}}, {}},       // general
            {TestPath{{"b", "bb"}}, {}}  // fast (missing)
        };
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));

        ASSERT_EQ(paths[0].second.size(), nObjs);
        for (int i = 0; i < nObjs; ++i) {
            ASSERT_BSONOBJ_EQ(paths[0].second[i].Obj(), fromjson("{}"));
        }

        ASSERT_EQ(paths[1].second.size(), nObjs);
        for (int i = 0; i < nObjs; ++i) {
            ASSERT(paths[1].second[i].eoo());
        }
    }
}

TEST_F(BSONColumnBlockBasedTest, DecompressMissingPath) {
    auto col = bsonColumnFromObjs({
        fromjson("{a: {b:  0}}"),
        fromjson("{a: {b:  10}}"),
        fromjson("{a: {b:  20}}"),
        fromjson("{a: {b:  30}}"),
    });

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();

    {
        // The bogus paths don't match anything in the reference object and so should all be
        // materialized as missing, aka a EOO BSONElement.
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{
            {TestPath{{"bogus"}}, {}},       // empty, fast path
            {TestPath{{"a"}}, {}},           // general path
            {TestPath{{"a", "b"}}, {}},      // fast path
            {TestPath{{"also-bogus"}}, {}},  // empty, fast path

        };
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));

        ASSERT_EQ(paths[0].second.size(), 4);
        for (int i = 0; i < 4; ++i) {
            ASSERT(paths[0].second[i].eoo());
        }

        ASSERT_EQ(paths[1].second.size(), 4);
        for (int i = 0; i < 4; ++i) {
            ASSERT_BSONOBJ_EQ(paths[1].second[i].Obj(), BSON("b" << (i * 10)));
        }

        ASSERT_EQ(paths[2].second.size(), 4);
        for (int i = 0; i < 4; ++i) {
            ASSERT_EQ(paths[2].second[i].Int(), i * 10);
        }

        ASSERT_EQ(paths[3].second.size(), 4);
        for (int i = 0; i < 4; ++i) {
            ASSERT(paths[3].second[i].eoo());
        }
    }
    {
        // Make sure that decompressing zero paths doesn't segfault or anything like that.
        std::vector<std::pair<TestPath, std::vector<BSONElement>>> paths{{}};
        col.decompress<BSONElementMaterializer>(allocator, std::span(paths));
    }
}

}  // namespace
}  // namespace bsoncolumn
}  // namespace mongo
