// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/expression_typing.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/type_system/type_system_test_util.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <utility>

namespace mongo::pipeline::type_system {
namespace {

using expression::PathSemantics;
using expression::traversePath;

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

std::string traversedDebugString(const Type& inputType,
                                 const std::string& path,
                                 PathSemantics semantics = PathSemantics::kFieldPath) {
    return traversePath(inputType, FieldPath(path), semantics).toDebugString();
}

TEST(ExpressionTypingTest, TraversePathReadsTheField) {
    auto input = openObject({{"a", allValues(BSONType::numberInt)}});
    ASSERT_EQ(traversedDebugString(input, "a"), "int");
}

TEST(ExpressionTypingTest, TraversePathThroughScalarIsMissing) {
    auto input = openObject({{"a", allValues(BSONType::numberInt)}});
    ASSERT_EQ(traversedDebugString(input, "a.b"), "missing");
    ASSERT_EQ(traversedDebugString(input, "a.b.c"), "missing");
}

TEST(ExpressionTypingTest, TraversePathThroughNestedObject) {
    auto inner = closedObject({{"b", allValues(BSONType::numberInt)}});
    auto input = openObject({{"a", inner}});
    ASSERT_TRUE(traversePath(input, FieldPath("a"), PathSemantics::kFieldPath) == inner);
    ASSERT_EQ(traversedDebugString(input, "a.b"), "int");
    ASSERT_EQ(traversedDebugString(input, "a.c"), "missing");
    ASSERT_EQ(traversedDebugString(input, "a.b.c"), "missing");
    ASSERT_EQ(traversedDebugString(input, "other.b"), "any");
}

TEST(ExpressionTypingTest, TraversePathThroughArrayIsArray) {
    auto objectOrArray = unionType(closedObject({{"b", allValues(BSONType::numberInt)}}),
                                   allValues(BSONType::array));
    auto input = openObject({{"a", objectOrArray}});
    ASSERT_EQ(traversedDebugString(input, "a.b"), "int|array(S)");
    ASSERT_EQ(traversedDebugString(input, "a.b.c"), "missing|array(S)");
}

TEST(ExpressionTypingTest, TraversePathDoesNotTraverseLeafArray) {
    auto input = openObject({{"a", allValues(BSONType::array)}});
    ASSERT_EQ(traversedDebugString(input, "a"), "array");
    ASSERT_EQ(traversedDebugString(input, "a.b"), "array(S)");
}

TEST(ExpressionTypingTest, TraversePathThroughMixedObjectAndScalarKeepsMissing) {
    auto objectOrNumber =
        unionType(openObject({{"b", allValues(BSONType::string)}}), allValues(BSONType::numberInt));
    auto input = openObject({{"a", objectOrNumber}});
    ASSERT_EQ(traversedDebugString(input, "a.b"), "missing|string");
}

TEST(ExpressionTypingTest, TraversePathOnUnknownDocumentIsAny) {
    ASSERT_EQ(traversedDebugString(Type::anyObject(), "a"), "any");
    ASSERT_EQ(traversedDebugString(Type::anyObject(), "a.b"), "any");
}

TEST(ExpressionTypingTest, TraversePathOnClosedObjectIsMissing) {
    ASSERT_EQ(traversedDebugString(closedObject({}), "a"), "missing");
    ASSERT_EQ(traversedDebugString(closedObject({}), "a.b"), "missing");
}

TEST(ExpressionTypingTest, TraversePathOnNeverIsNever) {
    ASSERT_EQ(traversedDebugString(Type::never(), "a"), "never");
    ASSERT_EQ(traversedDebugString(Type::never(), "a.b"), "never");
}

TEST(ExpressionTypingTest, TraversePathOnNonObjectInput) {
    ASSERT_EQ(traversedDebugString(Type::anyArray(), "a"), "array(S)");
    ASSERT_EQ(traversedDebugString(allValues(BSONType::string), "a"), "missing");
    ASSERT_EQ(traversedDebugString(Type::any(), "a"), "any");
}

TEST(ExpressionTypingTest, TraversePathWithOtherSemanticsIsAny) {
    auto input = openObject({{"a", allValues(BSONType::numberInt)}});
    ASSERT_EQ(traversedDebugString(input, "a", PathSemantics::kOther), "any");
    ASSERT_EQ(traversedDebugString(input, "a.b", PathSemantics::kOther), "any");
}

TEST(ExpressionTypingTest, TraversePathMatchesEvaluationOnCorpus) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    static const auto corpus = generateDocumentCorpus("x.y.z");
    for (const std::string path : {"x", "x.y", "x.y.z"}) {
        auto expr = ExpressionFieldPath::createPathFromString(
            expCtx.get(), path, expCtx->variablesParseState);
        for (const auto& document : corpus) {
            const Value result = expr->evaluate(Document(document), &expCtx->variables, {});
            const Type traversed = traversePath(
                Type::fromValue(Value(document)), FieldPath(path), PathSemantics::kFieldPath);
            ASSERT_TRUE(traversed == Type::fromValue(result))
                << "$" << path << " evaluates to " << result.toString() << " on "
                << document.toString() << " but inferred " << traversed.toDebugString();
            ASSERT_TRUE(traversePath(Type::anyObject(), FieldPath(path), PathSemantics::kFieldPath)
                            .hasValue(result));
        }
    }
}

}  // namespace
}  // namespace mongo::pipeline::type_system
