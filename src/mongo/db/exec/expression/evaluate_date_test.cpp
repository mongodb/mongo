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

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/query/compiler/dependency_analysis/expression_dependencies.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <initializer_list>
#include <ostream>
#include <set>
#include <string>
#include <vector>


namespace mongo {

namespace expression_evaluation_test {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionEvaluateDateFromPartsTest = AggregationContextFixture;

TEST_F(ExpressionEvaluateDateFromPartsTest, TestThatOutOfRangeValuesRollOver) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromParts" << BSON("year" << 2017 << "month" << -1));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    auto dateVal = Date_t::fromMillisSinceEpoch(1477958400000);  // 11/1/2016 in ms.
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "day" << -1));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(1483056000000);  // 12/30/2016
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "hour" << 25));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(1483318800000);  // 1/2/2017 01:00:00
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "minute" << 61));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(1483232460000);  // 1/1/2017 01:01:00
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "second" << 61));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    dateVal = Date_t::fromMillisSinceEpoch(1483228861000);  // 1/1/2017 00:01:01
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));
}

}  // namespace expression_evaluation_test

namespace evaluate_date_expressions_test {

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
using EvaluateDateExpressionsTest = AggregationContextFixture;

TEST_F(EvaluateDateExpressionsTest, RejectsArraysWithinObjectSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << BSON_ARRAY(Date_t{}) << "timezone"
                                                   << "Europe/London"));
        // It will parse as an ExpressionArray, and fail at runtime.
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"_id", 0}};
        ASSERT_THROWS_CODE(
            dateExp->evaluate(contextDoc, &expCtx->variables), AssertionException, 16006);

        // Test that it rejects an array for the timezone option.
        spec =
            BSON(expName << BSON("date" << Date_t{} << "timezone" << BSON_ARRAY("Europe/London")));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        contextDoc = Document{{"_id", 0}};
        ASSERT_THROWS_CODE(
            dateExp->evaluate(contextDoc, &expCtx->variables), AssertionException, 40517);
    }
}

TEST_F(EvaluateDateExpressionsTest, RejectsTypesThatCannotCoerceToDate) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << "$stringField");
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"stringField", "string"_sd}};
        ASSERT_THROWS_CODE(
            dateExp->evaluate(contextDoc, &expCtx->variables), AssertionException, 16006);
    }
}

TEST_F(EvaluateDateExpressionsTest, AcceptsObjectIds) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << "$oid");
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"oid", OID::gen()}};
        dateExp->evaluate(contextDoc, &expCtx->variables);  // Should not throw.
    }
}

TEST_F(EvaluateDateExpressionsTest, AcceptsTimestamps) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << "$ts");
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"ts", Timestamp{Date_t{}}}};
        dateExp->evaluate(contextDoc, &expCtx->variables);  // Should not throw.
    }
}

TEST_F(EvaluateDateExpressionsTest, RejectsNonStringTimezone) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "$intField"));
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"intField", 4}};
        ASSERT_THROWS_CODE(
            dateExp->evaluate(contextDoc, &expCtx->variables), AssertionException, 40517);
    }
}

TEST_F(EvaluateDateExpressionsTest, RejectsUnrecognizedTimeZoneSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "UNRECOGNIZED!"));
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"_id", 0}};
        ASSERT_THROWS_CODE(
            dateExp->evaluate(contextDoc, &expCtx->variables), AssertionException, 40485);
    }
}

TEST_F(EvaluateDateExpressionsTest, DoesRespectTimeZone) {
    // Make sure they each successfully evaluate with a different TimeZone.
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        auto spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                << "America/New_York"));
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto contextDoc = Document{{"_id", 0}};
        dateExp->evaluate(contextDoc, &expCtx->variables);  // Should not throw.
    }

    // Make sure the time zone is used during evaluation.
    auto date = Date_t::fromMillisSinceEpoch(1496777923000LL);  // 2017-06-06T19:38:43:234Z.
    auto specWithoutTimezone = BSON("$hour" << BSON("date" << date));
    auto hourWithoutTimezone =
        Expression::parseExpression(expCtx.get(), specWithoutTimezone, expCtx->variablesParseState)
            ->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_EQ(hourWithoutTimezone, Value(19));

    auto specWithTimezone = BSON("$hour" << BSON("date" << date << "timezone"
                                                        << "America/New_York"));
    auto hourWithTimezone =
        Expression::parseExpression(expCtx.get(), specWithTimezone, expCtx->variablesParseState)
            ->evaluate({}, &expCtx->variables);
    ASSERT_VALUE_EQ(hourWithTimezone, Value(15));
}

