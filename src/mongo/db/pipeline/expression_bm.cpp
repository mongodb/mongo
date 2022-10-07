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
#include "mongo/db/pipeline/expression_bm_fixture.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"

namespace mongo {
namespace {

class ClassicExpressionBenchmarkFixture : public ExpressionBenchmarkFixture {
    void benchmarkExpression(BSONObj expressionSpec,
                             benchmark::State& state,
                             const std::vector<Document>& documents) override final {
        QueryTestServiceContext testServiceContext;
        auto opContext = testServiceContext.makeOperationContext();
        NamespaceString nss("test.bm");
        auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);

        // Build an expression.
        auto expression = Expression::parseExpression(
            exprContext.get(), expressionSpec, exprContext->variablesParseState);

        expression = expression->optimize();

        // Prepare parameters for the 'evaluate()' call.
        auto variables = &(exprContext->variables);

        // Run the test.
        for (auto keepRunning : state) {
            for (const auto& document : documents) {
                benchmark::DoNotOptimize(expression->evaluate(document, variables));
            }
            benchmark::ClobberMemory();
        }
    }
};

BENCHMARK_EXPRESSIONS_CLASSIC_ONLY(ClassicExpressionBenchmarkFixture)
BENCHMARK_EXPRESSIONS(ClassicExpressionBenchmarkFixture)

}  // namespace
}  // namespace mongo
