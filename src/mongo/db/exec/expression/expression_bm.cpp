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
public:
    void benchmarkLogicalArray(benchmark::State& state, std::string_view op) {
        const bool all = op == "$allElementsTrue";
        std::vector<Value> values(state.range(0), Value(all));
        if (state.range(1) && !values.empty()) {
            values.front() = Value(!all);
        }
        // Field-dependent inputs keep optimization from folding away the scan.
        const auto spec = op == "$in" ? BSON("$in" << BSON_ARRAY(true << "$array"))
                                      : BSON(std::string(op) << BSON_ARRAY("$array"));
        benchmarkExpression(spec, state, {Document{{"array", Value(std::move(values))}}});
    }

private:
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

BENCHMARK_DEFINE_F(ClassicExpressionBenchmarkFixture, LogicalArrayIn)(benchmark::State& state) {
    benchmarkLogicalArray(state, "$in");
}

BENCHMARK_DEFINE_F(ClassicExpressionBenchmarkFixture, LogicalArrayAny)(benchmark::State& state) {
    benchmarkLogicalArray(state, "$anyElementTrue");
}

BENCHMARK_DEFINE_F(ClassicExpressionBenchmarkFixture, LogicalArrayAll)(benchmark::State& state) {
    benchmarkLogicalArray(state, "$allElementsTrue");
}

BENCHMARK_REGISTER_F(ClassicExpressionBenchmarkFixture, LogicalArrayIn)
    ->ArgsProduct({{0, 16, 1024, 16384, 500000}, {0, 1}})
    ->ArgNames({"elements", "shortCircuit"});
BENCHMARK_REGISTER_F(ClassicExpressionBenchmarkFixture, LogicalArrayAny)
    ->ArgsProduct({{0, 16, 1024, 16384, 500000}, {0, 1}})
    ->ArgNames({"elements", "shortCircuit"});
BENCHMARK_REGISTER_F(ClassicExpressionBenchmarkFixture, LogicalArrayAll)
    ->ArgsProduct({{0, 16, 1024, 16384, 500000}, {0, 1}})
    ->ArgNames({"elements", "shortCircuit"});

}  // namespace
}  // namespace mongo
