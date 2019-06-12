
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace ExpressionDateFromPartsTest {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionDateFromPartsTest = AggregationContextFixture;

TEST_F(ExpressionDateFromPartsTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();

    // Test that it serializes to the full format if given an object specification.
    BSONObj spec =
        BSON("$dateFromParts" << BSON(
                 "year" << 2017 << "month" << 6 << "day" << 27 << "hour" << 14 << "minute" << 37
                        << "second"
                        << 15
                        << "millisecond"
                        << 414
                        << "timezone"
                        << "America/Los_Angeles"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto expectedSerialization =
        Value(Document{{"$dateFromParts",
                        Document{{"year", Document{{"$const", 2017}}},
                                 {"month", Document{{"$const", 6}}},
                                 {"day", Document{{"$const", 27}}},
                                 {"hour", Document{{"$const", 14}}},
                                 {"minute", Document{{"$const", 37}}},
                                 {"second", Document{{"$const", 15}}},
                                 {"millisecond", Document{{"$const", 414}}},
                                 {"timezone", Document{{"$const", "America/Los_Angeles"_sd}}}}}});
    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);
}

TEST_F(ExpressionDateFromPartsTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();
    auto spec = BSON("$dateFromParts" << BSON("year" << 2017));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both year, month and day are provided, and are both
    // constants.
    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "month" << 6 << "day" << 27));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both year, hour and minute are provided, and are both
    // expressions which evaluate to constants.
    spec = BSON("$dateFromParts" << BSON("year" << BSON("$add" << BSON_ARRAY(1900 << 107)) << "hour"
                                                << BSON("$add" << BSON_ARRAY(13 << 1))
                                                << "minute"
                                                << BSON("$add" << BSON_ARRAY(40 << 3))));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both year and milliseconds are provided, and year is an
    // expressions which evaluate to a constant, with milliseconds a constant
    spec = BSON("$dateFromParts" << BSON(
                    "year" << BSON("$add" << BSON_ARRAY(1900 << 107)) << "millisecond" << 514));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both isoWeekYear, and isoWeek are provided, and are both
    // constants.
    spec = BSON("$dateFromParts" << BSON("isoWeekYear" << 2017 << "isoWeek" << 26));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both isoWeekYear, isoWeek and isoDayOfWeek are provided,
    // and are both expressions which evaluate to constants.
    spec = BSON("$dateFromParts" << BSON("isoWeekYear" << BSON("$add" << BSON_ARRAY(1017 << 1000))
                                                       << "isoWeek"
                                                       << BSON("$add" << BSON_ARRAY(20 << 6))
                                                       << "isoDayOfWeek"
                                                       << BSON("$add" << BSON_ARRAY(3 << 2))));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both year and month are provided, but
    // year is not a constant.
    spec = BSON("$dateFromParts" << BSON("year"
                                         << "$year"
                                         << "month"
                                         << 6));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both year and day are provided, but
    // day is not a constant.
    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "day"
                                                << "$day"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both isoWeekYear and isoDayOfWeek are provided,
    // but isoDayOfWeek is not a constant.
    spec = BSON("$dateFromParts" << BSON("isoWeekYear" << 2017 << "isoDayOfWeek"
                                                       << "$isoDayOfWeekday"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
}

TEST_F(ExpressionDateFromPartsTest, TestThatOutOfRangeValuesRollOver) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromParts" << BSON("year" << 2017 << "month" << -1));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto dateVal = Date_t::fromMillisSinceEpoch(1477958400000);  // 11/1/2016 in ms.
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "day" << -1));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(1483056000000);  // 12/30/2016
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "hour" << 25));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(1483318800000);  // 1/2/2017 01:00:00
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "minute" << 61));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(1483232460000);  // 1/1/2017 01:01:00
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "second" << 61));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(1483228861000);  // 1/1/2017 00:01:01
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));
}

}  // namespace ExpressionDateFromPartsTest