TEST_F(EvaluateDateExpressionsTest, DoesResultInNullIfGivenNullishInput) {
    // Make sure they each successfully evaluate with a different TimeZone.
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        auto contextDoc = Document{{"_id", 0}};

        // Test that the expression results in null if the date is nullish and the timezone is not
        // specified.
        auto spec = BSON(expName << BSON("date" << "$missing"));
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        spec = BSON(expName << BSON("date" << BSONNULL));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        spec = BSON(expName << BSON("date" << BSONUndefined));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        // Test that the expression results in null if the date is present but the timezone is
        // nullish.
        spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                           << "$missing"));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        spec = BSON(expName << BSON("date" << Date_t{} << "timezone" << BSONNULL));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        spec = BSON(expName << BSON("date" << Date_t{} << "timezone" << BSONUndefined));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        // Test that the expression results in null if the date and timezone both nullish.
        spec = BSON(expName << BSON("date" << "$missing"
                                           << "timezone" << BSONUndefined));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));

        // Test that the expression results in null if the date is nullish and timezone is present.
        spec = BSON(expName << BSON("date" << "$missing"
                                           << "timezone"
                                           << "Europe/London"));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate(contextDoc, &expCtx->variables));
    }
}

}  // namespace evaluate_date_expressions_test

namespace expression_evaluate_date_to_string_test {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionEvaluateDateToStringTest = AggregationContextFixture;

TEST_F(ExpressionEvaluateDateToStringTest, ReturnsOnNullValueWhenInputIsNullish) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateToString" << BSON("format" << "%Y-%m-%d"
                                                      << "date" << BSONNULL << "onNull"
                                                      << "null default"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("null default"_sd), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateToString" << BSON("format" << "%Y-%m-%d"
                                                 << "date" << BSONNULL << "onNull" << BSONNULL));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateToString" << BSON("format" << "%Y-%m-%d"
                                                 << "date"
                                                 << "$missing"
                                                 << "onNull"
                                                 << "null default"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("null default"_sd), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateToString" << BSON("format" << "%Y-%m-%d"
                                                 << "date"
                                                 << "$missing"
                                                 << "onNull"
                                                 << "$alsoMissing"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionEvaluateDateToStringTest, ReturnsNullIfInputDateIsNullish) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$dateToString: {date: '$date'}}");
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL),
                    dateExp->evaluate(Document{{"date", BSONNULL}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionEvaluateDateToStringTest, ReturnsNullIfFormatIsNullish) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$dateToString: {date: '$date', format: '$format'}}");
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(
        Value(BSONNULL),
        dateExp->evaluate(Document{{"date", Date_t{}}, {"format", BSONNULL}}, &expCtx->variables));
    ASSERT_VALUE_EQ(Value(BSONNULL),
                    dateExp->evaluate(Document{{"date", Date_t{}}}, &expCtx->variables));
}

TEST_F(ExpressionEvaluateDateToStringTest, UsesDefaultFormatIfNoneSpecified) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$dateToString: {date: '$date'}}");
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("1970-01-01T00:00:00.000Z"_sd),
                    dateExp->evaluate(Document{{"date", Date_t{}}}, &expCtx->variables));
}

TEST_F(ExpressionEvaluateDateToStringTest, FailsForInvalidTimezoneRegardlessOfInputDate) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$dateToString: {date: '$date', timezone: '$tz'}}");
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(
        dateExp->evaluate(Document{{"date", BSONNULL}, {"tz", "invalid"_sd}}, &expCtx->variables),
        AssertionException,
        40485);
    ASSERT_THROWS_CODE(
        dateExp->evaluate(Document{{"date", BSONNULL}, {"tz", 5}}, &expCtx->variables),
        AssertionException,
        40517);
}

