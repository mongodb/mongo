/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/query/compiler/parsers/matcher/matcher_type_set_parser.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/unittest.h"

#include <cmath>
#include <limits>

namespace mongo::parsers::matcher {

TEST(MatcherTypeSetParserTest, ParseFromElementCanParseNumberAlias) {
    auto obj = BSON("" << "number");
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().allNumbers);
    ASSERT_TRUE(result.getValue().bsonTypes.empty());
}

TEST(MatcherTypeSetParserTest, ParseFromElementCanParseLongAlias) {
    auto obj = BSON("" << "long");
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonTypes.size(), 1u);
    ASSERT_TRUE(result.getValue().hasType(BSONType::numberLong));
}

TEST(MatcherTypeSetParserTest, ParseFromElementFailsToParseUnknownAlias) {
    auto obj = BSON("" << "unknown");
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFromElementFailsToParseWrongElementType) {
    auto obj = BSON("" << BSON("" << ""));
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());

    obj = fromjson("{'': null}");
    result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFromElementFailsToParseUnknownBSONType) {
    auto obj = BSON("" << 99);
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFromElementFailsToParseEOOTypeCode) {
    auto obj = BSON("" << 0);
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "Invalid numerical type code: 0. Instead use {$exists:false}.");
}

TEST(MatcherTypeSetParserTest, ParseFromElementFailsToParseEOOTypeName) {
    auto obj = BSON("" << "missing");
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
    ASSERT_EQ(result.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.getStatus().reason(),
              "'missing' is not a legal type name. "
              "To query for non-existence of a field, use {$exists:false}.");
}


TEST(MatcherTypeSetParserTest, ParseFromElementCanParseRoundDoubleTypeCode) {
    auto obj = BSON("" << 2.0);
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonTypes.size(), 1u);
    ASSERT_TRUE(result.getValue().hasType(BSONType::string));
}

TEST(MatcherTypeSetParserTest, ParseFailsWhenElementIsNonRoundDoubleTypeCode) {
    auto obj = BSON("" << 2.5);
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFromElementCanParseRoundDecimalTypeCode) {
    auto obj = BSON("" << Decimal128(2));
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonTypes.size(), 1U);
    ASSERT_TRUE(result.getValue().hasType(BSONType::string));
}

TEST(MatcherTypeSetParserTest, ParseFailsWhenElementIsNonRoundDecimalTypeCode) {
    auto obj = BSON("" << Decimal128(2.5));
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFailsWhenDoubleElementIsTooPositiveForInteger) {
    double doubleTooLarge = scalbn(1, std::numeric_limits<long long>::digits);
    auto obj = BSON("" << doubleTooLarge);
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFailsWhenDoubleElementIsTooNegativeForInteger) {
    double doubleTooNegative = std::numeric_limits<double>::lowest();
    auto obj = BSON("" << doubleTooNegative);
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFailsWhenDoubleElementIsNaN) {
    auto obj = BSON("" << std::nan(""));
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFailsWhenDoubleElementIsInfinite) {
    auto obj = BSON("" << std::numeric_limits<double>::infinity());
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFailsWhenDecimalElementIsTooPositiveForInteger) {
    auto obj = BSON("" << Decimal128(static_cast<int64_t>(std::numeric_limits<int>::max()) + 1));
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFailsWhenDecimalElementIsTooNegativeForInteger) {
    auto obj = BSON("" << Decimal128(static_cast<int64_t>(std::numeric_limits<int>::min()) - 1));
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFailsWhenDecimalElementIsNaN) {
    auto obj = BSON("" << Decimal128::kPositiveNaN);
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFailsWhenDecimalElementIsInfinite) {
    auto obj = BSON("" << Decimal128::kPositiveInfinity);
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFromElementFailsWhenArrayHasUnknownType) {
    auto obj = BSON("" << BSON_ARRAY("long" << "unknown"));
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFailsWhenArrayElementIsNotStringOrNumber) {
    auto obj = BSON("" << BSON_ARRAY("long" << BSONObj()));
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_NOT_OK(result.getStatus());
}

TEST(MatcherTypeSetParserTest, ParseFromElementCanParseEmptyArray) {
    auto obj = BSON("" << BSONArray());
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().isEmpty());
}

TEST(MatcherTypeSetParserTest, ParseFromElementCanParseArrayWithSingleType) {
    auto obj = BSON("" << BSON_ARRAY("string"));
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_FALSE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonTypes.size(), 1u);
    ASSERT_TRUE(result.getValue().hasType(BSONType::string));
}

TEST(MatcherTypeSetParserTest, ParseFromElementCanParseArrayWithMultipleTypes) {
    auto obj = BSON("" << BSON_ARRAY("string" << 3 << "number"));
    auto result = parsers::matcher::parseMatcherTypeSet(obj.firstElement());
    ASSERT_OK(result.getStatus());
    ASSERT_TRUE(result.getValue().allNumbers);
    ASSERT_EQ(result.getValue().bsonTypes.size(), 2u);
    ASSERT_TRUE(result.getValue().hasType(BSONType::string));
    ASSERT_TRUE(result.getValue().hasType(BSONType::object));
}

}  // namespace mongo::parsers::matcher