namespace ExpressionDateToPartsTest {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionDateToPartsTest = AggregationContextFixture;

TEST_F(ExpressionDateToPartsTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();

    // Test that it serializes to the full format if given an object specification.
    BSONObj spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "timezone"
                                                      << "Europe/London"
                                                      << "iso8601"
                                                      << false));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto expectedSerialization =
        Value(Document{{"$dateToParts",
                        Document{{"date", Document{{"$const", Date_t{}}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}},
                                 {"iso8601", Document{{"$const", false}}}}}});
    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);
}

TEST_F(ExpressionDateToPartsTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();
    auto spec = BSON("$dateToParts" << BSON("date" << Date_t{}));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and timezone are provided, and are both
    // constants.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "timezone"
                                              << "UTC"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and timezone are provided, and are both
    // expressions which evaluate to constants.
    spec = BSON("$dateToParts" << BSON("date" << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))
                                              << "timezone"
                                              << BSON("$concat" << BSON_ARRAY("Europe"
                                                                              << "/"
                                                                              << "London"))));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and iso8601 are provided, and are both
    // constants.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "iso8601" << true));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and iso8601 are provided, and are both
    // expressions which evaluate to constants.
    spec = BSON("$dateToParts" << BSON("date" << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))
                                              << "iso8601"
                                              << BSON("$not" << false)));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both date and timezone are provided, but
    // date is not a constant.
    spec = BSON("$dateToParts" << BSON("date"
                                       << "$date"
                                       << "timezone"
                                       << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both date and timezone are provided, but
    // timezone is not a constant.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "timezone"
                                              << "$tz"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both date and iso8601 are provided, but
    // iso8601 is not a constant.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "iso8601"
                                              << "$iso8601"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
}

}  // namespace ExpressionDateToPartsTest

namespace DateExpressionsTest {

std::vector<StringData> dateExpressions = {"$year"_sd,
                                           "$isoWeekYear"_sd,
                                           "$month"_sd,
                                           "$dayOfMonth"_sd,
                                           "$hour"_sd,
                                           "$minute"_sd,
                                           "$second"_sd,
                                           "$millisecond"_sd,
                                           "$week"_sd,
                                           "$isoWeek"_sd,
                                           "$dayOfYear"_sd};

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using DateExpressionTest = AggregationContextFixture;

TEST_F(DateExpressionTest, ParsingAcceptsAllFormats) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        auto possibleSyntaxes = {
            // Single argument.
            BSON(expName << Date_t{}),
            BSON(expName << "$date"),
            BSON(expName << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))),
            // Single argument wrapped in an array.
            BSON(expName << BSON_ARRAY("$date")),
            BSON(expName << BSON_ARRAY(Date_t{})),
            BSON(expName << BSON_ARRAY(BSON("$add" << BSON_ARRAY(Date_t{} << 1000)))),
            // Object literal syntax.
            BSON(expName << BSON("date" << Date_t{})),
            BSON(expName << BSON("date"
                                 << "$date")),
            BSON(expName << BSON("date" << BSON("$add" << BSON_ARRAY("$date" << 1000)))),
            BSON(expName << BSON("date" << Date_t{} << "timezone"
                                        << "Europe/London")),
            BSON(expName << BSON("date" << Date_t{} << "timezone"
                                        << "$tz"))};
        for (auto&& syntax : possibleSyntaxes) {
            Expression::parseExpression(expCtx, syntax, expCtx->variablesParseState);
        }
    }
}

TEST_F(DateExpressionTest, ParsingRejectsUnrecognizedFieldsInObjectSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "Europe/London"
                                                   << "extra"
                                                   << 4));
        ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                           AssertionException,
                           40535);
    }
}

TEST_F(DateExpressionTest, ParsingRejectsEmptyObjectSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSONObj());
        ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                           AssertionException,
                           40539);
    }
}

TEST_F(DateExpressionTest, RejectsEmptyArray) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSONArray());
        // It will parse as an ExpressionArray, and fail at runtime.
        ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                           AssertionException,
                           40536);
    }
}

