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

#include "mongo/bson/util/bsoncolumnbuilder.h"
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
    ElementStorage allocator;
    std::vector<Element> container{{}};
    col.decompressIterative<SBEColumnMaterializer>(container, allocator);
    assertSbeValueEquals(container.back(), expected);
}

template <typename T>
void assertMaterializedValue(const T& value, Element expected) {
    ElementStorage allocator{};
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
    ElementStorage allocator{};
    std::vector<Element> vec;
    mongo::bsoncolumn::Collector<SBEColumnMaterializer, decltype(vec)> collector{vec, allocator};

    // Not all types are compressed in BSONColumn. Values of these types are just stored as
    // uncompressed BSONElements. "Code with scope" is an example of this.
    BSONCodeWScope codeWScope{"print(`${x}`)", BSON("x" << 10)};
    auto obj = BSON("" << codeWScope);
    auto bsonElem = obj.firstElement();
    auto bytes = bsonElem.value();
    collector.append<BSONElement>(bsonElem);
    ASSERT_EQ(vec.back(),
              Element({value::TypeTags::bsonCodeWScope, value::bitcastFrom<const char*>(bytes)}));
    ASSERT_EQ(vec.back(), bson::convertFrom<true /* view */>(bsonElem));
}

TEST_F(BSONColumnMaterializerTest, SBEMaterializerMissing) {
    ElementStorage allocator{};
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

}  // namespace
}  // namespace mongo::sbe::bsoncolumn
