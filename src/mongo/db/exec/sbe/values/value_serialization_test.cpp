// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/sbe/values/row.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/shared_buffer.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace mongo::sbe {
using namespace std::literals::string_view_literals;
/**
 * This file contains tests for sbe::value::writeValueToStream.
 */
TEST(ValueSerializeForSorter, Serialize) {
    sbe::value::TagValueOwned testDataOwned =
        sbe::value::TagValueOwned::fromRaw(sbe::value::makeNewArray());
    auto testData = sbe::value::getArrayView(testDataOwned.value());

    testData->push_back_raw(value::TypeTags::Nothing, 0);
    testData->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(33550336));
    auto [ridTag, ridVal] = value::makeNewRecordId(8589869056);
    testData->push_back_raw(ridTag, ridVal);
    testData->push_back_raw(value::TypeTags::NumberInt64,
                            value::bitcastFrom<int64_t>(137438691328));
    testData->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.305e18));

    auto [decimalTag, decimalVal] =
        value::makeCopyDecimal(Decimal128("2658455991569831744654692615953842176"));
    testData->push_back_raw(decimalTag, decimalVal);

    testData->push_back_raw(value::TypeTags::Date, value::bitcastFrom<int64_t>(1234));
    testData->push_back_raw(value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(5678));
    testData->push_back_raw(value::TypeTags::Boolean, value::bitcastFrom<bool>(true));
    testData->push_back_raw(value::TypeTags::Null, 0);
    testData->push_back_raw(value::TypeTags::MinKey, 0);
    testData->push_back_raw(value::TypeTags::MaxKey, 0);
    testData->push_back_raw(value::TypeTags::bsonUndefined, 0);

    std::string_view smallString = "perfect"sv;
    invariant(sbe::value::canUseSmallString(smallString));
    std::string_view bigString = "too big string to fit into value"sv;
    invariant(!sbe::value::canUseSmallString(bigString));
    std::string_view smallStringWithNull = "a\0b"sv;
    invariant(smallStringWithNull.size() <= sbe::value::kSmallStringMaxLength);
    std::string_view bigStringWithNull = "too big string \0 to fit into value"sv;
    invariant(bigStringWithNull.size() > sbe::value::kSmallStringMaxLength);

    std::vector<std::string_view> stringCases = {
        smallString,
        smallStringWithNull,
        bigString,
        bigStringWithNull,
        ""sv,
        "a"sv,
        "a\0"sv,
        "\0"sv,
        "\0\0\0"sv,
    };

    for (const auto& stringCase : stringCases) {
        auto [stringTag, stringVal] = value::makeNewString(stringCase);
        testData->push_back_raw(stringTag, stringVal);
    }

    for (const auto& stringCase : stringCases) {
        auto [symbolTag, symbolVal] = value::makeNewBsonSymbol(stringCase);
        testData->push_back_raw(symbolTag, symbolVal);
    }

    auto [objectTag, objectVal] = value::makeNewObject();
    testData->push_back_raw(objectTag, objectVal);

    auto object = value::getObjectView(objectVal);
    object->push_back_raw("num", value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1));

    auto [arrayTag, arrayVal] = value::makeNewArray();
    object->push_back_raw("arr", arrayTag, arrayVal);

    auto array = value::getArrayView(arrayVal);
    array->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    array->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3));

    auto [arraySetTag, arraySetVal] = value::makeNewArraySet();
    object->push_back_raw("set", arraySetTag, arraySetVal);

    auto arraySet = value::getArraySetView(arraySetVal);
    arraySet->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(4));
    arraySet->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(5));

    auto [oidTag, oidVal] = value::makeCopyObjectId({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12});
    testData->push_back_raw(oidTag, oidVal);

    auto [arrayMultiSetTag, arrayMultiSetVal] = value::makeNewArrayMultiSet();
    object->push_back_raw("mset", arrayMultiSetTag, arrayMultiSetVal);

    value::ArrayMultiSet* arrayMultiSet = value::getArrayMultiSetView(arrayMultiSetVal);
    arrayMultiSet->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(6));
    arrayMultiSet->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(6));
    arrayMultiSet->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(7));

    auto [msetTag, msetVal] = value::makeCopyArrayMultiSet(*arrayMultiSet);
    testData->push_back_raw(msetTag, msetVal);

    uint8_t byteArray[] = {8, 7, 6, 5, 4, 3, 2, 1};
    auto bson =
        BSON("obj" << BSON("a" << 1 << "b" << 2) << "arr" << BSON_ARRAY(1 << 2 << 3)  //
                   << "binDataGeneral" << BSONBinData(byteArray, sizeof(byteArray), BinDataGeneral)
                   << "binDataDeprecated"
                   << BSONBinData(byteArray, sizeof(byteArray), ByteArrayDeprecated)
                   << "malformedBinDataDeprecated" << BSONBinData(nullptr, 0, ByteArrayDeprecated));

    auto [bsonObjTag, bsonObjVal] = value::copyValue(
        value::TypeTags::bsonObject, value::bitcastFrom<const char*>(bson["obj"].value()));
    testData->push_back_raw(bsonObjTag, bsonObjVal);

    auto [bsonArrayTag, bsonArrayVal] = value::copyValue(
        value::TypeTags::bsonArray, value::bitcastFrom<const char*>(bson["arr"].value()));
    testData->push_back_raw(bsonArrayTag, bsonArrayVal);

    auto [bsonBinDataGeneralTag, bsonBinDataGeneralVal] =
        value::copyValue(value::TypeTags::bsonBinData,
                         value::bitcastFrom<const char*>(bson["binDataGeneral"].value()));
    testData->push_back_raw(bsonBinDataGeneralTag, bsonBinDataGeneralVal);

    auto [bsonBinDataDeprecatedTag, bsonBinDataDeprecatedVal] =
        value::copyValue(value::TypeTags::bsonBinData,
                         value::bitcastFrom<const char*>(bson["binDataDeprecated"].value()));
    testData->push_back_raw(bsonBinDataDeprecatedTag, bsonBinDataDeprecatedVal);

    key_string::Builder keyStringBuilder(key_string::Version::V1);
    keyStringBuilder.appendNumberLong(1);
    keyStringBuilder.appendNumberLong(2);
    keyStringBuilder.appendNumberLong(3);
    auto [keyStringTag, keyStringVal] = value::makeKeyString(keyStringBuilder.getValueCopy());
    testData->push_back_raw(keyStringTag, keyStringVal);

    auto [plainCodeTag, plainCodeVal] =
        value::makeCopyBsonJavascript("function test() { return 'Hello world!'; }"sv);
    testData->push_back_raw(value::TypeTags::bsonJavascript, plainCodeVal);

    auto [codeWithNullTag, codeWithNullVal] =
        value::makeCopyBsonJavascript("function test() { return 'Danger\0us!'; }"sv);
    testData->push_back_raw(value::TypeTags::bsonJavascript, codeWithNullVal);

    auto regexBson =
        BSON("noOptions" << BSONRegEx("[a-z]+") << "withOptions" << BSONRegEx(".*", "i")
                         << "emptyPatternNoOptions" << BSONRegEx("") << "emptyPatternWithOptions"
                         << BSONRegEx("", "s"));

    for (const auto& element : regexBson) {
        auto [copyTag, copyVal] = value::copyValue(
            value::TypeTags::bsonRegex, value::bitcastFrom<const char*>(element.value()));
        testData->push_back_raw(copyTag, copyVal);
    }

    auto [dbptrTag, dbptrVal] = value::makeNewBsonDBPointer(
        "db.c", value::ObjectIdType{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}.data());
    testData->push_back_raw(dbptrTag, dbptrVal);

    auto [cwsTag1, cwsVal1] = value::makeNewBsonCodeWScope(
        "function test() { return 'Hello world!'; }", BSONObj().objdata());
    testData->push_back_raw(cwsTag1, cwsVal1);

    auto [cwsTag2, cwsVal2] = value::makeNewBsonCodeWScope(
        "function test() { return 'Danger\0us!'; }", BSON("a" << 1).objdata());
    testData->push_back_raw(cwsTag2, cwsVal2);

    auto [cwsTag3, cwsVal3] =
        value::makeNewBsonCodeWScope("", BSON("b" << 2 << "c" << BSON_ARRAY(3 << 4)).objdata());
    testData->push_back_raw(cwsTag3, cwsVal3);

    value::MultiMap map{};
    map.insert({value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1)},
               {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2)});
    map.insert({value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(3)},
               {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(4)});

    auto [mapTag, mapVal] = value::makeCopyMultiMap(map);
    testData->push_back_raw(mapTag, mapVal);

    value::MaterializedRow originalRow{testData->size()};
    for (size_t i = 0; i < testData->size(); i++) {
        auto [tag, value] = testData->getAt(i);
        originalRow.reset(i, value::TagValueView{tag, value});
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
            sourceRow.reset(idx++, value::TagValueView{tag, val});
        }

        key_string::Builder kb{key_string::Version::kLatestVersion};
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
    sbe::value::TagValueOwned ridOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewRecordId(8589869056));
    runTest({{value::TypeTags::MinKey, 0},
             {value::TypeTags::MaxKey, 0},
             {ridOwned.tag(), ridOwned.value()}});
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
    sbe::value::TagValueOwned inputOwned =
        sbe::value::TagValueOwned::fromRaw(stage_builder::makeValue(
            BSON_ARRAY(12LL << "yar" << BSON_ARRAY(2.5) << 7.5 << BSON("foo" << 23))));

    runTest({{inputOwned.tag(), inputOwned.value()},
             {value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(0)}});
}