TEST_F(DateExpressionTest, RejectsArraysWithMoreThanOneElement) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON_ARRAY("$date"
                                                  << "$tz"));
        // It will parse as an ExpressionArray, and fail at runtime.
        ASSERT_THROWS_CODE(Expression::parseExpression(expCtx, spec, expCtx->variablesParseState),
                           AssertionException,
                           40536);
    }
}

TEST_F(DateExpressionTest, RejectsArraysWithinObjectSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << BSON_ARRAY(Date_t{}) << "timezone"
                                                   << "Europe/London"));
        // It will parse as an ExpressionArray, and fail at runtime.
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"_id", 0}};
        ASSERT_THROWS_CODE(
            dateExp->evaluate(contextDoc, &expCtx->variables), AssertionException, 16006);

        // Test that it rejects an array for the timezone option.
        spec =
            BSON(expName << BSON("date" << Date_t{} << "timezone" << BSON_ARRAY("Europe/London")));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        contextDoc = Document{{"_id", 0}};
        ASSERT_THROWS_CODE(
            dateExp->evaluate(contextDoc, &expCtx->variables), AssertionException, 40533);
    }
}

TEST_F(DateExpressionTest, RejectsTypesThatCannotCoerceToDate) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << "$stringField");
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"stringField", "string"_sd}};
        ASSERT_THROWS_CODE(
            dateExp->evaluate(contextDoc, &expCtx->variables), AssertionException, 16006);
    }
}

TEST_F(DateExpressionTest, AcceptsObjectIds) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << "$oid");
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"oid", OID::gen()}};
        dateExp->evaluate(contextDoc, &expCtx->variables);  // Should not throw.
    }
}

TEST_F(DateExpressionTest, AcceptsTimestamps) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << "$ts");
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"ts", Timestamp{Date_t{}}}};
        dateExp->evaluate(contextDoc, &expCtx->variables);  // Should not throw.
    }
}

TEST_F(DateExpressionTest, RejectsNonStringTimezone) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "$intField"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"intField", 4}};
        ASSERT_THROWS_CODE(
            dateExp->evaluate(contextDoc, &expCtx->variables), AssertionException, 40533);
    }
}

TEST_F(DateExpressionTest, RejectsUnrecognizedTimeZoneSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "UNRECOGNIZED!"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"_id", 0}};
        ASSERT_THROWS_CODE(
            dateExp->evaluate(contextDoc, &expCtx->variables), AssertionException, 40485);
    }
}

TEST_F(DateExpressionTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        // Test that it serializes to the full format if given an object specification.
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "Europe/London"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto expectedSerialization =
            Value(Document{{expName,
                            Document{{"date", Document{{"$const", Date_t{}}}},
                                     {"timezone", Document{{"$const", "Europe/London"_sd}}}}}});
        ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
        ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);

        // Test that it serializes to the full format if given a date.
        spec = BSON(expName << Date_t{});
        expectedSerialization =
            Value(Document{{expName, Document{{"date", Document{{"$const", Date_t{}}}}}}});
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
        ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);

        // Test that it serializes to the full format if given a date within an array.
        spec = BSON(expName << BSON_ARRAY(Date_t{}));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
        ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);
    }
}

TEST_F(DateExpressionTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        // Test that it becomes a constant if only date is provided, and it is constant.
        auto spec = BSON(expName << BSON("date" << Date_t{}));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it becomes a constant if both date and timezone are provided, and are both
        // constants.
        spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                           << "Europe/London"));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it becomes a constant if both date and timezone are provided, and are both
        // expressions which evaluate to constants.
        spec = BSON(expName << BSON("date" << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))
                                           << "timezone"
                                           << BSON("$concat" << BSON_ARRAY("Europe"
                                                                           << "/"
                                                                           << "London"))));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it does *not* become a constant if both date and timezone are provided, but
        // date is not a constant.
        spec = BSON(expName << BSON("date"
                                    << "$date"
                                    << "timezone"
                                    << "Europe/London"));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it does *not* become a constant if both date and timezone are provided, but
        // timezone is not a constant.
        spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                           << "$tz"));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
    }
}

