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

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace ExpressionDateFromPartsTest {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionDateFromPartsTest = AggregationContextFixture;

TEST_F(ExpressionDateFromPartsTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();

    // Test that it serializes to the full format if given an object specification.
    BSONObj spec =
        BSON("$dateFromParts" << BSON("year" << 2017 << "month" << 6 << "day" << 27 << "hour" << 14
                                             << "minute" << 37 << "second" << 15 << "millisecond"
                                             << 414 << "timezone"
                                             << "America/Los_Angeles"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
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
    ASSERT_VALUE_EQ(
        dateExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
        expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(), expectedSerialization);
}

TEST_F(ExpressionDateFromPartsTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();
    auto spec = BSON("$dateFromParts" << BSON("year" << 2017));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both year, month and day are provided, and are both
    // constants.
    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "month" << 6 << "day" << 27));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both year, hour and minute are provided, and are both
    // expressions which evaluate to constants.
    spec = BSON("$dateFromParts" << BSON("year" << BSON("$add" << BSON_ARRAY(1900 << 107)) << "hour"
                                                << BSON("$add" << BSON_ARRAY(13 << 1)) << "minute"
                                                << BSON("$add" << BSON_ARRAY(40 << 3))));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both year and milliseconds are provided, and year is an
    // expressions which evaluate to a constant, with milliseconds a constant
    spec = BSON("$dateFromParts" << BSON("year" << BSON("$add" << BSON_ARRAY(1900 << 107))
                                                << "millisecond" << 514));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both isoWeekYear, and isoWeek are provided, and are both
    // constants.
    spec = BSON("$dateFromParts" << BSON("isoWeekYear" << 2017 << "isoWeek" << 26));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both isoWeekYear, isoWeek and isoDayOfWeek are provided,
    // and are both expressions which evaluate to constants.
    spec = BSON("$dateFromParts" << BSON("isoWeekYear"
                                         << BSON("$add" << BSON_ARRAY(1017 << 1000)) << "isoWeek"
                                         << BSON("$add" << BSON_ARRAY(20 << 6)) << "isoDayOfWeek"
                                         << BSON("$add" << BSON_ARRAY(3 << 2))));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both year and month are provided, but
    // year is not a constant.
    spec = BSON("$dateFromParts" << BSON("year" << "$year"
                                                << "month" << 6));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both year and day are provided, but
    // day is not a constant.
    spec = BSON("$dateFromParts" << BSON("year" << 2017 << "day"
                                                << "$day"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both isoWeekYear and isoDayOfWeek are provided,
    // but isoDayOfWeek is not a constant.
    spec = BSON("$dateFromParts" << BSON("isoWeekYear" << 2017 << "isoDayOfWeek"
                                                       << "$isoDayOfWeekday"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
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
                                                      << "iso8601" << false));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    auto expectedSerialization =
        Value(Document{{"$dateToParts",
                        Document{{"date", Document{{"$const", Date_t{}}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}},
                                 {"iso8601", Document{{"$const", false}}}}}});
    ASSERT_VALUE_EQ(
        dateExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
        expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(), expectedSerialization);
}

TEST_F(ExpressionDateToPartsTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();
    auto spec = BSON("$dateToParts" << BSON("date" << Date_t{}));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and timezone are provided, and are both
    // constants.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "timezone"
                                              << "UTC"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and timezone are provided, and are both
    // expressions which evaluate to constants.
    spec = BSON("$dateToParts" << BSON("date"
                                       << BSON("$add" << BSON_ARRAY(Date_t{} << 1000)) << "timezone"
                                       << BSON("$concat" << BSON_ARRAY("Europe" << "/"
                                                                                << "London"))));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and iso8601 are provided, and are both
    // constants.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "iso8601" << true));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both date and iso8601 are provided, and are both
    // expressions which evaluate to constants.
    spec = BSON("$dateToParts" << BSON("date" << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))
                                              << "iso8601" << BSON("$not" << false)));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both date and timezone are provided, but
    // date is not a constant.
    spec = BSON("$dateToParts" << BSON("date" << "$date"
                                              << "timezone"
                                              << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both date and timezone are provided, but
    // timezone is not a constant.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "timezone"
                                              << "$tz"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both date and iso8601 are provided, but
    // iso8601 is not a constant.
    spec = BSON("$dateToParts" << BSON("date" << Date_t{} << "iso8601"
                                              << "$iso8601"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
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
            BSON(expName << BSON("date" << "$date")),
            BSON(expName << BSON("date" << BSON("$add" << BSON_ARRAY("$date" << 1000)))),
            BSON(expName << BSON("date" << Date_t{} << "timezone"
                                        << "Europe/London")),
            BSON(expName << BSON("date" << Date_t{} << "timezone"
                                        << "$tz"))};
        for (auto&& syntax : possibleSyntaxes) {
            Expression::parseExpression(expCtx.get(), syntax, expCtx->variablesParseState);
        }
    }
}

TEST_F(DateExpressionTest, ParsingRejectsUnrecognizedFieldsInObjectSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "Europe/London"
                                                   << "extra" << 4));
        ASSERT_THROWS_CODE(
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
            AssertionException,
            40535);
    }
}

