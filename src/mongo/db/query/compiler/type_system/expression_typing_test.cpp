// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/expression_typing.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <utility>

namespace mongo::pipeline::type_system {
namespace {

boost::intrusive_ptr<Expression> parseExpr(BSONObj spec, ExpressionContextForTest& expCtx) {
    return Expression::parseExpression(&expCtx, spec, expCtx.variablesParseState);
}

/// Returns the type the expression evaluates to.
std::string outputTypeDebugString(const std::string& expression,
                                  Type inputType = Type::anyObject()) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = parseExpr(fromjson(expression), *expCtx);
    return getOutputType(inputType, expr.get()).toDebugString();
}

/// Returns the type the root document must have when the expression evaluates to 'assumeTrue'.
std::string narrowedDebugString(const std::string& expression,
                                Type inputType,
                                bool assumeTrue = true) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = parseExpr(fromjson(expression), *expCtx);
    return narrowType(std::move(inputType), expr.get(), assumeTrue).toDebugString();
}

TEST(ExpressionTypingTest, UnsupportedExpressionTypeIsAny) {
    auto input = Type::anyObject();
    auto expr = "{$arrayElemAt: ['$a', 1]}";
    auto output = "any";
    auto narrowed = "object";
    ASSERT_EQ(outputTypeDebugString(expr), output);
    ASSERT_EQ(narrowedDebugString(expr, input), narrowed);
}

TEST(ExpressionTypingTest, EmptyOrMatchesNoDocument) {
    auto input = Type::anyObject();
    auto expr = "{$expr: {$or: []}}";
    // $or always produces a boolean.
    auto output = "bool(S)";
    // An empty $or is always false, so no document passes.
    auto narrowed = "never";
    auto narrowedAssumeFalse = "object";
    ASSERT_EQ(outputTypeDebugString(expr), output);
    ASSERT_EQ(narrowedDebugString(expr, input), narrowed);
    ASSERT_EQ(narrowedDebugString(expr, input, false), narrowedAssumeFalse);
}


}  // namespace
}  // namespace mongo::pipeline::type_system
