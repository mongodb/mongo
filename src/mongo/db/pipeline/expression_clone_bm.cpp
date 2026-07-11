// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/expression_bm_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"

#include <vector>

#include <benchmark/benchmark.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

class ExpressionCloneBenchmarkFixture : public ExpressionBenchmarkFixture {
protected:
    void benchmarkExpression(BSONObj expressionSpec,
                             benchmark::State& state,
                             const std::vector<Document>& documents) final {
        auto opContext = getServiceContext()->makeOperationContext();
        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.bm");
        auto exprContext = make_intrusive<ExpressionContextForTest>(opContext.get(), nss);

        // Build an expression.
        auto expression = Expression::parseExpression(
            exprContext.get(), expressionSpec, exprContext->variablesParseState);

        // Run the test.
        for (auto keepRunning : state) {
            benchmark::DoNotOptimize(cloneExpression(expression.get()));
            benchmark::ClobberMemory();
        }
    }

    virtual boost::intrusive_ptr<Expression> cloneExpression(const Expression* expr) const = 0;
};

class ExpressionNativeCloneBenchmarkFixture : public ExpressionCloneBenchmarkFixture {
protected:
    boost::intrusive_ptr<Expression> cloneExpression(const Expression* expr) const final {
        return expr->clone(*expr->getExpressionContext());
    }
};

class ExpressionSerializeDeserializeCloneBenchmarkFixture : public ExpressionCloneBenchmarkFixture {
protected:
    boost::intrusive_ptr<Expression> cloneExpression(const Expression* expr) const final {
        BSONObjBuilder bob;
        bob << "" << expr->serialize();
        return Expression::parseOperand(expr->getExpressionContext(),
                                        bob.obj().firstElement(),
                                        expr->getExpressionContext()->variablesParseState);
    }
};

BENCHMARK_EXPRESSIONS(ExpressionNativeCloneBenchmarkFixture)
BENCHMARK_EXPRESSIONS(ExpressionSerializeDeserializeCloneBenchmarkFixture)

}  // namespace
}  // namespace mongo