TEST_F(ValueSerializeForKeyString, SbeArray) {
    sbe::value::TagValueOwned testDataOwned =
        sbe::value::TagValueOwned::fromRaw(sbe::value::makeNewArray());
    auto testData = sbe::value::getArrayView(testDataOwned.value());

    testData->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    testData->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    testData->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));

    runTest({{testDataOwned.tag(), testDataOwned.value()}});
}

TEST_F(ValueSerializeForKeyString, ArraySet) {
    sbe::value::TagValueOwned arraySetOwned =
        sbe::value::TagValueOwned::fromRaw(sbe::value::makeNewArraySet());
    auto* arraySet = sbe::value::getArraySetView(arraySetOwned.value());

    arraySet->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    arraySet->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    arraySet->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));

    runTest({{arraySetOwned.tag(), arraySetOwned.value()}});
}

TEST_F(ValueSerializeForKeyString, ArrayMultiSet) {
    sbe::value::TagValueOwned arrayMultiSetOwned =
        sbe::value::TagValueOwned::fromRaw(sbe::value::makeNewArrayMultiSet());
    auto* arrayMultiSet = sbe::value::getArrayMultiSetView(arrayMultiSetOwned.value());

    arrayMultiSet->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    arrayMultiSet->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1));
    arrayMultiSet->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.0));

    runTest({{arrayMultiSetOwned.tag(), arrayMultiSetOwned.value()}});
}

