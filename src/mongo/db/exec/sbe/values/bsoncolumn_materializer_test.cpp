/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/values/bsoncolumn_materializer.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo::sbe::bsoncolumn {
namespace {

using Element = SBEColumnMaterializer::Element;

// Test 'Container' that implements the 'appendPositionInfo' function. This can be removed after the
// new API is integrated into SBE.
class PositionInfoTestContainer {
public:
    void push_back(const Element& e) {
        _container.push_back(e);
    }

    Element back() {
        return _container.back();
    }

    void appendPositionInfo(int32_t n) {
        _positions.push_back(n);
    }

    std::vector<Element> getElems() {
        return _container;
    }

    std::vector<int32_t> getPositions() {
        return _positions;
    }

private:
    std::vector<Element> _container;
    std::vector<int32_t> _positions;
};

class BSONColumnMaterializerTest : public unittest::Test {
public:
    void assertSbeValueEquals(Element actual, Element expected, bool omitStringTypeCheck = false) {
        if (actual.first == value::TypeTags::StringSmall && omitStringTypeCheck) {
            // Generic conversion won't produce StringSmall from BSONElements, but
            // SBEColumnMaterializer will, don't compare the type tag for that case.
            auto strActual = value::print(actual);
            auto strExpected = value::print(expected);
            ASSERT_EQ(strActual, strExpected);
            return;
        }
        auto strActual = value::printTagAndVal(actual);
        auto strExpected = value::printTagAndVal(expected);
        ASSERT_EQ(strActual, strExpected);
    }

    void verifyDecompressionIterative(BSONObj& obj, Element expected) {
        BSONColumnBuilder cb;
        cb.append(obj.firstElement());
        auto binData = cb.finalize();

        mongo::bsoncolumn::BSONColumnBlockBased col{static_cast<const char*>(binData.data),
                                                    (size_t)binData.length};
        boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
        std::vector<Element> container{{}};
        col.decompressIterative<SBEColumnMaterializer>(container, allocator);
        assertSbeValueEquals(container.back(), expected);
    }

    template <typename T>
    void assertMaterializedValue(const T& value, Element expected) {
        boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
        std::vector<Element> vec;
        mongo::bsoncolumn::Collector<SBEColumnMaterializer, decltype(vec)> collector{vec,
                                                                                     allocator};

        // Show we translate to an SBE value by appending a primitive type.
        collector.append(value);
        assertSbeValueEquals(vec.back(), expected);

        // Show we translate to an SBE value by appending a BSONElement.
        BSONObj obj = BSON("" << value);
        BSONElement elem = obj.firstElement();
        collector.append<T>(elem);
        assertSbeValueEquals(vec.back(), expected);

        // Finally, show that the BSONElement -> SBE translation matches what the generic (and
        // presumably slower) conversion does.
        Element converted = bson::convertFrom<true /* view */>(elem);
        assertSbeValueEquals(vec.back(), converted, true);
    }

    template <typename T>
    void verifyDecompressWithDifferentTypes(const std::vector<T> values) {
        std::vector<BSONObj> objs;
        for (std::size_t i = 0; i < values.size(); ++i) {
            auto bsonObj = BSON("a" << BSON("b" << values[i]));
            objs.emplace_back(bsonObj);
        }

        BSONColumnBuilder cb;
        for (auto&& o : objs) {
            cb.append(o);
        }

        mongo::bsoncolumn::BSONColumnBlockBased col{cb.finalize()};

        boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
        std::vector<std::pair<SBEPath, PositionInfoTestContainer>> paths{
            {SBEPath{value::CellBlock::PathRequest(
                 value::CellBlock::PathRequestType::kFilter,
                 {value::CellBlock::Get{"a"}, value::CellBlock::Id{}})},
             {}}};

        // Decompress only the values of "a" to the vector.
        col.decompress<SBEColumnMaterializer>(allocator, std::span(paths));

        std::vector<Element> container = paths[0].second.getElems();
        ASSERT_EQ(container.size(), values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            auto bsonObj = BSON("b" << values[i]);
            auto expectedElement = std::pair(value::TypeTags::bsonObject,
                                             value::bitcastFrom<const char*>(bsonObj.objdata()));
            assertSbeValueEquals(container[i], expectedElement);
        }

        std::vector<int32_t> positions = paths[0].second.getPositions();
        ASSERT_EQ(positions.size(), values.size());
        for (std::size_t i = 0; i < values.size(); ++i) {
            ASSERT_EQ(positions[i], 1);
        }
    }

    void verifyDecompressWithPositions(const std::vector<BSONObj> input,
                                       const SBEPath path,
                                       const std::vector<int32_t> expectedPositions) {
        BSONColumnBuilder cb;
        for (auto&& o : input) {
            cb.append(o);
        }
        mongo::bsoncolumn::BSONColumnBlockBased col{cb.finalize()};
        std::vector<std::pair<SBEPath, PositionInfoTestContainer>> paths{{path, {}}};
        boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();

        col.decompress<SBEColumnMaterializer>(allocator, std::span(paths));

        std::vector<Element> container = paths[0].second.getElems();

        for (size_t i = 0; i < _expectedElements.size(); ++i) {
            assertSbeValueEquals(container[i], _expectedElements[i]);
        }
        std::vector<int32_t> positions = paths[0].second.getPositions();
        ASSERT_EQ(positions, expectedPositions);

        clearExpectedElements();
    }

    void addExpectedObj(BSONObj obj) {
        addExpectedElement(value::copyValue(value::TypeTags::bsonObject,
                                            value::bitcastFrom<const char*>(obj.objdata())));
    }

    void addExpectedArray(BSONArray array) {
        addExpectedElement(value::copyValue(value::TypeTags::bsonArray,
                                            value::bitcastFrom<const char*>(array.objdata())));
    }

    void addExpectedElement(Element e) {
        _expectedElements.push_back(e);
    }

    void setExpectedElements(std::vector<Element> elems) {
        _expectedElements = elems;
    }

    void clearExpectedElements() {
        for (auto&& elem : _expectedElements) {
            value::releaseValue(elem.first, elem.second);
        }
        _expectedElements.clear();
    }

private:
    // To hold the expected SBE elements. This must be a member variable, so the BSON values will
    // not be freed before validation.
    std::vector<Element> _expectedElements;
};

TEST_F(BSONColumnMaterializerTest, SBEMaterializer) {
    assertMaterializedValue(true, {value::TypeTags::Boolean, 1});
    assertMaterializedValue(false, {value::TypeTags::Boolean, 0});
    assertMaterializedValue((int32_t)100,
                            {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(100)});
    assertMaterializedValue((int64_t)1000,
                            {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1000)});
    assertMaterializedValue((double)128.125,
                            {value::TypeTags::NumberDouble, value::bitcastFrom<double>(128.125)});

    Decimal128 decimal{1024.75};
    BSONObj obj = BSON("" << decimal);
    const char* decimalStorage = obj.firstElement().value();
    assertMaterializedValue(
        decimal, {value::TypeTags::NumberDecimal, value::bitcastFrom<const char*>(decimalStorage)});

    Date_t date = Date_t::fromMillisSinceEpoch(1702334800770);
    assertMaterializedValue(
        date, {value::TypeTags::Date, value::bitcastFrom<long long>(date.toMillisSinceEpoch())});

    Timestamp ts{date};
    obj = BSON("" << ts);
    uint64_t uts = ConstDataView{obj.firstElement().value()}.read<LittleEndian<uint64_t>>();
    assertMaterializedValue(ts, {value::TypeTags::Timestamp, uts});

    StringData strSmall{"cramped"};
    assertMaterializedValue(strSmall, value::makeSmallString(strSmall));

    StringData strBig{"spacious"};
    obj = BSON("" << strBig);
    const char* strStorage = obj.firstElement().value();
    assertMaterializedValue(
        strBig, {value::TypeTags::bsonString, value::bitcastFrom<const char*>(strStorage)});

    uint8_t binData[] = {100, 101, 102, 103};
    BSONBinData bsonBinData{binData, sizeof(binData), BinDataGeneral};
    obj = BSON("" << bsonBinData);
    auto bdStorage = obj.firstElement().value();
    assertMaterializedValue(
        bsonBinData, {value::TypeTags::bsonBinData, value::bitcastFrom<const char*>(bdStorage)});

    BSONCode code{StringData{"x = 0"}};
    obj = BSON("" << code);
    auto codeStorage = obj.firstElement().value();
    assertMaterializedValue(
        code, {value::TypeTags::bsonJavascript, value::bitcastFrom<const char*>(codeStorage)});

    auto oid = OID::gen();
    obj = BSON("" << oid);
    auto oidStorage = obj.firstElement().value();
    assertMaterializedValue(
        oid, {value::TypeTags::bsonObjectId, value::bitcastFrom<const char*>(oidStorage)});
}

TEST_F(BSONColumnMaterializerTest, SBEMaterializerOtherTypes) {
    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<Element> vec;
    mongo::bsoncolumn::Collector<SBEColumnMaterializer, decltype(vec)> collector{vec, allocator};

    // Not all types are compressed in BSONColumn. Values of these types are just stored as
    // uncompressed BSONElements. "Code with scope" is an example of this.
    BSONCodeWScope codeWScope{"print(`${x}`)", BSON("x" << 10)};
    auto obj = BSON("" << codeWScope);
    auto bsonElem = obj.firstElement();
    auto bytes = bsonElem.value();

    // Test with copy.
    collector.append<BSONElement>(bsonElem);
    assertSbeValueEquals(
        vec.back(),
        Element({value::TypeTags::bsonCodeWScope, value::bitcastFrom<const char*>(bytes)}));
    assertSbeValueEquals(vec.back(), bson::convertFrom<true /* view */>(bsonElem));
    // Since we are making a copy and storing it in the ElementStorage, the address of the data
    // should not be the same.
    ASSERT_NOT_EQUALS(
        vec.back(),
        Element({value::TypeTags::bsonCodeWScope, value::bitcastFrom<const char*>(bytes)}));

    // Test without copy by ensuring the addresses are the same.
    collector.appendPreallocated(bsonElem);
    ASSERT_EQ(vec.back(),
              Element({value::TypeTags::bsonCodeWScope, value::bitcastFrom<const char*>(bytes)}));
    ASSERT_EQ(vec.back(), bson::convertFrom<true /* view */>(bsonElem));
}

TEST_F(BSONColumnMaterializerTest, SBEMaterializerMissing) {
    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<Element> vec;
    mongo::bsoncolumn::Collector<SBEColumnMaterializer, decltype(vec)> collector{vec, allocator};

    collector.appendMissing();
    ASSERT_EQ(vec.back(), Element({value::TypeTags::Nothing, 0}));
    ASSERT_EQ(vec.back(), bson::convertFrom<true /* view */>(BSONElement{}));
}

// Basic test for decompressIterative. There will be more exhaustive tests in bsoncolumn_test.cpp.
TEST_F(BSONColumnMaterializerTest, DecompressIterativeSimpleWithSBEMaterializer) {
    BSONObj obj = BSON("" << true);
    verifyDecompressionIterative(obj, {value::TypeTags::Boolean, 1});

    obj = BSON("" << false);
    verifyDecompressionIterative(obj, {value::TypeTags::Boolean, 0});

    obj = BSON("" << (int32_t)100);
    verifyDecompressionIterative(obj,
                                 {value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(100)});

    obj = BSON("" << (int64_t)10000);
    verifyDecompressionIterative(
        obj, {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(10000)});

    obj = BSON("" << (double)123.982);
    verifyDecompressionIterative(
        obj, {value::TypeTags::NumberDouble, value::bitcastFrom<double>(123.982)});

    Decimal128 decimal{123.982};
    obj = BSON("" << decimal);
    verifyDecompressionIterative(obj,
                                 {value::TypeTags::NumberDecimal,
                                  value::bitcastFrom<const char*>(obj.firstElement().value())});

    Date_t date = Date_t::fromMillisSinceEpoch(1702334800770);
    obj = BSON("" << date);
    verifyDecompressionIterative(
        obj, {value::TypeTags::Date, value::bitcastFrom<long long>(date.toMillisSinceEpoch())});

    Timestamp ts{date};
    obj = BSON("" << ts);
    uint64_t uts = ConstDataView{obj.firstElement().value()}.read<LittleEndian<uint64_t>>();
    verifyDecompressionIterative(obj, {value::TypeTags::Timestamp, uts});

    StringData strBig{"hello_world"};
    obj = BSON("" << strBig);
    verifyDecompressionIterative(
        obj,
        {value::TypeTags::bsonString, value::bitcastFrom<const char*>(obj.firstElement().value())});

    uint8_t binData[] = {100, 101, 102, 103};
    BSONBinData bsonBinData{binData, sizeof(binData), BinDataGeneral};
    obj = BSON("" << bsonBinData);
    verifyDecompressionIterative(obj,
                                 {value::TypeTags::bsonBinData,
                                  value::bitcastFrom<const char*>(obj.firstElement().value())});

    BSONCode code{StringData{"x = 0"}};
    obj = BSON("" << code);
    verifyDecompressionIterative(obj,
                                 {value::TypeTags::bsonJavascript,
                                  value::bitcastFrom<const char*>(obj.firstElement().value())});

    auto oid = OID::gen();
    obj = BSON("" << oid);
    verifyDecompressionIterative(obj,
                                 {value::TypeTags::bsonObjectId,
                                  value::bitcastFrom<const char*>(obj.firstElement().value())});

    // Not all types are compressed in BSONColumn. Since the decompression code is identical, we
    // will test returning one of the uncompressed types.
    BSONCodeWScope codeWScope{"print(`${x}`)", BSON("x" << 10)};
    obj = BSON("" << codeWScope);
    verifyDecompressionIterative(obj,
                                 {value::TypeTags::bsonCodeWScope,
                                  value::bitcastFrom<const char*>(obj.firstElement().value())});

    // Test EOO.
    obj = {};
    verifyDecompressionIterative(obj, {value::TypeTags::Nothing, 0});
}

TEST_F(BSONColumnMaterializerTest, DecompressSimpleSBEPath) {
    std::vector<BSONObj> input = {
        BSON("a" << 10 << "b" << 20),
        BSON("a" << 11 << "b" << 21),
        BSON("a" << 12 << "b" << 23),
        BSON("a" << 13 << "b" << 24),
    };
    std::vector<int32_t> expectedPositions{1, 1, 1, 1};
    // The requested path is Get(a) / Id.
    {
        SBEPath path{
            value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                          {value::CellBlock::Get{"a"}, value::CellBlock::Id{}})};
        setExpectedElements({{value::TypeTags::NumberInt32, 10},
                             {value::TypeTags::NumberInt32, 11},
                             {value::TypeTags::NumberInt32, 12},
                             {value::TypeTags::NumberInt32, 13}});
        verifyDecompressWithPositions(input, path, expectedPositions);
    }
    // The requested path is Get(b) / Id.
    {
        SBEPath path{
            value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                          {value::CellBlock::Get{"b"}, value::CellBlock::Id{}})};
        auto mockRefObj = fromjson("{a: 10, b: 20}");
        ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

        setExpectedElements({{value::TypeTags::NumberInt32, 20},
                             {value::TypeTags::NumberInt32, 21},
                             {value::TypeTags::NumberInt32, 23},
                             {value::TypeTags::NumberInt32, 24}});
        verifyDecompressWithPositions(input, path, expectedPositions);
    }
}

