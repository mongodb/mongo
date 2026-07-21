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
    auto [testDataTag, testDataVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard testDataGuard{testDataTag, testDataVal};
    auto testData = sbe::value::getArrayView(testDataVal);

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

    testData->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    testData->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    testData->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));

    runTest({{testDataTag, testDataVal}});
}

TEST_F(ValueSerializeForKeyString, ArraySet) {
    auto [tag, val] = sbe::value::makeNewArraySet();
    sbe::value::ValueGuard guard{tag, val};
    auto* arraySet = sbe::value::getArraySetView(val);

    arraySet->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    arraySet->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    arraySet->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));

    runTest({{tag, val}});
}

TEST_F(ValueSerializeForKeyString, ArrayMultiSet) {
    auto [tag, val] = sbe::value::makeNewArrayMultiSet();
    sbe::value::ValueGuard guard{tag, val};
    auto* arrayMultiSet = sbe::value::getArrayMultiSetView(val);

    arrayMultiSet->push_back_raw(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    arrayMultiSet->push_back_raw(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(1));
    arrayMultiSet->push_back_raw(value::TypeTags::NumberDouble, value::bitcastFrom<double>(2.0));

    runTest({{tag, val}});
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

    auto aString = "a"sv;
    auto aStringNullTerm = "a\0"sv;
    auto nullTerm = "\0"sv;
    auto nullTerms = "\0\0\0"sv;

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

    testData->push_back_raw("A", value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(1));
    testData->push_back_raw("b", value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(2));
    testData->push_back_raw("C", value::TypeTags::NumberDouble, value::bitcastFrom<double>(3.0));

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
    key_string::Builder keyStringBuilder(key_string::Version::V1);
    keyStringBuilder.appendNumberLong(1);
    keyStringBuilder.appendNumberLong(2);
    keyStringBuilder.appendNumberLong(3);
    keyStringBuilder.appendString("aaa");
    auto ks = keyStringBuilder.getValueCopy();
    auto [keyStringTag, keyStringVal] = value::makeKeyString(ks);
    sbe::value::ValueGuard testGuard{keyStringTag, keyStringVal};

    runTest({{keyStringTag, keyStringVal}});
}

TEST_F(ValueSerializeForKeyString, BsonJavaScript) {
    auto [plainCodeTag, plainCodeVal] =
        value::makeCopyBsonJavascript("function test() { return 'Hello world!'; }"sv);
    sbe::value::ValueGuard testDataGuard{plainCodeTag, plainCodeVal};

    auto [codeWithNullTag, codeWithNullVal] =
        value::makeCopyBsonJavascript("function test() { return 'Danger\0us!'; }"sv);
    sbe::value::ValueGuard testDataGuard2{codeWithNullTag, codeWithNullVal};

    runTest({{plainCodeTag, plainCodeVal}, {codeWithNullTag, codeWithNullVal}});
}

TEST_F(ValueSerializeForKeyString, BsonRegex) {
    auto [noFlagsTag, noFlagsVal] = value::makeNewBsonRegex("[a-z]+"sv, ""sv);
    sbe::value::ValueGuard testDataGuard{noFlagsTag, noFlagsVal};

    auto [withFlagsTag, withFlagsVal] = value::makeNewBsonRegex(".*"sv, "i"sv);
    sbe::value::ValueGuard testDataGuard2{withFlagsTag, withFlagsVal};

    auto [empPatterNoFlagsTag, empPatterNoFlagsVal] = value::makeNewBsonRegex(""sv, ""sv);
    sbe::value::ValueGuard testDataGuard3{empPatterNoFlagsTag, empPatterNoFlagsVal};

    auto [empPatterWithFlagsTag, empPatterWithFlagsVal] = value::makeNewBsonRegex(""sv, "s"sv);
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

// Test that roundtripping through KeyString works for ObjectIdType: ObjectId; bsonObjectId.
TEST_F(ValueSerializeForKeyString, RoundtripObjectIdType) {
    auto [objectIdTag, objectIdVal] = value::makeNewObjectId();

    auto oid = OID::gen();
    auto obj = BSON("" << oid);
    auto oidStorage = obj.firstElement().value();

    sbe::value::ValueGuard testDataGuard{objectIdTag, objectIdVal};
    runTest({{objectIdTag, objectIdVal},
             {value::TypeTags::bsonObjectId, value::bitcastFrom<const char*>(oidStorage)}});
}
}  // namespace mongo::sbe
