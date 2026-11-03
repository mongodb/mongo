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

using expression::narrowPath;
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

std::string narrowedPathDebugString(const Type& inputType,
                                    const std::string& path,
                                    Type constraint,
                                    bool assumeTrue = true,
                                    PathSemantics semantics = PathSemantics::kFieldPath) {
    return narrowPath(inputType, FieldPath(path), semantics, std::move(constraint), assumeTrue)
        .toDebugString();
}

TEST(ExpressionPathTypingTest, NarrowPathNarrowsTheField) {
    auto input = openObject({{"a", Type::any()}, {"b", allValues(BSONType::string)}});
    ASSERT_EQ(narrowedPathDebugString(input, "a", numbers()), "{a: number, b: string, ...}");
}

TEST(ExpressionPathTypingTest, NarrowDottedPathImpliesObjectPrefix) {
    // Like {$isNumber: '$a.b'}: a number at 'a.b' rules out an array or a scalar at 'a'.
    auto input = openObject({{"b", allValues(BSONType::string)}});
    ASSERT_EQ(narrowedPathDebugString(input, "a.b", numbers()),
              "{a: {b: number, ...}, b: string, ...}");
    ASSERT_EQ(narrowedPathDebugString(Type::anyObject(), "a.b.c", numbers()),
              "{a: {b: {c: number, ...}, ...}, ...}");
}

TEST(ExpressionPathTypingTest, NarrowPathToSubsetKeepsSubset) {
    // Like {$eq: ['$a.b', 1]}: only a subset of the numbers is possible.
    ASSERT_EQ(narrowedPathDebugString(Type::anyObject(), "a.b", numbers(Extent::kSubset)),
              "{a: {b: number(S), ...}, ...}");
}

TEST(ExpressionPathTypingTest, NarrowPathDoesNotAddArraysHoldingMatch) {
    // Unlike a MatchExpression, '$a' does not traverse the array at 'a' (because it's the leaf
    // component).
    auto input = openObject({{"a", unionType(allValues(BSONType::numberInt), Type::anyArray())}});
    ASSERT_EQ(narrowedPathDebugString(input, "a", allValues(BSONType::numberInt)), "{a: int, ...}");
}

TEST(ExpressionPathTypingTest, NarrowDottedPathToArrayKeepsArraysOnPrefix) {
    ASSERT_EQ(narrowedPathDebugString(Type::anyObject(), "a.b", Type::anyArray()),
              "{a: {b: array, ...}|array, ...}");
    ASSERT_EQ(narrowedPathDebugString(Type::anyObject(), "a.b", Type::someArray()),
              "{a: {b: array(S), ...}|array(S), ...}");
}

TEST(ExpressionPathTypingTest, NarrowDottedPathToMissingKeepsScalarsOnPrefix) {
    ASSERT_EQ(narrowedPathDebugString(Type::anyObject(), "a.b", Type::missing()),
              "{a: ~(object|array)|{b: missing, ...}, ...}");
}

TEST(ExpressionPathTypingTest, NegatedNarrowDottedPath) {
    ASSERT_EQ(narrowedPathDebugString(Type::anyObject(), "a.b", Type::missing(), false),
              "{a: {b: ~missing, ...}|array, ...}");
    ASSERT_EQ(narrowedPathDebugString(Type::anyObject(), "a.b", complement(Type::missing()), false),
              "{a: ~(object|array)|{b: missing, ...}, ...}");
}

TEST(ExpressionPathTypingTest, NarrowDottedPathOnScalarPrefix) {
    auto input = openObject({{"a", allValues(BSONType::numberInt)}});
    ASSERT_EQ(narrowedPathDebugString(input, "a.b", allValues(BSONType::numberInt)), "never");
    ASSERT_EQ(narrowedPathDebugString(input, "a.b", Type::missing()), "{a: int, ...}");
    ASSERT_EQ(narrowedPathDebugString(input, "a.b", Type::missing(), false), "never");
}

TEST(ExpressionPathTypingTest, NarrowDottedPathOnArrayPrefix) {
    auto input = openObject({{"a", Type::anyArray()}});
    ASSERT_EQ(narrowedPathDebugString(input, "a.b", allValues(BSONType::numberInt)), "never");
    ASSERT_EQ(narrowedPathDebugString(input, "a.b", Type::anyArray()), "{a: array, ...}");
    ASSERT_EQ(narrowedPathDebugString(input, "a.b", Type::someArray()), "{a: array(S), ...}");
    ASSERT_EQ(narrowedPathDebugString(input, "a.b", Type::anyArray(), false), "never");
}