TEST_F(ExpressionEvaluateDateToStringTest, FailsForInvalidFormatStrings) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateToString" << BSON("date" << Date_t{} << "format"
                                                    << "%n"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 18536);

    spec = BSON("$dateToString" << BSON("date" << Date_t{} << "format"
                                               << "%Y%"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 18535);
}

TEST_F(ExpressionEvaluateDateToStringTest, FailsForInvalidFormatRegardlessOfInputDate) {
    auto expCtx = getExpCtx();

    auto spec = fromjson("{$dateToString: {date: '$date', format: '$format', onNull: 0}}");
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
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

}  // namespace expression_evaluate_date_to_string_test

namespace expression_evaluate_date_from_string_test {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionEvaluateDateFromStringTest = AggregationContextFixture;

TEST_F(ExpressionEvaluateDateFromStringTest, RejectsUnparsableString) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << "60.Monday1770/06:59"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(ExpressionEvaluateDateFromStringTest, RejectsTimeZoneInString) {
    auto expCtx = getExpCtx();

    auto spec =
        BSON("$dateFromString" << BSON("dateString" << "2017-07-13T10:02:57 Europe/London"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    spec = BSON("$dateFromString" << BSON("dateString" << "July 4, 2017 Europe/London"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(ExpressionEvaluateDateFromStringTest, RejectsTimeZoneInStringAndArgument) {
    auto expCtx = getExpCtx();

    // Test with "Z" and timezone
    auto spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-14T15:24:38Z"
                                                            << "timezone"
                                                            << "Europe/London"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    // Test with timezone abbreviation and timezone
    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-14T15:24:38 PDT"
                                                       << "timezone"
                                                       << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    // Test with GMT offset and timezone
    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-14T15:24:38+02:00"
                                                       << "timezone"
                                                       << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    // Test with GMT offset and GMT timezone
    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-14 -0400"
                                                       << "timezone"
                                                       << "GMT"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(ExpressionEvaluateDateFromStringTest, RejectsNonStringFormat) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-13T10:02:57"
                                                            << "format" << 2));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40684);

    spec = BSON("$dateFromString" << BSON("dateString" << "July 4, 2017"
                                                       << "format" << true));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40684);
}

TEST_F(ExpressionEvaluateDateFromStringTest, RejectsStringsThatDoNotMatchFormat) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << "2017-07"
                                                            << "format"
                                                            << "%Y-%m-%d"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);

    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07"
                                                       << "format"
                                                       << "%m-%Y"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::ConversionFailure);
}

TEST_F(ExpressionEvaluateDateFromStringTest, EscapeCharacterAllowsPrefixUsage) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << "2017 % 01 % 01"
                                                            << "format"
                                                            << "%Y %% %m %% %d"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-01-01T00:00:00.000Z", dateExp->evaluate({}, &expCtx->variables).toString());
}


TEST_F(ExpressionEvaluateDateFromStringTest, EvaluatesToNullIfFormatIsNullish) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << "1/1/2017"
                                                            << "format" << BSONNULL));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString" << "1/1/2017"
                                                       << "format"
                                                       << "$missing"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString" << "1/1/2017"
                                                       << "format" << BSONUndefined));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionEvaluateDateFromStringTest, ReadWithUTCOffset) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-28T10:47:52.912"
                                                            << "timezone"
                                                            << "-01:00"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T11:47:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-28T10:47:52.912"
                                                       << "timezone"
                                                       << "+01:00"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T09:47:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-28T10:47:52.912"
                                                       << "timezone"
                                                       << "+0445"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T06:02:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-28T10:47:52.912"
                                                       << "timezone"
                                                       << "+10:45"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T00:02:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString" << "1945-07-28T10:47:52.912"
                                                       << "timezone"
                                                       << "-08:00"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ("1945-07-28T18:47:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());
}

TEST_F(ExpressionEvaluateDateFromStringTest, ConvertStringWithUTCOffsetAndFormat) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << "10:47:52.912 on 7/28/2017"
                                                            << "timezone"
                                                            << "-01:00"
                                                            << "format"
                                                            << "%H:%M:%S.%L on %m/%d/%Y"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T11:47:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString" << "10:47:52.912 on 7/28/2017"
                                                       << "timezone"
                                                       << "+01:00"
                                                       << "format"
                                                       << "%H:%M:%S.%L on %m/%d/%Y"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-07-28T09:47:52.912Z", dateExp->evaluate({}, &expCtx->variables).toString());
}

