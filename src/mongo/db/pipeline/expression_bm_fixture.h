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

#pragma once

#include "mongo/platform/basic.h"

#include <benchmark/benchmark.h>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/platform/random.h"

namespace mongo {

class ExpressionBenchmarkFixture : public benchmark::Fixture {
private:
    static constexpr int32_t kSeed = 1;

public:
    ExpressionBenchmarkFixture() : random(kSeed){};

    void benchmarkExpression(BSONObj expressionSpec, benchmark::State& benchmarkState);

    virtual void benchmarkExpression(BSONObj expressionSpec,
                                     benchmark::State& benchmarkState,
                                     const std::vector<Document>& documents) = 0;

    void noOpBenchmark(benchmark::State& state);

    void benchmarkDateDiffEvaluateMinute300Years(benchmark::State& state);
    void benchmarkDateDiffEvaluateMinute2Years(benchmark::State& state);
    void benchmarkDateDiffEvaluateMinute2YearsWithTimezone(benchmark::State& state);
    void benchmarkDateDiffEvaluateWeek(benchmark::State& state);

    void benchmarkDateAddEvaluate10Days(benchmark::State& state);
    void benchmarkDateAddEvaluate600Minutes(benchmark::State& state);
    void benchmarkDateAddEvaluate100KSeconds(benchmark::State& state);
    void benchmarkDateAddEvaluate100Years(benchmark::State& state);
    void benchmarkDateAddEvaluate12HoursWithTimezone(benchmark::State& state);

    void benchmarkDateTruncEvaluateMinute15NewYork(benchmark::State& state);
    void benchmarkDateTruncEvaluateMinute15UTC(benchmark::State& state);
    void benchmarkDateTruncEvaluateHour1UTCMinus0700(benchmark::State& state);
    void benchmarkDateTruncEvaluateWeek2NewYorkValue2100(benchmark::State& state);
    void benchmarkDateTruncEvaluateWeek2UTCValue2100(benchmark::State& state);
    void benchmarkDateTruncEvaluateMonth6NewYorkValue2100(benchmark::State& state);
    void benchmarkDateTruncEvaluateMonth6NewYorkValue2030(benchmark::State& state);
    void benchmarkDateTruncEvaluateMonth6UTCValue2030(benchmark::State& state);
    void benchmarkDateTruncEvaluateYear1NewYorkValue2020(benchmark::State& state);
    void benchmarkDateTruncEvaluateYear1UTCValue2020(benchmark::State& state);
    void benchmarkDateTruncEvaluateYear1NewYorkValue2100(benchmark::State& state);

    void benchmarkGetFieldEvaluateExpression(benchmark::State& state);
    void benchmarkGetFieldEvaluateShortSyntaxExpression(benchmark::State& state);
    void benchmarkGetFieldNestedExpression(benchmark::State& state);
    void benchmarkSetFieldEvaluateExpression(benchmark::State& state);
    void benchmarkSetFieldWithRemoveExpression(benchmark::State& state);
    void benchmarkUnsetFieldEvaluateExpression(benchmark::State& state);

    void benchmarkSetIsSubset_allPresent(benchmark::State& state);
    void benchmarkSetIsSubset_nonePresent(benchmark::State& state);
    void benchmarkSetIntersection(benchmark::State& state);
    void benchmarkSetDifference(benchmark::State& state);
    void benchmarkSetEquals(benchmark::State& state);
    void benchmarkSetUnion(benchmark::State& state);

private:
    void testDateDiffExpression(long long startDate,
                                long long endDate,
                                std::string unit,
                                boost::optional<std::string> timezone,
                                boost::optional<std::string> startOfWeek,
                                benchmark::State& state);
    void testDateTruncExpression(long long date,
                                 std::string unit,
                                 unsigned long binSize,
                                 boost::optional<std::string> timezone,
                                 boost::optional<std::string> startOfWeek,
                                 benchmark::State& state);
    void testDateAddExpression(long long startDate,
                               std::string unit,
                               long long amount,
                               boost::optional<std::string> timezone,
                               benchmark::State& state);
    void testSetFieldExpression(std::string fieldname,
                                std::string oldFieldValue,
                                std::string newFieldValue,
                                benchmark::State& state);