TEST_F(DateExpressionTest, DoesRespectTimeZone) {
    // Make sure they each successfully evaluate with a different TimeZone.
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        auto spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                << "America/New_York"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"_id", 0}};
        dateExp->evaluate(contextDoc, &expCtx->variables);  // Should not throw.
    }

    // Make sure the time zone is used during evaluation.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);  // 2017-06-06T19:38:43:234Z.
    auto specWithoutTimezone = BSON("$hour" << BSON("date" << date));
    auto hourWithoutTimezone =
        Expression::parseExpression(expCtx, specWithoutTimezone, expCtx->variablesParseState)
            ->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_EQ(hourWithoutTimezone, Value(19));

    auto specWithTimezone = BSON("$hour" << BSON("date" << date << "timezone"
                                                        << "America/New_York"));
    auto hourWithTimezone =
        Expression::parseExpression(expCtx, specWithTimezone, expCtx->variablesParseState)
            ->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_EQ(hourWithTimezone, Value(15));
}

TEST_F(DateExpressionTest, DoesResultInNullIfGivenNullishInput) {
    // Make sure they each successfully evaluate with a different TimeZone.
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        auto contextDoc = Document{{"_id", 0}};

        // Test that the expression results in null if the date is nullish and the timezone is not
        // specified.
        auto spec = BSON(expName << BSON("date"
                                         << "$missing"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        spec = BSON(expName << BSON("date" << BSONNULL));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        spec = BSON(expName << BSON("date" << BSONUndefined));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        // Test that the expression results in null if the date is present but the timezone is
        // nullish.
        spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                           << "$missing"));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        spec = BSON(expName << BSON("date" << Date_t{} << "timezone" << BSONNULL));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        spec = BSON(expName << BSON("date" << Date_t{} << "timezone" << BSONUndefined));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        // Test that the expression results in null if the date and timezone both nullish.
        spec = BSON(expName << BSON("date"
                                    << "$missing"
                                    << "timezone"
                                    << BSONUndefined));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        // Test that the expression results in null if the date is nullish and timezone is present.
        spec = BSON(expName << BSON("date"
                                    << "$missing"
                                    << "timezone"
                                    << "Europe/London"));
        dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));
    }
}

}  // namespace DateExpressionsTest

namespace ExpressionDateToStringTest {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionDateToStringTest = AggregationContextFixture;

TEST_F(ExpressionDateToStringTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();

    // Test that it serializes to the full format if given an object specification.
    BSONObj spec = BSON("$dateToString" << BSON("date" << Date_t{} << "timezone"
                                                       << "Europe/London"
                                                       << "format"
                                                       << "%Y-%m-%d"
                                                       << "onNull"
                                                       << "nullDefault"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto expectedSerialization =
        Value(Document{{"$dateToString",
                        Document{{"date", Document{{"$const", Date_t{}}}},
                                 {"format", Document{{"$const", "%Y-%m-%d"_sd}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}},
                                 {"onNull", Document{{"$const", "nullDefault"_sd}}}}}});

    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);
}

TEST_F(ExpressionDateToStringTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();

    // Test that it becomes a constant if date is constant, and both format and timezone are
    // missing.
    auto spec = BSON("$dateToString" << BSON("date" << Date_t{}));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both format and date are constant, and timezone is
    // missing.
    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << Date_t{}));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if format, date and timezone are provided, and all are
    // constants.
    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << Date_t{}
                                        << "timezone"
                                        << "Europe/Amsterdam"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if format, date and timezone are provided, and all
    // expressions which evaluate to constants.
    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m%d"
                                        << "date"
                                        << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))
                                        << "timezone"
                                        << BSON("$concat" << BSON_ARRAY("Europe"
                                                                        << "/"
                                                                        << "London"))));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if all parameters are constant, including the optional
    // 'onNull'.
    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << Date_t{}
                                        << "timezone"
                                        << "Europe/Amsterdam"
                                        << "onNull"
                                        << "null default"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both format, date and timezone are provided, but
    // date is not a constant.
    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << "$date"
                                        << "timezone"
                                        << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both format, date and timezone are provided, but
    // timezone is not a constant.
    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << Date_t{}
                                        << "timezone"
                                        << "$tz"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if 'onNull' does not evaluate to a constant.
    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << Date_t{}
                                        << "onNull"
                                        << "$onNull"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if 'format' does not evaluate to a constant.
    spec = BSON("$dateToString" << BSON("format"
                                        << "$format"
                                        << "date"
                                        << Date_t{}));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
}