TEST_F(DateExpressionTest, ParsingRejectsEmptyObjectSpecification) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSONObj());
        ASSERT_THROWS_CODE(
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
            AssertionException,
            40539);
    }
}

TEST_F(DateExpressionTest, RejectsEmptyArray) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSONArray());
        // It will parse as an ExpressionArray, and fail at runtime.
        ASSERT_THROWS_CODE(
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
            AssertionException,
            40536);
    }
}

TEST_F(DateExpressionTest, RejectsArraysWithMoreThanOneElement) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        BSONObj spec = BSON(expName << BSON_ARRAY("$date" << "$tz"));
        // It will parse as an ExpressionArray, and fail at runtime.
        ASSERT_THROWS_CODE(
            Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState),
            AssertionException,
            40536);
    }
}

TEST_F(DateExpressionTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        // Test that it serializes to the full format if given an object specification.
        BSONObj spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                                   << "Europe/London"));
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        auto expectedSerialization =
            Value(Document{{expName,
                            Document{{"date", Document{{"$const", Date_t{}}}},
                                     {"timezone", Document{{"$const", "Europe/London"_sd}}}}}});
        ASSERT_VALUE_EQ(
            dateExp->serialize(SerializationOptions{
                .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
            expectedSerialization);
        ASSERT_VALUE_EQ(dateExp->serialize(), expectedSerialization);

        // Test that it serializes to the full format if given a date.
        spec = BSON(expName << Date_t{});
        expectedSerialization =
            Value(Document{{expName, Document{{"date", Document{{"$const", Date_t{}}}}}}});
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(
            dateExp->serialize(SerializationOptions{
                .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
            expectedSerialization);
        ASSERT_VALUE_EQ(dateExp->serialize(), expectedSerialization);

        // Test that it serializes to the full format if given a date within an array.
        spec = BSON(expName << BSON_ARRAY(Date_t{}));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_VALUE_EQ(
            dateExp->serialize(SerializationOptions{
                .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
            expectedSerialization);
        ASSERT_VALUE_EQ(dateExp->serialize(), expectedSerialization);
    }
}

TEST_F(DateExpressionTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateExpressions) {
        // Test that it becomes a constant if only date is provided, and it is constant.
        auto spec = BSON(expName << BSON("date" << Date_t{}));
        auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it becomes a constant if both date and timezone are provided, and are both
        // constants.
        spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                           << "Europe/London"));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it becomes a constant if both date and timezone are provided, and are both
        // expressions which evaluate to constants.
        spec = BSON(expName << BSON("date" << BSON("$add" << BSON_ARRAY(Date_t{} << 1000))
                                           << "timezone"
                                           << BSON("$concat" << BSON_ARRAY("Europe" << "/"
                                                                                    << "London"))));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it does *not* become a constant if both date and timezone are provided, but
        // date is not a constant.
        spec = BSON(expName << BSON("date" << "$date"
                                           << "timezone"
                                           << "Europe/London"));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

        // Test that it does *not* become a constant if both date and timezone are provided, but
        // timezone is not a constant.
        spec = BSON(expName << BSON("date" << Date_t{} << "timezone"
                                           << "$tz"));
        dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
        ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
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
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    auto expectedSerialization =
        Value(Document{{"$dateToString",
                        Document{{"date", Document{{"$const", Date_t{}}}},
                                 {"format", Document{{"$const", "%Y-%m-%d"_sd}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}},
                                 {"onNull", Document{{"$const", "nullDefault"_sd}}}}}});

    ASSERT_VALUE_EQ(
        dateExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
        expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(), expectedSerialization);
}

TEST_F(ExpressionDateToStringTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();

    // Test that it becomes a constant if date is constant, and both format and timezone are
    // missing.
    auto spec = BSON("$dateToString" << BSON("date" << Date_t{}));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if both format and date are constant, and timezone is
    // missing.
    spec = BSON("$dateToString" << BSON("format" << "%Y-%m-%d"
                                                 << "date" << Date_t{}));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if format, date and timezone are provided, and all are
    // constants.
    spec = BSON("$dateToString" << BSON("format" << "%Y-%m-%d"
                                                 << "date" << Date_t{} << "timezone"
                                                 << "Europe/Amsterdam"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if format, date and timezone are provided, and all
    // expressions which evaluate to constants.
    spec = BSON("$dateToString" << BSON(
                    "format" << "%Y-%m%d"
                             << "date" << BSON("$add" << BSON_ARRAY(Date_t{} << 1000)) << "timezone"
                             << BSON("$concat" << BSON_ARRAY("Europe" << "/"
                                                                      << "London"))));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant if all parameters are constant, including the optional
    // 'onNull'.
    spec = BSON("$dateToString" << BSON("format" << "%Y-%m-%d"
                                                 << "date" << Date_t{} << "timezone"
                                                 << "Europe/Amsterdam"
                                                 << "onNull"
                                                 << "null default"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both format, date and timezone are provided, but
    // date is not a constant.
    spec = BSON("$dateToString" << BSON("format" << "%Y-%m-%d"
                                                 << "date"
                                                 << "$date"
                                                 << "timezone"
                                                 << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if both format, date and timezone are provided, but
    // timezone is not a constant.
    spec = BSON("$dateToString" << BSON("format" << "%Y-%m-%d"
                                                 << "date" << Date_t{} << "timezone"
                                                 << "$tz"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if 'onNull' does not evaluate to a constant.
    spec = BSON("$dateToString" << BSON("format" << "%Y-%m-%d"
                                                 << "date" << Date_t{} << "onNull"
                                                 << "$onNull"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if 'format' does not evaluate to a constant.
    spec = BSON("$dateToString" << BSON("format" << "$format"
                                                 << "date" << Date_t{}));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
}

}  // namespace ExpressionDateToStringTest

namespace ExpressionDateFromStringTest {

// This provides access to an ExpressionContext that has a valid ServiceContext with a
// TimeZoneDatabase via getExpCtx(), but we'll use a different name for this test suite.
using ExpressionDateFromStringTest = AggregationContextFixture;

TEST_F(ExpressionDateFromStringTest, SerializesToObjectSyntax) {
    auto expCtx = getExpCtx();

    // Test that it serializes to the full format if given an object specification.
    BSONObj spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:06:44Z"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    auto expectedSerialization = Value(
        Document{{"$dateFromString",
                  Document{{"dateString", Document{{"$const", "2017-07-04T13:06:44Z"_sd}}}}}});

    ASSERT_VALUE_EQ(
        dateExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
        expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(), expectedSerialization);

    // Test that it serializes to the full format if given an object specification.
    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:06:44Z"
                                                       << "timezone"
                                                       << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    expectedSerialization =
        Value(Document{{"$dateFromString",
                        Document{{"dateString", Document{{"$const", "2017-07-04T13:06:44Z"_sd}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}}}}});

    ASSERT_VALUE_EQ(
        dateExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
        expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(), expectedSerialization);

    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:06:44Z"
                                                       << "timezone"
                                                       << "Europe/London"
                                                       << "format"
                                                       << "%Y-%d-%mT%H:%M:%S"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    expectedSerialization =
        Value(Document{{"$dateFromString",
                        Document{{"dateString", Document{{"$const", "2017-07-04T13:06:44Z"_sd}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}},
                                 {"format", Document{{"$const", "%Y-%d-%mT%H:%M:%S"_sd}}}}}});

    ASSERT_VALUE_EQ(
        dateExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
        expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(), expectedSerialization);

    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:06:44Z"
                                                       << "timezone"
                                                       << "Europe/London"
                                                       << "format"
                                                       << "%Y-%d-%mT%H:%M:%S"
                                                       << "onNull"
                                                       << "nullDefault"
                                                       << "onError"
                                                       << "errorDefault"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    expectedSerialization =
        Value(Document{{"$dateFromString",
                        Document{{"dateString", Document{{"$const", "2017-07-04T13:06:44Z"_sd}}},
                                 {"timezone", Document{{"$const", "Europe/London"_sd}}},
                                 {"format", Document{{"$const", "%Y-%d-%mT%H:%M:%S"_sd}}},
                                 {"onNull", Document{{"$const", "nullDefault"_sd}}},
                                 {"onError", Document{{"$const", "errorDefault"_sd}}}}}});

    ASSERT_VALUE_EQ(
        dateExp->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
        expectedSerialization);
    ASSERT_VALUE_EQ(dateExp->serialize(), expectedSerialization);
}

TEST_F(ExpressionDateFromStringTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto expCtx = getExpCtx();

    // Test that it becomes a constant if all parameters evaluate to a constant value.
    auto spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:09:57Z"));
    auto dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    Date_t dateVal = Date_t::fromMillisSinceEpoch(1499173797000);
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:09:57"
                                                       << "timezone"
                                                       << "Europe/London"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it becomes a constant with the dateString, timezone, and format being a constant.
    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:09:57"
                                                       << "timezone"
                                                       << "Europe/London"
                                                       << "format"
                                                       << "%Y-%m-%dT%H:%M:%S"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    dateVal = Date_t::fromMillisSinceEpoch(1499170197000);
    ASSERT_VALUE_EQ(Value(dateVal), dateExp->evaluate({}, &expCtx->variables));

    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:09:57"
                                                       << "onNull"
                                                       << "Null default"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:09:57"
                                                       << "onError"
                                                       << "Error default"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:09:57"
                                                       << "onError"
                                                       << "Error default"
                                                       << "onNull"
                                                       << "null default"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if dateString is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString" << "$date"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if timezone is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:09:57Z"
                                                       << "timezone"
                                                       << "$tz"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if format is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:09:57Z"
                                                       << "timezone"
                                                       << "Europe/London"
                                                       << "format"
                                                       << "$format"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if onNull is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:09:57Z"
                                                       << "onNull"
                                                       << "$onNull"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));

    // Test that it does *not* become a constant if onError is not a constant.
    spec = BSON("$dateFromString" << BSON("dateString" << "2017-07-04T13:09:57Z"
                                                       << "onError"
                                                       << "$onError"));
    dateExp = Expression::parseExpression(expCtx.get(), spec, expCtx->variablesParseState);
    ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateExp->optimize().get()));
}

}  // namespace ExpressionDateFromStringTest

namespace {

/**
 * Parses expression 'expressionSpec' which is expected to parse successfully and then serializes
 * expression instance to compare with 'expectedSerializedExpressionSpec'.
 */
void assertParsesAndSerializesExpression(boost::intrusive_ptr<ExpressionContextForTest> expCtx,
                                         BSONObj expressionSpec,
                                         BSONObj expectedSerializedExpressionSpec) {
    const auto expression =
        Expression::parseExpression(expCtx.get(), expressionSpec, expCtx->variablesParseState);
    const auto expectedSerialization = Value(expectedSerializedExpressionSpec);
    ASSERT_VALUE_EQ(
        expression->serialize(SerializationOptions{
            .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
        expectedSerialization);
    ASSERT_VALUE_EQ(expression->serialize(), expectedSerialization);

    // Verify that parsed and then serialized expression is the same.
    ASSERT_VALUE_EQ(Expression::parseExpression(
                        expCtx.get(), expectedSerializedExpressionSpec, expCtx->variablesParseState)
                        ->serialize(),
                    expectedSerialization);
}

/**
 * Asserts that 'optimize()' for 'expression' returns the same expression when not all parameters
 * evaluate to constants.
 */
void assertExpressionNotOptimized(boost::intrusive_ptr<Expression> expression) {
    const auto optimizedExpression = expression->optimize();
    ASSERT_EQUALS(expression.get(), optimizedExpression.get()) << " expression was optimized out";
}
}  // namespace

namespace ExpressionDateDiffTest {
class ExpressionDateDiffTest : public AggregationContextFixture {
public:
    /**
     * Parses expression 'expression' and asserts that the expression fails to parse with error
     * 'expectedErrorCode' and exception message 'expectedErrorMessage'.
     */
    void assertFailsToParseExpression(BSONObj expression,
                                      int expectedErrorCode,
                                      std::string expectedErrorMessage) {
        auto expCtx = getExpCtx();
        ASSERT_THROWS_CODE_AND_WHAT(
            Expression::parseExpression(expCtx.get(), expression, expCtx->variablesParseState),
            AssertionException,
            expectedErrorCode,
            expectedErrorMessage);
    }

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

TEST_F(ExpressionDateDiffTest, ParsesAndSerializesValidExpression) {
    assertParsesAndSerializesExpression(
        getExpCtx(),
        BSON("$dateDiff" << BSON("startDate" << "$startDateField"
                                             << "endDate"
                                             << "$endDateField"
                                             << "unit"
                                             << "day"
                                             << "timezone"
                                             << "America/New_York"
                                             << "startOfWeek"
                                             << "Monday")),
        BSON("$dateDiff" << BSON("startDate" << "$startDateField"
                                             << "endDate"
                                             << "$endDateField"
                                             << "unit" << BSON("$const" << "day") << "timezone"
                                             << BSON("$const" << "America/New_York")
                                             << "startOfWeek" << BSON("$const" << "Monday"))));
    assertParsesAndSerializesExpression(getExpCtx(),
                                        BSON("$dateDiff" << BSON("startDate" << "$startDateField"
                                                                             << "endDate"
                                                                             << "$endDateField"
                                                                             << "unit"
                                                                             << "$unit")),
                                        BSON("$dateDiff" << BSON("startDate" << "$startDateField"
                                                                             << "endDate"
                                                                             << "$endDateField"
                                                                             << "unit"
                                                                             << "$unit")));
}

TEST_F(ExpressionDateDiffTest, ParsesInvalidExpression) {
    // Verify that invalid fields are rejected.
    assertFailsToParseExpression(BSON("$dateDiff" << BSON("startDate" << "$startDateField"
                                                                      << "endDate"
                                                                      << "$endDateField"
                                                                      << "unit"
                                                                      << "day"
                                                                      << "timeGone"
                                                                      << "yes")),
                                 5166302,
                                 "Unrecognized argument to $dateDiff: timeGone");

    // Verify that field 'startDate' is required.
    assertFailsToParseExpression(BSON("$dateDiff" << BSON("endDate" << "$endDateField"
                                                                    << "unit"
                                                                    << "day")),
                                 5166303,
                                 "Missing 'startDate' parameter to $dateDiff");

    // Verify that field 'endDate' is required.
    assertFailsToParseExpression(BSON("$dateDiff" << BSON("startDate" << "$startDateField"
                                                                      << "unit"
                                                                      << "day")),
                                 5166304,
                                 "Missing 'endDate' parameter to $dateDiff");

    // Verify that field 'unit' is required.
    assertFailsToParseExpression(BSON("$dateDiff" << BSON("startDate" << "$startDateField"
                                                                      << "endDate"
                                                                      << "$endDateField")),
                                 5166305,
                                 "Missing 'unit' parameter to $dateDiff");

    // Verify that only $dateDiff: {..} is accepted.
    assertFailsToParseExpression(BSON("$dateDiff" << "startDate"),
                                 5166301,
                                 "$dateDiff only supports an object as its argument");
}

TEST_F(ExpressionDateDiffTest, OptimizesToConstantIfAllInputsAreConstant) {
    auto dateDiffExpression = buildExpressionWithParameters(
        Value{Date_t::fromMillisSinceEpoch(0)},
        Value{Date_t::fromMillisSinceEpoch(31571873000) /*1971-mm-dd*/},
        Value{"year"_sd},
        Value{"GMT"_sd},
        Value{"Sunday"_sd});

    // Verify that 'optimize()' returns a constant expression when all parameters evaluate to
    // constants.
    auto optimizedDateDiffExpression1 = dateDiffExpression->optimize();
    auto constantExpression = dynamic_cast<ExpressionConstant*>(optimizedDateDiffExpression1.get());
    ASSERT(constantExpression);
    ASSERT_VALUE_EQ(Value{1LL}, constantExpression->getValue());
}

TEST_F(ExpressionDateDiffTest, DoesNotOptimizeToConstantIfNotAllInputsAreConstant) {
    auto dateDiffExpression = buildExpressionWithParameters(Value{Date_t::fromMillisSinceEpoch(0)},
                                                            Value{Date_t::fromMillisSinceEpoch(0)},
                                                            Value{"$year"_sd},
                                                            Value{} /* Time zone not specified*/);
    assertExpressionNotOptimized(dateDiffExpression);
}

TEST_F(ExpressionDateDiffTest, AddsDependencies) {
    auto dateDiffExpression = buildExpressionWithParameters(Value{"$startDateField"_sd},
                                                            Value{"$endDateField"_sd},
                                                            Value{"$unitField"_sd},
                                                            Value{"$timezoneField"_sd},
                                                            Value{"$startOfWeekField"_sd});

    // Verify that dependencies for $dateDiff expression are determined correctly.
    auto depsTracker = expression::getDependencies(dateDiffExpression.get());
    ASSERT_TRUE(
        (depsTracker.fields ==
         OrderedPathSet{
             "startDateField", "endDateField", "unitField", "timezoneField", "startOfWeekField"}));
}
}  // namespace ExpressionDateDiffTest

namespace {
class ExpressionDateTruncTest : public AggregationContextFixture {
public:
    /**
     * Builds a $dateTrunc expression with given values of parameters.
     */
    auto buildExpressionWithParameters(
        Value date, Value unit, Value binSize, Value timezone, Value startOfWeek = Value{}) {
        const auto expCtx = getExpCtx();
        const auto expression = BSON(
            "$dateTrunc" << BSON("date" << date << "unit" << unit << "binSize" << binSize
                                        << "timezone" << timezone << "startOfWeek" << startOfWeek));
        return Expression::parseExpression(expCtx.get(), expression, expCtx->variablesParseState);
    }
};

TEST_F(ExpressionDateTruncTest, ParsesAndSerializesValidExpression) {
    assertParsesAndSerializesExpression(
        getExpCtx(),
        BSON("$dateTrunc" << BSON("date" << "$dateField"
                                         << "unit"
                                         << "day"
                                         << "binSize"
                                         << "$binSizeField"
                                         << "timezone"
                                         << "America/New_York"
                                         << "startOfWeek"
                                         << "Monday")),
        BSON("$dateTrunc" << BSON("date" << "$dateField"
                                         << "unit" << BSON("$const" << "day") << "binSize"
                                         << "$binSizeField"
                                         << "timezone" << BSON("$const" << "America/New_York")
                                         << "startOfWeek" << BSON("$const" << "Monday"))));
    auto expressionSpec = BSON("$dateTrunc" << BSON("date" << "$dateField"
                                                           << "unit"
                                                           << "$unit"));
    assertParsesAndSerializesExpression(getExpCtx(), expressionSpec, expressionSpec);
}

TEST_F(ExpressionDateTruncTest, OptimizesToConstantIfAllInputsAreConstant) {
    const auto dateTruncExpression = buildExpressionWithParameters(
        Value{Date_t::fromMillisSinceEpoch(1612137600000) /*2021-02-01*/},
        Value{"year"_sd},
        Value{1LL},
        Value{"GMT"_sd},
        Value{"Sunday"_sd});

    // Verify that 'optimize()' returns a constant expression when all parameters evaluate to
    // constants.
    const auto optimizedDateTruncExpression = dateTruncExpression->optimize();
    const auto constantExpression =
        dynamic_cast<ExpressionConstant*>(optimizedDateTruncExpression.get());
    ASSERT(constantExpression);
    ASSERT_VALUE_EQ(Value{Date_t::fromMillisSinceEpoch(1609459200000)} /*2021-01-01*/,
                    constantExpression->getValue());
}

TEST_F(ExpressionDateTruncTest, DoesNotOptimizeToConstantIfNotAllInputsAreConstant) {
    const Value someDate{Date_t::fromMillisSinceEpoch(0)};
    const Value year{"year"_sd};
    const Value utc{"UTC"_sd};
    assertExpressionNotOptimized(
        buildExpressionWithParameters(Value{"$date"_sd}, year, Value{1LL}, utc));
    assertExpressionNotOptimized(
        buildExpressionWithParameters(someDate, Value{"$year"_sd}, Value{1LL}, utc));
    assertExpressionNotOptimized(
        buildExpressionWithParameters(someDate, year, Value{"$binSize"_sd}, utc));
    assertExpressionNotOptimized(
        buildExpressionWithParameters(someDate, year, Value{1LL}, Value{"$timezone"_sd}));
    assertExpressionNotOptimized(
        buildExpressionWithParameters(someDate, year, Value{1LL}, utc, Value{"$startOfWeek"_sd}));
}

TEST_F(ExpressionDateTruncTest, AddsDependencies) {
    const auto dateTruncExpression = buildExpressionWithParameters(Value{"$dateField"_sd},
                                                                   Value{"$unitField"_sd},
                                                                   Value{"$binSizeField"_sd},
                                                                   Value{"$timezoneField"_sd},
                                                                   Value{"$startOfWeekField"_sd});

    // Verify that dependencies for $dateTrunc expression are determined correctly.
    const auto depsTracker = expression::getDependencies(dateTruncExpression.get());
    ASSERT_TRUE(
        (depsTracker.fields ==
         OrderedPathSet{
             "dateField", "unitField", "binSizeField", "timezoneField", "startOfWeekField"}));
}
}  // namespace

namespace ExpressionDateArithmeticsTest {
using ExpressionDateArithmeticsTest = AggregationContextFixture;

std::vector<StringData> dateArithmeticsExp = {"$dateAdd"_sd, "$dateSubtract"_sd};

TEST_F(ExpressionDateArithmeticsTest, SerializesToObject) {
    auto expCtx = getExpCtx();

    for (auto&& expName : dateArithmeticsExp) {
        BSONObj doc = BSON(expName << BSON("startDate" << Date_t{} << "unit"
                                                       << "day"
                                                       << "amount" << 1));
        auto dateAddExp =
            Expression::parseExpression(expCtx.get(), doc, expCtx->variablesParseState);
        auto expectedSerialization =
            Value(Document{{expName,
                            Document{{"startDate", Document{{"$const", Date_t{}}}},
                                     {"unit", Document{{"$const", "day"_sd}}},
                                     {"amount", Document{{"$const", 1}}}}}});
        ASSERT_VALUE_EQ(
            dateAddExp->serialize(SerializationOptions{
                .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
            expectedSerialization);
        ASSERT_VALUE_EQ(dateAddExp->serialize(), expectedSerialization);

        // with timezone
        doc = BSON(expName << BSON("startDate" << Date_t{} << "unit"
                                               << "day"
                                               << "amount" << -1 << "timezone"
                                               << "America/New_York"));
        dateAddExp = Expression::parseExpression(expCtx.get(), doc, expCtx->variablesParseState);
        expectedSerialization =
            Value(Document{{expName,
                            Document{{"startDate", Document{{"$const", Date_t{}}}},
                                     {"unit", Document{{"$const", "day"_sd}}},
                                     {"amount", Document{{"$const", -1}}},
                                     {"timezone", Document{{"$const", "America/New_York"_sd}}}}}});
        ASSERT_VALUE_EQ(
            dateAddExp->serialize(SerializationOptions{
                .verbosity = boost::make_optional(ExplainOptions::Verbosity::kQueryPlanner)}),
            expectedSerialization);
        ASSERT_VALUE_EQ(dateAddExp->serialize(), expectedSerialization);
    }
}

TEST_F(ExpressionDateArithmeticsTest, ParsesInvalidDocument) {
    auto expCtx = getExpCtx();

    struct TestCase {
        BSONObj doc;
        int errorCode;
    };

    for (auto&& expName : dateArithmeticsExp) {
        std::vector<TestCase> testCases = {
            {BSON(expName << BSON("startDate" << Date_t{} << "unit"
                                              << "day")),
             5166402},
            {BSON(expName << BSON("startDate" << Date_t{} << "amount" << 1)), 5166402},

            {BSON(expName << BSON("unit" << "day"
                                         << "amount" << 1 << "timezone"
                                         << "Europe/London")),
             5166402},
            {BSON(expName << BSON("startDate" << Date_t{} << "timeUnit"
                                              << "day"
                                              << "amount" << 1 << "timezone"
                                              << "Europe/London")),
             5166401}};

        for (auto&& testCase : testCases) {
            ASSERT_THROWS_CODE(Expression::parseExpression(
                                   expCtx.get(), testCase.doc, expCtx->variablesParseState),
                               AssertionException,
                               testCase.errorCode);
        }
    }
}

TEST_F(ExpressionDateArithmeticsTest, OptimizesToConstant) {
    auto expCtx = getExpCtx();
    Date_t testDate = Date_t::fromMillisSinceEpoch(1604139115000);

    for (auto&& expName : dateArithmeticsExp) {
        BSONObj doc = BSON(expName << BSON("startDate" << testDate << "unit"
                                                       << "day"
                                                       << "amount" << 1));
        auto dateAddExp =
            Expression::parseExpression(expCtx.get(), doc, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateAddExp->optimize().get()));

        doc = BSON(expName << BSON("startDate" << testDate << "unit"
                                               << "day"
                                               << "amount" << 1 << "timezone"
                                               << "Europe/London"));
        dateAddExp = Expression::parseExpression(expCtx.get(), doc, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateAddExp->optimize().get()));

        // Test that $$NOW will be optimized as constant.
        doc = BSON(expName << BSON("startDate" << "$$NOW"
                                               << "unit"
                                               << "day"
                                               << "amount" << 1));
        dateAddExp = Expression::parseExpression(expCtx.get(), doc, expCtx->variablesParseState);
        ASSERT(dynamic_cast<ExpressionConstant*>(dateAddExp->optimize().get()));


        // Test that expression does not optimize to constant if some of the parameters is not a
        // constant
        doc = BSON(expName << BSON("startDate" << "$sentDate"
                                               << "unit"
                                               << "day"
                                               << "amount" << 1));
        dateAddExp = Expression::parseExpression(expCtx.get(), doc, expCtx->variablesParseState);
        ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateAddExp->optimize().get()));

        doc = BSON(expName << BSON("startDate" << testDate << "unit"
                                               << "$unit"
                                               << "amount" << 100));
        dateAddExp = Expression::parseExpression(expCtx.get(), doc, expCtx->variablesParseState);
        ASSERT_FALSE(dynamic_cast<ExpressionConstant*>(dateAddExp->optimize().get()));
    }
}

TEST_F(ExpressionDateArithmeticsTest, AddsDependencies) {
    auto expCtx = getExpCtx();
    for (auto&& expName : dateArithmeticsExp) {
        BSONObj doc = BSON(expName << BSON("startDate" << "$date"
                                                       << "unit"
                                                       << "$unit"
                                                       << "amount"
                                                       << "$amount"
                                                       << "timezone"
                                                       << "$timezone"));
        auto dateAddExp =
            Expression::parseExpression(expCtx.get(), doc, expCtx->variablesParseState);
        DepsTracker dependencies;
        expression::addDependencies(dateAddExp.get(), &dependencies);
        ASSERT_EQ(dependencies.fields.size(), 4UL);
        ASSERT_EQ(dependencies.fields.count("date"), 1UL);
        ASSERT_EQ(dependencies.fields.count("unit"), 1UL);
        ASSERT_EQ(dependencies.fields.count("amount"), 1UL);
        ASSERT_EQ(dependencies.fields.count("timezone"), 1UL);
    }
}
}  // namespace ExpressionDateArithmeticsTest

}  // namespace mongo
