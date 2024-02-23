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

class BSONColumnMaterializerTest : public unittest::Test {};

using Element = SBEColumnMaterializer::Element;

void assertSbeValueEquals(Element actual, Element expected, bool omitStringTypeCheck = false) {
    if (actual.first == value::TypeTags::StringSmall && omitStringTypeCheck) {
        // Generic conversion won't produce StringSmall from BSONElements, but SBEColumnMaterializer
        // will, don't compare the type tag for that case.
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
    mongo::bsoncolumn::Collector<SBEColumnMaterializer, decltype(vec)> collector{vec, allocator};

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

TEST_F(BSONColumnMaterializerTest, SBEMaterializerPath) {
    BSONColumnBuilder cb;
    std::vector<BSONObj> objs = {
        BSON("a" << 10 << "b" << 20),
        BSON("a" << 11 << "b" << 21),
        BSON("a" << 12 << "b" << 23),
        BSON("a" << 13 << "b" << 24),
    };
    for (auto&& o : objs) {
        cb.append(o);
    }

    mongo::bsoncolumn::BSONColumnBlockBased col{cb.finalize()};

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<SBEPath, std::vector<Element>>> paths{
        {SBEPath{
             value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                           {value::CellBlock::Get{"a"}, value::CellBlock::Id{}})},
         {}}};

    // Decompress only the values of "a" to the vector.
    col.decompress<SBEColumnMaterializer>(allocator, std::span(paths));

    auto& container = paths[0].second;
    ASSERT_EQ(container.size(), 4);
    ASSERT_EQ(container[0], Element({value::TypeTags::NumberInt32, 10}));
    ASSERT_EQ(container[1], Element({value::TypeTags::NumberInt32, 11}));
    ASSERT_EQ(container[2], Element({value::TypeTags::NumberInt32, 12}));
    ASSERT_EQ(container[3], Element({value::TypeTags::NumberInt32, 13}));
}

TEST_F(BSONColumnMaterializerTest, DecompressWithSBEPathTestSecondField) {
    BSONColumnBuilder cb;
    std::vector<BSONObj> objs = {BSON("a" << 10 << "b" << 20),
                                 BSON("a" << 11 << "b" << 21),
                                 BSON("a" << 12 << "b" << 22),
                                 BSON("a" << 13 << "b" << 23)};
    for (auto&& o : objs) {
        cb.append(o);
    }

    mongo::bsoncolumn::BSONColumnBlockBased col{cb.finalize()};

    SBEPath path{
        value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                      {value::CellBlock::Get{"b"}, value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: 10, b: 20}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<SBEPath, std::vector<Element>>> paths{{path, {}}};

    // Decompress only the values of "b" to the vector.
    col.decompress<SBEColumnMaterializer>(allocator, std::span(paths));

    auto& container = paths[0].second;
    ASSERT_EQ(container.size(), 4);
    ASSERT_EQ(container[0], Element({value::TypeTags::NumberInt32, 20}));
    ASSERT_EQ(container[1], Element({value::TypeTags::NumberInt32, 21}));
    ASSERT_EQ(container[2], Element({value::TypeTags::NumberInt32, 22}));
    ASSERT_EQ(container[3], Element({value::TypeTags::NumberInt32, 23}));
}

TEST_F(BSONColumnMaterializerTest, DecompressArrayWithSBEPathNoTraverse) {
    BSONColumnBuilder cb;
    std::vector<BSONObj> objs = {BSON("a" << BSON_ARRAY(0 << 10)),
                                 BSON("a" << BSON_ARRAY(10 << 20)),
                                 BSON("a" << BSON_ARRAY(20 << 30)),
                                 BSON("a" << BSON_ARRAY(30 << 40))};
    for (auto&& o : objs) {
        cb.append(o);
    }

    mongo::bsoncolumn::BSONColumnBlockBased col{cb.finalize()};

    SBEPath path{
        value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                      {value::CellBlock::Get{"a"}, value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: [0, 10]}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<SBEPath, std::vector<Element>>> paths{{path, {}}};

    // Decompress the array without traversal.
    col.decompress<SBEColumnMaterializer>(allocator, std::span(paths));

    ASSERT_EQ(paths[0].second.size(), 4);
    for (int i = 0; i < 4; ++i) {
        auto bsonArr = BSON_ARRAY(i * 10 << (i + 1) * 10);
        auto expectedElement = std::pair(value::TypeTags::bsonArray,
                                         value::bitcastFrom<const char*>(bsonArr.objdata()));
        assertSbeValueEquals(paths[0].second[i], expectedElement);
    }
}

TEST_F(BSONColumnMaterializerTest, DecompressArrayWithSBEPathWithTraverse) {
    BSONColumnBuilder cb;
    std::vector<BSONObj> objs = {BSON("a" << BSON_ARRAY(0 << 10)),
                                 BSON("a" << BSON_ARRAY(20 << 30)),
                                 BSON("a" << BSON_ARRAY(40 << 50)),
                                 BSON("a" << BSON_ARRAY(60 << 70))};
    for (auto&& o : objs) {
        cb.append(o);
    }

    mongo::bsoncolumn::BSONColumnBlockBased col{cb.finalize()};

    SBEPath path{value::CellBlock::PathRequest(
        value::CellBlock::PathRequestType::kFilter,
        {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{}, value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: [0, 10]}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 2);

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<SBEPath, std::vector<Element>>> paths{{path, {}}};

    // Decompress the array with traversal.
    col.decompress<SBEColumnMaterializer>(allocator, std::span(paths));

    auto& container = paths[0].second;
    ASSERT_EQ(container.size(), 8);
    ASSERT_EQ(paths[0].second.size(), 8);
    for (int i = 0; i < 8; ++i) {
        assertSbeValueEquals(paths[0].second[i], {value::TypeTags::NumberInt32, i * 10});
    }
}

TEST_F(BSONColumnMaterializerTest, DecompressNestedArrayWithSBEPath) {
    BSONColumnBuilder cb;
    std::vector<BSONObj> objs = {BSON("a" << BSON_ARRAY(BSON_ARRAY(0 << 10))),
                                 BSON("a" << BSON_ARRAY(BSON_ARRAY(10 << 20))),
                                 BSON("a" << BSON_ARRAY(BSON_ARRAY(20 << 30))),
                                 BSON("a" << BSON_ARRAY(BSON_ARRAY(30 << 40)))};
    for (auto&& o : objs) {
        cb.append(o);
    }

    mongo::bsoncolumn::BSONColumnBlockBased col{cb.finalize()};

    SBEPath path{value::CellBlock::PathRequest(
        value::CellBlock::PathRequestType::kFilter,
        {value::CellBlock::Get{"a"}, value::CellBlock::Traverse{}, value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: [[0, 10]]}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<SBEPath, std::vector<Element>>> paths{{path, {}}};

    // Decompress the nested array.
    col.decompress<SBEColumnMaterializer>(allocator, std::span(paths));

    ASSERT_EQ(paths[0].second.size(), 4);
    for (int i = 0; i < 4; ++i) {
        auto bsonArr = BSON_ARRAY(i * 10 << (i + 1) * 10);
        auto expectedElement = std::pair(value::TypeTags::bsonArray,
                                         value::bitcastFrom<const char*>(bsonArr.objdata()));
        assertSbeValueEquals(paths[0].second[i], expectedElement);
    }
}

TEST_F(BSONColumnMaterializerTest, DecompressNestedObjectWithSBEPath) {
    BSONColumnBuilder cb;
    std::vector<BSONObj> objs = {BSON("a" << BSON("b" << 0)),
                                 BSON("a" << BSON("b" << 10)),
                                 BSON("a" << BSON("b" << 20)),
                                 BSON("a" << BSON("b" << 30))};
    for (auto&& o : objs) {
        cb.append(o);
    }

    mongo::bsoncolumn::BSONColumnBlockBased col{cb.finalize()};

    SBEPath path{
        value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                      {value::CellBlock::Get{"a"}, value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: {b: 0}}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<SBEPath, std::vector<Element>>> paths{{path, {}}};

    // Decompress the nested object.
    col.decompress<SBEColumnMaterializer>(allocator, std::span(paths));

    ASSERT_EQ(paths[0].second.size(), 4);
    for (int i = 0; i < 4; ++i) {
        auto bsonObj = BSON("b" << i * 10);
        auto expectedElement = std::pair(value::TypeTags::bsonObject,
                                         value::bitcastFrom<const char*>(bsonObj.objdata()));
        assertSbeValueEquals(paths[0].second[i], expectedElement);
    }
}


TEST_F(BSONColumnMaterializerTest, DecompressNestedObjectGetWithSBEPath) {
    BSONColumnBuilder cb;
    std::vector<BSONObj> objs = {BSON("a" << BSON("b" << 0)),
                                 BSON("a" << BSON("b" << 10)),
                                 BSON("a" << BSON("b" << 20)),
                                 BSON("a" << BSON("b" << 30))};
    for (auto&& o : objs) {
        cb.append(o);
    }

    mongo::bsoncolumn::BSONColumnBlockBased col{cb.finalize()};

    SBEPath path{value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                               {value::CellBlock::Get{"a"},
                                                value::CellBlock::Traverse{},
                                                value::CellBlock::Get{"b"},
                                                value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: {b: 0}}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<SBEPath, std::vector<Element>>> paths{{path, {}}};

    // Decompress the nested object with traversal.
    col.decompress<SBEColumnMaterializer>(allocator, std::span(paths));

    ASSERT_EQ(paths[0].second.size(), 4);
    for (int i = 0; i < 4; ++i) {
        assertSbeValueEquals(paths[0].second[i], {value::TypeTags::NumberInt32, i * 10});
    }
}

TEST_F(BSONColumnMaterializerTest, DecompressNestedObjectInArrayWithSBEPath) {
    BSONColumnBuilder cb;
    std::vector<BSONObj> objs = {BSON("a" << BSON_ARRAY(BSON("b" << 0))),
                                 BSON("a" << BSON_ARRAY(BSON("b" << 10))),
                                 BSON("a" << BSON_ARRAY(BSON("b" << 20))),
                                 BSON("a" << BSON_ARRAY(BSON("b" << 30)))};
    for (auto&& o : objs) {
        cb.append(o);
    }

    mongo::bsoncolumn::BSONColumnBlockBased col{cb.finalize()};

    SBEPath path{value::CellBlock::PathRequest(value::CellBlock::PathRequestType::kFilter,
                                               {value::CellBlock::Get{"a"},
                                                value::CellBlock::Traverse{},
                                                value::CellBlock::Get{"b"},
                                                value::CellBlock::Id{}})};
    auto mockRefObj = fromjson("{a: {b: 0}}");
    ASSERT_EQ(path.elementsToMaterialize(mockRefObj).size(), 1);

    boost::intrusive_ptr<ElementStorage> allocator = new ElementStorage();
    std::vector<std::pair<SBEPath, std::vector<Element>>> paths{{path, {}}};

    // Decompress the nested object within the array.
    col.decompress<SBEColumnMaterializer>(allocator, std::span(paths));

    ASSERT_EQ(paths[0].second.size(), 4);
    for (int i = 0; i < 4; ++i) {
        assertSbeValueEquals(paths[0].second[i], {value::TypeTags::NumberInt32, i * 10});
    }
}
}  // namespace
}  // namespace mongo::sbe::bsoncolumn
