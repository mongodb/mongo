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

#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
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
    auto [ridTag, ridVal] = value::makeNewRecordId(8589869056);
    testData->push_back(ridTag, ridVal);
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

class ValueSerializeForKeyString : public mongo::unittest::Test {
protected:
    void runTest(const std::vector<std::pair<sbe::value::TypeTags, sbe::value::Value>>& inputData) {
        value::MaterializedRow sourceRow{inputData.size()};
        auto idx = 0;
        for (auto& [tag, val] : inputData) {
            sourceRow.reset(idx++, false, tag, val);
        }

        KeyString::Builder kb{KeyString::Version::kLatestVersion};
        sourceRow.serializeIntoKeyString(kb);

        auto ks = kb.getValueCopy();

        BufBuilder buf;
        value::MaterializedRow roundTripRow =
            value::MaterializedRow::deserializeFromKeyString(ks, &buf);

        ASSERT(value::MaterializedRowEq()(sourceRow, roundTripRow));
    }
};

TEST_F(ValueSerializeForKeyString, Numerics) {
    runTest({{value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1)},
             {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2)},
             {value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0)}});
}

TEST_F(ValueSerializeForKeyString, RecordIdMinKeyMaxKey) {
    auto [ridTag, ridVal] = value::makeNewRecordId(8589869056);
    sbe::value::ValueGuard guard{ridTag, ridVal};
    runTest({{value::TypeTags::MinKey, 0}, {value::TypeTags::MaxKey, 0}, {ridTag, ridVal}});
}

TEST_F(ValueSerializeForKeyString, BoolNullAndNothing) {
    runTest({{value::TypeTags::Nothing, 0},
             {value::TypeTags::Null, 0},
             {value::TypeTags::Boolean, value::bitcastFrom<bool>(false)},
             {value::TypeTags::bsonUndefined, 0},
             {value::TypeTags::Boolean, value::bitcastFrom<bool>(true)}});
}

TEST_F(ValueSerializeForKeyString, AllNothing) {
    runTest({{value::TypeTags::Nothing, 0},
             {value::TypeTags::Nothing, 0},
             {value::TypeTags::Nothing, 0}});
}

TEST_F(ValueSerializeForKeyString, BsonArray) {
    auto [inputTag, inputVal] = stage_builder::makeValue(
        BSON_ARRAY(12LL << "yar" << BSON_ARRAY(2.5) << 7.5 << BSON("foo" << 23)));
    sbe::value::ValueGuard testDataGuard{inputTag, inputVal};

    runTest({{inputTag, inputVal}, {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0)}});
}

TEST_F(ValueSerializeForKeyString, SbeArray) {
    auto [testDataTag, testDataVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard testDataGuard{testDataTag, testDataVal};
    auto testData = sbe::value::getArrayView(testDataVal);

    testData->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    testData->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    testData->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));

    runTest({{testDataTag, testDataVal}});
}

