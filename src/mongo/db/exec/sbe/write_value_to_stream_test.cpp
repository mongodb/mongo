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

/**
 * This file contains tests for sbe::value::writeValueToStream.
 */

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/db/exec/sbe/util/print_options.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/datetime/date_time_support.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"

#include <cstdint>
#include <cstring>
#include <iosfwd>
#include <string>
#include <utility>

constexpr char kStringShort[] = "this is a short string!";
constexpr char kStringLong[] =
    "this is a super duper duper duper duper duper duper duper duper duper duper duper duper duper "
    "duper duper duper duper duper duper duper duper duper duper duper long string!";
constexpr char kCode[] = "function test() { return 'Hello world!'; }";
constexpr char kCodeLong[] =
    "function product(a, b) {console.log(\"Logging a very very very very "
    "very very very very very very very very very very very very very very very very very large "
    "string\");"
    "return a * b; // Function returns the product of a and b }";
constexpr char kPatternShort[] = "^a.*";
constexpr char kPatternLong[] =
    "a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a."
    "a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a"
    ".a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.";
constexpr char kFlag[] = "imxs";

namespace mongo::sbe {

void writeToStream(std::ostream& os, std::pair<value::TypeTags, value::Value> value) {
    os << value;
}

std::pair<value::TypeTags, value::Value> makeNestedArray(size_t depth,
                                                         value::Value arr,
                                                         value::Value topArr) {
    if (depth < 1) {
        return {value::TypeTags::Array, topArr};
    }
    auto [aTag, aVal] = value::makeNewArray();
    auto arrV = value::getArrayView(arr);
    arrV->push_back(aTag, aVal);
    return makeNestedArray(depth - 1, aVal, topArr);
}

std::pair<value::TypeTags, value::Value> makeNestedObject(size_t depth,
                                                          value::Value obj,
                                                          value::Value topObj) {
    if (depth < 1) {
        return {value::TypeTags::Object, topObj};
    }
    auto [oTag, oVal] = value::makeNewObject();
    auto objV = value::getObjectView(obj);
    objV->push_back(std::to_string(depth), oTag, oVal);
    return makeNestedObject(depth - 1, oVal, topObj);
}

std::pair<value::TypeTags, value::Value> makeNestedMultiMap(size_t depth,
                                                            value::Value map,
                                                            value::Value topMap) {
    if (depth < 1) {
        return {value::TypeTags::MultiMap, topMap};
    }
    auto [mTag, mVal] = value::makeNewMultiMap();
    value::getMultiMapView(map)->insert(
        {value::TypeTags::NumberInt64, value::bitcastTo<int64_t>(depth)}, {mTag, mVal});
    return makeNestedMultiMap(depth - 1, mVal, topMap);
}

TEST(WriteValueToStream, ShortBSONBinDataTest) {
    auto bsonString =
        BSON("binData" << BSONBinData(kStringShort, strlen(kStringShort), BinDataGeneral));
    auto val = value::bitcastFrom<const char*>(bsonString["binData"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonBinData, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString =
        "BinData(0, " + hexblob::encode(kStringShort, std::string(kStringShort).length()) + ")";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, LongBSONBinDataTest) {
    auto bsonString =
        BSON("binData" << BSONBinData(kStringLong, strlen(kStringLong), BinDataGeneral));
    auto val = value::bitcastFrom<const char*>(bsonString["binData"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonBinData, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString = "BinData(0, " +
        hexblob::encode(kStringLong, PrintOptions::kDefaultBinDataMaxDisplayLength) + "...)";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, NewUUIDBSONBinDataTest) {
    uint8_t array[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    auto bsonString = BSON("binData" << BSONBinData(array, value::kNewUUIDLength, newUUID));
    auto val = value::bitcastFrom<const char*>(bsonString["binData"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonBinData, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString = "UUID(\"00010203-0405-0607-0809-0a0b0c0d0e0f\")";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, ByteArrayDeprecatedBSONBinDataTest) {
    uint8_t array[] = {0, 1, 2, 3, 4, 5};
    auto bsonString = BSON("binData" << BSONBinData(array, 6, ByteArrayDeprecated));
    auto val = value::bitcastFrom<const char*>(bsonString["binData"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonBinData, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString = "BinData(2, 0405)";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, MalformedByteArrayDeprecatedBSONBinDataTest) {
    uint8_t array[] = {0, 1};
    auto bsonString = BSON("binData" << BSONBinData(array, 2, ByteArrayDeprecated));
    auto val = value::bitcastFrom<const char*>(bsonString["binData"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonBinData, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString = "BinData(2, )";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, ShortStringBigTest) {
    auto [tag, val] = value::makeNewString(kStringShort);
    value::ValueGuard guard{tag, val};
    std::ostringstream oss;
    writeToStream(oss, {tag, val});
    auto expectedString = "\"" + std::string(kStringShort) + "\"";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, LongStringBigTest) {
    auto [tag, val] = value::makeNewString(kStringLong);
    value::ValueGuard guard{tag, val};
    std::ostringstream oss;
    writeToStream(oss, {tag, val});
    auto expectedString = "\"" +
        std::string(kStringLong).substr(0, PrintOptions::kDefaultStringMaxDisplayLength) + "\"" +
        "...";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, BigArrayTest) {
    auto [aTag, aVal] = value::makeNewArray();
    value::ValueGuard aGuard{aTag, aVal};
    auto [sTag, sVal] = value::makeNewString("a");
    value::ValueGuard sGuard{sTag, sVal};
    auto testArr = value::getArrayView(aVal);
    for (size_t i = 0; i < PrintOptions::kDefaultArrayObjectOrNestingMaxDepth + 1; ++i) {
        testArr->push_back(sTag, sVal);
    }
    std::ostringstream oss;
    writeToStream(oss, {value::TypeTags::Array, aVal});
    auto expectedArray = R"(["a", "a", "a", "a", "a", "a", "a", "a", "a", "a", ...])";
    ASSERT_EQUALS(expectedArray, oss.str());
}

TEST(WriteValueToStream, NestedArrayTest) {
    auto [aTag, aVal] = value::makeNewArray();
    auto [tag, val] =
        makeNestedArray(PrintOptions::kDefaultArrayObjectOrNestingMaxDepth, aVal, aVal);
    value::ValueGuard guard{tag, val};
    std::ostringstream oss;
    writeToStream(oss, {tag, val});
    auto expectedArray = "[[[[[[[[[[...]]]]]]]]]]";
    ASSERT_EQUALS(expectedArray, oss.str());
}

TEST(WriteValueToStream, NestedObjectTest) {
    auto [oTag, oVal] = value::makeNewObject();
    auto [tag, val] =
        makeNestedObject(PrintOptions::kDefaultArrayObjectOrNestingMaxDepth, oVal, oVal);
    value::ValueGuard guard{tag, val};
    std::ostringstream oss;
    writeToStream(oss, {tag, val});
    auto expectedObj =
        R"({"10" : {"9" : {"8" : {"7" : {"6" : {"5" : {"4" : {"3" : {"2" : {...}}}}}}}}}})";
    ASSERT_EQUALS(expectedObj, oss.str());
}

TEST(WriteValueToStream, BigArrayInObjectInArrayTest) {
    auto [aTag, aVal] = value::makeNewArray();
    value::ValueGuard aGuard{aTag, aVal};

    auto arrV = value::getArrayView(aVal);
    auto [oTag, oVal] = value::makeNewObject();
    arrV->push_back(oTag, oVal);

    auto [iaTag, iaVal] = value::makeNewArray();
    auto objV = value::getObjectView(oVal);
    objV->push_back("field", iaTag, iaVal);

    auto testArr = value::getArrayView(iaVal);
    auto [sTag, sVal] = value::makeNewString("a");
    for (size_t i = 0; i < PrintOptions::kDefaultArrayObjectOrNestingMaxDepth + 1; ++i) {
        testArr->push_back(sTag, sVal);
    }

    std::ostringstream oss;
    writeToStream(oss, {aTag, aVal});
    auto expectedObj = R"([{"field" : ["a", "a", "a", "a", "a", "a", "a", "a", "a", "a", ...]}])";
    ASSERT_EQUALS(expectedObj, oss.str());
}


TEST(WriteValueToStream, BigObjectInArrayInObjectTest) {
    auto [oTag, oVal] = value::makeNewObject();
    value::ValueGuard oGuard{oTag, oVal};

    auto objV = value::getObjectView(oVal);
    auto [aTag, aVal] = value::makeNewArray();
    objV->push_back("field", aTag, aVal);

    auto [ioTag, ioVal] = value::makeNewObject();
    auto arrV = value::getArrayView(aVal);
    arrV->push_back(ioTag, ioVal);

    auto testObj = value::getObjectView(ioVal);
    auto [sTag, sVal] = value::makeNewString("a");
    for (size_t i = 0; i < PrintOptions::kDefaultArrayObjectOrNestingMaxDepth + 1; ++i) {
        testObj->push_back(std::to_string(i), sTag, sVal);
    }

    std::ostringstream oss;
    writeToStream(oss, {oTag, oVal});
    auto expectedObj =
        R"({"field" : [{"0" : "a", "1" : "a", "2" : "a", "3" : "a", "4" : "a", "5" : "a", "6" : "a",)"
        R"( "7" : "a", "8" : "a", "9" : "a", ...}]})";
    ASSERT_EQUALS(expectedObj, oss.str());
}

TEST(WriteValueToStream, SmallArrayTest) {
    auto [aTag, aVal] = value::makeNewArray();
    value::ValueGuard aGuard{aTag, aVal};
    auto [sTag, sVal] = value::makeNewString("a");
    value::ValueGuard sGuard{sTag, sVal};
    auto testArr = value::getArrayView(aVal);
    for (size_t i = 0; i < PrintOptions::kDefaultArrayObjectOrNestingMaxDepth - 1; ++i) {
        testArr->push_back(sTag, sVal);
    }
    std::ostringstream oss;
    writeToStream(oss, {aTag, aVal});
    auto expectedArray = R"(["a", "a", "a", "a", "a", "a", "a", "a", "a"])";
    ASSERT_EQUALS(expectedArray, oss.str());
}

TEST(WriteValueToStream, BigObjTest) {
    auto [oTag, oVal] = value::makeNewObject();
    value::ValueGuard aGuard{oTag, oVal};
    auto [sTag, sVal] = value::makeNewString("a");
    value::ValueGuard sGuard{sTag, sVal};
    auto testObj = value::getObjectView(oVal);
    for (size_t i = 0; i < PrintOptions::kDefaultArrayObjectOrNestingMaxDepth + 1; ++i) {
        testObj->push_back(std::to_string(i), sTag, sVal);
    }
    std::ostringstream oss;
    writeToStream(oss, {value::TypeTags::Object, oVal});
    auto expectedArray =
        R"({"0" : "a", "1" : "a", "2" : "a", "3" : "a", "4" : "a", "5" : "a", "6" : "a", "7" : "a",)"
        R"( "8" : "a", "9" : "a", ...})";
    ASSERT_EQUALS(expectedArray, oss.str());
}

TEST(WriteValueToStream, SmallObjTest) {
    auto [oTag, oVal] = value::makeNewObject();
    value::ValueGuard aGuard{oTag, oVal};
    auto [sTag, sVal] = value::makeNewString("a");
    value::ValueGuard sGuard{sTag, sVal};
    auto testObj = value::getObjectView(oVal);
    for (size_t i = 0; i < PrintOptions::kDefaultArrayObjectOrNestingMaxDepth - 1; ++i) {
        testObj->push_back(std::to_string(i), sTag, sVal);
    }
    std::ostringstream oss;
    writeToStream(oss, {value::TypeTags::Object, oVal});
    auto expectedArray =
        R"({"0" : "a", "1" : "a", "2" : "a", "3" : "a", "4" : "a", "5" : "a", "6" : "a", "7" : "a",)"
        R"( "8" : "a"})";
    ASSERT_EQUALS(expectedArray, oss.str());
}

TEST(WriteValueToStream, SmallBsonRegex) {
    auto [regTag, regVal] = value::makeNewBsonRegex(kPatternShort, kFlag);
    value::ValueGuard regGuard{regTag, regVal};
    std::ostringstream oss;
    writeToStream(oss, {regTag, regVal});
    auto expectedRegex = "/^a.*/imxs";
    ASSERT_EQUALS(expectedRegex, oss.str());
}

TEST(WriteValueToStream, BigBsonRegex) {
    auto [regTag, regVal] = value::makeNewBsonRegex(kPatternLong, kFlag);
    value::ValueGuard regGuard{regTag, regVal};
    std::ostringstream oss;
    writeToStream(oss, {regTag, regVal});
    auto expectedRegex =
        "/a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a."
        "a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a.a. ... "
        "/imxs";
    ASSERT_EQUALS(expectedRegex, oss.str());
}

TEST(WriteValueToStream, StringSmallTest) {
    auto [tag, val] = value::makeNewString("F");
    value::ValueGuard guard{tag, val};
    std::ostringstream oss;
    writeToStream(oss, {tag, val});
    ASSERT_EQUALS("\"F\"", oss.str());
}

TEST(WriteValueToStream, ShortBSONStringTest) {
    auto bsonString = BSON("string" << kStringShort);
    auto val = value::bitcastFrom<const char*>(bsonString["string"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonString, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString = "\"" + std::string(kStringShort) + "\"";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, LongBSONStringTest) {
    auto bsonString = BSON("string" << kStringLong);
    auto val = value::bitcastFrom<const char*>(bsonString["string"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonString, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString = "\"" +
        std::string(kStringLong).substr(0, PrintOptions::kDefaultStringMaxDisplayLength) + "\"" +
        "...";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, ShortBSONSymbolTest) {
    auto bsonSymbol = BSON("symbol" << BSONSymbol(kStringShort));
    auto val = value::bitcastFrom<const char*>(bsonSymbol["symbol"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonSymbol, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString = "Symbol(\"" + std::string(kStringShort) + "\")";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, LongBSONSymbolTest) {
    auto bsonSymbol = BSON("symbol" << BSONSymbol(kStringLong));
    auto val = value::bitcastFrom<const char*>(bsonSymbol["symbol"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonSymbol, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString = "Symbol(\"" +
        std::string(kStringLong).substr(0, PrintOptions::kDefaultStringMaxDisplayLength) + "\"" +
        "...)";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, BSONCodeTest) {
    auto bsonCode = BSON("code" << BSONCode(kCode));
    auto val = value::bitcastFrom<const char*>(bsonCode["code"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonJavascript, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString = "Javascript(" + std::string(kCode) + ")";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, BSONLongCodeTest) {
    auto bsonCode = BSON("code" << BSONCode(kCodeLong));
    auto val = value::bitcastFrom<const char*>(bsonCode["code"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonJavascript, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString =
        "Javascript(function product(a, b) {console.log(\"Logging a very very very very very "
        "very "
        "very very very very very very very very very very very very very very very large st...)";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, BSONCodeWScopeTest) {
    auto bsonCodeWScope = BSON("cws" << BSONCodeWScope(kCode, BSONObj()));
    auto val = value::bitcastFrom<const char*>(bsonCodeWScope["cws"].value());
    const std::pair<value::TypeTags, value::Value> value(value::TypeTags::bsonCodeWScope, val);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString = "CodeWScope(" + std::string(kCode) + ", {})";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, TimezoneTest) {
    auto timezone = TimeZoneDatabase::utcZone();
    auto value = value::makeCopyTimeZone(timezone);
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString = "TimeZone(UTC)";
    ASSERT_EQUALS(expectedString, oss.str());
    value::releaseValue(value.first, value.second);
}

TEST(WriteValueToStream, LongMultiMapTest) {
    auto [mapTag, mapVal] = value::makeNewMultiMap();
    value::ValueGuard mapGuard{mapTag, mapVal};
    auto [sTag, sVal] = value::makeNewString("a");
    value::ValueGuard sGuard{sTag, sVal};
    auto testMap = value::getMultiMapView(mapVal);
    for (size_t i = 0; i < PrintOptions::kDefaultArrayObjectOrNestingMaxDepth + 1; ++i) {
        testMap->insert({sTag, sVal}, {sTag, sVal});
    }
    std::ostringstream oss;
    writeToStream(oss, {mapTag, mapVal});
    auto expectedArray =
        R"([{k : "a", v : "a"}, {k : "a", v : "a"}, {k : "a", v : "a"}, {k : "a", v : "a"}, {k : "a", v : "a"}, {k : "a", v : "a"}, {k : "a", v : "a"}, {k : "a", v : "a"}, {k : "a", v : "a"}, {k : "a", v : "a"}, ...])";
    ASSERT_EQUALS(expectedArray, oss.str());
}

TEST(WriteValueToStream, NestedMultiMapTest) {
    auto [mTag, mVal] = value::makeNewMultiMap();
    auto [tag, val] =
        makeNestedMultiMap(PrintOptions::kDefaultArrayObjectOrNestingMaxDepth + 1, mVal, mVal);
    value::ValueGuard guard{tag, val};
    std::ostringstream oss;
    writeToStream(oss, {tag, val});
    auto expectedArray =
        "[{k : 11, v : [{k : 10, v : [{k : 9, v : [{k : 8, v : [{k : 7, v : [{k : 6, v : [{k : 5, "
        "v : [{k : 4, v : [{k : 3, v : [{k : 2, v : [...]}]}]}]}]}]}]}]}]}]}]";
    ASSERT_EQUALS(expectedArray, oss.str());
}

TEST(WriteValueToStream, LongKeyString) {
    key_string::Builder builder{key_string::Version::V1};
    builder.appendString(kStringLong);
    const std::pair<value::TypeTags, value::Value> value =
        value::makeKeyString(builder.getValueCopy());
    value::ValueGuard guard{value};
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString =
        "KS("
        "3C74686973206973206120737570657220647570657220647570657220647570657220647570657220647570"
        "657220647570657220647570657220647570657220647570657220647570657220647570...)";
    ASSERT_EQUALS(expectedString, oss.str());
}

TEST(WriteValueToStream, LongRecordId) {
    const std::pair<value::TypeTags, value::Value> value =
        value::makeNewRecordId(kStringLong, static_cast<int32_t>(strlen(kStringLong)));
    value::ValueGuard guard{value};
    std::ostringstream oss;
    writeToStream(oss, value);
    auto expectedString =
        "RecordId("
        "7468697320697320612073757065722064757065722064757065722064757065722064757065722064757065"
        "722064757065722064757065722064757065722064757065722064757065722064757065...)";
    ASSERT_EQUALS(expectedString, oss.str());
}

}  // namespace mongo::sbe