TEST_F(BSONColumnMaterializerTest, DecompressArrayWithSBEPathNoTraverse) {
    std::vector<BSONObj> input = {BSON("a" << BSON_ARRAY(0 << 10)),
                                  BSON("a" << BSON_ARRAY(10 << 20)),
                                  BSON("a" << BSON_ARRAY(20 << 30)),
                                  BSON("a" << BSON_ARRAY(30 << 40))};

    SBEPath path{
        value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                      {value::CellBlock::Get{"a"}, value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: [0, 10]}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

    for (int i = 0; i < 4; i++) {
        addExpectedArray(BSON_ARRAY(i * 10 << (i + 1) * 10));
    }

    std::vector<int32_t> expectedPositions{1, 1, 1, 1};
    verifyDecompressWithPositions(input, path, expectedPositions);
}

TEST_F(BSONColumnMaterializerTest, DecompressArrayWithSBEPathWithTraverse) {
    std::vector<BSONObj> input = {BSON("a" << BSON_ARRAY(0 << 10)),
                                  BSON("a" << BSON_ARRAY(20 << 30)),
                                  BSON("a" << BSON_ARRAY(40 << 50)),
                                  BSON("a" << BSON_ARRAY(60 << 70))};
    SBEPath path{value::CellBlock::PathRequest(
        value::CellBlock::PathRequestType::kFilter,
        {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{}, value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: [0, 10]}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 2);

    for (int i = 0; i < 8; ++i) {
        addExpectedElement({value::TypeTags::NumberInt32, i * 10});
    }

    std::vector<int32_t> expectedPositions{2, 2, 2, 2};
    verifyDecompressWithPositions(input, path, expectedPositions);
}

TEST_F(BSONColumnMaterializerTest, DecompressNestedArrayWithSBEPath) {
    std::vector<BSONObj> input = {BSON("a" << BSON_ARRAY(BSON_ARRAY(0 << 10))),
                                  BSON("a" << BSON_ARRAY(BSON_ARRAY(10 << 20))),
                                  BSON("a" << BSON_ARRAY(BSON_ARRAY(20 << 30))),
                                  BSON("a" << BSON_ARRAY(BSON_ARRAY(30 << 40)))};
    SBEPath path{value::CellBlock::PathRequest(
        value::CellBlock::PathRequestType::kFilter,
        {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{}, value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: [[0, 10]]}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

    for (int i = 0; i < 4; i++) {
        addExpectedArray(BSON_ARRAY(i * 10 << (i + 1) * 10));
    }

    std::vector<int32_t> expectedPositions{1, 1, 1, 1};
    verifyDecompressWithPositions(input, path, expectedPositions);
}

TEST_F(BSONColumnMaterializerTest, DecompressNestedObjectWithSBEPath) {
    std::vector<BSONObj> input = {BSON("a" << BSON("b" << 0)),
                                  BSON("a" << BSON("b" << 10)),
                                  BSON("a" << BSON("b" << 20)),
                                  BSON("a" << BSON("b" << 30))};
    SBEPath path{
        value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                      {value::CellBlock::Get{"a"}, value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: {b: 0}}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

    for (int i = 0; i < 4; i++) {
        addExpectedObj(BSON("b" << i * 10));
    }

    std::vector<int32_t> expectedPositions{1, 1, 1, 1};
    verifyDecompressWithPositions(input, path, expectedPositions);
}

TEST_F(BSONColumnMaterializerTest, DecompressNestedObjectGetWithSBEPath) {
    std::vector<BSONObj> input = {BSON("a" << BSON("b" << 0)),
                                  BSON("a" << BSON("b" << 10)),
                                  BSON("a" << BSON("b" << 20)),
                                  BSON("a" << BSON("b" << 30))};

    SBEPath path{value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                               {value::CellBlock::Get{"a"},
                                                value::CellBlock::Traverse{},
                                                value::CellBlock::Get{"b"},
                                                value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: {b: 0}}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

    for (int i = 0; i < 4; ++i) {
        addExpectedElement({value::TypeTags::NumberInt32, i * 10});
    }
    std::vector<int32_t> expectedPositions{1, 1, 1, 1};
    verifyDecompressWithPositions(input, path, expectedPositions);
}

TEST_F(BSONColumnMaterializerTest, DecompressNestedObjectInArrayWithSBEPath) {
    std::vector<BSONObj> input = {BSON("a" << BSON_ARRAY(BSON("b" << 0))),
                                  BSON("a" << BSON_ARRAY(BSON("b" << 10))),
                                  BSON("a" << BSON_ARRAY(BSON("b" << 20))),
                                  BSON("a" << BSON_ARRAY(BSON("b" << 30)))};

    SBEPath path{value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                               {value::CellBlock::Get{"a"},
                                                value::CellBlock::Traverse{},
                                                value::CellBlock::Get{"b"},
                                                value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: {b: 0}}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

    std::vector<Element> expected;
    for (int i = 0; i < 4; ++i) {
        addExpectedElement({value::TypeTags::NumberInt32, i * 10});
    }
    std::vector<int32_t> expectedPositions{1, 1, 1, 1};
    verifyDecompressWithPositions(input, path, expectedPositions);
}

TEST_F(BSONColumnMaterializerTest, DecompressArrayWithScalarsAndObjects) {
    std::vector<BSONObj> input = {
        BSON("a" << BSON_ARRAY(BSON("b" << 1) << BSON("b" << BSON_ARRAY(1 << 2 << 3)))),
        BSON("a" << BSON_ARRAY(BSON("b" << 10) << BSON("b" << BSON_ARRAY(0 << 10 << 11)))),
        BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(3 << 4 << 5)))),
        BSON("a" << BSON_ARRAY(BSON("b" << BSON_ARRAY(9 << 9 << 9)))),
    };
    SBEPath path{value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                               {value::CellBlock::Get{"a"},
                                                value::CellBlock::Traverse{},
                                                value::CellBlock::Get{"b"},
                                                value::CellBlock::Id{}})};
    addExpectedElement({value::TypeTags::NumberInt32, 1});
    addExpectedArray(BSON_ARRAY(1 << 2 << 3));
    addExpectedElement({value::TypeTags::NumberInt32, 10});
    addExpectedArray(BSON_ARRAY(0 << 10 << 11));
    addExpectedArray(BSON_ARRAY(3 << 4 << 5));
    addExpectedArray(BSON_ARRAY(9 << 9 << 9));

    std::vector<int32_t> expectedPositions{2, 2, 1, 1};

    verifyDecompressWithPositions(input, path, expectedPositions);
}


TEST_F(BSONColumnMaterializerTest, DecompressObjsWithNestedArrays) {
    std::vector<BSONObj> input = {
        fromjson("{a:1, b:1}"),
        fromjson("{a:2, b:2}"),
        fromjson("{a:[3,4,[40]], b:2}"),
        fromjson("{a:[6,7,[70]], b:2}"),
    };

    // The requested path is Get(a) / Id.
    {
        SBEPath path{
            value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                          {value::CellBlock::Get{"a"}, value::CellBlock::Id{}})};
        addExpectedElement({value::TypeTags::NumberInt32, 1});
        addExpectedElement({value::TypeTags::NumberInt32, 2});
        addExpectedArray(BSON_ARRAY(3 << 4 << BSON_ARRAY(40)));
        addExpectedArray(BSON_ARRAY(6 << 7 << BSON_ARRAY(70)));

        std::vector<int32_t> expectedPositions{1, 1, 1, 1};

        verifyDecompressWithPositions(input, path, expectedPositions);
    }

    // The requested path is Get(a) / Traverse / Id.
    {
        SBEPath path{value::CellBlock::PathRequest(
            value::CellBlock::PathRequestType::kFilter,
            {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{}, value::CellBlock::Id{}})};

        for (int i = 0; i < 8; ++i) {
            // Indexes 4 and 7 are arrays.
            if (i == 4 || i == 7) {
                addExpectedArray(BSON_ARRAY(i * 10));
                continue;
            }
            addExpectedElement({value::TypeTags::NumberInt32, i + 1});
        }
        std::vector<int32_t> expectedPositions{1, 1, 3, 3};

        verifyDecompressWithPositions(input, path, expectedPositions);
    }
}

TEST_F(BSONColumnMaterializerTest, DecompressDoublyNestedArrays) {
    BSONColumnBuilder cb;
    std::vector<BSONObj> input = {
        fromjson("{a: [[{b: 1}], {b:2}]}"),
        fromjson("{a: [[{b: 10}], {b:12}]}"),
        fromjson("{a: [{b: [[3,4]]}, {b: [5, 6]}, {b:7}]}"),
        fromjson("{a: [{b: [[8,8]]}, {b: [8, 8]}, {b:8}]}"),
    };

    SBEPath path{value::CellBlock::PathRequest(
        value::CellBlock::PathRequestType::kFilter,
        {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{}, value::CellBlock::Id{}})};

    addExpectedArray(BSON_ARRAY(BSON("b" << 1)));
    addExpectedObj(BSON("b" << 2));
    addExpectedArray(BSON_ARRAY(BSON("b" << 10)));
    addExpectedObj(BSON("b" << 12));
    addExpectedObj(BSON("b" << BSON_ARRAY(BSON_ARRAY(3 << 4))));
    addExpectedObj(BSON("b" << BSON_ARRAY(5 << 6)));
    addExpectedObj(BSON("b" << 7));
    addExpectedObj(BSON("b" << BSON_ARRAY(BSON_ARRAY(8 << 8))));
    addExpectedObj(BSON("b" << BSON_ARRAY(8 << 8)));
    addExpectedObj(BSON("b" << 8));

    std::vector<int32_t> expectedPositions{2, 2, 3, 3};
    verifyDecompressWithPositions(input, path, expectedPositions);
}

TEST_F(BSONColumnMaterializerTest, DecompressAllUnmatchedPath) {
    std::vector<BSONObj> input = {BSON("b" << 1), BSON("b" << 2), BSON("b" << 3)};
    SBEPath path{
        value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                      {value::CellBlock::Get{"a"}, value::CellBlock::Id{}})};
    setExpectedElements({{value::TypeTags::Nothing, EOO},
                         {value::TypeTags::Nothing, EOO},
                         {value::TypeTags::Nothing, EOO}});
    std::vector<int32_t> expectedPositions{1, 1, 1};
    verifyDecompressWithPositions(input, path, expectedPositions);
}

TEST_F(BSONColumnMaterializerTest, DecompressSomeUnmatchedPath) {
    std::vector<BSONObj> input = {
        fromjson("{a:1, b: 1}"),
        fromjson("{b: 2}"),
        fromjson("{b: {a : 1}}"),
        fromjson("{a: 5}"),
    };

    SBEPath path{
        value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                      {value::CellBlock::Get{"a"}, value::CellBlock::Id{}})};
    setExpectedElements({{value::TypeTags::NumberInt32, 1},
                         {value::TypeTags::Nothing, EOO},
                         {value::TypeTags::Nothing, EOO},
                         {value::TypeTags::NumberInt32, 5}});

    std::vector<int32_t> expectedPositions{1, 1, 1, 1};
    verifyDecompressWithPositions(input, path, expectedPositions);
}

