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

    void benchmarkAddIntegers(benchmark::State& state);
    void benchmarkAddDoubles(benchmark::State& state);
    void benchmarkAddDecimals(benchmark::State& state);
    void benchmarkAddDates(benchmark::State& state);
    void benchmarkAddNullAndMissing(benchmark::State& state);
    void benchmarkAddArray(benchmark::State& state);

    void benchmarkArrayArrayElemAt0(benchmark::State& state);
    void benchmarkArrayArrayElemAtLast(benchmark::State& state);
    void benchmarkArrayFilter0(benchmark::State& state);
    void benchmarkArrayFilter10(benchmark::State& state);
    void benchmarkArrayInFound0(benchmark::State& state);
    void benchmarkArrayInFound9(benchmark::State& state);
    void benchmarkArrayInNotFound(benchmark::State& state);

    void benchmarkCompareEq(benchmark::State& state);
    void benchmarkCompareGte(benchmark::State& state);
    void benchmarkCompareLte(benchmark::State& state);
    void benchmarkCompareNe(benchmark::State& state);

    void benchmarkConditionalCond(benchmark::State& state);
    void benchmarkConditionalIfNullFalse(benchmark::State& state);
    void benchmarkConditionalIfNullTrue(benchmark::State& state);
    void benchmarkConditionalSwitchCase0(benchmark::State& state);
    void benchmarkConditionalSwitchCase1(benchmark::State& state);
    void benchmarkConditionalSwitchDefault(benchmark::State& state);

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

    void benchmarkLogicalAndFalse0(benchmark::State& state);
    void benchmarkLogicalAndFalse1(benchmark::State& state);
    void benchmarkLogicalAndTrue(benchmark::State& state);
    void benchmarkLogicalOrTrue0(benchmark::State& state);
    void benchmarkLogicalOrTrue1(benchmark::State& state);
    void benchmarkLogicalOrFalse(benchmark::State& state);

    void benchmarkMultiplyIntegers(benchmark::State& state);
    void benchmarkMultiplyDoubles(benchmark::State& state);
    void benchmarkMultiplyDecimals(benchmark::State& state);
    void benchmarkMultiplyNullAndMissing(benchmark::State& state);
    void benchmarkMultiplyArray(benchmark::State& state);

    void benchmarkSetIsSubset_allPresent(benchmark::State& state);
    void benchmarkSetIsSubset_nonePresent(benchmark::State& state);
    void benchmarkSetIntersection(benchmark::State& state);
    void benchmarkSetDifference(benchmark::State& state);
    void benchmarkSetEquals(benchmark::State& state);
    void benchmarkSetUnion(benchmark::State& state);

    void benchmarkSubtractIntegers(benchmark::State& state);
    void benchmarkSubtractDoubles(benchmark::State& state);
    void benchmarkSubtractDecimals(benchmark::State& state);
    void benchmarkSubtractDates(benchmark::State& state);
    void benchmarkSubtractNullAndMissing(benchmark::State& state);

    void benchmarkValueConst(benchmark::State& state);
    void benchmarkValueLiteral(benchmark::State& state);

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
    void testBinaryOpExpression(const std::string& binaryOp,
                                const std::vector<Document>& generator,
                                benchmark::State& state);

    BSONArray randomBSONArray(int count, int max, int offset = 0);

    PseudoRandom random;
};

/**
 * SERVER-70260: When fixed, the benchmarks in this macro should be merged alphabetically into the
 * BENCHMARK_EXPRESSIONS() macro below and the current macro deleted. Please preserve the groupings
 * (separated by blank lines between groups), which are based on the BM names' prefixes {Array,
 * Conditional}.
 *
 * Macro to register benchmark expressions that crash under the test framework in SBE.
 *   Fixture: class name of the implementing child class for Classic engine.
 */