TEST_F(ExpressionEvaluateDateFromStringTest, ConvertStringWithISODateFormat) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << "Day 7 Week 53 Year 2017"
                                                            << "format"
                                                            << "Day %u Week %V Year %G"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ("2018-01-07T00:00:00.000Z", dateExp->evaluate({}, &expCtx->variables).toString());

    // Week and day of week default to '1' if not specified.
    spec = BSON("$dateFromString" << BSON("dateString" << "Week 53 Year 2017"
                                                       << "format"
                                                       << "Week %V Year %G"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ("2018-01-01T00:00:00.000Z", dateExp->evaluate({}, &expCtx->variables).toString());

    spec = BSON("$dateFromString" << BSON("dateString" << "Day 7 Year 2017"
                                                       << "format"
                                                       << "Day %u Year %G"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ("2017-01-08T00:00:00.000Z", dateExp->evaluate({}, &expCtx->variables).toString());
}

TEST_F(ExpressionEvaluateDateFromStringTest, ReturnsOnNullForNullishInput) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                            << "Null default"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("Null default"_sd), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString" << "$missing"
                                                       << "onNull"
                                                       << "Null default"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("Null default"_sd), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString" << "$missing"
                                                       << "onNull"
                                                       << "$alsoMissing"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull" << BSONNULL));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionEvaluateDateFromStringTest, InvalidFormatTakesPrecedenceOverOnNull) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                            << "Null default"
                                                            << "format" << 5));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40684);

    spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                       << "Null default"
                                                       << "format"
                                                       << "%"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 18535);
}

TEST_F(ExpressionEvaluateDateFromStringTest, InvalidFormatTakesPrecedenceOverOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << "Invalid dateString"
                                                            << "onError"
                                                            << "Not used default"
                                                            << "format" << 5));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40684);

    spec = BSON("$dateFromString" << BSON("dateString" << 5 << "onError"
                                                       << "Not used default"
                                                       << "format"
                                                       << "%"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 18535);
}

TEST_F(ExpressionEvaluateDateFromStringTest, InvalidTimezoneTakesPrecedenceOverOnNull) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                            << "Null default"
                                                            << "timezone" << 5));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40517);

    spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                       << "Null default"
                                                       << "timezone"
                                                       << "invalid timezone string"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40485);
}

TEST_F(ExpressionEvaluateDateFromStringTest, InvalidTimezoneTakesPrecedenceOverOnError) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << "Invalid dateString"
                                                            << "onError"
                                                            << "On error default"
                                                            << "timezone" << 5));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40517);

    spec = BSON("$dateFromString" << BSON("dateString" << 5 << "onError"
                                                       << "On error default"
                                                       << "timezone"
                                                       << "invalid timezone string"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_THROWS_CODE(dateExp->evaluate({}, &expCtx->variables), AssertionException, 40485);
}

TEST_F(ExpressionEvaluateDateFromStringTest, OnNullTakesPrecedenceOverOtherNullishParameters) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                            << "Null default"
                                                            << "timezone" << BSONNULL));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("Null default"_sd), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString" << BSONNULL << "onNull"
                                                       << "Null default"
                                                       << "format" << BSONNULL));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value("Null default"_sd), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionEvaluateDateFromStringTest, OnNullOnlyUsedIfInputStringIsNullish) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString" << BSON("dateString" << "2018-02-14"
                                                            << "onNull"
                                                            << "Null default"
                                                            << "timezone" << BSONNULL));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString" << "2018-02-14"
                                                       << "onNull"
                                                       << "Null default"
                                                       << "format" << BSONNULL));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_VALUE_EQ(Value(BSONNULL), dateExp->evaluate({}, &expCtx->variables));
}

TEST_F(ExpressionEvaluateDateFromStringTest, ReturnsOnErrorForParseFailures) {
    auto expCtx = getExpCtx();

    std::vector<std::string> invalidDates = {
        "60.Monday1770/06:59", "July 4th", "12:50:53", "2017, 12:50:53"};
    for (const auto& date : invalidDates) {
        auto spec = BSON("$dateFromString" << BSON("dateString" << date << "onError"
                                                                << "Error default"));
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value("Error default"_sd), dateExp->evaluate({}, &expCtx->variables));
    }
}