TEST_F(BSONColumnMaterializerTest, DecompressUnmatchedPathInObject) {
    std::vector<BSONObj> input = {
        fromjson("{a: {b: 1}}"),                 // matches the path
        fromjson("{a: [{o: 123}]}"),             // doesn't match inner the path
        fromjson("{a: [1, 2, 3]}"),              // doesn't match inner the path
        fromjson("{a: [{b: [4, 5]}, {b: 6}]}"),  // matches the path
        fromjson("{b: [7, 8]}"),  // doesn't match the first path, but will be traversed by
                                  // 2nd and should return EOO.
    };
    SBEPath path{value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                               {value::CellBlock::Get{"a"},
                                                value::CellBlock::Traverse{},
                                                value::CellBlock::Get{"b"},
                                                value::CellBlock::Traverse{},
                                                value::CellBlock::Id{}})};

    // TODO SERVER-87339 remove the last field. The container should only have 8 elements.
    setExpectedElements({{value::TypeTags::NumberInt32, 1},
                         {value::TypeTags::Nothing, EOO},
                         {value::TypeTags::Nothing, EOO},
                         {value::TypeTags::NumberInt32, 4},
                         {value::TypeTags::NumberInt32, 5},
                         {value::TypeTags::NumberInt32, 6},
                         {value::TypeTags::Nothing, EOO},
                         {value::TypeTags::Nothing, EOO},
                         {value::TypeTags::Nothing, EOO}});
    // TODO SERVER-87339 the last position info should be 2.
    std::vector<int32_t> expectedPositions{1, 1, 1, 3, 3};
    verifyDecompressWithPositions(input, path, expectedPositions);
}