TEST_F(ExpressionDateToStringTest, ReturnsOnNullValueWhenInputIsNullish) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateToString" << BSON("format"
                                             << "%Y-%m-%d"
                                             << "date"
                                             << BSONNULL
                                             << "onNull"
                                             << "null default"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("null default"_sd), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << BSONNULL
                                        << "onNull"
                                        << BSONNULL));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << "$missing"
                                        << "onNull"
                                        << "null default"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("null default"_sd), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateToString" << BSON("format"
                                        << "%Y-%m-%d"
                                        << "date"
                                        << "$missing"
                                        << "onNull"
                                        << "$alsoMissing"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionDateToStringTest, ReturnsNullIfInputDateIsNullish) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$dateToString: {date: '$date'}}");
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL),
                    dateExp->evaluate(Document{{"date", BSONNULL}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionDateToStringTest, ReturnsNullIfFormatIsNullish) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$dateToString: {date: '$date', format: '$format'}}");
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(
        Value(BSONNULL),
        dateExp->evaluate(Document{{"date", Date_t{}}, {"format", BSONNULL}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(BSONNULL),
                    dateExp->evaluate(Document{{"date", Date_t{}}}, &expCtx->variables));
}

TEST_F(ExpressionDateToStringTest, UsesDefaultFormatIfNoneSpecified) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$dateToString: {date: '$date'}}");
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("1970-01-01T00:00:00.000Z"_sd),
                    dateExp->evaluate(Document{{"date", Date_t{}}}, &expCtx->variables));
}

TEST_F(ExpressionDateToStringTest, FailsForInvalidTimezoneRegardlessOfInputDate) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$dateToString: {date: '$date', timezone: '$tz'}}");
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(
        dateExp->evaluate(Document{{"date", BSONNULL}, {"tz", "invalid"_sd}}, &expCtx->variables),
        AssertionException,
        40485);
    ASSERT_THROWS_CODE(
        dateExp->evaluate(Document{{"date", BSONNULL}, {"tz", 5}}, &expCtx->variables),
        AssertionException,
        40517);
}

TEST_F(ExpressionDateToStringTest, FailsForInvalidFormatStrings) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateToString" << BSON("date" << Date_t{} << "format"
                                                    << "%n"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 18536);

    spec = BSON("$dateToString" << BSON("date" << Date_t{} << "format"
                                               << "%Y%"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 18535);
}

TEST_F(ExpressionDateToStringTest, FailsForInvalidFormatRegardlessOfInputDate) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$dateToString: {date: '$date', format: '$format', onNull: 0}}");
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(
        dateExp->evaluate(Document{{"date", BSONNULL}, {"format", 5}}, &expCtx->variables),
        AssertionException,
        18533);
    ASSERT_THROWS_CODE(
        dateExp->evaluate(Document{{"date", BSONNULL}, {"format", "%n"_sd}}, &expCtx->variables),
        AssertionException,
        18536);
    ASSERT_THROWS_CODE(
        dateExp->evaluate(Document{{"date", BSONNULL}, {"format", "%"_sd}}, &expCtx->variables),
        AssertionException,
        18535);
    ASSERT_THROWS_CODE(
        dateExp->evaluate(Document{{"date", "Invalid date"_sd}, {"format", 5}}, &expCtx->variables),
        AssertionException,
        18533);
}

}  // namespace ExpressionDateToStringTest

