// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/expression.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/expression_bm_fixture.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/util/intrusive_counter.h"

#include <memory>
#include <vector>

#include <benchmark/benchmark.h>


namespace mongo {
namespace {

class ClassicExpressionBenchmarkFixture : public ExpressionBenchmarkFixture {
    void benchmarkExpression(BSONObj expressionSpec,
                             benchmark::State& state,
                             const std::vector<Document>& documents) final {
        auto opContext = getServiceContext()->makeOperationContext();
        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");
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

BENCHMARK_EXPRESSIONS(ClassicExpressionBenchmarkFixture)

}  // namespace
}  // namespace mongo