#define BENCHMARK_EXPRESSIONS_CLASSIC_ONLY(Fixture)                            \
                                                                               \
    BENCHMARK_F(Fixture, ArrayArrayElemAt0)(benchmark::State & state) {        \
        benchmarkArrayArrayElemAt0(state);                                     \
    }                                                                          \
    BENCHMARK_F(Fixture, ArrayArrayElemAtLast)(benchmark::State & state) {     \
        benchmarkArrayArrayElemAtLast(state);                                  \
    }                                                                          \
    BENCHMARK_F(Fixture, ArrayFilter0)(benchmark::State & state) {             \
        benchmarkArrayFilter0(state);                                          \
    }                                                                          \
    BENCHMARK_F(Fixture, ArrayFilter10)(benchmark::State & state) {            \
        benchmarkArrayFilter10(state);                                         \
    }                                                                          \
                                                                               \
    BENCHMARK_F(Fixture, ConditionalCond)(benchmark::State & state) {          \
        benchmarkConditionalCond(state);                                       \
    }                                                                          \
    BENCHMARK_F(Fixture, ConditionalIfNullFalse)(benchmark::State & state) {   \
        benchmarkConditionalIfNullFalse(state);                                \
    }                                                                          \
    BENCHMARK_F(Fixture, ConditionalIfNullTrue)(benchmark::State & state) {    \
        benchmarkConditionalIfNullTrue(state);                                 \
    }                                                                          \
    BENCHMARK_F(Fixture, ConditionalSwitchCase0)(benchmark::State & state) {   \
        benchmarkConditionalSwitchCase0(state);                                \
    }                                                                          \
    BENCHMARK_F(Fixture, ConditionalSwitchCase1)(benchmark::State & state) {   \
        benchmarkConditionalSwitchCase1(state);                                \
    }                                                                          \
    BENCHMARK_F(Fixture, ConditionalSwitchDefault)(benchmark::State & state) { \
        benchmarkConditionalSwitchDefault(state);                              \
    }

/**
 * Macro to register benchmark expressions for both Classic and SBE engines.
 *   Fixture: class name of the implementing child class for current engine.
 */