namespace ExpressionDateFromStringTest {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionDateFromStringTest = AggregationContextFixture;

TEST_F(ExpressionDateFromStringTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();

    // Test that it serializes to the full format if given an object specification.
    BSONObj spec = BSON("$dateFromString" << BSON("dateString"
                                                  << "2017-07-04T13:06:44Z"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    auto expectedSerialization = Value(
        Document{{"$dateFromString",
                  Document{{"dateString", Document{{"$const", "2017-07-04T13:06:44Z"_sd}}}}}});

    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);

    // Test that it serializes to the full format if given an object specification.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:06:44Z"
                                          << "timezone"
                                          << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    expectedSerialization =
        Value(Document{{"$dateFromString",
                        Document{{"dateString", Document{{"$const", "2017-07-04T13:06:44Z"_sd}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}}}}});

    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:06:44Z"
                                          << "timezone"
                                          << "Europe/London"
                                          << "format"
                                          << "%Y-%d-%mT%H:%M:%S"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    expectedSerialization =
        Value(Document{{"$dateFromString",
                        Document{{"dateString", Document{{"$const", "2017-07-04T13:06:44Z"_sd}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}},
                                 {"format", Document{{"$const", "%Y-%d-%mT%H:%M:%S"_sd}}}}}});

    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:06:44Z"
                                          << "timezone"
                                          << "Europe/London"
                                          << "format"
                                          << "%Y-%d-%mT%H:%M:%S"
                                          << "onNull"
                                          << "nullDefault"
                                          << "onError"
                                          << "errorDefault"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    expectedSerialization =
        Value(Document{{"$dateFromString",
                        Document{{"dateString", Document{{"$const", "2017-07-04T13:06:44Z"_sd}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}},
                                 {"format", Document{{"$const", "%Y-%d-%mT%H:%M:%S"_sd}}},
                                 {"onNull", Document{{"$const", "nullDefault"_sd}}},
                                 {"onError", Document{{"$const", "errorDefault"_sd}}}}}});

    ASSERT_VALUE_EQ(dateExp->serialize(true), expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(false), expectedSerialization);
}

TEST_F(ExpressionDateFromStringTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();

    // Test that it becomes a constant if all parameters evaluate to a constant value.
    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2017-07-04T13:09:57Z"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    Date_t dateVal = Date_t::fromMillisSinceEpoch(1499173797000);
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:09:57"
                                          << "timezone"
                                          << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant with the dateString, timezone, and format being a constant.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:09:57"
                                          << "timezone"
                                          << "Europe/London"
                                          << "format"
                                          << "%Y-%m-%dT%H:%M:%S"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    dateVal = Date_t::fromMillisSinceEpoch(1499170197000);
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:09:57"
                                          << "onNull"
                                          << "Null default"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:09:57"
                                          << "onError"
                                          << "Error default"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:09:57"
                                          << "onError"
                                          << "Error default"
                                          << "onNull"
                                          << "null default"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if dateString is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "$date"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if timezone is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:09:57Z"
                                          << "timezone"
                                          << "$tz"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if format is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:09:57Z"
                                          << "timezone"
                                          << "Europe/London"
                                          << "format"
                                          << "$format"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if onNull is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:09:57Z"
                                          << "onNull"
                                          << "$onNull"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if onError is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-04T13:09:57Z"
                                          << "onError"
                                          << "$onError"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
}

TEST_F(ExpressionDateFromStringTest, RejectsUnparsableString) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "60.Monday1770/06:59"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(ExpressionDateFromStringTest, RejectsTimeZoneInString) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2017-07-13T10:02:57 Europe/London"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "July 4, 2017 Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(ExpressionDateFromStringTest, RejectsTimeZoneInStringAndArgument) {
    auto expCtx = getExpCtx();

    // Test with "Z" and timezone
    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2017-07-14T15:24:38Z"
                                               << "timezone"
                                               << "Europe/London"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    // Test with timezone abbreviation and timezone
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-14T15:24:38 PDT"
                                          << "timezone"
                                          << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    // Test with GMT offset and timezone
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-14T15:24:38+02:00"
                                          << "timezone"
                                          << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    // Test with GMT offset and GMT timezone
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-14 -0400"
                                          << "timezone"
                                          << "GMT"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(ExpressionDateFromStringTest, RejectsNonStringFormat) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2017-07-13T10:02:57"
                                               << "format"
                                               << 2));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40684);

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "July 4, 2017"
                                          << "format"
                                          << true));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40684);
}