TEST_F(ExpressionEvaluateDateFromStringTest, ReturnsOnErrorForFormatMismatch) {
    auto expCtx = getExpCtx();

    const std::string date = "2018/02/06";
    std::vector<std::string> unmatchedFormats = {"%Y", "%Y/%m/%d:%H", "Y/m/d"};
    for (const auto& format : unmatchedFormats) {
        auto spec =
            BSON("$dateFromString" << BSON("dateString" << date << "format" << format << "onError"
                                                        << "Error default"));
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value("Error default"_sd), dateExp->evaluate({}, &expCtx->variables));
    }
}

TEST_F(ExpressionEvaluateDateFromStringTest, OnNullEvaluatedLazily) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString"
                     << BSON("dateString" << "$date"
                                          << "onNull" << BSON("$divide" << BSON_ARRAY(1 << 0))));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ(
        "2018-02-14T00:00:00.000Z",
        dateExp->evaluate(Document{{"date", "2018-02-14"_sd}}, &expCtx->variables).toString());
    ASSERT_THROWS_CODE(
        dateExp->evaluate({}, &expCtx->variables), AssertionException, ErrorCodes::BadValue);
}

TEST_F(ExpressionEvaluateDateFromStringTest, OnErrorEvaluatedLazily) {
    auto expCtx = getExpCtx();

    auto spec = BSON("$dateFromString"
                     << BSON("dateString" << "$date"
                                          << "onError" << BSON("$divide" << BSON_ARRAY(1 << 0))));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_EQ(
        "2018-02-14T00:00:00.000Z",
        dateExp->evaluate(Document{{"date", "2018-02-14"_sd}}, &expCtx->variables).toString());
    ASSERT_THROWS_CODE(dateExp->evaluate(Document{{"date", 5}}, &expCtx->variables),
                       AssertionException,
                       ErrorCodes::BadValue);
}

}  // namespace expression_evaluate_date_from_string_test

