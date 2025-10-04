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
        return expr->clone();
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