TEST_F(ExpressionDateFromStringTest, RejectsStringsThatDoNotMatchFormat) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2017-07"
                                               << "format"
                                               << "%Y-%m-%d"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07"
                                          << "format"
                                          << "%m-%Y"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(ExpressionDateFromStringTest, EscapeCharacterAllowsPrefixUsage) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2017 % 01 % 01"
                                               << "format"
                                               << "%Y %% %m %% %d"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-01-01T00:00:00.000Z", dateExp->evaluate({}, &expCtx->variables).toString());
}


TEST_F(ExpressionDateFromStringTest, EvaluatesToNullIfFormatIsNullish) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "1/1/2017"
                                               << "format"
                                               << BSONNULL));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "1/1/2017"
                                          << "format"
                                          << "$missing"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "1/1/2017"
                                          << "format"
                                          << BSONUndefined));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionDateFromStringTest, ReadWithUTCOffset) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2017-07-28T10:47:52.912"
                                               << "timezone"
                                               << "-01:00"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T11:47:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-28T10:47:52.912"
                                          << "timezone"
                                          << "+01:00"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T09:47:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-28T10:47:52.912"
                                          << "timezone"
                                          << "+0445"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T06:02:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2017-07-28T10:47:52.912"
                                          << "timezone"
                                          << "+10:45"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T00:02:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "1945-07-28T10:47:52.912"
                                          << "timezone"
                                          << "-08:00"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ("1945-07-28T18:47:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());
}

TEST_F(ExpressionDateFromStringTest, ConvertStringWithUTCOffsetAndFormat) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "10:47:52.912 on 7/28/2017"
                                               << "timezone"
                                               << "-01:00"
                                               << "format"
                                               << "%H:%M:%S.%L on %m/%d/%Y"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T11:47:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "10:47:52.912 on 7/28/2017"
                                          << "timezone"
                                          << "+01:00"
                                          << "format"
                                          << "%H:%M:%S.%L on %m/%d/%Y"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T09:47:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());
}

TEST_F(ExpressionDateFromStringTest, ConvertStringWithISODateFormat) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "Day 7 Week 53 Year 2017"
                                               << "format"
                                               << "Day %u Week %V Year %G"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ("2018-01-07T00:00:00.000Z", dateExp->evaluate({}, &expCtx->variables).toString());

    // Week and day of week default to '1' if not specified.
    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "Week 53 Year 2017"
                                          << "format"
                                          << "Week %V Year %G"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ("2018-01-01T00:00:00.000Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "Day 7 Year 2017"
                                          << "format"
                                          << "Day %u Year %G"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-01-08T00:00:00.000Z", dateExp->evaluate({}, &expCtx->variables).toString());
}

TEST_F(ExpressionDateFromStringTest, ReturnsOnNullForNullishInput) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                            << "Null default"));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("Null default"_sd), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "$missing"
                                          << "onNull"
                                          << "Null default"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("Null default"_sd), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "$missing"
                                          << "onNull"
                                          << "$alsoMissing"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull" << BSONNULL));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionDateFromStringTest, InvalidFormatTakesPrecedenceOverOnNull) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                            << "Null default"
                                                            << "format"
                                                            << 5));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40684);

    spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                       << "Null default"
                                                       << "format"
                                                       << "%"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 18535);
}