TEST_F(ValueSerializeForKeyString, DateTime) {
    runTest({{value::TypeTags::Date, value::bitcastFrom<int64_t>(1234)},
             {value::TypeTags::Timestamp, value::bitcastFrom<uint64_t>(5678)}});
}

TEST_F(ValueSerializeForKeyString, SmallString) {
    std::string_view smallString = "perfect"sv;
    ASSERT(sbe::value::canUseSmallString(smallString));
    std::string_view smallStringWithNull = "a\0b"sv;
    ASSERT(smallStringWithNull.size() <= sbe::value::kSmallStringMaxLength);
}

TEST_F(ValueSerializeForKeyString, BigString) {
    std::string_view bigString = "too big string to fit into value"sv;
    ASSERT(!sbe::value::canUseSmallString(bigString));
    std::string_view bigStringWithNull = "too big string \0 to fit into value"sv;
    ASSERT(bigStringWithNull.size() > sbe::value::kSmallStringMaxLength);

    sbe::value::TagValueOwned bigStringOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewString(bigString));

    sbe::value::TagValueOwned bigStringSymbolOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonSymbol(bigString));

    sbe::value::TagValueOwned bigStringWithNullOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewString(bigStringWithNull));

    sbe::value::TagValueOwned bigStringSymbolNullOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonSymbol(bigStringWithNull));

    runTest({{bigStringOwned.tag(), bigStringOwned.value()},
             {bigStringSymbolOwned.tag(), bigStringSymbolOwned.value()},
             {bigStringWithNullOwned.tag(), bigStringWithNullOwned.value()},
             {bigStringSymbolNullOwned.tag(), bigStringSymbolNullOwned.value()}});
}