namespace expression_evaluate_date_diff_test {
class ExpressionEvaluateDateDiffTest : public AggregationContextFixture {
public:
    /**
     * Builds a $dateDiff expression with given values of parameters.
     */
    auto buildExpressionWithParameters(
        Value startDate, Value endDate, Value unit, Value timezone, Value startOfWeek = Value{}) {
        auto expCtx = getExpCtx();
        auto expression =
            BSON("$dateDiff" << BSON("startDate" << startDate << "endDate" << endDate << "unit"
                                                 << unit << "timezone" << timezone << "startOfWeek"
                                                 << startOfWeek));
        return Expression::parseExpression(expCtx.get(), expression, expCtx->variablesParseState);
    }
};

TEST_F(ExpressionEvaluateDateDiffTest, EvaluatesExpression) {
    struct TestCase {
        Value startDate;
        Value endDate;
        Value unit;
        Value timezone;
        Value expectedResult;
        int expectedErrorCode{0};
        std::string expectedErrorMessage;
        Value startOfWeek;
    };
    auto expCtx = getExpCtx();
    const auto anyDate = Value{Date_t{}};
    const auto null = Value{BSONNULL};
    const auto hour = Value{"hour"_sd};
    const auto utc = Value{"GMT"_sd};
    const auto objectId = Value{OID::gen()};
    const std::vector<TestCase> testCases{
        {// Sunny day case.
         Value{Date_t::fromMillisSinceEpoch(1604255016000) /* 2020-11-01T18:23:36 UTC+00:00 */},
         Value{Date_t::fromMillisSinceEpoch(1604260800000) /* 2020-11-01T20:00:00 UTC+00:00 */},
         hour,
         utc,
         Value{2}},
        {// 'startDate' is null.
         null,
         anyDate,
         hour,
         utc,
         null},
        {// 'endDate' is null.
         anyDate,
         null,
         hour,
         utc,
         null},
        {// 'unit' is null.
         anyDate,
         anyDate,
         null,
         utc,
         null},
        {// Invalid 'startDate' type.
         Value{"date"_sd},
         anyDate,
         hour,
         utc,
         null,
         5166307,  // Error code.
         "$dateDiff requires 'startDate' to be a date, but got string"},
        {// Invalid 'endDate' type.
         anyDate,
         Value{"date"_sd},
         hour,
         utc,
         null,
         5166307,  // Error code.
         "$dateDiff requires 'endDate' to be a date, but got string"},
        {// Invalid 'unit' type.
         anyDate,
         anyDate,
         Value{2},
         utc,
         null,
         5439013,  // Error code.
         "$dateDiff requires 'unit' to be a string, but got int"},
        {// Invalid 'unit' value.
         anyDate,
         anyDate,
         Value{"century"_sd},
         utc,
         null,
         ErrorCodes::FailedToParse,  // Error code.
         "$dateDiff parameter 'unit' value parsing failed :: caused by :: unknown time unit value: "
         "century"},
        {// Invalid 'timezone' value.
         anyDate,
         anyDate,
         hour,
         Value{"INVALID"_sd},
         null,
         40485,  // Error code.
         "$dateDiff parameter 'timezone' value parsing failed :: caused by :: unrecognized time "
         "zone identifier: \"INVALID\""},
        {// Accepts OID.
         objectId,
         objectId,
         hour,
         utc,
         Value{0}},
        {// Accepts timestamp.
         Value{Timestamp{Seconds(1604255016), 0} /* 2020-11-01T18:23:36 UTC+00:00 */},
         Value{Timestamp{Seconds(1604260800), 0} /* 2020-11-01T20:00:00 UTC+00:00 */},
         Value{"minute"_sd},
         Value{} /* 'timezone' not specified*/,
         Value{97}},
        {
            // Ignores 'startOfWeek' parameter value when unit is not week.
            anyDate,
            anyDate,
            Value{"day"_sd},
            Value{},             //'timezone' is not specified
            Value{0},            // expectedResult
            0,                   // expectedErrorCode
            "",                  // expectedErrorMessage
            Value{"INVALID"_sd}  // startOfWeek
        },
        {
            // 'startOfWeek' is null.
            anyDate,
            anyDate,
            Value{"week"_sd},  // unit
            Value{},           //'timezone' is not specified
            null,              // expectedResult
            0,                 // expectedErrorCode
            "",                // expectedErrorMessage
            null               // startOfWeek
        },
        {
            // Invalid 'startOfWeek' value type.
            anyDate,
            anyDate,
            Value{"week"_sd},  // unit
            Value{},           //'timezone' is not specified
            null,              // expectedResult
            5439015,           // expectedErrorCode
            "$dateDiff requires 'startOfWeek' to be a string, but got int",  // expectedErrorMessage
            Value{1}                                                         // startOfWeek
        },
        {
            // Invalid 'startOfWeek' value.
            anyDate,
            anyDate,
            Value{"week"_sd},  // unit
            Value{},           //'timezone' is not specified
            null,              // expectedResult
            5439016,           // expectedErrorCode
            "$dateDiff parameter 'startOfWeek' value cannot be recognized as a day of a week: "
            "Satur",           // expectedErrorMessage
            Value{"Satur"_sd}  // startOfWeek
        },
        {
            // Sunny day case for 'startOfWeek'.
            Value{Date_t::fromMillisSinceEpoch(
                1611446400000) /* 2021-01-24T00:00:00 UTC+00:00 Sunday*/},
            Value{Date_t::fromMillisSinceEpoch(
                1611532800000) /* 2021-01-25T00:00:00 UTC+00:00 Monday*/},
            Value{"week"_sd},   // unit
            Value{},            //'timezone' is not specified
            Value{1},           // expectedResult
            0,                  // expectedErrorCode
            "",                 // expectedErrorMessage
            Value{"Monday"_sd}  // startOfWeek
        },
        {
            // 'startOfWeek' not specified, defaults to "Sunday".
            Value{Date_t::fromMillisSinceEpoch(
                1611360000000) /* 2021-01-23T00:00:00 UTC+00:00 Saturday*/},
            Value{Date_t::fromMillisSinceEpoch(
                1611446400000) /* 2021-01-24T00:00:00 UTC+00:00 Sunday*/},
            Value{"week"_sd},  // unit
            Value{},           //'timezone' is not specified
            Value{1},          // expectedResult
        },
    };

    // Week time unit and 'startOfWeek' specific test cases.
    for (auto&& testCase : testCases) {
        auto dateDiffExpression = buildExpressionWithParameters(testCase.startDate,
                                                                testCase.endDate,
                                                                testCase.unit,
                                                                testCase.timezone,
                                                                testCase.startOfWeek);
        if (testCase.expectedErrorCode) {
            ASSERT_THROWS_CODE_AND_WHAT(dateDiffExpression->evaluate({}, &(expCtx->variables)),
                                        AssertionException,
                                        testCase.expectedErrorCode,
                                        testCase.expectedErrorMessage);
        } else {
            ASSERT_VALUE_EQ(Value{testCase.expectedResult},
                            dateDiffExpression->evaluate({}, &(expCtx->variables)));
        }
    }
}

}  // namespace expression_evaluate_date_diff_test

