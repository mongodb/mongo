// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/util/jparse_util.h"

#include "mongo/bson/bson_validate.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo {
void parseFuzzerJsonAndAssertEq(const char* jsonStr, BSONObj expected) {
    auto bob = fromFuzzerJson(jsonStr);
    ASSERT_TRUE(validateBSON(bob).isOK());
    ASSERT_BSONOBJ_EQ(bob, expected);
}

Status tryParseFuzzerJsonAndAssertFail(const char* jsonStr) {
    try {
        fromFuzzerJson(jsonStr);
        return Status::OK();
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

TEST(JParseUtilTest, TrailingCommasInObject) {
    auto jsonStr = "{$match: {$and: [{a: 1}, {b: 2}]},}";
    BSONObj expected =
        BSON("$match" << BSON("$and" << BSON_ARRAY(BSON("a" << 1) << BSON("b" << 2))));
    parseFuzzerJsonAndAssertEq(jsonStr, expected);
}

TEST(JParseUtilTest, TrailingCommaInArrayFailsToParse) {
    auto jsonStr = "{$match: {$and: [{a: 1}, {b: 2}, ]}}";
    auto status = tryParseFuzzerJsonAndAssertFail(jsonStr);
    ASSERT_EQ(9180302, status.code());
    ASSERT_STRING_CONTAINS(status.reason(),
                           "code 9: FailedToParse: Attempted to parse a number array element, not "
                           "recognizing any other keywords");
}

TEST(JParseUtilTest, RegexForwardSlash) {
    auto jsonStr =
        "{$regexFindAll: {input: \"$obj.obj.obj.obj.str\", regex: /transparent|Forward/, options: "
        "\"\"}}";
    BSONObj expected = BSON(
        "$regexFindAll" << BSON("input" << "$obj.obj.obj.obj.str"
                                        << "regex" << BSONRegEx("transparent|Forward") << "options"
                                        << ""));
    parseFuzzerJsonAndAssertEq(jsonStr, expected);
}

TEST(JParseUtilTest, RegexForwardSlashWithOptions) {
    auto jsonStr =
        "{$regexFindAll: {input: \"$obj.obj.obj.obj.str\", regex: /transparent|Forward/, options: "
        "\"\"}}";
    BSONObj expected = BSON(
        "$regexFindAll" << BSON("input" << "$obj.obj.obj.obj.str"
                                        << "regex" << BSONRegEx("transparent|Forward") << "options"
                                        << ""));
    parseFuzzerJsonAndAssertEq(jsonStr, expected);
}

TEST(JParseUtilTest, InvalidRegexFailsToParse) {
    auto jsonStr =
        "{$regexFindAll: {input: \"$obj.obj.obj.obj.str\", regex: /transparent|Forward, options: "
        "\"i\"}}";
    auto status = tryParseFuzzerJsonAndAssertFail(jsonStr);
    ASSERT_EQ(9180302, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "code 9: FailedToParse: Unexpected end of input");
}

TEST(JParseUtilTest, IsoDate) {
    auto jsonStr =
        "{$toDate: {$max: [ISODate(\"0001-01-01T00:00:00Z\"), "
        "ISODate(\"2019-10-23T08:58:17.868Z\")]}}";
    BSONObj expected = BSON(
        "$toDate" << BSON("$max" << BSON_ARRAY(Date_t::fromMillisSinceEpoch(-62135596800000)
                                               << Date_t::fromMillisSinceEpoch(1571821097868))));
    parseFuzzerJsonAndAssertEq(jsonStr, expected);
}

TEST(JParseUtilTest, IsoDateInNewDate) {
    auto jsonStr = "{$match: {dateField: {$lt: new Date(\"2019-10-23T08:58:17.868Z\")}}}";
    BSONObj expected =
        BSON("$match" << BSON("dateField"
                              << BSON("$lt" << Date_t::fromMillisSinceEpoch(1571821097868))));
    parseFuzzerJsonAndAssertEq(jsonStr, expected);
}

TEST(JParseUtilTest, InvalidDateFailsToParse) {
    auto jsonStr = "{$match: {dateField: {$lt: new Date(\"1970-01-01T00:00:00.0.0Z\")}}}";
    auto status = tryParseFuzzerJsonAndAssertFail(jsonStr);
    ASSERT_EQ(9180302, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "code 9: FailedToParse: Expecting '}' or ','");
}

TEST(JParseUtilTest, NumberLongInDoubleQuotations) {
    auto jsonStr = "{$match: {num: {$gt: NumberLong(\"123456789\")}}}";
    BSONObj expected = BSON("$match" << BSON("num" << BSON("$gt" << 123456789)));
    parseFuzzerJsonAndAssertEq(jsonStr, expected);
}

TEST(JParseUtilTest, MismatchedStringDelimitersFailToParse) {
    auto jsonStr = "{$match: {num: {$gt: NumberLong(\"123456789')}}}";
    auto status = tryParseFuzzerJsonAndAssertFail(jsonStr);
    ASSERT_EQ(9180302, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "code 9: FailedToParse: Expecting \"");
}

TEST(JParseUtilTest, NumberLongInSingleQuotationsFailToParse) {
    auto jsonStr = "{$match: {num: {$gt: NumberLong('123456789')}}}";
    auto status = tryParseFuzzerJsonAndAssertFail(jsonStr);
    ASSERT_EQ(9180302, status.code());
    ASSERT_STRING_CONTAINS(status.reason(), "Bad character is in this snippet: \"berLong('");
}

TEST(JParseUtilTest, NumberConversionTestNumToDate) {
    auto jsonStr = "{$match: {num: {$convert: {input: {$literal: -314159295}, to: 9}}}}";
    BSONObj expected =
        BSON("$match" << BSON("num"
                              << BSON("$convert" << BSON("input" << BSON("$literal" << -314159295.0)
                                                                 << "to" << 9.0))));
    parseFuzzerJsonAndAssertEq(jsonStr, expected);
}

TEST(JParseUtilTest, NumberConversionTestRoundNumToDate) {
    auto jsonStr = "{$match: {num: {$convert: {input: {$round: [0, 5]}, to: \"date\"}}}}";
    BSONObj expected =
        BSON("$match" << BSON(
                 "num" << BSON("$convert"
                               << BSON("input" << BSON("$round" << BSON_ARRAY(0.0 << 5.0)) << "to"
                                               << "date"))));
    parseFuzzerJsonAndAssertEq(jsonStr, expected);
}
}  // namespace mongo