TEST_F(ValueSerializeForKeyString, ArraySet) {
    auto [tag, val] = sbe::value::makeNewArraySet();
    sbe::value::ValueGuard guard{tag, val};
    auto* arraySet = sbe::value::getArraySetView(val);

    arraySet->push_back(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    arraySet->push_back(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    arraySet->push_back(value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));

    runTest({{tag, val}});
}

TEST_F(ValueSerializeForKeyString, DateTime) {
    runTest({{value::TypeTags::Date, value::bitcastFrom<int64_t>(1234)},
             {value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(5678)}});
}

TEST_F(ValueSerializeForKeyString, SmallString) {
    StringData smallString = "perfect"_sd;
    ASSERT(sbe::value::canUseSmallString(smallString));
    StringData smallStringWithNull = "a\0b"_sd;
    ASSERT(smallStringWithNull.size() <= sbe::value::kSmallStringMaxLength);
}

TEST_F(ValueSerializeForKeyString, BigString) {
    StringData bigString = "too big string to fit into value"_sd;
    ASSERT(!sbe::value::canUseSmallString(bigString));
    StringData bigStringWithNull = "too big string \0 to fit into value"_sd;
    ASSERT(bigStringWithNull.size() > sbe::value::kSmallStringMaxLength);

    auto [bigStringTag, bigStringVal] = value::makeNewString(bigString);
    sbe::value::ValueGuard testDataGuard{bigStringTag, bigStringVal};

    auto [bigStringSymbolTag, bigStringSymbolVal] = value::makeNewBsonSymbol(bigString);
    sbe::value::ValueGuard testDataGuard2{bigStringSymbolTag, bigStringSymbolVal};

    auto [bigStringWithNullTag, bigStringWithNullVal] = value::makeNewString(bigStringWithNull);
    sbe::value::ValueGuard testDataGuard3{bigStringWithNullTag, bigStringWithNullVal};

    auto [bigStringSymbolNullTag, bigStringSymbolNullVal] =
        value::makeNewBsonSymbol(bigStringWithNull);
    sbe::value::ValueGuard testDataGuard4{bigStringSymbolNullTag, bigStringSymbolNullVal};

    runTest({{bigStringTag, bigStringVal},
             {bigStringSymbolTag, bigStringSymbolVal},
             {bigStringWithNullTag, bigStringWithNullVal},
             {bigStringSymbolNullTag, bigStringSymbolNullVal}});
}

TEST_F(ValueSerializeForKeyString, EmptyAndNullTerminatedStrings) {

    auto aString = "a"_sd;
    auto aStringNullTerm = "a\0"_sd;
    auto nullTerm = "\0"_sd;
    auto nullTerms = "\0\0\0"_sd;

    auto [aStringTag, aStringVal] = value::makeNewString(aString);
    sbe::value::ValueGuard testDataGuard{aStringTag, aStringVal};

    auto [aStringSymbolNullTag, aStringSymbolNullVal] = value::makeNewBsonSymbol(aString);
    sbe::value::ValueGuard testDataGuard2{aStringSymbolNullTag, aStringSymbolNullVal};

    auto [aStringNullTermTag, aStringNullTermVal] = value::makeNewString(aStringNullTerm);
    sbe::value::ValueGuard testDataGuard3{aStringNullTermTag, aStringNullTermVal};

    auto [aStringSymbolNullTermTag, aStringSymbolNullTermVal] =
        value::makeNewBsonSymbol(aStringNullTerm);
    sbe::value::ValueGuard testDataGuard4{aStringSymbolNullTermTag, aStringSymbolNullTermVal};

    auto [nullTermTag, nullTermVal] = value::makeNewString(nullTerm);
    sbe::value::ValueGuard testDataGuard5{nullTermTag, nullTermVal};

    auto [symbolNullTermTag, symbolNullTermVal] = value::makeNewBsonSymbol(nullTerm);
    sbe::value::ValueGuard testDataGuard6{symbolNullTermTag, symbolNullTermVal};

    auto [nullTermsTag, nullTermsVal] = value::makeNewString(nullTerms);
    sbe::value::ValueGuard testDataGuard7{nullTermsTag, nullTermsVal};

    auto [symbolNullTermsTag, symbolNullTermsVal] = value::makeNewBsonSymbol(nullTerms);
    sbe::value::ValueGuard testDataGuard8{symbolNullTermsTag, symbolNullTermsVal};

    runTest({{aStringTag, aStringVal},
             {aStringSymbolNullTag, aStringSymbolNullVal},
             {aStringNullTermTag, aStringNullTermVal},
             {aStringSymbolNullTermTag, aStringSymbolNullTermVal},
             {nullTermTag, nullTermVal},
             {symbolNullTermTag, symbolNullTermVal},
             {nullTermsTag, nullTermsVal},
             {symbolNullTermsTag, symbolNullTermsVal}});
}

TEST_F(ValueSerializeForKeyString, SbeObject) {
    auto [testDataTag, testDataVal] = sbe::value::makeNewObject();
    sbe::value::ValueGuard testDataGuard{testDataTag, testDataVal};
    auto testData = sbe::value::getObjectView(testDataVal);

    testData->push_back("A", value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    testData->push_back("b", value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    testData->push_back("C", value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));

    runTest({{testDataTag, testDataVal}});
}

TEST_F(ValueSerializeForKeyString, BsonBinData) {
    uint8_t byteArray[] = {8, 7, 6, 5, 4, 3, 2, 1};
    auto bson = BSON_ARRAY(BSONBinData(byteArray, sizeof(byteArray), BinDataGeneral)
                           << BSONBinData(byteArray, sizeof(byteArray), ByteArrayDeprecated));

    auto [binDataTag, binDataVal] = value::copyValue(
        value::TypeTags::bsonBinData, value::bitcastFrom<const char*>(bson[0].value()));
    sbe::value::ValueGuard testDataGuard{binDataTag, binDataVal};

    auto [binDataTagDeprecated, binDataValDeprecated] = value::copyValue(
        value::TypeTags::bsonBinData, value::bitcastFrom<const char*>(bson[0].value()));
    sbe::value::ValueGuard testDataGuardDep{binDataTagDeprecated, binDataValDeprecated};

    runTest({{binDataTag, binDataVal}, {binDataTagDeprecated, binDataValDeprecated}});
}

TEST_F(ValueSerializeForKeyString, KeyString) {
    KeyString::Builder keyStringBuilder(KeyString::Version::V1);
    keyStringBuilder.appendNumberLong(1);
    keyStringBuilder.appendNumberLong(2);
    keyStringBuilder.appendNumberLong(3);
    keyStringBuilder.appendString("aaa");
    auto ks = keyStringBuilder.getValueCopy();
    auto [keyStringTag, keyStringVal] = value::makeCopyKeyString(ks);
    sbe::value::ValueGuard testGuard{keyStringTag, keyStringVal};

    runTest({{keyStringTag, keyStringVal}});
}

TEST_F(ValueSerializeForKeyString, BsonJavaScript) {
    auto [plainCodeTag, plainCodeVal] =
        value::makeCopyBsonJavascript("function test() { return 'Hello world!'; }"_sd);
    sbe::value::ValueGuard testDataGuard{plainCodeTag, plainCodeVal};

    auto [codeWithNullTag, codeWithNullVal] =
        value::makeCopyBsonJavascript("function test() { return 'Danger\0us!'; }"_sd);
    sbe::value::ValueGuard testDataGuard2{codeWithNullTag, codeWithNullVal};

    runTest({{plainCodeTag, plainCodeVal}, {codeWithNullTag, codeWithNullVal}});
}

TEST_F(ValueSerializeForKeyString, BsonRegex) {
    auto [noFlagsTag, noFlagsVal] = value::makeNewBsonRegex("[a-z]+"_sd, ""_sd);
    sbe::value::ValueGuard testDataGuard{noFlagsTag, noFlagsVal};

    auto [withFlagsTag, withFlagsVal] = value::makeNewBsonRegex(".*"_sd, "i"_sd);
    sbe::value::ValueGuard testDataGuard2{withFlagsTag, withFlagsVal};

    auto [empPatterNoFlagsTag, empPatterNoFlagsVal] = value::makeNewBsonRegex(""_sd, ""_sd);
    sbe::value::ValueGuard testDataGuard3{empPatterNoFlagsTag, empPatterNoFlagsVal};

    auto [empPatterWithFlagsTag, empPatterWithFlagsVal] = value::makeNewBsonRegex(""_sd, "s"_sd);
    sbe::value::ValueGuard testDataGuard4{empPatterWithFlagsTag, empPatterWithFlagsVal};

    runTest({{noFlagsTag, noFlagsVal},
             {withFlagsTag, withFlagsVal},
             {empPatterNoFlagsTag, empPatterNoFlagsVal},
             {empPatterWithFlagsTag, empPatterWithFlagsVal}});
}

TEST_F(ValueSerializeForKeyString, BsonDBPointer) {
    auto [dbptrTag, dbptrVal] = value::makeNewBsonDBPointer(
        "db.c", value::ObjectIdType{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}.data());
    sbe::value::ValueGuard testDataGuard{dbptrTag, dbptrVal};

    runTest({{dbptrTag, dbptrVal}});
}

TEST_F(ValueSerializeForKeyString, BsonCodeWScope) {
    auto [cwsTag1, cwsVal1] = value::makeNewBsonCodeWScope(
        "function test() { return 'Hello world!'; }", BSONObj().objdata());
    sbe::value::ValueGuard testDataGuard{cwsTag1, cwsVal1};

    auto [cwsTag2, cwsVal2] = value::makeNewBsonCodeWScope(
        "function test() { return 'Danger\0us!'; }", BSON("a" << 1).objdata());
    sbe::value::ValueGuard testDataGuard2{cwsTag2, cwsVal2};

    auto [cwsTag3, cwsVal3] =
        value::makeNewBsonCodeWScope("", BSON("b" << 2 << "c" << BSON_ARRAY(3 << 4)).objdata());
    sbe::value::ValueGuard testDataGuard3{cwsTag3, cwsVal3};

    runTest({{cwsTag1, cwsVal1}, {cwsTag2, cwsVal2}, {cwsTag3, cwsVal3}});
}

// Test that roundtripping through KeyString works for a wide row. KeyStrings used in indexes are
// typically constrained in the number of components they can have, since we limit compound indexes
// to at most 32 components. But roundtripping rows wider than 32 still needs to work.
//
// This test was originally designed to reproduce SERVER-76321.
TEST_F(ValueSerializeForKeyString, RoundtripWideRow) {
    std::vector<std::pair<sbe::value::TypeTags, sbe::value::Value>> row;
    for (int32_t i = 0; i < 40; ++i) {
        row.emplace_back(sbe::value::TypeTags::NumberInt32, sbe::value::bitcastFrom<int32_t>(i));
    }
    runTest(row);
}
}  // namespace mongo::sbe