namespace expression_evaluate_date_arithmetics_test {
using ExpressionEvaluateDateArithmeticsTest = AggregationContextFixture;

std::vector<StringData> dateArithmeticsExp = {"$dateAdd"_sd, "$dateSubtract"_sd};

TEST_F(ExpressionEvaluateDateArithmeticsTest, EvaluatesToNullWithNullInput) {
    auto expCtx = getExpCtx();

    for (auto&& expName : dateArithmeticsExp) {
        auto testDocuments = {
            BSON(expName << BSON("startDate" << BSONNULL << "unit"
                                             << "day"
                                             << "amount" << 1)),
            BSON(expName << BSON("startDate" << "$missingField"
                                             << "unit"
                                             << "day"
                                             << "amount" << 1)),
            BSON(expName << BSON("startDate" << Date_t{} << "unit" << BSONNULL << "amount" << 1)),
            BSON(expName << BSON("startDate" << Date_t{} << "unit"
                                             << "day"
                                             << "amount"
                                             << "$missingField")),
            BSON(expName << BSON("startDate" << Date_t{} << "unit"
                                             << "day"
                                             << "amount" << 123 << "timezone" << BSONNULL)),
        };

        for (auto&& doc : testDocuments) {
            auto dateAddExp =
                Expression::parseExpression(expCtx.get(), doc, expCtx->variablesParseState);
            ASSERT_VALUE_EQ(Value(BSONNULL), dateAddExp->evaluate({}, &expCtx->variables));
        }
    }
}

TEST_F(ExpressionEvaluateDateArithmeticsTest, ThrowsExceptionOnInvalidInput) {
    auto expCtx = getExpCtx();

    struct TestCase {
        BSONObj doc;
        int errorCode;
    };

    for (auto&& expName : dateArithmeticsExp) {
        std::vector<TestCase> testCases = {
            {BSON(expName << BSON("startDate" << "myDate"
                                              << "unit" << 123 << "amount" << 1)),
             5439013},
            {BSON(expName << BSON("startDate" << Date_t{} << "unit" << 123 << "amount" << 1)),
             5439013},
            {BSON(expName << BSON("startDate" << Date_t{} << "unit"
                                              << "decade"
                                              << "amount" << 1)),
             9},
            {BSON(expName << BSON("startDate" << Date_t{} << "unit"
                                              << "day"
                                              << "amount" << 1.789)),
             5166405},
            {BSON(expName << BSON("startDate" << Date_t{} << "unit"
                                              << "day"
                                              << "amount" << 5 << "timezone" << 12)),
             40517},
            {BSON(expName << BSON("startDate" << Date_t{} << "unit"
                                              << "day"
                                              << "amount" << 5 << "timezone"
                                              << "Unknown")),
             40485},
        };

        for (auto&& testCase : testCases) {
            auto dateAddExp = Expression::parseExpression(
                expCtx.get(), testCase.doc, expCtx->variablesParseState);
            ASSERT_THROWS_CODE(dateAddExp->evaluate({}, &expCtx->variables),
                               AssertionException,
                               testCase.errorCode);
        }
    }
}

TEST_F(ExpressionEvaluateDateArithmeticsTest, RegularEvaluationDateAdd) {
    auto expCtx = getExpCtx();
    struct TestCase {
        BSONObj doc;
        Date_t expected;
    };

    const long long startInstant = 1604139005000LL;  // 2020-10-31T10:10:05
    Date_t testDate = Date_t::fromMillisSinceEpoch(startInstant);
    const auto objId = OID::gen();

    std::vector<TestCase> testCases = {
        {BSON("$dateAdd" << BSON("startDate" << testDate << "unit"
                                             << "month"
                                             << "amount" << 1)),
         Date_t::fromMillisSinceEpoch(startInstant + 30LL * 24 * 60 * 60 * 1000)},
        {BSON("$dateAdd" << BSON("startDate" << testDate << "unit"
                                             << "day"
                                             << "amount" << 1)),
         Date_t::fromMillisSinceEpoch(startInstant + 24 * 60 * 60 * 1000)},
        {BSON("$dateAdd" << BSON("startDate" << testDate << "unit"
                                             << "hour"
                                             << "amount" << -1)),
         Date_t::fromMillisSinceEpoch(startInstant - 1 * 60 * 60 * 1000)},
        {BSON("$dateAdd" << BSON("startDate" << testDate << "unit"
                                             << "minute"
                                             << "amount" << 5)),
         Date_t::fromMillisSinceEpoch(startInstant + 5 * 60 * 1000)},
        {BSON("$dateAdd" << BSON("startDate" << testDate << "unit"
                                             << "day"
                                             << "amount" << -7 << "timezone"
                                             << "Europe/Amsterdam")),
         // Subtracts additional 60 minutes due to crossing DST change in Ams zone.
         Date_t::fromMillisSinceEpoch(startInstant - (7 * 24 + 1) * 60 * 60 * 1000)},
        {BSON("$dateAdd" << BSON("startDate" << Timestamp(1605001115, 0) << "unit"
                                             << "second"
                                             << "amount" << 1)),
         Date_t::fromMillisSinceEpoch(1605001116000)},
        {BSON("$dateAdd" << BSON("startDate" << objId << "unit"
                                             << "second"
                                             << "amount" << 0)),
         objId.asDateT()},
    };

    for (auto&& testCase : testCases) {
        auto dateAddExp =
            Expression::parseExpression(expCtx.get(), testCase.doc, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(testCase.expected), dateAddExp->evaluate({}, &expCtx->variables));
    }
}

TEST_F(ExpressionEvaluateDateArithmeticsTest, RegularEvaluationDateSubtract) {
    auto expCtx = getExpCtx();

    struct TestCase {
        BSONObj doc;
        Date_t expected;
    };
    const long long startInstant = 1604139005000LL;  // 2020-10-31T10:10:05
    Date_t testDate = Date_t::fromMillisSinceEpoch(startInstant);
    std::vector<TestCase> testCases = {
        {BSON("$dateSubtract" << BSON("startDate" << testDate << "unit"
                                                  << "month"
                                                  << "amount" << 1)),
         Date_t::fromMillisSinceEpoch(startInstant - 31LL * 24 * 60 * 60 * 1000)},
        {BSON("$dateSubtract" << BSON("startDate" << testDate << "unit"
                                                  << "month"
                                                  << "amount" << -1)),
         // Adds 30 days for day adjustment to 2020-11-30.
         Date_t::fromMillisSinceEpoch(startInstant + 30LL * 24 * 60 * 60 * 1000)},
        {BSON("$dateSubtract" << BSON("startDate" << testDate << "unit"
                                                  << "day"
                                                  << "amount" << 3)),
         Date_t::fromMillisSinceEpoch(startInstant - 3LL * 24 * 60 * 60 * 1000)},
        {BSON("$dateSubtract" << BSON("startDate" << testDate << "unit"
                                                  << "minute"
                                                  << "amount" << 10)),
         Date_t::fromMillisSinceEpoch(startInstant - 10 * 60 * 1000)},
        {BSON("$dateSubtract" << BSON("startDate" << testDate << "unit"
                                                  << "day"
                                                  << "amount" << 7 << "timezone"
                                                  << "Europe/Amsterdam")),
         // Subtracts additional 60 minutes due to crossing DST change in Ams zone.
         Date_t::fromMillisSinceEpoch(startInstant - (7 * 24 + 1) * 60 * 60 * 1000)}};

    for (auto&& testCase : testCases) {
        auto dateAddExp =
            Expression::parseExpression(expCtx.get(), testCase.doc, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(Value(testCase.expected), dateAddExp->evaluate({}, &expCtx->variables));
    }
}

}  // namespace expression_evaluate_date_arithmetics_test

}  // namespace mongo