TEST_F(ValueSerializeForKeyString, EmptyAndNullTerminatedStrings) {

    auto aString = "a"sv;
    auto aStringNullTerm = "a\0"sv;
    auto nullTerm = "\0"sv;
    auto nullTerms = "\0\0\0"sv;

    sbe::value::TagValueOwned aStringOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewString(aString));

    sbe::value::TagValueOwned aStringSymbolOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonSymbol(aString));

    sbe::value::TagValueOwned aStringNullTermOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewString(aStringNullTerm));

    sbe::value::TagValueOwned aStringSymbolNullTermOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonSymbol(aStringNullTerm));

    sbe::value::TagValueOwned nullTermOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewString(nullTerm));

    sbe::value::TagValueOwned symbolNullTermOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonSymbol(nullTerm));

    sbe::value::TagValueOwned nullTermsOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewString(nullTerms));

    sbe::value::TagValueOwned symbolNullTermsOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonSymbol(nullTerms));

    runTest({{aStringOwned.tag(), aStringOwned.value()},
             {aStringSymbolOwned.tag(), aStringSymbolOwned.value()},
             {aStringNullTermOwned.tag(), aStringNullTermOwned.value()},
             {aStringSymbolNullTermOwned.tag(), aStringSymbolNullTermOwned.value()},
             {nullTermOwned.tag(), nullTermOwned.value()},
             {symbolNullTermOwned.tag(), symbolNullTermOwned.value()},
             {nullTermsOwned.tag(), nullTermsOwned.value()},
             {symbolNullTermsOwned.tag(), symbolNullTermsOwned.value()}});
}

TEST_F(ValueSerializeForKeyString, SbeObject) {
    sbe::value::TagValueOwned testDataOwned =
        sbe::value::TagValueOwned::fromRaw(sbe::value::makeNewObject());
    auto testData = sbe::value::getObjectView(testDataOwned.value());

    testData->push_back_raw("A", value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    testData->push_back_raw("b", value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    testData->push_back_raw("C", value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));

    runTest({{testDataOwned.tag(), testDataOwned.value()}});
}

TEST_F(ValueSerializeForKeyString, BsonBinData) {
    uint8_t byteArray[] = {8, 7, 6, 5, 4, 3, 2, 1};
    auto bson = BSON_ARRAY(BSONBinData(byteArray, sizeof(byteArray), BinDataGeneral)
                           << BSONBinData(byteArray, sizeof(byteArray), ByteArrayDeprecated));

    sbe::value::TagValueOwned binDataOwned = sbe::value::TagValueOwned::fromRaw(value::copyValue(
        value::TypeTags::bsonBinData, value::bitcastFrom<const char*>(bson[0].value())));

    sbe::value::TagValueOwned binDataDeprecatedOwned =
        sbe::value::TagValueOwned::fromRaw(value::copyValue(
            value::TypeTags::bsonBinData, value::bitcastFrom<const char*>(bson[1].value())));

    runTest({{binDataOwned.tag(), binDataOwned.value()},
             {binDataDeprecatedOwned.tag(), binDataDeprecatedOwned.value()}});
}

TEST_F(ValueSerializeForKeyString, KeyString) {
    key_string::Builder keyStringBuilder(key_string::Version::V1);
    keyStringBuilder.appendNumberLong(1);
    keyStringBuilder.appendNumberLong(2);
    keyStringBuilder.appendNumberLong(3);
    keyStringBuilder.appendString("aaa");
    auto ks = keyStringBuilder.getValueCopy();
    sbe::value::TagValueOwned keyStringOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeKeyString(ks));

    runTest({{keyStringOwned.tag(), keyStringOwned.value()}});
}