TEST_F(BSONColumnMaterializerTest, DecompressMultipleBuffers) {
    BSONColumnBuilder cb;
    std::vector<BSONObj> input = {fromjson("{a: [0, 1], b: {c: 0}}"),
                                  fromjson("{a: [2, 3], b: {c: 1}}")};

    for (auto&& o : input) {
        cb.append(o);
    }

    // Decompress the path Get(a) / Traverse / Id and Get(b) / Id.
    mongo::bsoncolumn::BSONColumnBlockBased col{cb.finalize()};
    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<SBEPath, PositionInfoTestContainer>> paths{
        {SBEPath{value::CellBlock::PathRequest(
             value::CellBlock::PathRequestType::kFilter,
             {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{}, value::CellBlock::Id{}})},
         {}},
        {SBEPath{
             value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                           {value::CellBlock::Get{"b"}, value::CellBlock::Id{}})},
         {}}};
    col.decompress<SBEColumnMaterializer>(allocator, std::span(paths));

    // Validate the first buffer.
    std::vector<Element> container = paths[0].second.getElems();
    ASSERT_EQ(container.size(), 4);
    for (size_t i = 0; i < container.size(); i++) {
        assertSbeValueEquals(container[i], {value::TypeTags::NumberInt32, i});
    }
    std::vector<int32_t> positions = paths[0].second.getPositions();
    ASSERT_EQ(positions, std::vector<int32_t>({2, 2}));

    // Validate the second buffer.
    container = paths[1].second.getElems();
    ASSERT_EQ(container.size(), 2);
    for (int i = 0; i < 2; i++) {
        auto bsonObj = BSON("c" << i);
        auto expectedElement = std::pair(value::TypeTags::bsonObject,
                                         value::bitcastFrom<const char*>(bsonObj.objdata()));
        assertSbeValueEquals(container[i], expectedElement);
    }
    positions = paths[1].second.getPositions();
    ASSERT_EQ(positions, std::vector<int32_t>({1, 1}));
}

