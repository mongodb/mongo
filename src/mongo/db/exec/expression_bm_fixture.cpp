/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/exec/expression_bm_fixture.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/server_options.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/time_support.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>

#include <benchmark/benchmark.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/random/normal_distribution.hpp>

namespace mongo {

namespace {
using Parser = std::function<boost::intrusive_ptr<Expression>(
    ExpressionContext* const, BSONElement, const VariablesParseState&)>;

BSONArray rangeBSONArray(int start, int end) {
    BSONArrayBuilder builder;
    for (int i = start; i < end; i++) {
        builder.append(std::to_string(i));
    }
    return builder.arr();
}

template <typename T>
BSONArray vectorToBSON(const std::vector<T>& vec) {
    BSONArrayBuilder builder;
    for (const auto& val : vec) {
        builder.append(val);
    }
    return builder.arr();
}

template <typename Generator>
std::vector<Document> randomPairs(int count, Generator generator) {
    std::vector<Document> documents;
    documents.reserve(count);

    for (int i = 0; i < count; ++i) {
        documents.emplace_back(BSON("lhs" << generator() << "rhs" << generator()));
    }

    return documents;
}

auto getDecimalGenerator(PseudoRandom& random) {
    static constexpr int64_t kMax = 9'000'000'000'000'000;
    static constexpr Decimal128 kHighDigits(1'000'000'000'000'000'000);
    return [&]() {
        return Decimal128(random.nextInt64(kMax))
            .multiply(kHighDigits)
            .add(Decimal128(random.nextInt64(kMax)));
    };
}

std::vector<double> generateNormal(size_t n) {
    std::mt19937 generator(2023u);
    boost::random::normal_distribution<double> dist(0.0 /* mean */, 1.0 /* sigma */);

    std::vector<double> inputs;
    inputs.reserve(n);
    for (size_t i = 0; i < n; i++) {
        inputs.push_back(dist(generator));
    }

    return inputs;
}

void configureLogging(bool disable) {
    auto& lv2Manager = logv2::LogManager::global();
    logv2::LogDomainGlobal::ConfigurationOptions lv2Config;
    if (disable)
        lv2Config.makeDisabled();
    uassertStatusOK(lv2Manager.getGlobalDomainInternal().configure(lv2Config));
}

/**
 * Registers a new expression that is gated behind a feature flag and therefore does not get put
 * into the map by default.
 */
void registerExpressionForBenchmark(std::string expression, Parser parser) {
    // Temporarily disable logging because we don't want logs from expression registration to
    // clutter the benchmark results table.
    configureLogging(true);
    ON_BLOCK_EXIT([&] { configureLogging(false); });
    try {
        Expression::registerExpression(expression,
                                       parser,
                                       AllowedWithApiStrict::kNeverInVersion1,
                                       AllowedWithClientType::kAny,
                                       nullptr /* featureFlag */);
    } catch (const DBException& e) {
        ASSERT(e.reason() == "Duplicate expression (" + expression + ") registered.");
    }
}

Document genDocument(size_t fieldsPerLevel, size_t depth) {
    if (depth < 1) {
        return {};
    }

    static const std::vector<Value> values{
        Value(BSONNULL),
        Value(true),
        Value(123123123),
        Value(12313123123LL),
        Value(std::vector<Value>{Value(1), Value(2)}),
        Value(Decimal128{"-9223372036854775809.99"}),
        Value(Timestamp::max()),
        Value(OID::max()),
        Value("\"string\""_sd),
        Value(UUID::parse("00000000-0000-0000-0000-000000000000").getValue()),
        Value(BSONRegEx{"abc", "g"}),
        Value(MINKEY),
        Value(MAXKEY),
    };

    MutableDocument doc;
    for (size_t i = 0; i < fieldsPerLevel; i++) {
        std::string key = str::stream() << "field" << i;
        doc.setField(key, values[i % values.size()]);
    }
    doc.setField(str::stream() << "level" << depth, Value(genDocument(fieldsPerLevel, depth - 1)));
    return doc.freeze();
}

std::string genJsonString(size_t fieldsPerLevel, size_t depth) {
    if (depth < 1) {
        return "{}";
    }

    static const std::vector<StringData> values{
        "null",
        "true",
        "false",
        "18446744073709551615",
        "\"string\"",
        "[1,2,3]",
    };

    str::stream ss{};
    ss << '{';
    for (size_t i = 0; i < fieldsPerLevel; i++) {
        if (i > 0) {
            ss << ',';
        }
        ss << "\"field" << i << "\":" << values[i % values.size()];
    }
    ss << ",\"child\":" << genJsonString(fieldsPerLevel, depth - 1) << '}';
    return ss;
}

}  // namespace

void ExpressionBenchmarkFixture::increaseJSHeapSizeLimit() {
    originalPerQueryJSHeapSizeLimit = internalQueryJavaScriptHeapSizeLimitMB.load();
    internalQueryJavaScriptHeapSizeLimitMB.store(
        mongo::getGlobalScriptEngine()->getJSHeapLimitMB());
}

void ExpressionBenchmarkFixture::resetJSHeapSizeLimit() {
    internalQueryJavaScriptHeapSizeLimitMB.store(originalPerQueryJSHeapSizeLimit);
}

void ExpressionBenchmarkFixture::SetUp(benchmark::State& state) {
    QueryFCVEnvironmentForTest::setUp();
    // Temporarily disable logging because we don't want logs from service context
    // initialization to clutter the benchmark results table.
    configureLogging(true);
    ON_BLOCK_EXIT([&] { configureLogging(false); });
    _scopedGlobalServiceContext = std::make_unique<QueryTestScopedGlobalServiceContext>();
    ScriptEngine::setup(ExecutionEnvironment::Server);

    // Increase the JS heap size limit. Some benchmarks run JSON.parse() and JSON.stringify() with
    // large inputs which can be memory intensive.
    increaseJSHeapSizeLimit();
}

void ExpressionBenchmarkFixture::TearDown(benchmark::State& state) {
    resetJSHeapSizeLimit();
    ScriptEngine::dropScopeCache();
    _scopedGlobalServiceContext = nullptr;
}

void ExpressionBenchmarkFixture::benchmarkExpression(BSONObj expressionSpec,
                                                     benchmark::State& state) {
    std::vector<Document> documents = {{}};
    benchmarkExpression(expressionSpec, state, documents);
}

void ExpressionBenchmarkFixture::noOpBenchmark(benchmark::State& state) {
    benchmarkExpression(BSON("$const" << 1), state);
}

/**
 * Tests performance of aggregation expression
 *   {"$arrayElemAt": ["$array", 0]}
 * against document
 *   {"_id": ObjectId(...), "array": ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]}
 */
void ExpressionBenchmarkFixture::benchmarkArrayArrayElemAt0(benchmark::State& state) {
    BSONArray array = rangeBSONArray(0, 10);

    benchmarkExpression(BSON("$arrayElemAt" << BSON_ARRAY("$array" << 0)),
                        state,
                        std::vector<Document>(1, {{"array"_sd, array}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$arrayElemAt": ["$array", -1]}
 * against document
 *   {"_id": ObjectId(...), "array": ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]}
 */
void ExpressionBenchmarkFixture::benchmarkArrayArrayElemAtLast(benchmark::State& state) {
    BSONArray array = rangeBSONArray(0, 10);

    benchmarkExpression(BSON("$arrayElemAt" << BSON_ARRAY("$array" << -1)),
                        state,
                        std::vector<Document>(1, {{"array"_sd, array}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$filter": {"input": "$array", "cond": {"$gte": ["$$this", "A"]}}}
 * against document
 *   {"_id": ObjectId(...), "array": ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]}
 * No entries will pass the filter as the entries are strings, not numbers. ("Filter0" indicates
 * 0 entries will pass the filter.)
 */
void ExpressionBenchmarkFixture::benchmarkArrayFilter0(benchmark::State& state) {
    BSONArray array = rangeBSONArray(0, 10);

    benchmarkExpression(
        BSON("$filter" << BSON("input" << "$array"
                                       << "cond"
                                       << BSON("$gte" << BSON_ARRAY("$$this" << "A"_sd)))),
        state,
        std::vector<Document>(1, {{"array"_sd, array}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$filter": {"input": "$array", "cond": {"$gte": ["$$this", "0"]}}}
 * against document
 *   {"_id": ObjectId(...), "array": ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]}
 * All ten entries will pass the filter and be returned in the result array. ("Filter10" indicates
 * 10 entries will pass the filter.)
 */
void ExpressionBenchmarkFixture::benchmarkArrayFilter10(benchmark::State& state) {
    BSONArray array = rangeBSONArray(0, 10);

    benchmarkExpression(
        BSON("$filter" << BSON("input" << "$array"
                                       << "cond"
                                       << BSON("$gte" << BSON_ARRAY("$$this" << "0"_sd)))),
        state,
        std::vector<Document>(1, {{"array"_sd, array}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$in": ["0", "$array"]}
 * against document
 *   {"_id": ObjectId(...), "array": ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]}
 */
void ExpressionBenchmarkFixture::benchmarkArrayInFound0(benchmark::State& state) {
    BSONArray array = rangeBSONArray(0, 10);

    benchmarkExpression(BSON("$in" << BSON_ARRAY("0"_sd << "$array")),
                        state,
                        std::vector<Document>(1, {{"array"_sd, array}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$in": ["9", "$array"]}
 * against document
 *   {"_id": ObjectId(...), "array": ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]}
 */
void ExpressionBenchmarkFixture::benchmarkArrayInFound9(benchmark::State& state) {
    BSONArray array = rangeBSONArray(0, 10);

    benchmarkExpression(BSON("$in" << BSON_ARRAY("9"_sd << "$array")),
                        state,
                        std::vector<Document>(1, {{"array"_sd, array}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$in": ["A", "$array"]}
 * against document
 *   {"_id": ObjectId(...), "array": ["0", "1", "2", "3", "4", "5", "6", "7", "8", "9"]}
 */
void ExpressionBenchmarkFixture::benchmarkArrayInNotFound(benchmark::State& state) {
    BSONArray array = rangeBSONArray(0, 10);

    benchmarkExpression(BSON("$in" << BSON_ARRAY("A"_sd << "$array")),
                        state,
                        std::vector<Document>(1, {{"array"_sd, array}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$eq": ["1", "$value"]}
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkCompareEq(benchmark::State& state) {
    benchmarkExpression(BSON("$eq" << BSON_ARRAY("1" << "$value")),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$gte": ["1", "$value"]}
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkCompareGte(benchmark::State& state) {
    benchmarkExpression(BSON("$gte" << BSON_ARRAY("1" << "$value")),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$lte": ["1", "$value"]}
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkCompareLte(benchmark::State& state) {
    benchmarkExpression(BSON("$lte" << BSON_ARRAY("1" << "$value")),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$ne": ["1", "$value"]}
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkCompareNe(benchmark::State& state) {
    benchmarkExpression(BSON("$ne" << BSON_ARRAY("1" << "$value")),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$cond": [{$eq: ["1", "$value"]}, "1", "0"]}
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkConditionalCond(benchmark::State& state) {
    benchmarkExpression(
        BSON("$cond" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("1"_sd << "$value")) << "1"_sd
                                                                                 << "0"_sd)),
        state,
        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$ifNull": ["$value", "0"]}
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkConditionalIfNullFalse(benchmark::State& state) {
    benchmarkExpression(BSON("$ifNull" << BSON_ARRAY("$value" << "0"_sd)),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$ifNull": ["$value", "0"]}
 * against document
 *   {"_id": ObjectId(...), "value": null}
 */
void ExpressionBenchmarkFixture::benchmarkConditionalIfNullTrue(benchmark::State& state) {
    benchmarkExpression(BSON("$ifNull" << BSON_ARRAY("$value" << "0"_sd)),
                        state,
                        std::vector<Document>(1, {Document(fromjson("{value: null}"))}));
}

/**
 * Tests performance of aggregation expression
 *   {"$switch": {
 *      "branches": [
 *         {"case": {"$eq": ["$value", "1"]}, "then": 0},
 *         {"case": {"$eq": ["$value", "0"]}, "then": 1},
 *       ],
 *       "default": -1
 *     }
 *   }
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 * The first case (position 0) will execute.
 */
void ExpressionBenchmarkFixture::benchmarkConditionalSwitchCase0(benchmark::State& state) {
    benchmarkExpression(
        BSON("$switch" << BSON(
                 "branches"
                 << BSON_ARRAY(
                        BSON("case" << BSON("$eq" << BSON_ARRAY("$value" << "1"_sd)) << "then" << 0)
                        << BSON("case" << BSON("$eq" << BSON_ARRAY("$value" << "0"_sd)) << "then"
                                       << 1))
                 << "default" << -1)),
        state,
        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$switch": {
 *      "branches": [
 *         {"case": {"$eq": ["$value", "0"]}, "then": 0},
 *         {"case": {"$eq": ["$value", "1"]}, "then": 1},
 *       ],
 *       "default": -1
 *     }
 *   }
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 * The second case (position 1) will execute.
 */
void ExpressionBenchmarkFixture::benchmarkConditionalSwitchCase1(benchmark::State& state) {
    benchmarkExpression(
        BSON("$switch" << BSON(
                 "branches"
                 << BSON_ARRAY(
                        BSON("case" << BSON("$eq" << BSON_ARRAY("$value" << "0"_sd)) << "then" << 0)
                        << BSON("case" << BSON("$eq" << BSON_ARRAY("$value" << "1"_sd)) << "then"
                                       << 1))
                 << "default" << -1)),
        state,
        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$switch": {
 *      "branches": [
 *         {"case": {"$eq": ["$value", "0"]}, "then": 0},
 *         {"case": {"$eq": ["$value", "2"]}, "then": 1},
 *       ],
 *       "default": -1
 *     }
 *   }
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 * The default case will execute.
 */
void ExpressionBenchmarkFixture::benchmarkConditionalSwitchDefault(benchmark::State& state) {
    benchmarkExpression(
        BSON("$switch" << BSON(
                 "branches"
                 << BSON_ARRAY(
                        BSON("case" << BSON("$eq" << BSON_ARRAY("$value" << "0"_sd)) << "then" << 0)
                        << BSON("case" << BSON("$eq" << BSON_ARRAY("$value" << "2"_sd)) << "then"
                                       << 1))
                 << "default" << -1)),
        state,
        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

void ExpressionBenchmarkFixture::benchmarkDateDiffEvaluateMinute300Years(benchmark::State& state) {
    testDateDiffExpression(-1640989478000LL /* 1918-01-01*/,
                           7826117722000LL /* 2218-01-01*/,
                           "minute",
                           boost::none /*timezone*/,
                           boost::none /*startOfWeek*/,
                           state);
}

void ExpressionBenchmarkFixture::benchmarkDateDiffEvaluateMinute2Years(benchmark::State& state) {
    testDateDiffExpression(1542448721000LL /* 2018-11-17*/,
                           1605607121000LL /* 2020-11-17*/,
                           "minute",
                           boost::none /*timezone*/,
                           boost::none /*startOfWeek*/,
                           state);
}

void ExpressionBenchmarkFixture::benchmarkDateDiffEvaluateMinute2YearsWithTimezone(
    benchmark::State& state) {
    testDateDiffExpression(1542448721000LL /* 2018-11-17*/,
                           1605607121000LL /* 2020-11-17*/,
                           "minute",
                           std::string{"America/New_York"},
                           boost::none /*startOfWeek*/,
                           state);
}

void ExpressionBenchmarkFixture::benchmarkDateDiffEvaluateWeek(benchmark::State& state) {
    testDateDiffExpression(7826117722000LL /* 2218-01-01*/,
                           4761280721000LL /*2120-11-17*/,
                           "week",
                           boost::none /*timezone*/,
                           std::string("Sunday") /*startOfWeek*/,
                           state);
}


void ExpressionBenchmarkFixture::benchmarkDateAddEvaluate10Days(benchmark::State& state) {
    testDateAddExpression(1604131115000LL,
                          "day",
                          10LL,
                          boost::none, /* timezone */
                          state);
}

void ExpressionBenchmarkFixture::benchmarkDateAddEvaluate600Minutes(benchmark::State& state) {
    testDateAddExpression(1604131115000LL,
                          "minute",
                          600LL,
                          boost::none, /* timezone */
                          state);
}

void ExpressionBenchmarkFixture::benchmarkDateAddEvaluate100KSeconds(benchmark::State& state) {
    testDateAddExpression(1604131115000LL,
                          "second",
                          100000LL,
                          boost::none, /* timezone */
                          state);
}

void ExpressionBenchmarkFixture::benchmarkDateAddEvaluate100Years(benchmark::State& state) {
    testDateAddExpression(1604131115000LL,
                          "year",
                          100LL,
                          boost::none, /* timezone */
                          state);
}

void ExpressionBenchmarkFixture::benchmarkDateAddEvaluate12HoursWithTimezone(
    benchmark::State& state) {
    testDateAddExpression(1604131115000LL, "hour", 12LL, std::string{"America/New_York"}, state);
}

void ExpressionBenchmarkFixture::benchmarkDateFromString(benchmark::State& state) {
    testDateFromStringExpression(
        "08/14/2023, 12:24:36", std::string{"UTC"}, std::string{"%m/%d/%Y, %H:%M:%S"}, state);
}

void ExpressionBenchmarkFixture::benchmarkDateFromStringNewYork(benchmark::State& state) {
    testDateFromStringExpression("08/14/2023, 12:24:36",
                                 std::string{"America/New_York"},
                                 std::string{"%m/%d/%Y, %H:%M:%S"},
                                 state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateMinute15NewYork(
    benchmark::State& state) {
    testDateTruncExpression(1615460825000LL /* year 2021*/,
                            "minute",
                            15,
                            std::string{"America/New_York"},
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateMinute15UTC(benchmark::State& state) {
    testDateTruncExpression(1615460825000LL /* year 2021*/,
                            "minute",
                            15,
                            boost::none,
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateHour1UTCMinus0700(
    benchmark::State& state) {
    testDateTruncExpression(1615460825000LL /* year 2021*/,
                            "hour",
                            1,
                            std::string{"-07:00"},
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateWeek2NewYorkValue2100(
    benchmark::State& state) {
    testDateTruncExpression(4108446425000LL /* year 2100*/,
                            "week",
                            2,
                            std::string{"America/New_York"},
                            std::string{"monday"} /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateWeek2UTCValue2100(
    benchmark::State& state) {
    testDateTruncExpression(4108446425000LL /* year 2100*/,
                            "week",
                            2,
                            std::string{"UTC"},
                            std::string{"monday"} /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateMonth6NewYorkValue2100(
    benchmark::State& state) {
    testDateTruncExpression(4108446425000LL /* year 2100*/,
                            "month",
                            6,
                            std::string{"America/New_York"},
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateMonth6NewYorkValue2030(
    benchmark::State& state) {
    testDateTruncExpression(1893466800000LL /* year 2030*/,
                            "month",
                            6,
                            std::string{"America/New_York"},
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateMonth6UTCValue2030(
    benchmark::State& state) {
    testDateTruncExpression(1893466800000LL /* year 2030*/,
                            "month",
                            8,
                            boost::none,
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateYear1NewYorkValue2020(
    benchmark::State& state) {
    testDateTruncExpression(1583924825000LL /* year 2020*/,
                            "year",
                            1,
                            std::string{"America/New_York"},
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateYear1UTCValue2020(
    benchmark::State& state) {
    testDateTruncExpression(1583924825000LL /* year 2020*/,
                            "year",
                            1,
                            boost::none,
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkDateTruncEvaluateYear1NewYorkValue2100(
    benchmark::State& state) {
    testDateTruncExpression(4108446425000LL /* year 2100*/,
                            "year",
                            1,
                            std::string{"America/New_York"},
                            boost::none /* startOfWeek */,
                            state);
}

void ExpressionBenchmarkFixture::benchmarkYearNoTZ(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$year", Date_t::fromMillisSinceEpoch(1893466800000LL) /* year 2030*/, boost::none, state);
}

void ExpressionBenchmarkFixture::benchmarkYearConstTzUTC(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$year",
        Date_t::fromMillisSinceEpoch(1893466800000LL) /* year 2030*/,
        std::string{"UTC"},
        state);
}

void ExpressionBenchmarkFixture::benchmarkYearConstTzUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$year",
        Date_t::fromMillisSinceEpoch(1583924825000LL) /* year 2020*/,
        std::string{"-07:00"},
        state);
}

void ExpressionBenchmarkFixture::benchmarkYearConstTzNewYork(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$year",
        Date_t::fromMillisSinceEpoch(4108446425000LL) /* year 2100*/,
        std::string{"America/New_York"},
        state);
}

void ExpressionBenchmarkFixture::benchmarkYearUTC(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$year",
        Date_t::fromMillisSinceEpoch(1893466800000LL) /* year 2030*/,
        std::string{"UTC"},
        state);
}

void ExpressionBenchmarkFixture::benchmarkYearUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$year",
        Date_t::fromMillisSinceEpoch(1583924825000LL) /* year 2020*/,
        std::string{"-07:00"},
        state);
}

void ExpressionBenchmarkFixture::benchmarkYearNewYork(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$year",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkMonthNoTZ(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$month", Date_t::fromMillisSinceEpoch(1893466800000LL), boost::none, state);
}

void ExpressionBenchmarkFixture::benchmarkMonthConstTzUTC(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$month", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkMonthConstTzUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$month", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkMonthConstTzNewYork(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$month",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkMonthUTC(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$month", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkMonthUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$month", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkMonthNewYork(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$month",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkHourNoTZ(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$hour", Date_t::fromMillisSinceEpoch(1893466800000LL), boost::none, state);
}

void ExpressionBenchmarkFixture::benchmarkHourConstTzUTC(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$hour", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkHourConstTzUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$hour", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkHourConstTzNewYork(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$hour",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkHourUTC(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$hour", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkHourUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$hour", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkHourNewYork(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$hour",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkMinuteNoTZ(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$minute", Date_t::fromMillisSinceEpoch(1893466800000LL), boost::none, state);
}

void ExpressionBenchmarkFixture::benchmarkMinuteConstTzUTC(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$minute", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkMinuteConstTzUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$minute", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkMinuteConstTzNewYork(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$minute",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkMinuteUTC(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$minute", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkMinuteUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$minute", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkMinuteNewYork(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$minute",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkSecondNoTZ(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$second", Date_t::fromMillisSinceEpoch(1893466800000LL), boost::none, state);
}

void ExpressionBenchmarkFixture::benchmarkSecondConstTzUTC(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$second", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkSecondConstTzUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$second", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkSecondConstTzNewYork(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$second",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkSecondUTC(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$second", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkSecondUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$second", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkSecondNewYork(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$second",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkMillisecondNoTZ(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$millisecond", Date_t::fromMillisSinceEpoch(1893466800000LL), boost::none, state);
}

void ExpressionBenchmarkFixture::benchmarkMillisecondConstTzUTC(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$millisecond", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkMillisecondConstTzUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$millisecond",
                                           Date_t::fromMillisSinceEpoch(1583924825000LL),
                                           std::string{"-07:00"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkMillisecondConstTzNewYork(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$millisecond",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkMillisecondUTC(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$millisecond", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkMillisecondUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$millisecond",
                                           Date_t::fromMillisSinceEpoch(1583924825000LL),
                                           std::string{"-07:00"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkMillisecondNewYork(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$millisecond",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkWeekNoTZ(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$week", Date_t::fromMillisSinceEpoch(1893466800000LL), boost::none, state);
}

void ExpressionBenchmarkFixture::benchmarkWeekConstTzUTC(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$week", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkWeekConstTzUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$week", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkWeekConstTzNewYork(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$week",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkWeekUTC(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$week", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkWeekUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$week", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkWeekNewYork(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$week",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekYearNoTZ(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$isoWeekYear", Date_t::fromMillisSinceEpoch(1893466800000LL), boost::none, state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekYearConstTzUTC(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$isoWeekYear", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekYearConstTzUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$isoWeekYear",
                                           Date_t::fromMillisSinceEpoch(1583924825000LL),
                                           std::string{"-07:00"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekYearConstTzNewYork(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$isoWeekYear",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekYearUTC(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$isoWeekYear", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekYearUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$isoWeekYear",
                                           Date_t::fromMillisSinceEpoch(1583924825000LL),
                                           std::string{"-07:00"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekYearNewYork(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$isoWeekYear",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkISODayOfWeekNoTZ(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$isoDayOfWeek", Date_t::fromMillisSinceEpoch(1893466800000LL), boost::none, state);
}

void ExpressionBenchmarkFixture::benchmarkISODayOfWeekConstTzUTC(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$isoDayOfWeek", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkISODayOfWeekConstTzUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$isoDayOfWeek",
                                           Date_t::fromMillisSinceEpoch(1583924825000LL),
                                           std::string{"-07:00"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkISODayOfWeekConstTzNewYork(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$isoDayOfWeek",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkISODayOfWeekUTC(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$isoDayOfWeek", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkISODayOfWeekUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$isoDayOfWeek",
                                           Date_t::fromMillisSinceEpoch(1583924825000LL),
                                           std::string{"-07:00"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkISODayOfWeekNewYork(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$isoDayOfWeek",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekNoTZ(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$isoWeek", Date_t::fromMillisSinceEpoch(1893466800000LL), boost::none, state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekConstTzUTC(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$isoWeek", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekConstTzUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithConstantTimezone(
        "$isoWeek", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekConstTzNewYork(benchmark::State& state) {
    testDateExpressionWithConstantTimezone("$isoWeek",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekUTC(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$isoWeek", Date_t::fromMillisSinceEpoch(1893466800000LL), std::string{"UTC"}, state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekUTCMinus0700(benchmark::State& state) {
    testDateExpressionWithVariableTimezone(
        "$isoWeek", Date_t::fromMillisSinceEpoch(1583924825000LL), std::string{"-07:00"}, state);
}

void ExpressionBenchmarkFixture::benchmarkISOWeekNewYork(benchmark::State& state) {
    testDateExpressionWithVariableTimezone("$isoWeek",
                                           Date_t::fromMillisSinceEpoch(4108446425000LL),
                                           std::string{"America/New_York"},
                                           state);
}

/**
 * Tests performance of $getField expression.
 */
void ExpressionBenchmarkFixture::benchmarkGetFieldEvaluateExpression(benchmark::State& state) {
    BSONObjBuilder objBuilder;
    objBuilder << "field"
               << "x.y$z"
               << "input" << BSON("$const" << BSON("x.y$z" << "abc"));

    benchmarkExpression(BSON("$getField" << objBuilder.obj()), state);
}

void ExpressionBenchmarkFixture::benchmarkGetFieldEvaluateShortSyntaxExpression(
    benchmark::State& state) {
    benchmarkExpression(BSON("$getField" << BSON("$const" << "$foo")), state);
}

void ExpressionBenchmarkFixture::benchmarkGetFieldNestedExpression(benchmark::State& state) {
    BSONObjBuilder innerObjBuilder;
    innerObjBuilder << "field"
                    << "a.b"
                    << "input" << BSON("$const" << BSON("a.b" << BSON("c" << 123)));
    BSONObjBuilder outerObjBuilder;
    outerObjBuilder << "field"
                    << "c"
                    << "input" << BSON("$getField" << innerObjBuilder.obj());
    benchmarkExpression(BSON("$getField" << outerObjBuilder.obj()), state);
}

/**
 * Tests performance of aggregation expression
 *   {"$and": [{"$eq": ["$value0", "1"]}, {"$eq": ["$value1", "0"]}]}
 * against document
 *   {"_id": ObjectId(...), "value0": "0", "value1": "1"}
 * This returns false on the first condition (position 0).
 */
void ExpressionBenchmarkFixture::benchmarkLogicalAndFalse0(benchmark::State& state) {
    benchmarkExpression(
        BSON("$and" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$value0" << "1"_sd))
                                  << BSON("$eq" << BSON_ARRAY("$value1" << "0"_sd)))),
        state,
        std::vector<Document>(1, {Document(fromjson("{value0: \"0\", value1: \"1\"}"))}));
}

/**
 * Tests performance of aggregation expression
 *   {"$and": [{"$eq": ["$value0", "0"]}, {"$eq": ["$value1", "2"]}]}
 * against document
 *   {"_id": ObjectId(...), "value0": "0", "value1": "1"}
 * This returns false on the second condition (position 1).
 */
void ExpressionBenchmarkFixture::benchmarkLogicalAndFalse1(benchmark::State& state) {
    benchmarkExpression(
        BSON("$and" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$value0" << "0"_sd))
                                  << BSON("$eq" << BSON_ARRAY("$value1" << "2"_sd)))),
        state,
        std::vector<Document>(1, {Document(fromjson("{value0: \"0\", value1: \"1\"}"))}));
}

/**
 * Tests performance of aggregation expression
 *   {"$and": [{"$eq": ["$value0", "0"]}, {"$eq": ["$value1", "1"]}]}
 * against document
 *   {"_id": ObjectId(...), "value0": "0", "value1": "1"}
 * This returns true as both conditions are met.
 */
void ExpressionBenchmarkFixture::benchmarkLogicalAndTrue(benchmark::State& state) {
    benchmarkExpression(
        BSON("$and" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$value0" << "0"_sd))
                                  << BSON("$eq" << BSON_ARRAY("$value1" << "1"_sd)))),
        state,
        std::vector<Document>(1, {Document(fromjson("{value0: \"0\", value1: \"1\"}"))}));
}

/**
 * Tests performance of aggregation expression
 *   {"$or": [{"$eq": ["$value", "1"]}, {"$eq": ["$value", "0"]}]}
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 * This returns true on the first condition (position 0).
 */
void ExpressionBenchmarkFixture::benchmarkLogicalOrTrue0(benchmark::State& state) {
    benchmarkExpression(BSON("$or" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$value" << "1"_sd))
                                                 << BSON("$eq" << BSON_ARRAY("$value" << "0"_sd)))),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$or": [{"$eq": ["$value", "0"]}, {"$eq": ["$value", "1"]}]}
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 * This returns true on the second condition (position 1).
 */
void ExpressionBenchmarkFixture::benchmarkLogicalOrTrue1(benchmark::State& state) {
    benchmarkExpression(BSON("$or" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$value" << "0"_sd))
                                                 << BSON("$eq" << BSON_ARRAY("$value" << "1"_sd)))),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$or": [{"$eq": ["$value", "0"]}, {"$eq": ["$value", "2"]}]}
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 * This returns false as neither condition is met.
 */
void ExpressionBenchmarkFixture::benchmarkLogicalOrFalse(benchmark::State& state) {
    benchmarkExpression(BSON("$or" << BSON_ARRAY(BSON("$eq" << BSON_ARRAY("$value" << "0"_sd))
                                                 << BSON("$eq" << BSON_ARRAY("$value" << "2"_sd)))),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

void ExpressionBenchmarkFixture::benchmarkSetFieldEvaluateExpression(benchmark::State& state) {
    testSetFieldExpression("a.b", "x", "y", state);
}

// The following two functions test different syntax for equivalent expressions:
// $unsetField is an alias for $setField with $$REMOVE.
void ExpressionBenchmarkFixture::benchmarkSetFieldWithRemoveExpression(benchmark::State& state) {
    testSetFieldExpression("a$b", "x", "$$REMOVE", state);
}

void ExpressionBenchmarkFixture::benchmarkUnsetFieldEvaluateExpression(benchmark::State& state) {
    BSONObjBuilder objBuilder;
    objBuilder << "field"
               << "a$b.c"
               << "input"
               << BSON("$const" << BSON("a$b.c" << "x"
                                                << "f1" << 1 << "f2" << 2));

    benchmarkExpression(BSON("$unsetField" << objBuilder.obj()), state);
}

/**
 * Tests performance of $set* expressions.
 */
void ExpressionBenchmarkFixture::benchmarkSetIsSubset_allPresent(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax);
    BSONArray rhs = rangeBSONArray(0, kMax);

    benchmarkExpression(BSON("$setIsSubset" << BSON_ARRAY("$arr" << rhs)),
                        state,
                        std::vector<Document>(100, {{"arr"_sd, lhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSetIsSubset_nonePresent(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax, kMax);
    BSONArray rhs = rangeBSONArray(0, kMax);

    benchmarkExpression(BSON("$setIsSubset" << BSON_ARRAY("$arr" << rhs)),
                        state,
                        std::vector<Document>(100, {{"arr"_sd, lhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSetIntersection(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax);
    BSONArray rhs = randomBSONArray(100000, kMax);

    benchmarkExpression(BSON("$setIntersection" << BSON_ARRAY("$lhs" << "$rhs")),
                        state,
                        std::vector<Document>(100, {{"lhs", lhs}, {"rhs", rhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSetDifference(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax);
    BSONArray rhs = randomBSONArray(100000, kMax);

    benchmarkExpression(BSON("$setDifference" << BSON_ARRAY("$lhs" << "$rhs")),
                        state,
                        std::vector<Document>(100, {{"lhs", lhs}, {"rhs", rhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSetEquals(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax);
    BSONArray rhs = randomBSONArray(100000, kMax);

    benchmarkExpression(BSON("$setEquals" << BSON_ARRAY("$lhs" << "$rhs")),
                        state,
                        std::vector<Document>(100, {{"lhs", lhs}, {"rhs", rhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSetUnion(benchmark::State& state) {
    const int kMax = 100000;
    BSONArray lhs = randomBSONArray(100000, kMax);
    BSONArray rhs = randomBSONArray(100000, kMax);

    benchmarkExpression(BSON("$setUnion" << BSON_ARRAY("$lhs" << "$rhs")),
                        state,
                        std::vector<Document>(100, {{"lhs"_sd, lhs}, {"rhs"_sd, rhs}}));
}

void ExpressionBenchmarkFixture::benchmarkSubtractIntegers(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() {
        return random.nextInt32(kMax);
    };
    testBinaryOpExpression("$subtract", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkSubtractDoubles(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() {
        return random.nextCanonicalDouble() * kMax;
    };
    testBinaryOpExpression("$subtract", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkSubtractDecimals(benchmark::State& state) {
    const int kCount = 1000;
    testBinaryOpExpression("$subtract", randomPairs(kCount, getDecimalGenerator(random)), state);
}

void ExpressionBenchmarkFixture::benchmarkSubtractDates(benchmark::State& state) {
    const int kCount = 1000;
    const int64_t kMin = -93724129050000;
    const int64_t kMax = 32503676400000;
    auto generator = [this]() {
        int64_t timestamp = std::uniform_int_distribution<int64_t>(kMin, kMax)(random.urbg());
        return Date_t::fromMillisSinceEpoch(timestamp);
    };
    testBinaryOpExpression("$subtract", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkSubtractNullAndMissing(benchmark::State& state) {
    const int kCount = 1000;
    std::vector<Document> documents;
    for (int i = 0; i < kCount / 4; ++i) {
        documents.emplace_back(BSON("empty" << true));
        documents.emplace_back(BSON("lhs" << BSONNULL));
        documents.emplace_back(BSON("rhs" << BSONNULL));
        documents.emplace_back(BSON("lhs" << BSONNULL << "rhs" << BSONNULL));
    }
    testBinaryOpExpression("$subtract", documents, state);
}

void ExpressionBenchmarkFixture::benchmarkAddIntegers(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() {
        return random.nextInt32(kMax);
    };
    testBinaryOpExpression("$add", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkAddDoubles(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() {
        return random.nextCanonicalDouble() * kMax;
    };
    testBinaryOpExpression("$add", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkAddDecimals(benchmark::State& state) {
    const int kCount = 1000;
    testBinaryOpExpression("$add", randomPairs(kCount, getDecimalGenerator(random)), state);
}

void ExpressionBenchmarkFixture::benchmarkAddDates(benchmark::State& state) {
    const int kCount = 1000;
    const int kMaxInt = 1000000;
    const int64_t kMinDate = -93724129050000;
    const int64_t kMaxDate = 32503676400000;

    std::vector<Document> documents;
    documents.reserve(kCount);

    auto nextDate = [this]() {
        int64_t timestamp =
            std::uniform_int_distribution<int64_t>(kMinDate, kMaxDate)(random.urbg());
        return Date_t::fromMillisSinceEpoch(timestamp);
    };

    for (int i = 0; i < kCount; ++i) {
        documents.emplace_back(BSON("lhs" << nextDate() << "rhs" << random.nextInt32(kMaxInt)));
    }

    testBinaryOpExpression("$add", documents, state);
}

void ExpressionBenchmarkFixture::benchmarkAddNullAndMissing(benchmark::State& state) {
    const int kCount = 1000;
    std::vector<Document> documents;
    for (int i = 0; i < kCount / 4; ++i) {
        documents.emplace_back(BSON("empty" << true));
        documents.emplace_back(BSON("lhs" << BSONNULL));
        documents.emplace_back(BSON("rhs" << BSONNULL));
        documents.emplace_back(BSON("lhs" << BSONNULL << "rhs" << BSONNULL));
    }
    testBinaryOpExpression("$add", documents, state);
}

void ExpressionBenchmarkFixture::benchmarkAddArray(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    BSONArrayBuilder elements;
    for (int i = 0; i < kCount; ++i) {
        elements.append(random.nextInt32(kMax));
    }
    Document document{BSON("operands" << elements.done())};
    BSONArrayBuilder operands;
    for (int i = 0; i < kCount; ++i) {
        operands.append("$operands." + std::to_string(i));
    }
    BSONObj expr = BSON("$add" << operands.arr());
    benchmarkExpression(std::move(expr), state, {document});
}

void ExpressionBenchmarkFixture::benchmarkMultiplyIntegers(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() {
        return random.nextInt32(kMax);
    };
    testBinaryOpExpression("$multiply", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkMultiplyDoubles(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() {
        return random.nextCanonicalDouble() * kMax;
    };
    testBinaryOpExpression("$multiply", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkMultiplyDecimals(benchmark::State& state) {
    const int kCount = 1000;
    testBinaryOpExpression("$multiply", randomPairs(kCount, getDecimalGenerator(random)), state);
}

void ExpressionBenchmarkFixture::benchmarkMultiplyNullAndMissing(benchmark::State& state) {
    const int kCount = 1000;
    std::vector<Document> documents;
    for (int i = 0; i < kCount / 4; ++i) {
        documents.emplace_back(BSON("empty" << true));
        documents.emplace_back(BSON("lhs" << BSONNULL));
        documents.emplace_back(BSON("rhs" << BSONNULL));
        documents.emplace_back(BSON("lhs" << BSONNULL << "rhs" << BSONNULL));
    }
    testBinaryOpExpression("$multiply", documents, state);
}

void ExpressionBenchmarkFixture::benchmarkMultiplyArray(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 2;
    BSONArrayBuilder elements;
    for (int i = 0; i < kCount; ++i) {
        elements.append(random.nextInt32(kMax) + 1);
    }
    Document document{BSON("operands" << elements.done())};
    BSONArrayBuilder operands;
    for (int i = 0; i < kCount; ++i) {
        operands.append("$operands." + std::to_string(i));
    }
    BSONObj expr = BSON("$multiply" << operands.arr());
    benchmarkExpression(std::move(expr), state, {document});
}

void ExpressionBenchmarkFixture::benchmarkPowIntegers(benchmark::State& state) {
    const int kCount = 1000;

    std::vector<Document> documents;
    documents.reserve(kCount);

    // the combination of base and any value up to exponent in the same idx create result that fits
    // always in int32_t or in an int64_t
    std::vector<int> exponent{1'000'000'000, 1'000'000'000, 62, 39, 31, 27, 24, 22, 20, 19, 18};

    for (int i = 0; i < kCount; ++i) {
        int idx = random.nextInt32(exponent.size());
        documents.emplace_back(BSON("lhs" << idx << "rhs" << random.nextInt32(exponent[idx])));
    }

    testBinaryOpExpression("$pow", documents, state);
}

void ExpressionBenchmarkFixture::benchmarkPowDoubles(benchmark::State& state) {
    const int kCount = 1000;
    const int kMax = 1'000'000'000;
    auto generator = [this]() {
        return random.nextCanonicalDouble() * kMax;
    };
    testBinaryOpExpression("$pow", randomPairs(kCount, generator), state);
}

void ExpressionBenchmarkFixture::benchmarkPowDecimals(benchmark::State& state) {
    const int kCount = 1000;
    testBinaryOpExpression("$pow", randomPairs(kCount, getDecimalGenerator(random)), state);
}

void ExpressionBenchmarkFixture::benchmarkPowNullAndMissing(benchmark::State& state) {
    const int kCount = 1000;

    std::vector<Document> documents;
    documents.reserve(kCount);

    for (int i = 0; i < kCount / 4; ++i) {
        documents.emplace_back(BSON("empty" << true));
        documents.emplace_back(BSON("lhs" << BSONNULL));
        documents.emplace_back(BSON("rhs" << BSONNULL));
        documents.emplace_back(BSON("lhs" << BSONNULL << "rhs" << BSONNULL));
    }
    testBinaryOpExpression("$pow", documents, state);
}

/**
 * Tests performance of aggregation expression
 *   {"$const": "1"}
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkValueConst(benchmark::State& state) {
    benchmarkExpression(
        BSON("$const" << "1"_sd), state, std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

/**
 * Tests performance of aggregation expression
 *   {"$literal": "1"}
 * against document
 *   {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkValueLiteral(benchmark::State& state) {
    benchmarkExpression(
        BSON("$literal" << "1"_sd), state, std::vector<Document>(1, {{"value"_sd, "1"_sd}}));
}

void ExpressionBenchmarkFixture::benchmarkObjectToArray(benchmark::State& state) {
    BSONObjBuilder builder{};
    for (int i = 0; i < 10; ++i) {
        auto key = "key" + std::to_string(i);
        auto value = "value" + std::to_string(i);
        builder.append(key, value);
    }
    benchmarkExpression(BSON("$objectToArray" << "$input"_sd),
                        state,
                        std::vector<Document>(1, {{"input"_sd, builder.obj()}}));
}

void ExpressionBenchmarkFixture::benchmarkArrayToObject1(benchmark::State& state) {
    BSONArrayBuilder builder{};
    for (int i = 0; i < 10; ++i) {
        auto key = "key" + std::to_string(i);
        auto value = "value" + std::to_string(i);
        builder.append(BSON_ARRAY(key << value));
    }
    benchmarkExpression(BSON("$arrayToObject" << "$input"_sd),
                        state,
                        std::vector<Document>(1, {{"input"_sd, builder.arr()}}));
}

void ExpressionBenchmarkFixture::benchmarkArrayToObject2(benchmark::State& state) {
    BSONArrayBuilder builder{};
    for (int i = 0; i < 10; ++i) {
        auto key = "key" + std::to_string(i);
        auto value = "value" + std::to_string(i);
        builder.append(BSON("k" << key << "v" << value));
    }
    benchmarkExpression(BSON("$arrayToObject" << "$input"_sd),
                        state,
                        std::vector<Document>(1, {{"input"_sd, builder.arr()}}));
}

void ExpressionBenchmarkFixture::testDateDiffExpression(long long startDate,
                                                        long long endDate,
                                                        std::string unit,
                                                        boost::optional<std::string> timezone,
                                                        boost::optional<std::string> startOfWeek,
                                                        benchmark::State& state) {
    // Build a $dateDiff expression.
    BSONObjBuilder objBuilder;
    objBuilder << "startDate" << Date_t::fromMillisSinceEpoch(startDate) << "endDate"
               << "$endDate"
               << "unit" << unit;
    if (timezone) {
        objBuilder << "timezone" << *timezone;
    }
    if (startOfWeek) {
        objBuilder << "startOfWeek" << *startOfWeek;
    }
    benchmarkExpression(
        BSON("$dateDiff" << objBuilder.obj()),
        state,
        std::vector<Document>(1, {{"endDate"_sd, Date_t::fromMillisSinceEpoch(endDate)}}));
}

void ExpressionBenchmarkFixture::testDateTruncExpression(long long date,
                                                         std::string unit,
                                                         unsigned long binSize,
                                                         boost::optional<std::string> timezone,
                                                         boost::optional<std::string> startOfWeek,
                                                         benchmark::State& state) {
    // Build a $dateTrunc expression.
    BSONObjBuilder objBuilder;
    objBuilder << "date"
               << "$date"
               << "unit" << unit << "binSize" << static_cast<long long>(binSize);
    if (timezone) {
        objBuilder << "timezone" << *timezone;
    }
    if (startOfWeek) {
        objBuilder << "startOfWeek" << *startOfWeek;
    }
    benchmarkExpression(
        BSON("$dateTrunc" << objBuilder.obj()),
        state,
        std::vector<Document>(1, {{"date"_sd, Date_t::fromMillisSinceEpoch(date)}}));
}

void ExpressionBenchmarkFixture::testDateAddExpression(long long startDate,
                                                       std::string unit,
                                                       long long amount,
                                                       boost::optional<std::string> timezone,
                                                       benchmark::State& state) {
    BSONObjBuilder objBuilder;
    objBuilder << "startDate"
               << "$startDate"
               << "unit" << unit << "amount" << amount;
    if (timezone) {
        objBuilder << "timezone" << *timezone;
    }
    benchmarkExpression(
        BSON("$dateAdd" << objBuilder.obj()),
        state,
        std::vector<Document>(1, {{"startDate"_sd, Date_t::fromMillisSinceEpoch(startDate)}}));
}

void ExpressionBenchmarkFixture::testDateExpressionWithConstantTimezone(
    std::string exprName,
    Date_t date,
    boost::optional<std::string> timezone,
    benchmark::State& state) {
    BSONObjBuilder objBuilder;
    objBuilder << "date"
               << "$date";
    if (timezone) {
        objBuilder << "timezone" << *timezone;
    }
    benchmarkExpression(
        BSON(exprName << objBuilder.obj()), state, std::vector<Document>(1, {{"date"_sd, date}}));
}

void ExpressionBenchmarkFixture::testDateExpressionWithVariableTimezone(
    std::string exprName, Date_t date, boost::optional<std::string> tz, benchmark::State& state) {
    BSONObjBuilder objBuilder;
    objBuilder << "date"
               << "$date"
               << "timezone"
               << "$timezone";
    benchmarkExpression(BSON(exprName << objBuilder.obj()),
                        state,
                        std::vector<Document>(1, {{"date"_sd, date}, {"timezone"_sd, *tz}}));
}

void ExpressionBenchmarkFixture::testDateFromStringExpression(std::string dateString,
                                                              boost::optional<std::string> timezone,
                                                              boost::optional<std::string> format,
                                                              benchmark::State& state) {
    BSONObjBuilder objBuilder;
    objBuilder << "dateString" << dateString;
    if (timezone) {
        objBuilder << "timezone" << *timezone;
    }
    if (format) {
        objBuilder << "format" << *format;
    }
    benchmarkExpression(BSON("$dateFromString" << objBuilder.obj()), state);
}

void ExpressionBenchmarkFixture::testSetFieldExpression(std::string fieldname,
                                                        std::string oldFieldValue,
                                                        std::string newFieldValue,
                                                        benchmark::State& state) {
    BSONObjBuilder objBuilder;
    objBuilder << "field" << fieldname << "input"
               << BSON("$const" << BSON(fieldname << oldFieldValue << "f1" << 1 << "f2" << 2))
               << "value" << newFieldValue;

    benchmarkExpression(BSON("$setField" << objBuilder.obj()), state);
}

void ExpressionBenchmarkFixture::testBinaryOpExpression(const std::string& binaryOp,
                                                        const std::vector<Document>& documents,
                                                        benchmark::State& state) {
    BSONObj expr = BSON(binaryOp << BSON_ARRAY("$lhs" << "$rhs"));
    benchmarkExpression(std::move(expr), state, documents);
}

BSONArray ExpressionBenchmarkFixture::randomBSONArray(int count, int max, int offset) {
    BSONArrayBuilder builder;
    for (int i = 0; i < count; i++) {
        builder.append(std::to_string(offset + random.nextInt32(max)));
    }
    return builder.arr();
}

/**
 * Tests performance of $percentile expression against a single array field of specified size.
 */
void ExpressionBenchmarkFixture::benchmarkPercentile(benchmark::State& state,
                                                     int arraySize,
                                                     const std::vector<double>& ps) {
    std::vector<double> inputs = generateNormal(arraySize);
    benchmarkExpression(BSON("$percentile" << BSON("input" << "$data"
                                                           << "p" << vectorToBSON(ps) << "method"
                                                           << "approximate")),
                        state,
                        std::vector<Document>(1, {{"data"_sd, vectorToBSON(inputs)}}));
}

/**
 * Tests performance of $strLenBytes expression against a single string field.
 */
void ExpressionBenchmarkFixture::benchmarkStrLenBytes(benchmark::State& state) {
    benchmarkExpression(BSON("$strLenBytes" << "$data"),
                        state,
                        std::vector<Document>(1, {{"data"_sd, "hello world"_sd}}));
}

void ExpressionBenchmarkFixture::benchmarkStrLenCP(benchmark::State& state) {
    benchmarkExpression(BSON("$strLenCP" << "$data"),
                        state,
                        std::vector<Document>(1, {{"data"_sd, "hello world"_sd}}));
}

void ExpressionBenchmarkFixture::benchmarkStrLenCPMultiBytes(benchmark::State& state) {
    benchmarkExpression(BSON("$strLenCP" << "$data"),
                        state,
                        std::vector<Document>(1, {{"data"_sd, "ψϒϒϒЁϴЁЁ"_sd}}));
}

void ExpressionBenchmarkFixture::benchmarkStrLenCPLargeString(benchmark::State& state) {
    benchmarkExpression(
        BSON("$strLenCP" << "$data"),
        state,
        std::vector<Document>(
            1,
            {{"data"_sd,
              "ψhello world, this is a long string that will provide another set of benchmarks"_sd}}));
}

/**
 * Tests performance of $trim expression against a single string field.
 */
void ExpressionBenchmarkFixture::benchmarkTrim(benchmark::State& state, std::string chars) {
    benchmarkExpression(BSON("$trim" << BSON("input" << "$data"
                                                     << "chars" << chars)),
                        state,
                        std::vector<Document>(1, {{"data"_sd, " hello world   "_sd}}));
}

/**
 * Tests performance of $trunc expression against a single field.
 */
void ExpressionBenchmarkFixture::benchmarkTrunc(benchmark::State& state, int place) {
    benchmarkExpression(BSON("$trunc" << BSON_ARRAY("$value" << place)),
                        state,
                        std::vector<Document>(1, {{"value"_sd, 123.45}}));
}

/**
 * Tests performance of aggregation expression {"$strcasecmp": ["hello", "$value"]}
 */
void ExpressionBenchmarkFixture::benchmarkStrcasecmp(benchmark::State& state) {
    benchmarkExpression(BSON("$strcasecmp" << BSON_ARRAY("hello" << "$value")),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "Hello"_sd}}));
}
void ExpressionBenchmarkFixture::benchmarkStrcasecmpLargeString(benchmark::State& state) {
    benchmarkExpression(
        BSON("$strcasecmp" << BSON_ARRAY("hello" << "$value")),
        state,
        std::vector<Document>(
            1,
            {{"value"_sd,
              "This is a long string that will provide another set of benchmarks"_sd}}));
}

/**
 * Tests performance of aggregation expression {"$substrBytes": ["$value", 0, 4]}
 */
void ExpressionBenchmarkFixture::benchmarkSubstrBytes(benchmark::State& state) {
    benchmarkExpression(BSON("$substrBytes" << BSON_ARRAY("$value" << 0 << 4)),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "hello world"_sd}}));
}

void ExpressionBenchmarkFixture::benchmarkSubstrBytesLargeString(benchmark::State& state) {
    benchmarkExpression(
        BSON("$substrBytes" << BSON_ARRAY("$value" << 0 << 4)),
        state,
        std::vector<Document>(
            1,
            {{"value"_sd,
              "This is a long string that will provide another set of benchmarks"_sd}}));
}

/**
 * Tests performance of aggregation expression {"$substrCP": ["$value", 0, 4]}
 */
void ExpressionBenchmarkFixture::benchmarkSubstrCP(benchmark::State& state) {
    benchmarkExpression(BSON("$substrCP" << BSON_ARRAY("$value" << 0 << 4)),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "hello world"_sd}}));
}

void ExpressionBenchmarkFixture::benchmarkSubstrCPMultiBytes(benchmark::State& state) {
    benchmarkExpression(BSON("$substrCP" << BSON_ARRAY("$value" << 0 << 4)),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "ψϒϒϒЁϴЁЁ"_sd}}));
}

void ExpressionBenchmarkFixture::benchmarkSubstrCPLargeString(benchmark::State& state) {
    benchmarkExpression(
        BSON("$substrCP" << BSON_ARRAY("$value" << 0 << 4)),
        state,
        std::vector<Document>(
            1,
            {{"value"_sd,
              "This is a long string that will provide another set of benchmarks"_sd}}));
}

/**
 * Tests performance of $sortArray on array of ints.
 */
void ExpressionBenchmarkFixture::benchmarkSortArrayInt(benchmark::State& state) {
    int n = 500;
    std::vector<int> vectorOfInts;
    vectorOfInts.reserve(n);
    for (int i = 0; i < n; i++) {
        vectorOfInts.push_back(i);
    }
    benchmarkExpression(BSON("$sortArray" << BSON("input" << "$array"
                                                          << "sortBy" << BSON("a" << 1))),
                        state,
                        std::vector<Document>(1, {{"array"_sd, vectorToBSON<int>(vectorOfInts)}}));
}

/**
 * Tests performance of $sortArray on array of strings.
 */
void ExpressionBenchmarkFixture::benchmarkSortArrayString(benchmark::State& state) {
    int n = 500;
    std::vector<std::string> v;
    v.reserve(n);
    for (int i = 0; i < n; i++) {
        v.push_back("This is a long string that will provide another set of benchmarks");
    }
    benchmarkExpression(BSON("$sortArray" << BSON("input" << "$array"
                                                          << "sortBy" << BSON("a" << 1))),
                        state,
                        std::vector<Document>(1, {{"array"_sd, vectorToBSON<std::string>(v)}}));
}

/**
 * Tests performance of $sortArray on array of BSONObj.
 */
void ExpressionBenchmarkFixture::benchmarkSortArrayBSONObj(benchmark::State& state) {
    int n = 500;
    std::vector<BSONObj> v;
    v.reserve(n);
    for (int i = 0; i < n; i++) {
        v.push_back(BSON("a" << 1));
    }
    benchmarkExpression(BSON("$sortArray" << BSON("input" << "$array"
                                                          << "sortBy" << BSON("a" << 1))),
                        state,
                        std::vector<Document>(1, {{"array"_sd, vectorToBSON<BSONObj>(v)}}));
}

/**
 * Tests performance of $sortArray on 2D array.
 */
void ExpressionBenchmarkFixture::benchmarkSortArray2D(benchmark::State& state) {
    int n = 500;
    std::vector<BSONArray> v;
    v.reserve(n);
    for (int i = 0; i < n; i++) {
        v.push_back(rangeBSONArray(i * n, (i + 1) * n));
    }
    benchmarkExpression(BSON("$sortArray" << BSON("input" << "$array"
                                                          << "sortBy" << BSON("a" << 1))),
                        state,
                        std::vector<Document>(1, {{"array"_sd, vectorToBSON<BSONArray>(v)}}));
}

/**
 * Tests performance of aggregation expression {"$rand":{}}
 */
void ExpressionBenchmarkFixture::benchmarkRand(benchmark::State& state) {
    benchmarkExpression(BSON("$rand" << BSONObj()), state, std::vector<Document>(1));
}

/**
 * Tests performance of aggregation expression {"$max": "$value"} against document
 * {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkMaxSingleInput(benchmark::State& state) {
    benchmarkExpression(
        BSON("$max" << BSON_ARRAY("$value")), state, std::vector<Document>(1, {{"value"_sd, 1}}));
}

/**
 * Tests performance of aggregation expression {"$max": ["a", "$value", "$value"]} against document
 * {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkMaxSingleArrayInput(benchmark::State& state) {
    benchmarkExpression(BSON("$max" << BSON_ARRAY("a" << "$value"
                                                      << "$value")),
                        state,
                        std::vector<Document>(1, {{"value"_sd, 1}}));
}

/**
 * Tests performance of aggregation expression {"$min": "$value"} against document
 * {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkMinSingleInput(benchmark::State& state) {
    benchmarkExpression(
        BSON("$min" << BSON_ARRAY("$value")), state, std::vector<Document>(1, {{"value"_sd, 1}}));
}

/**
 * Tests performance of aggregation expression {"$min": ["a", "$value", "$value"]} against document
 * {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkMinSingleArrayInput(benchmark::State& state) {
    benchmarkExpression(BSON("$min" << BSON_ARRAY("a" << "$value"
                                                      << "$value")),
                        state,
                        std::vector<Document>(1, {{"value"_sd, 1}}));
}

/**
 * Tests performance of aggregation expression {"$stdDevPop": ["$value", "$value", "$value"]}
 * against document {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkStdDevPop(benchmark::State& state) {
    benchmarkExpression(BSON("$stdDevPop" << BSON_ARRAY("$value" << "$value"
                                                                 << "$value")),
                        state,
                        std::vector<Document>(1, {{"value"_sd, 1}}));
}

/**
 * Tests performance of aggregation expression {"$stdDevSamp": ["$value", "$value", "$value"]}
 * against document {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkStdDevSamp(benchmark::State& state) {
    benchmarkExpression(BSON("$stdDevSamp" << BSON_ARRAY("$value" << "$value"
                                                                  << "$value")),
                        state,
                        std::vector<Document>(1, {{"value"_sd, 1}}));
}

/**
 * Tests performance of aggregation expression {"$avg": ["$value", "$value", "$value"]} against
 * document {"_id": ObjectId(...), "value": "1"}
 */
void ExpressionBenchmarkFixture::benchmarkAvg(benchmark::State& state) {
    benchmarkExpression(BSON("$avg" << BSON_ARRAY("$value" << "$value"
                                                           << "$value")),
                        state,
                        std::vector<Document>(1, {{"value"_sd, 1}}));
}

/**
 * Tests performance of aggregation expression {"$sum": "$array"]} against document
 * {"_id": ObjectId(...), "array": ["0", "1", "2", "3", "4", "5", "6", "7", "8","9"]}
 */
void ExpressionBenchmarkFixture::benchmarkSum(benchmark::State& state) {
    BSONArray array = rangeBSONArray(0, 10);
    benchmarkExpression(
        BSON("$sum" << "$array"), state, std::vector<Document>(1, {{"array"_sd, array}}));
}

void ExpressionBenchmarkFixture::benchmarkRegexMatch(benchmark::State& state) {
    benchmarkExpression(BSON("$regexFind" << BSON("input" << "$value"
                                                          << "regex"
                                                          << "cafe")),
                        state,
                        std::vector<Document>(1, {{"value"_sd, "cafe"_sd}}));
}

void ExpressionBenchmarkFixture::benchmarkAddWithDottedFieldPath(benchmark::State& state) {
    benchmarkExpression(BSON("$add" << BSON_ARRAY("$lhs.a.b.c.d" << "$rhs.a.b.c.d")),
                        state,
                        std::vector<Document>(
                            1,
                            {{"lhs"_sd, BSON("a" << BSON("b" << BSON("c" << BSON("d" << 10))))},
                             {"rhs"_sd, BSON("a" << BSON("b" << BSON("c" << BSON("d" << 20))))}}));
}

/**
 * Tests performance of $reduce that sums an array.
 */
void ExpressionBenchmarkFixture::benchmarkReduceSum(benchmark::State& state) {
    const size_t numEntries = 16 * 1000;
    BSONArray entries = rangeBSONArray(0, numEntries);

    BSONObj constantDepthExpr = BSON("$sum" << BSON_ARRAY("$$value" << 1));

    BSONObj reduceExpression =
        BSON("$reduce" << BSON("input" << "$entries"
                                       << "initialValue" << 0 << "in" << constantDepthExpr));

    benchmarkExpression(
        reduceExpression, state, std::vector<Document>(1, {{"entries"_sd, entries}}));
}

/**
 * Tests performance of $reduce that's equivalent to the identity function (uses $concatArrays
 * to build the same array again).
 */
void ExpressionBenchmarkFixture::benchmarkReduceConcatArrays(benchmark::State& state) {
    const size_t numEntries = 1000;
    BSONArray entries = rangeBSONArray(0, numEntries);

    BSONArray emptyArray = rangeBSONArray(0, 0);

    BSONObj reduceExpression =
        BSON("$reduce" << BSON("input" << "$entries"
                                       << "initialValue" << emptyArray << "in"
                                       << BSON("$concatArrays"
                                               << BSON_ARRAY("$$value" << BSON_ARRAY("$$this")))));

    benchmarkExpression(
        reduceExpression, state, std::vector<Document>(1, {{"entries"_sd, entries}}));
}

/**
 * Tests performance of $reduce that transforms an array into a deeply nested document.
 *
 *     "$reduce": {
 *         "input": "$entries",
 *         "initialValue": [],
 *         "in": {"a": {"a" : {"a": ... {"a": {}}}}}
 *     }
 *
 * The nestedness of the "in" expression is configured by 'perIterationNestingDepth'.
 */
void ExpressionBenchmarkFixture::benchmarkReduceCreatingNestedObject(
    benchmark::State& state, size_t perIterationNestingDepth) {
    const size_t numEntries = 16 * 8 / perIterationNestingDepth;
    BSONArray entries = rangeBSONArray(0, numEntries);
    BSONObj recursiveObject = BSON("a" << "$$value");
    for (size_t depth = 1; depth < perIterationNestingDepth; depth++) {
        recursiveObject = BSON("a" << recursiveObject);
    }
    BSONArray emptyArray = rangeBSONArray(0, 0);
    BSONObj reduceExpression =
        BSON("$reduce" << BSON("input" << "$entries"
                                       << "initialValue" << emptyArray << "in" << recursiveObject));
    benchmarkExpression(
        reduceExpression, state, std::vector<Document>(1, {{"entries"_sd, entries}}));
}

void ExpressionBenchmarkFixture::benchmarkReduceCreatingNestedObject1(benchmark::State& state) {
    benchmarkReduceCreatingNestedObject(state, 1);
}
void ExpressionBenchmarkFixture::benchmarkReduceCreatingNestedObject2(benchmark::State& state) {
    benchmarkReduceCreatingNestedObject(state, 2);
}
void ExpressionBenchmarkFixture::benchmarkReduceCreatingNestedObject4(benchmark::State& state) {
    benchmarkReduceCreatingNestedObject(state, 4);
}
void ExpressionBenchmarkFixture::benchmarkReduceCreatingNestedObject8(benchmark::State& state) {
    benchmarkReduceCreatingNestedObject(state, 8);
}

/**
 * Tests performance of aggregation expression {"$replaceOne": {input: "$input", find: /<a.+>/,
 * replacement: "<a>"}]} against document {"_id": ObjectId(...), "input": _kLongHTMLString}
 */
void ExpressionBenchmarkFixture::benchmarkMQLReplaceOneRegex(benchmark::State& state) {
    benchmarkExpression(
        BSON("$replaceOne" << BSON("input" << "$input" << "find" << BSONRegEx("<a.+>")
                                           << "replacement" << "<a>")),
        state,
        std::vector<Document>(1, {{"input"_sd, _kLongHTMLString}}));
}

void ExpressionBenchmarkFixture::benchmarkJSReplaceOneRegex(benchmark::State& state) {
    benchmarkExpression(
        BSON("$function" << BSON("body" << "function(input) {return input.replace(/<a.+>/, '<a>');}"
                                        << "args" << BSON_ARRAY("$input") << "lang" << "js")),
        state,
        std::vector<Document>(1, {{"input"_sd, _kLongHTMLString}}));
}

/**
 * Tests performance of aggregation expression {"$replaceAll": {input: "$input", find: /<a.+>/,
 * replacement: "<a>"}]} against document {"_id": ObjectId(...), "input": _kLongHTMLString}
 */
void ExpressionBenchmarkFixture::benchmarkMQLReplaceAllRegex(benchmark::State& state) {
    benchmarkExpression(
        BSON("$replaceAll" << BSON("input" << "$input" << "find" << BSONRegEx("<a.+>")
                                           << "replacement" << "<a>")),
        state,
        std::vector<Document>(1, {{"input"_sd, _kLongHTMLString}}));
}

void ExpressionBenchmarkFixture::benchmarkJSReplaceAllRegex(benchmark::State& state) {
    benchmarkExpression(
        BSON("$function" << BSON("body"
                                 << "function(input) {return input.replace(/<a.+>/g, '<a>');}"
                                 << "args" << BSON_ARRAY("$input") << "lang" << "js")),
        state,
        std::vector<Document>(1, {{"input"_sd, _kLongHTMLString}}));
}

/**
 * Tests performance of aggregation expression {"split": ["$input", /<a.+>/]} against document
 * {"_id": ObjectId(...), "input": _kLongHTMLString}
 */
void ExpressionBenchmarkFixture::benchmarkMQLSplitRegex(benchmark::State& state) {
    benchmarkExpression(BSON("$split" << BSON_ARRAY("$input" << BSONRegEx("<a.+>"))),
                        state,
                        std::vector<Document>(1, {{"input"_sd, _kLongHTMLString}}));
}

void ExpressionBenchmarkFixture::benchmarkJSSplitRegex(benchmark::State& state) {
    benchmarkExpression(
        BSON("$function" << BSON("body" << "function(input) {return input.split(/<a.+>/);}"
                                        << "args" << BSON_ARRAY("$input") << "lang" << "js")),
        state,
        std::vector<Document>(1, {{"input"_sd, _kLongHTMLString}}));
}

/**
 * Tests performance of aggregation expression {"$convert": {input: "$input", to: "string",
 * base: <base>}]} against document {"_id": ObjectId(...), "input": <input>}
 */
void ExpressionBenchmarkFixture::benchmarkMQLConvertToString(int32_t base,
                                                             int32_t input,
                                                             benchmark::State& state) {
    benchmarkExpression(BSON("$convert" << BSON("input" << "$input" << "to"
                                                        << "string"
                                                        << "base" << base)),
                        state,
                        std::vector<Document>(1, {{"input"_sd, input}}));
}

/**
 * Tests performance of aggregation expression {"$convert": {input: "$input", to: "int",
 * base: <base>}]} against document {"_id": ObjectId(...), "input": <input>}
 */
void ExpressionBenchmarkFixture::benchmarkMQLConvertToInt(int32_t base,
                                                          StringData input,
                                                          benchmark::State& state) {
    benchmarkExpression(
        BSON("$convert" << BSON("input" << "$input" << "to" << "int" << "base" << base)),
        state,
        std::vector<Document>(1, {{"input"_sd, input}}));
}

void ExpressionBenchmarkFixture::benchmarkJSConvertToString(int32_t base,
                                                            int32_t input,
                                                            benchmark::State& state) {
    benchmarkExpression(
        BSON("$function" << BSON("body" << "function(input) {return input.toString(" +
                                     std::to_string(base) + ");}"
                                        << "args" << BSON_ARRAY("$input") << "lang"
                                        << "js")),
        state,
        std::vector<Document>(1, {{"input"_sd, input}}));
}

void ExpressionBenchmarkFixture::benchmarkJSConvertToInt(int32_t base,
                                                         StringData input,
                                                         benchmark::State& state) {
    benchmarkExpression(
        BSON("$function" << BSON("body" << "function(input) {return parseInt(input, " +
                                     std::to_string(base) + ");}"
                                        << "args" << BSON_ARRAY("$input") << "lang"
                                        << "js")),
        state,
        std::vector<Document>(1, {{"input"_sd, input}}));
}

void ExpressionBenchmarkFixture::benchmarkMQLCreateObjectId(benchmark::State& state) {
    benchmarkExpression(
        BSON("$createObjectId" << BSONObj()), state, std::vector<Document>(1, {{}}));
}

void ExpressionBenchmarkFixture::benchmarkJSCreateObjectId(benchmark::State& state) {
    benchmarkExpression(BSON("$function" << BSON("body" << "function() {return ObjectId();}"
                                                        << "args" << BSONArray() << "lang"
                                                        << "js")),
                        state,
                        std::vector<Document>(1, {{}}));
}

void ExpressionBenchmarkFixture::benchmarkMQLSubtype(benchmark::State& state) {
    benchmarkExpression(
        BSON("$subtype" << "$input"),
        state,
        std::vector<Document>(
            1, {{"input"_sd, Value(BSONBinData{"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, BinDataGeneral})}}));
}

void ExpressionBenchmarkFixture::benchmarkJSSubtype(benchmark::State& state) {
    benchmarkExpression(
        BSON("$function" << BSON("body" << "function(input) {return input.type;}"
                                        << "args" << BSON_ARRAY("$input") << "lang"
                                        << "js")),
        state,
        std::vector<Document>(
            1, {{"input"_sd, Value(BSONBinData{"gf1UcxdHTJ2HQ/EGQrO7mQ==", 16, BinDataGeneral})}}));
}

void ExpressionBenchmarkFixture::benchmarkMQLConvertObjectToString(size_t fields,
                                                                   size_t depth,
                                                                   benchmark::State& state) {
    benchmarkExpression(BSON("$convert" << BSON("input" << "$input" << "to"
                                                        << stdx::to_underlying(BSONType::string))),
                        state,
                        std::vector<Document>(1, {{"input"_sd, genDocument(fields, depth)}}));
}

void ExpressionBenchmarkFixture::benchmarkJSConvertObjectToString(size_t fields,
                                                                  size_t depth,
                                                                  benchmark::State& state) {
    benchmarkExpression(
        BSON("$function" << BSON("body" << "function(input) {return JSON.stringify(input);}"
                                        << "args" << BSON_ARRAY("$input") << "lang"
                                        << "js")),
        state,
        std::vector<Document>(1, {{"input"_sd, genDocument(fields, depth)}}));
}

void ExpressionBenchmarkFixture::benchmarkMQLConvertStringToObject(size_t fields,
                                                                   size_t depth,
                                                                   benchmark::State& state) {
    benchmarkExpression(
        BSON("$convert" << BSON("input" << "$input" << "to"
                                        << stdx::to_underlying(BSONType::object))),
        state,
        std::vector<Document>(1, {{"input"_sd, Value(genJsonString(fields, depth))}}));
}

void ExpressionBenchmarkFixture::benchmarkJSConvertStringToObject(size_t fields,
                                                                  size_t depth,
                                                                  benchmark::State& state) {
    benchmarkExpression(
        BSON("$function" << BSON("body" << "function(input) {return JSON.parse(input);}"
                                        << "args" << BSON_ARRAY("$input") << "lang"
                                        << "js")),
        state,
        std::vector<Document>(1, {{"input"_sd, Value(genJsonString(fields, depth))}}));
}

}  // namespace mongo