TEST_F(ValueSerializeForKeyString, BsonJavaScript) {
    sbe::value::TagValueOwned plainCodeOwned = sbe::value::TagValueOwned::fromRaw(
        value::makeCopyBsonJavascript("function test() { return 'Hello world!'; }"sv));

    sbe::value::TagValueOwned codeWithNullOwned = sbe::value::TagValueOwned::fromRaw(
        value::makeCopyBsonJavascript("function test() { return 'Danger\0us!'; }"sv));

    runTest({{plainCodeOwned.tag(), plainCodeOwned.value()},
             {codeWithNullOwned.tag(), codeWithNullOwned.value()}});
}

TEST_F(ValueSerializeForKeyString, BsonRegex) {
    sbe::value::TagValueOwned noFlagsOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonRegex("[a-z]+"sv, ""sv));

    sbe::value::TagValueOwned withFlagsOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonRegex(".*"sv, "i"sv));

    sbe::value::TagValueOwned empPatterNoFlagsOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonRegex(""sv, ""sv));

    sbe::value::TagValueOwned empPatterWithFlagsOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonRegex(""sv, "s"sv));

    runTest({{noFlagsOwned.tag(), noFlagsOwned.value()},
             {withFlagsOwned.tag(), withFlagsOwned.value()},
             {empPatterNoFlagsOwned.tag(), empPatterNoFlagsOwned.value()},
             {empPatterWithFlagsOwned.tag(), empPatterWithFlagsOwned.value()}});
}

TEST_F(ValueSerializeForKeyString, BsonDBPointer) {
    sbe::value::TagValueOwned dbptrOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonDBPointer(
            "db.c", value::ObjectIdType{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}.data()));

    runTest({{dbptrOwned.tag(), dbptrOwned.value()}});
}

TEST_F(ValueSerializeForKeyString, BsonCodeWScope) {
    sbe::value::TagValueOwned cwsOwned1 =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonCodeWScope(
            "function test() { return 'Hello world!'; }", BSONObj().objdata()));

    sbe::value::TagValueOwned cwsOwned2 =
        sbe::value::TagValueOwned::fromRaw(value::makeNewBsonCodeWScope(
            "function test() { return 'Danger\0us!'; }", BSON("a" << 1).objdata()));

    sbe::value::TagValueOwned cwsOwned3 = sbe::value::TagValueOwned::fromRaw(
        value::makeNewBsonCodeWScope("", BSON("b" << 2 << "c" << BSON_ARRAY(3 << 4)).objdata()));

    runTest({{cwsOwned1.tag(), cwsOwned1.value()},
             {cwsOwned2.tag(), cwsOwned2.value()},
             {cwsOwned3.tag(), cwsOwned3.value()}});
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

// Test that roundtripping through KeyString works for ObjectIdType: ObjectId; bsonObjectId.
TEST_F(ValueSerializeForKeyString, RoundtripObjectIdType) {
    sbe::value::TagValueOwned objectIdOwned =
        sbe::value::TagValueOwned::fromRaw(value::makeNewObjectId());

    auto oid = OID::gen();
    auto obj = BSON("" << oid);
    auto oidStorage = obj.firstElement().value();

    runTest({{objectIdOwned.tag(), objectIdOwned.value()},
             {value::TypeTags::bsonObjectId, value::bitcastFrom<const char*>(oidStorage)}});
}
}  // namespace mongo::sbe