#define BENCHMARK_EXPRESSIONS(Fixture)                                          \
                                                                                \
    BENCHMARK_F(Fixture, NoOp)                                                  \
    (benchmark::State & state) {                                                \
        noOpBenchmark(state);                                                   \
    }                                                                           \
                                                                                \
    BENCHMARK_F(Fixture, ArrayInFound0)(benchmark::State & state) {             \
        benchmarkArrayInFound0(state);                                          \
    }                                                                           \
    BENCHMARK_F(Fixture, ArrayInFound9)(benchmark::State & state) {             \
        benchmarkArrayInFound9(state);                                          \
    }                                                                           \
    BENCHMARK_F(Fixture, ArrayInNotFound)(benchmark::State & state) {           \
        benchmarkArrayInNotFound(state);                                        \
    }                                                                           \
                                                                                \
    BENCHMARK_F(Fixture, CompareEq)(benchmark::State & state) {                 \
        benchmarkCompareEq(state);                                              \
    }                                                                           \
    BENCHMARK_F(Fixture, CompareGte)(benchmark::State & state) {                \
        benchmarkCompareGte(state);                                             \
    }                                                                           \
    BENCHMARK_F(Fixture, CompareLte)(benchmark::State & state) {                \
        benchmarkCompareLte(state);                                             \
    }                                                                           \
    BENCHMARK_F(Fixture, CompareNe)(benchmark::State & state) {                 \
        benchmarkCompareNe(state);                                              \
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
    BENCHMARK_F(Fixture, LogicalAndFalse0)(benchmark::State & state) {          \
        benchmarkLogicalAndFalse0(state);                                       \
    }                                                                           \
    BENCHMARK_F(Fixture, LogicalAndFalse1)(benchmark::State & state) {          \
        benchmarkLogicalAndFalse1(state);                                       \
    }                                                                           \
    BENCHMARK_F(Fixture, LogicalAndTrue)(benchmark::State & state) {            \
        benchmarkLogicalAndTrue(state);                                         \
    }                                                                           \
    BENCHMARK_F(Fixture, LogicalOrTrue0)(benchmark::State & state) {            \
        benchmarkLogicalOrTrue0(state);                                         \
    }                                                                           \
    BENCHMARK_F(Fixture, LogicalOrTrue1)(benchmark::State & state) {            \
        benchmarkLogicalOrTrue1(state);                                         \
    }                                                                           \
    BENCHMARK_F(Fixture, LogicalOrFalse)(benchmark::State & state) {            \
        benchmarkLogicalOrFalse(state);                                         \
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
    }                                                                           \
                                                                                \
    BENCHMARK_F(Fixture, SubtractIntegers)(benchmark::State & state) {          \
        benchmarkSubtractIntegers(state);                                       \
    }                                                                           \
    BENCHMARK_F(Fixture, SubtractDoubles)(benchmark::State & state) {           \
        benchmarkSubtractDoubles(state);                                        \
    }                                                                           \
    BENCHMARK_F(Fixture, SubtractDecimals)(benchmark::State & state) {          \
        benchmarkSubtractDecimals(state);                                       \
    }                                                                           \
    BENCHMARK_F(Fixture, SubtractDates)(benchmark::State & state) {             \
        benchmarkSubtractDates(state);                                          \
    }                                                                           \
    BENCHMARK_F(Fixture, SubtractNullAndMissing)(benchmark::State & state) {    \
        benchmarkSubtractNullAndMissing(state);                                 \
    }                                                                           \
                                                                                \
    BENCHMARK_F(Fixture, AddIntegers)(benchmark::State & state) {               \
        benchmarkAddIntegers(state);                                            \
    }                                                                           \
    BENCHMARK_F(Fixture, AddDoubles)(benchmark::State & state) {                \
        benchmarkAddDoubles(state);                                             \
    }                                                                           \
    BENCHMARK_F(Fixture, AddDecimals)(benchmark::State & state) {               \
        benchmarkAddDecimals(state);                                            \
    }                                                                           \
    BENCHMARK_F(Fixture, AddDates)(benchmark::State & state) {                  \
        benchmarkAddDates(state);                                               \
    }                                                                           \
    BENCHMARK_F(Fixture, AddNullAndMissing)(benchmark::State & state) {         \
        benchmarkAddNullAndMissing(state);                                      \
    }                                                                           \
    BENCHMARK_F(Fixture, AddArray)(benchmark::State & state) {                  \
        benchmarkAddArray(state);                                               \
    }                                                                           \
                                                                                \
    BENCHMARK_F(Fixture, MultiplyIntegers)(benchmark::State & state) {          \
        benchmarkMultiplyIntegers(state);                                       \
    }                                                                           \
    BENCHMARK_F(Fixture, MultiplyDoubles)(benchmark::State & state) {           \
        benchmarkMultiplyDoubles(state);                                        \
    }                                                                           \
    BENCHMARK_F(Fixture, MultiplyDecimals)(benchmark::State & state) {          \
        benchmarkMultiplyDecimals(state);                                       \
    }                                                                           \
    BENCHMARK_F(Fixture, MultiplyNullAndMissing)(benchmark::State & state) {    \
        benchmarkMultiplyNullAndMissing(state);                                 \
    }                                                                           \
    BENCHMARK_F(Fixture, MultiplyArray)(benchmark::State & state) {             \
        benchmarkMultiplyArray(state);                                          \
    }                                                                           \
                                                                                \
    BENCHMARK_F(Fixture, ValueConst)(benchmark::State & state) {                \
        benchmarkValueConst(state);                                             \
    }                                                                           \
    BENCHMARK_F(Fixture, ValueLiteral)(benchmark::State & state) {              \
        benchmarkValueLiteral(state);                                           \
    }

}  // namespace mongo
