// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