// TODO SERVER-87339 Enable this test.
// TEST_F(BSONColumnMaterializerTest, DecompressArraysThatGetSmaller) {
//     std::vector<BSONObj> input = {
//         fromjson("{a: [0,1]}"),
//         fromjson("{a: [0]}"),
//     };

//     SBEPath path{value::CellBlock::PathRequest(
//         value::CellBlock::PathRequestType::kFilter,
//         {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{}, value::CellBlock::Id{}})};
//     setExpectedElements({{value::TypeTags::NumberInt32, 0},
//                                   {value::TypeTags::NumberInt32, 1},
//                                   {value::TypeTags::NumberInt32, 0}});
//     std::vector<int32_t> expectedPositions{2, 1};
//     verifyDecompressWithPositions(input, path, expectedPositions);
// }

// TODO SERVER-86960 Enable these tests for decompressing empty arrays. The position info
// should be 0 for empty arrays.
// TEST_F(BSONColumnMaterializerTest, DecompressEmptyArrays) {
//     std::vector<BSONObj> input = {
//         fromjson("{a:[1]}"),
//         fromjson("{a:[]}"),
//     };
//     SBEPath path{value::CellBlock::PathRequest(
//         value::CellBlock::PathRequestType::kFilter,
//         {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{}, value::CellBlock::Id{}})};
//     std::vector<int32_t> expectedPositions{1, 0};
//     verifyDecompressWithPositions(input, path, expectedPositions);
// }

