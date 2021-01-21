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

#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/hex.h"

constexpr char kStringShort[] = "this is a short string!";
constexpr char kStringLong[] =
    "this is a super duper duper duper duper duper duper duper duper duper duper duper duper duper "
    "duper duper duper duper duper duper duper duper duper duper duper long string!";

namespace mongo::sbe {

void writeToStream(std::ostream& os, std::pair<value::TypeTags, value::Value> value) {
    os << value;
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
    auto expectedString =
        "BinData(0, " + hexblob::encode(kStringLong, value::kBinDataMaxDisplayLength) + "...)";
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
    auto expectedString =
        "\"" + std::string(kStringLong).substr(0, value::kStringMaxDisplayLength) + "\"" + "...";
    ASSERT_EQUALS(expectedString, oss.str());
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
    auto expectedString =
        "\"" + std::string(kStringLong).substr(0, value::kStringMaxDisplayLength) + "\"" + "...";
    ASSERT_EQUALS(expectedString, oss.str());
}

}  // namespace mongo::sbe
