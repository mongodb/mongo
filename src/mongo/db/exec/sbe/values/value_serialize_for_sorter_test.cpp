/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {
/**
 * This file contains tests for sbe::value::writeValueToStream.
 */
TEST(ValueSerializeForSorter, Serialize) {
    auto [testDataTag, testDataVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard testDataGuard{testDataTag, testDataVal};
    auto testData = sbe::value::getArrayView(testDataVal);

    testData->push_back(value::TypeTags::Nothing, 0);
    testData->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(33550336));
    testData->push_back(value::TypeTags::RecordId, value::bitcastFrom<int64_t>(8589869056));
    testData->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(137438691328));
    testData->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.305e18));

    auto [decimalTag, decimalVal] =
        value::makeCopyDecimal(Decimal128("2658455991569831744654692615953842176"));
    testData->push_back(decimalTag, decimalVal);

    testData->push_back(value::TypeTags::Date, value::bitcastFrom<int64_t>(1234));
    testData->push_back(value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(5678));
    testData->push_back(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
    testData->push_back(value::TypeTags::Null, 0);
    testData->push_back(value::TypeTags::MinKey, 0);
    testData->push_back(value::TypeTags::MaxKey, 0);
    testData->push_back(value::TypeTags::bsonUndefined, 0);

    StringData smallString = "perfect"_sd;
    invariant(sbe::value::canUseSmallString(smallString));
    StringData bigString = "too big string to fit into value"_sd;
    invariant(!sbe::value::canUseSmallString(bigString));
    StringData smallStringWithNull = "a\0b"_sd;
    invariant(smallStringWithNull.size() <= sbe::value::kSmallStringMaxLength);
    StringData bigStringWithNull = "too big string \0 to fit into value"_sd;
    invariant(bigStringWithNull.size() > sbe::value::kSmallStringMaxLength);

    std::vector<StringData> stringCases = {
        smallString,
        smallStringWithNull,
        bigString,
        bigStringWithNull,
        ""_sd,
        "a"_sd,
        "a\0"_sd,
        "\0"_sd,
        "\0\0\0"_sd,
    };

    for (const auto& stringCase : stringCases) {
        auto [stringTag, stringVal] = value::makeNewString(stringCase);
        testData->push_back(stringTag, stringVal);
    }

    for (const auto& stringCase : stringCases) {
        auto [symbolTag, symbolVal] = value::makeNewBsonSymbol(stringCase);
        testData->push_back(symbolTag, symbolVal);
    }

    auto [objectTag, objectVal] = value::makeNewObject();
    testData->push_back(objectTag, objectVal);

    auto object = value::getObjectView(objectVal);
    object->push_back("num", value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1));

    auto [arrayTag, arrayVal] = value::makeNewArray();
    object->push_back("arr", arrayTag, arrayVal);

    auto array = value::getArrayView(arrayVal);
    array->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    array->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));

    auto [arraySetTag, arraySetVal] = value::makeNewArraySet();
    object->push_back("set", arraySetTag, arraySetVal);

    auto arraySet = value::getArraySetView(arraySetVal);
    arraySet->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(4));
    arraySet->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(5));

    auto [oidTag, oidVal] = value::makeCopyObjectId({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    testData->push_back(oidTag, oidVal);

    uint8_t byteArray[] = {8, 7, 6, 5, 4, 3, 2, 1};
    auto bson =
        BSON("obj" << BSON("a" << 1 << "b" << 2) << "arr" << BSON_ARRAY(1 << 2 << 3)  //
                   << "binDataGeneral" << BSONBinData(byteArray, sizeof(byteArray), BinDataGeneral)
                   << "binDataDeprecated"
                   << BSONBinData(byteArray, sizeof(byteArray), ByteArrayDeprecated)
                   << "malformedBinDataDeprecated" << BSONBinData(nullptr, 0, ByteArrayDeprecated));

    auto [bsonObjTag, bsonObjVal] = value::copyValue(
        value::TypeTags::bsonObject, value::bitcastFrom<const char*>(bson["obj"].value()));
    testData->push_back(bsonObjTag, bsonObjVal);

    auto [bsonArrayTag, bsonArrayVal] = value::copyValue(
        value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bson["arr"].value()));
    testData->push_back(bsonArrayTag, bsonArrayVal);

    auto [bsonBinDataGeneralTag, bsonBinDataGeneralVal] =
        value::copyValue(value::TypeTags::bsonBinData,
                         value::bitcastFrom<const char*>(bson["binDataGeneral"].value()));
    testData->push_back(bsonBinDataGeneralTag, bsonBinDataGeneralVal);

    auto [bsonBinDataDeprecatedTag, bsonBinDataDeprecatedVal] =
        value::copyValue(value::TypeTags::bsonBinData,
                         value::bitcastFrom<const char*>(bson["binDataDeprecated"].value()));
    testData->push_back(bsonBinDataDeprecatedTag, bsonBinDataDeprecatedVal);

    KeyString::Builder keyStringBuilder(KeyString::Version::V1);
    keyStringBuilder.appendNumberLong(1);
    keyStringBuilder.appendNumberLong(2);
    keyStringBuilder.appendNumberLong(3);
    auto [keyStringTag, keyStringVal] = value::makeCopyKeyString(keyStringBuilder.getValueCopy());
    testData->push_back(keyStringTag, keyStringVal);

    auto [plainCodeTag, plainCodeVal] =
        value::makeCopyBsonJavascript("function test() { return 'Hello world!'; }"_sd);
    testData->push_back(value::TypeTags::bsonJavascript, plainCodeVal);

    auto [codeWithNullTag, codeWithNullVal] =
        value::makeCopyBsonJavascript("function test() { return 'Danger\0us!'; }"_sd);
    testData->push_back(value::TypeTags::bsonJavascript, codeWithNullVal);

    auto regexBson =
        BSON("noOptions" << BSONRegEx("[a-z]+") << "withOptions" << BSONRegEx(".*", "i")
                         << "emptyPatternNoOptions" << BSONRegEx("") << "emptyPatternWithOptions"
                         << BSONRegEx("", "s"));

    for (const auto& element : regexBson) {
        auto [copyTag, copyVal] = value::copyValue(
            value::TypeTags::bsonRegex, value::bitcastFrom<const char*>(element.value()));
        testData->push_back(copyTag, copyVal);
    }

    auto [dbptrTag, dbptrVal] = value::makeNewBsonDBPointer(
        "db.c", value::ObjectIdType{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}.data());
    testData->push_back(dbptrTag, dbptrVal);

    auto [cwsTag1, cwsVal1] = value::makeNewBsonCodeWScope(
        "function test() { return 'Hello world!'; }", BSONObj().objdata());
    testData->push_back(cwsTag1, cwsVal1);

    auto [cwsTag2, cwsVal2] = value::makeNewBsonCodeWScope(
        "function test() { return 'Danger\0us!'; }", BSON("a" << 1).objdata());
    testData->push_back(cwsTag2, cwsVal2);

    auto [cwsTag3, cwsVal3] =
        value::makeNewBsonCodeWScope("", BSON("b" << 2 << "c" << BSON_ARRAY(3 << 4)).objdata());
    testData->push_back(cwsTag3, cwsVal3);

    value::MaterializedRow originalRow{testData->size()};
    for (size_t i = 0; i < testData->size(); i++) {
        auto [tag, value] = testData->getAt(i);
        originalRow.reset(i, false, tag, value);
    }

    BufBuilder builder;
    originalRow.serializeForSorter(builder);
    auto buffer = builder.release();

    BufReader reader(buffer.get(), buffer.capacity());
    value::MaterializedRow roundTripRow = value::MaterializedRow::deserializeForSorter(reader, {});

    ASSERT(value::MaterializedRowEq()(originalRow, roundTripRow));
}
}  // namespace mongo::sbe