// TEST_F(BSONColumnMaterializerTest, DecompressEmtpyArraysInObjects) {
//     std::vector<BSONObj> input = {
//         fromjson("{a: {b: 1}}"),
//         fromjson("{a: {b: []}}"),
//         fromjson("{a: {b: [2, 3]}}"),
//     };

//    SBEPath path{value::CellBlock::PathRequest(
//              value::CellBlock::PathRequestType::kFilter,
//              {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{},
//              value::CellBlock::Id{}})};
//
//     std::vector<int32_t> expectedPositions{1, 0, 2};
//     verifyDecompressWithPositions(input, path, expectedPositions);
// }

TEST_F(BSONColumnMaterializerTest, DecompressGeneralWithDecimals) {
    std::vector<Decimal128> decimals = {
        Decimal128(1024.75), Decimal128(1025.75), Decimal128(1026.75), Decimal128(1027.75)};

    verifyDecompressWithDifferentTypes(decimals);
}

TEST_F(BSONColumnMaterializerTest, DecompressGeneralWithBindata) {
    uint8_t binData0[] = {100, 101, 102, 103};
    uint8_t binData1[] = {101, 102, 103, 104};
    uint8_t binData2[] = {102, 103, 104, 105};
    uint8_t binData3[] = {103, 104, 105, 106};

    std::vector<BSONBinData> bsonBinDatas = {
        BSONBinData(binData0, sizeof(binData0), BinDataGeneral),
        BSONBinData(binData1, sizeof(binData1), BinDataGeneral),
        BSONBinData(binData2, sizeof(binData2), BinDataGeneral),
        BSONBinData(binData3, sizeof(binData3), BinDataGeneral)};

    verifyDecompressWithDifferentTypes(bsonBinDatas);
}