    BSONArray randomBSONArray(int count, int max, int offset = 0);

    PseudoRandom random;
};

#define BENCHMARK_EXPRESSIONS(Fixture)                                          \
                                                                                \
    BENCHMARK_F(Fixture, NoOp)                                                  \
    (benchmark::State & state) {                                                \
        noOpBenchmark(state);                                                   \
    }                                                                           \
                                                                                \
    BENCHMARK_F(Fixture, DateDiffEvaluateMinute300Years)                        \
    (benchmark::State & state) {                                                \
        benchmarkDateDiffEvaluateMinute300Years(state);                         \
    }                                                                           \
    BENCHMARK_F(Fixture, DateDiffEvaluateMinute2Years)                          \
    (benchmark::State & state) {                                                \
        benchmarkDateDiffEvaluateMinute2Years(state);                           \
    }                                                                           \
    BENCHMARK_F(Fixture, DateDiffEvaluateMinute2YearsWithTimezone)              \
    (benchmark::State & state) {                                                \
        benchmarkDateDiffEvaluateMinute2YearsWithTimezone(state);               \
    }                                                                           \
    BENCHMARK_F(Fixture, DateDiffEvaluateWeek)(benchmark::State & state) {      \
        benchmarkDateDiffEvaluateWeek(state);                                   \
    }                                                                           \
                                                                                \
    BENCHMARK_F(Fixture, DateAddEvaluate10Days)(benchmark::State & state) {     \
        benchmarkDateAddEvaluate10Days(state);                                  \
    }                                                                           \
    BENCHMARK_F(Fixture, DateAddEvaluate600Minutes)(benchmark::State & state) { \
        benchmarkDateAddEvaluate600Minutes(state);                              \
    }                                                                           \
    BENCHMARK_F(Fixture, DateAddEvaluate100KSeconds)                            \
    (benchmark::State & state) {                                                \
        benchmarkDateAddEvaluate100KSeconds(state);                             \
    }                                                                           \
    BENCHMARK_F(Fixture, DateAddEvaluate100Years)(benchmark::State & state) {   \
        benchmarkDateAddEvaluate100Years(state);                                \
    }                                                                           \
    BENCHMARK_F(Fixture, DateAddEvaluate12HoursWithTimezone)                    \
    (benchmark::State & state) {                                                \
        benchmarkDateAddEvaluate12HoursWithTimezone(state);                     \
    }                                                                           \
                                                                                \
    BENCHMARK_F(Fixture, DateTruncEvaluateMinute15NewYork)                      \
    (benchmark::State & state) {                                                \
        benchmarkDateTruncEvaluateMinute15NewYork(state);                       \
    }                                                                           \
    BENCHMARK_F(Fixture, DateTruncEvaluateMinute15UTC)                          \
    (benchmark::State & state) {                                                \
        benchmarkDateTruncEvaluateMinute15UTC(state);                           \
    }                                                                           \
    BENCHMARK_F(Fixture, DateTruncEvaluateHour1UTCMinus0700)                    \
    (benchmark::State & state) {                                                \
        benchmarkDateTruncEvaluateHour1UTCMinus0700(state);                     \
    }                                                                           \
    BENCHMARK_F(Fixture, DateTruncEvaluateWeek2NewYorkValue2100)                \
    (benchmark::State & state) {                                                \
        benchmarkDateTruncEvaluateWeek2NewYorkValue2100(state);                 \
    }                                                                           \
    BENCHMARK_F(Fixture, DateTruncEvaluateWeek2UTCValue2100)                    \
    (benchmark::State & state) {                                                \
        benchmarkDateTruncEvaluateWeek2UTCValue2100(state);                     \
    }                                                                           \
    BENCHMARK_F(Fixture, DateTruncEvaluateMonth6NewYorkValue2100)               \
    (benchmark::State & state) {                                                \
        benchmarkDateTruncEvaluateMonth6NewYorkValue2100(state);                \
    }                                                                           \
    BENCHMARK_F(Fixture, DateTruncEvaluateMonth6NewYorkValue2030)               \
    (benchmark::State & state) {                                                \
        benchmarkDateTruncEvaluateMonth6NewYorkValue2030(state);                \
    }                                                                           \
    BENCHMARK_F(Fixture, DateTruncEvaluateMonth6UTCValue2030)                   \
    (benchmark::State & state) {                                                \
        benchmarkDateTruncEvaluateMonth6UTCValue2030(state);                    \
    }                                                                           \
    BENCHMARK_F(Fixture, DateTruncEvaluateYear1NewYorkValue2020)                \
    (benchmark::State & state) {                                                \
        benchmarkDateTruncEvaluateYear1NewYorkValue2020(state);                 \
    }                                                                           \
    BENCHMARK_F(Fixture, DateTruncEvaluateYear1UTCValue2020)                    \
    (benchmark::State & state) {                                                \
        benchmarkDateTruncEvaluateYear1UTCValue2020(state);                     \
    }                                                                           \
    BENCHMARK_F(Fixture, DateTruncEvaluateYear1NewYorkValue2100)                \
    (benchmark::State & state) {                                                \
        benchmarkDateTruncEvaluateYear1NewYorkValue2100(state);                 \
    }                                                                           \
                                                                                \
    BENCHMARK_F(Fixture, GetFieldEvaluateExpression)                            \
    (benchmark::State & state) {                                                \
        benchmarkGetFieldEvaluateExpression(state);                             \
    }                                                                           \
    BENCHMARK_F(Fixture, GetFieldEvaluateShortSyntaxExpression)                 \
    (benchmark::State & state) {                                                \
        benchmarkGetFieldEvaluateShortSyntaxExpression(state);                  \
    }                                                                           \
    BENCHMARK_F(Fixture, GetFieldNestedExpression)(benchmark::State & state) {  \
        benchmarkGetFieldNestedExpression(state);                               \
    }                                                                           \
    BENCHMARK_F(Fixture, SetFieldEvaluateExpression)                            \
    (benchmark::State & state) {                                                \
        benchmarkSetFieldEvaluateExpression(state);                             \
    }                                                                           \
    BENCHMARK_F(Fixture, SetFieldWithRemoveExpression)                          \
    (benchmark::State & state) {                                                \
        benchmarkSetFieldWithRemoveExpression(state);                           \
    }                                                                           \
    BENCHMARK_F(Fixture, UnsetFieldEvaluateExpression)                          \
    (benchmark::State & state) {                                                \
        benchmarkUnsetFieldEvaluateExpression(state);                           \
    }                                                                           \
                                                                                \
    BENCHMARK_F(Fixture, SetIsSubset_allPresent)(benchmark::State & state) {    \
        benchmarkSetIsSubset_allPresent(state);                                 \
    }                                                                           \
    BENCHMARK_F(Fixture, SetIsSubset_nonePresent)(benchmark::State & state) {   \
        benchmarkSetIsSubset_nonePresent(state);                                \
    }                                                                           \
    BENCHMARK_F(Fixture, SetIntersection)(benchmark::State & state) {           \
        benchmarkSetIntersection(state);                                        \
    }                                                                           \
    BENCHMARK_F(Fixture, SetDifference)(benchmark::State & state) {             \
        benchmarkSetDifference(state);                                          \
    }                                                                           \
    BENCHMARK_F(Fixture, SetEquals)(benchmark::State & state) {                 \
        benchmarkSetEquals(state);                                              \
    }                                                                           \
    BENCHMARK_F(Fixture, SetUnion)(benchmark::State & state) {                  \
        benchmarkSetUnion(state);                                               \
    }

}  // namespace mongo