TEST(ExpressionPathTypingTest, NarrowDottedPathKeepsKnownFieldsOfPrefix) {
    auto inner = unionType(
        openObject({{"b", unionType(allValues(BSONType::numberInt), allValues(BSONType::string))},
                    {"c", allValues(BSONType::string)}}),
        allValues(BSONType::string));
    auto input = openObject({{"a", inner}});
    ASSERT_EQ(narrowedPathDebugString(input, "a.b", allValues(BSONType::numberInt)),
              "{a: {b: int, c: string, ...}, ...}");
}

TEST(ExpressionPathTypingTest, NarrowDottedPathOnClosedPrefix) {
    auto input = openObject({{"a", closedObject({})}});
    ASSERT_EQ(narrowedPathDebugString(input, "a.b", allValues(BSONType::numberInt)), "never");
    ASSERT_EQ(narrowedPathDebugString(input, "a.b", Type::missing()), "{a: {}, ...}");
}

TEST(ExpressionPathTypingTest, NarrowPathToNeverLeavesNoDocument) {
    ASSERT_EQ(narrowedPathDebugString(Type::anyObject(), "a", Type::never()), "never");
    ASSERT_EQ(narrowedPathDebugString(Type::anyObject(), "a.b", Type::any(), false), "never");
}

TEST(ExpressionPathTypingTest, NarrowPathOnNeverIsNever) {
    ASSERT_EQ(narrowedPathDebugString(Type::never(), "a", Type::any()), "never");
    ASSERT_EQ(narrowedPathDebugString(Type::never(), "a.b", Type::any()), "never");
}

TEST(ExpressionPathTypingTest, NarrowPathOnNonObjectInput) {
    auto input = allValues(BSONType::string);
    ASSERT_EQ(narrowedPathDebugString(input, "a", allValues(BSONType::numberInt)), "never");
    ASSERT_EQ(narrowedPathDebugString(input, "a", Type::missing()), "string");
}

TEST(ExpressionPathTypingTest, NarrowPathWithOtherSemanticsNarrowsNothing) {
    auto input = openObject({{"a", unionType(allValues(BSONType::numberInt), Type::anyArray())}});
    for (const std::string path : {"a", "a.b"}) {
        ASSERT_EQ(narrowedPathDebugString(
                      input, path, allValues(BSONType::numberInt), true, PathSemantics::kOther),
                  input.toDebugString());
    }
}

TEST(ExpressionPathTypingTest, NarrowPathMatchesEvaluationOnCorpus) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    static const auto corpus = generateCombinedCorpus({"x.y.z", "x.0.y"});
    const std::vector<Type> constraints{
        allValues(BSONType::numberInt),
        allValues(BSONType::null),
        Type::missing(),
        unionType(Type::missing(), allValues(BSONType::null)),
        Type::anyArray(),
        Type::anyObject(),
        unionType(allValues(BSONType::numberInt), Type::anyArray()),
        complement(Type::missing()),
        numbers(),
        Type::any(),
        Type::never(),
        numbers(Extent::kSubset),
        Type::someArray(),
        unionType(Type::missing(), Type::someArray()),
        Type(BSONType::object, Extent::kSubset),
        openObject({{"y", allValues(BSONType::numberInt)}}),
    };
    for (const std::string path : {"x", "x.y", "x.y.z", "x.0", "x.0.y"}) {
        auto expr = ExpressionFieldPath::createPathFromString(
            expCtx.get(), path, expCtx->variablesParseState);
        for (const auto& constraint : constraints) {
            // A constraint covering only some values of a type only requires admitting every
            // document where the path may evaluate to a value of the constraint.
            const bool exact = isAll(constraint.getExtent());
            for (bool assumeTrue : {true, false}) {
                const Type leaf = assumeTrue ? constraint : complement(constraint);
                const Type narrowed = narrowPath(Type::anyObject(),
                                                 FieldPath(path),
                                                 PathSemantics::kFieldPath,
                                                 constraint,
                                                 assumeTrue);
                for (const auto& document : corpus) {
                    const Value result = expr->evaluate(Document(document), &expCtx->variables, {});
                    const bool possible = leaf.hasValue(result);
                    const bool admitted = narrowed.hasValue(Value(document));
                    const bool assertion = exact ? admitted == possible : admitted || !possible;
                    ASSERT_TRUE(assertion)
                        << "$" << path << " evaluates to " << result.toString() << " on "
                        << document.toString() << " with constraint " << constraint.toDebugString()
                        << " assumeTrue " << assumeTrue << " but inferred "
                        << narrowed.toDebugString();
                }
            }
        }
    }
}

}  // namespace
}  // namespace mongo::pipeline::type_system