TEST_F(BSONColumnMaterializerTest, DecompressGeneralWithCode) {
    std::vector<BSONCode> codes = {BSONCode(StringData{"x = 0"}),
                                   BSONCode(StringData{"x = 1"}),
                                   BSONCode(StringData{"x = 2"}),
                                   BSONCode(StringData{"x = 3"})};

    verifyDecompressWithDifferentTypes(codes);
}

TEST_F(BSONColumnMaterializerTest, DecompressGeneralWithString) {
    std::vector<StringData> strs = {StringData("hello_world0"),
                                    StringData("hello_world1"),
                                    StringData("hello_world2"),
                                    StringData("hello_world3")};

    verifyDecompressWithDifferentTypes(strs);
}

TEST_F(BSONColumnMaterializerTest, DecompressGeneralWithOID) {
    std::vector<OID> oids = {OID("112233445566778899AABBCC"),
                             OID("112233445566778899AABBCB"),
                             OID("112233445566778899AABBAA"),
                             OID("112233445566778899AABBAB")};

    verifyDecompressWithDifferentTypes(oids);
}

TEST_F(BSONColumnMaterializerTest, DecompressGeneralWithDateAndTimestamp) {
    Date_t date0 = Date_t::fromMillisSinceEpoch(1702334800770);
    Date_t date1 = Date_t::fromMillisSinceEpoch(1702334800771);
    Date_t date2 = Date_t::fromMillisSinceEpoch(1702334800772);
    Date_t date3 = Date_t::fromMillisSinceEpoch(1702334800772);

    std::vector<Date_t> dates = {date0, date1, date2, date3};

    verifyDecompressWithDifferentTypes(dates);

    std::vector<Timestamp> timestamps = {
        Timestamp(date0), Timestamp(date1), Timestamp(date2), Timestamp(date3)};

    verifyDecompressWithDifferentTypes(timestamps);
}

}  // namespace
}  // namespace mongo::sbe::bsoncolumn