TEST_F(ExpressionDateFromStringTest, InvalidFormatTakesPrecedenceOverOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "Invalid dateString"
                                               << "onError"
                                               << "Not used default"
                                               << "format"
                                               << 5));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40684);

    spec = BSON("$dateFromString" << BSON("dateString" << 5 << "onError"
                                                       << "Not used default"
                                                       << "format"
                                                       << "%"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 18535);
}

TEST_F(ExpressionDateFromStringTest, InvalidTimezoneTakesPrecedenceOverOnNull) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                            << "Null default"
                                                            << "timezone"
                                                            << 5));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40517);

    spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                       << "Null default"
                                                       << "timezone"
                                                       << "invalid timezone string"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40485);
}

TEST_F(ExpressionDateFromStringTest, InvalidTimezoneTakesPrecedenceOverOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "Invalid dateString"
                                               << "onError"
                                               << "On error default"
                                               << "timezone"
                                               << 5));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40517);

    spec = BSON("$dateFromString" << BSON("dateString" << 5 << "onError"
                                                       << "On error default"
                                                       << "timezone"
                                                       << "invalid timezone string"));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40485);
}

TEST_F(ExpressionDateFromStringTest, OnNullTakesPrecedenceOverOtherNullishParameters) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                            << "Null default"
                                                            << "timezone"
                                                            << BSONNULL));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("Null default"_sd), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                       << "Null default"
                                                       << "format"
                                                       << BSONNULL));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("Null default"_sd), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionDateFromStringTest, OnNullOnlyUsedIfInputStringIsNullish) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "2018-02-14"
                                               << "onNull"
                                               << "Null default"
                                               << "timezone"
                                               << BSONNULL));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString"
                                          << "2018-02-14"
                                          << "onNull"
                                          << "Null default"
                                          << "format"
                                          << BSONNULL));
    dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionDateFromStringTest, ReturnsOnErrorForParseFailures) {
    auto expCtx = getExpCtx();

    std::vector<std::string> invalidDates = {
        "60.Monday1770/06:59", "July 4th", "12:50:53", "2017, 12:50:53"};
    for (auto date : invalidDates) {
        auto spec = BSON("$dateFromString" << BSON("dateString" << date << "onError"
                                                                << "Error default"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value("Error default"_sd), dateExp->evaluate({}, &expCtx->variables));
    }
}

TEST_F(ExpressionDateFromStringTest, ReturnsOnErrorForFormatMismatch) {
    auto expCtx = getExpCtx();

    const std::string date = "2018/02/06";
    std::vector<std::string> unmatchedFormats = {"%Y", "%Y/%m/%d:%H", "Y/m/d"};
    for (auto format : unmatchedFormats) {
        auto spec =
            BSON("$dateFromString" << BSON("dateString" << date << "format" << format << "onError"
                                                        << "Error default"));
        auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value("Error default"_sd), dateExp->evaluate({}, &expCtx->variables));
    }
}

TEST_F(ExpressionDateFromStringTest, OnNullEvaluatedLazily) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "$date"
                                               << "onNull"
                                               << BSON("$divide" << BSON_ARRAY(1 << 0))));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ(
        "2018-02-14T00:00:00.000Z",
        dateExp->evaluate(Document{{"date", "2018-02-14"_sd}}, &expCtx->variables).toString());
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 16608);
}

TEST_F(ExpressionDateFromStringTest, OnErrorEvaluatedLazily) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString"
                                               << "$date"
                                               << "onError"
                                               << BSON("$divide" << BSON_ARRAY(1 << 0))));
    auto dateExp = Expression::parseExpression(expCtx, spec, expCtx->variablesParseState);
    ASSERT_EQ(
        "2018-02-14T00:00:00.000Z",
        dateExp->evaluate(Document{{"date", "2018-02-14"_sd}}, &expCtx->variables).toString());
    ASSERT_THROWS_CODE(
        dateExp->evaluate(Document{{"date", 5}}, &expCtx->variables), AssertionException, 16608);
}

}  // namespace ExpressionDateFromStringTest
}  // namespace mongo
