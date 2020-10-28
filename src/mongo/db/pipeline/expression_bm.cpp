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

#include "mongo/platform/basic.h"

#include <benchmark/benchmark.h>

#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"

namespace mongo {
namespace {
/**
 * Tests performance of 'evaluate()' of $dateDiff expression.
 *
 * startDate - start date in milliseconds from the UNIX epoch.
 * endDate - end date in milliseconds from the UNIX epoch.
 * unit - a string expression of units to use for date difference calculation.
 * timezone - a string representation of timezone to use for date difference calculation.
 * state - benchmarking state.
 */
void testDateDiffExpression(long long startDate,
                            long long endDate,
                            std::string unit,
                            boost::optional<std::string> timezone,
                            benchmark::State& state) {
    QueryTestServiceContext testServiceContext;
    auto opContext = testServiceContext.makeOperationContext();
    NamespaceString nss("test.bm");
    boost::intrusive_ptr<ExpressionContextForTest> exprContext =
        new ExpressionContextForTest(opContext.get(), nss);

    // Build a $dateDiff expression.
    BSONObjBuilder objBuilder;
    objBuilder << "startDate" << Date_t::fromMillisSinceEpoch(startDate) << "endDate"
               << Date_t::fromMillisSinceEpoch(endDate) << "unit" << unit;
    if (timezone) {
        objBuilder << "timezone" << *timezone;
    }
    auto expression = BSON("$dateDiff" << objBuilder.obj());
    auto dateDiffExpression = Expression::parseExpression(
        exprContext.get(), expression, exprContext->variablesParseState);

    // Prepare parameters for the 'evaluate()' call.
    auto variables = &(exprContext->variables);
    Document document;

    // Run the test.
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(dateDiffExpression->evaluate(document, variables));
        benchmark::ClobberMemory();
    }
}

void BM_DateDiffEvaluateMinute300Years(benchmark::State& state) {
    testDateDiffExpression(-1640989478000LL /* 1918-01-01*/,
                           7826117722000LL /* 2218-01-01*/,
                           "minute",
                           boost::none /*timezone*/,
                           state);
}

void BM_DateDiffEvaluateMinute2Years(benchmark::State& state) {
    testDateDiffExpression(1542448721000LL /* 2018-11-17*/,
                           1605607121000LL /* 2020-11-17*/,
                           "minute",
                           boost::none /*timezone*/,
                           state);
}

void BM_DateDiffEvaluateMinute2YearsWithTimezone(benchmark::State& state) {
    testDateDiffExpression(1542448721000LL /* 2018-11-17*/,
                           1605607121000LL /* 2020-11-17*/,
                           "minute",
                           std::string{"America/New_York"},
                           state);
}

void BM_DateDiffEvaluateWeek(benchmark::State& state) {
    testDateDiffExpression(7826117722000LL /* 2218-01-01*/,
                           4761280721000LL /*2120-11-17*/,
                           "week",
                           boost::none /*timezone*/,
                           state);
}

BENCHMARK(BM_DateDiffEvaluateMinute300Years);
BENCHMARK(BM_DateDiffEvaluateMinute2Years);
BENCHMARK(BM_DateDiffEvaluateMinute2YearsWithTimezone);
BENCHMARK(BM_DateDiffEvaluateWeek);

/**
 * Tests performance of evaluate() method of $dateAdd
 */
void testDateAddExpression(long long startDate,
                           std::string unit,
                           long long amount,
                           boost::optional<std::string> timezone,
                           benchmark::State& state) {
    QueryTestServiceContext testServiceContext;
    auto opContext = testServiceContext.makeOperationContext();
    NamespaceString nss("test.bm");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx =
        new ExpressionContextForTest(opContext.get(), nss);

    BSONObjBuilder objBuilder;
    objBuilder << "startDate" << Date_t::fromMillisSinceEpoch(startDate) << "unit" << unit
               << "amount" << amount;
    if (timezone) {
        objBuilder << "timezone" << *timezone;
    }
    auto doc = BSON("$dateAdd" << objBuilder.obj());
    auto dateAddExp = Expression::parseExpression(expCtx.get(), doc, expCtx->variablesParseState);
    auto variables = &(expCtx->variables);
    Document document;

    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(dateAddExp->evaluate(document, variables));
        benchmark::ClobberMemory();
    }
}

void BM_DateAddEvaluate10Days(benchmark::State& state) {
    testDateAddExpression(1604131115000LL,
                          "day",
                          10LL,
                          boost::none, /* timezone */
                          state);
}

void BM_DateAddEvaluate100KSeconds(benchmark::State& state) {
    testDateAddExpression(1604131115000LL,
                          "second",
                          100000LL,
                          boost::none, /* timezone */
                          state);
}

void BM_DateAddEvaluate100Years(benchmark::State& state) {
    testDateAddExpression(1604131115000LL,
                          "year",
                          100LL,
                          boost::none, /* timezone */
                          state);
}

void BM_DateAddEvaluate12HoursWithTimezone(benchmark::State& state) {
    testDateAddExpression(1604131115000LL, "hour", 12LL, std::string{"America/New_York"}, state);
}

BENCHMARK(BM_DateAddEvaluate10Days);
BENCHMARK(BM_DateAddEvaluate100KSeconds);
BENCHMARK(BM_DateAddEvaluate100Years);
BENCHMARK(BM_DateAddEvaluate12HoursWithTimezone);

}  // namespace
}  // namespace mongo
