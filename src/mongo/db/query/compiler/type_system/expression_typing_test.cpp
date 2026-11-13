// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/expression_typing.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/type_system/type_system_test_util.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <utility>
#include <vector>

namespace mongo::pipeline::type_system {
namespace {

using namespace std::literals::string_view_literals;

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

TEST(ExpressionTypingTest, NegatedOrOfEqualitiesLeavesTypeUnchanged) {
    auto input =
        openObject({{"a", allValues(BSONType::numberInt)}, {"b", allValues(BSONType::string)}});
    const auto expr = "{$not: {$or: [{$eq: ['$a', 5]}, {$eq: ['$b', 'lalala']}]}}";
    ASSERT_EQ(narrowedDebugString(expr, input), "{a: int, b: string, ...}");
    ASSERT_EQ(narrowedDebugString(expr, input, false), "{a: int, b: string, ...}");
}

TEST(ExpressionTypingTest, ConstantOutputType) {
    ASSERT_EQ(outputTypeDebugString("{$const: 5}"), "int(S)");
    ASSERT_EQ(outputTypeDebugString("{$const: null}"), "null");
    ASSERT_EQ(outputTypeDebugString("{$const: {a: 'str'}}"), "{a: string(S)}");
    ASSERT_EQ(outputTypeDebugString("{$const: [1, 'str']}"), "array(S)");
}

TEST(ExpressionTypingTest, ConstantOutputTypeMatchesEvaluationOnScalars) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    for (const auto& value : scalarRepresentatives()) {
        auto expr = ExpressionConstant::create(expCtx.get(), value);
        const Type output = getOutputType(Type::anyObject(), expr.get());
        ASSERT_TRUE(output.hasValue(value))
            << "constant " << value.toString() << " but inferred " << output.toDebugString();
    }
}

TEST(ExpressionTypingTest, EqualityNarrowingPreservesEvaluationResults) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto values = equalityNarrowingCorpus();

    auto path =
        ExpressionFieldPath::createPathFromString(expCtx.get(), "a", expCtx->variablesParseState);
    for (const auto& constant : values) {
        auto literal = ExpressionConstant::create(expCtx.get(), constant);
        for (bool reverse : {false, true}) {
            auto expr = reverse
                ? ExpressionCompare::create(expCtx.get(), ExpressionCompare::EQ, literal, path)
                : ExpressionCompare::create(expCtx.get(), ExpressionCompare::EQ, path, literal);
            const auto narrowedTrue = narrowType(Type::anyObject(), expr.get(), true);
            const auto narrowedFalse = narrowType(Type::anyObject(), expr.get(), false);
            for (const auto& candidate : values) {
                Document doc{{"a", candidate}};
                const bool matches = expr->evaluate(doc, &expCtx->variables, {}).getBool();
                const auto& narrowed = matches ? narrowedTrue : narrowedFalse;
                ASSERT_TRUE(narrowed.hasValue(Value(doc)))
                    << "constant " << constant.toString() << ", candidate " << candidate.toString()
                    << ", reverse " << reverse << ", result " << matches << ", type "
                    << narrowed.toDebugString();
            }
        }
    }
}

TEST(ExpressionTypingTest, EqualityNarrowingUsesComparisonClasses) {
    auto input = Type::anyObject();
    const auto assertFieldType = [&](const std::string& expr, const Type& expected) {
        auto expCtx = make_intrusive<ExpressionContextForTest>();
        auto parsed = parseExpr(fromjson(expr), *expCtx);
        ASSERT_EQ(narrowType(input, parsed.get(), true).getField("a").toDebugString(),
                  expected.toDebugString());
    };
    assertFieldType("{$eq: ['$a', 1]}", numbers(Extent::kSubset));
    assertFieldType("{$eq: [1, '$a']}", numbers(Extent::kSubset));
    assertFieldType("{$eq: ['$a', {$literal: 1}]}", numbers(Extent::kSubset));
    assertFieldType("{$eq: ['$a', {$const: {x: 1}}]}", Type(BSONType::object, Extent::kSubset));
    assertFieldType("{$eq: ['$a', {x: {y: 1}}]}", Type(BSONType::object, Extent::kSubset));
    assertFieldType("{$eq: ['$a', 'str']}", Type(sameCanonicalClass(BSONType::string)));
    assertFieldType("{$eq: ['$a', [1, 'str']]}", Type::someArray());
    assertFieldType("{$eq: ['$a', {$const: {}}]}", Type(BSONType::object, Extent::kSubset));
    assertFieldType("{$eq: ['$$CURRENT.a', 1]}", numbers(Extent::kSubset));
}

TEST(ExpressionTypingTest, EqualityNarrowingDottedPaths) {
    for (const auto& expr : {"{$eq: ['$a.b', 1]}",
                             "{$eq: [1, '$a.b']}",
                             "{$eq: ['$$ROOT.a.b', 1]}",
                             "{$eq: ['$$CURRENT.a.b', 1]}"}) {
        ASSERT_EQ(narrowedDebugString(expr, Type::anyObject()), "{a: {b: number(S), ...}, ...}")
            << expr;
    }
    ASSERT_EQ(narrowedDebugString("{$eq: ['$a.b.c', 1]}", Type::anyObject()),
              "{a: {b: {c: number(S), ...}, ...}, ...}");
    ASSERT_EQ(narrowedDebugString("{$eq: ['$a.b', {$const: [1]}]}", Type::anyObject()),
              "{a: {b: array(S), ...}|array(S), ...}");

    auto input = openObject({{"a", openObject({{"b", allValues(BSONType::string)}})}});
    ASSERT_EQ(narrowedDebugString("{$eq: ['$a.b', 1]}", input), "never");
}

TEST(ExpressionTypingTest, EqualityNarrowingDottedPathsWhenFalse) {
    ASSERT_EQ(narrowedDebugString("{$eq: ['$a.b', 1]}", Type::anyObject(), false), "object");
    ASSERT_EQ(narrowedDebugString("{$eq: ['$$ROOT.a.b.c', null]}", Type::anyObject(), false),
              "{a: ~object|{b: ~object|{c: ~null, ...}, ...}, ...}");
    ASSERT_EQ(
        narrowedDebugString("{$eq: [{$const: [1]}, '$$CURRENT.a.b']}", Type::anyObject(), false),
        "object");
}

TEST(ExpressionTypingTest, EqualityNarrowingUsesOtherOperandOutputType) {
    ASSERT_EQ(narrowedDebugString("{$eq: ['$a', {$or: [true]}]}", Type::anyObject()),
              "{a: bool(S), ...}");
}

TEST(ExpressionTypingTest, EqualityNarrowingWithUntypedOperandLeavesInputUnchanged) {
    auto input = openObject({{"a", allValues(BSONType::string)}});
    for (const auto& expr : {"{$eq: ['$a', {$arrayElemAt: ['$b', 1]}]}",
                             "{$eq: [{$arrayElemAt: ['$b', 1]}, '$a.b']}"}) {
        ASSERT_EQ(narrowedDebugString(expr, input), "{a: string, ...}") << expr;
        ASSERT_EQ(narrowedDebugString(expr, input, false), "{a: string, ...}") << expr;
    }
}

TEST(ExpressionTypingTest, EqualityNarrowingIntersectsWithExistingFieldType) {
    const auto stringInput = openObject({{"a", allValues(BSONType::string)}});
    const auto doubleInput = openObject({{"a", allValues(BSONType::numberDouble)}});
    ASSERT_EQ(narrowedDebugString("{$eq: ['$a', 1]}", stringInput), "never");
    ASSERT_EQ(narrowedDebugString("{$eq: ['$a', 1]}", doubleInput), "{a: double(S), ...}");
}

TEST(ExpressionTypingTest, EqualityNarrowingKeepsUnequalValuesOfTheSameType) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto expr = parseExpr(fromjson("{$eq: ['$a', 1]}"), *expCtx);
    auto trueType = narrowType(Type::anyObject(), expr.get(), true);
    auto falseType = narrowType(Type::anyObject(), expr.get(), false);
    ASSERT_EQ(trueType.toDebugString(), "{a: number(S), ...}");
    ASSERT_EQ(falseType.toDebugString(), "object");
}

TEST(ExpressionTypingTest, EqualityNarrowingExcludesNullWhenFalse) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto expr = parseExpr(fromjson("{$eq: ['$a', null]}"), *expCtx);
    auto falseType = narrowType(Type::anyObject(), expr.get(), false);
    ASSERT_EQ(falseType.toDebugString(), "{a: ~null, ...}");
}

TEST(ExpressionTypingTest, NegatedEqualityNarrowsOnlyForSingletonOperand) {
    auto input =
        openObject({{"a", allValues(BSONType::null)},
                    {"b", unionType(allValues(BSONType::null), allValues(BSONType::numberInt))}});
    for (const auto& expr : {"{$eq: ['$a', '$b']}", "{$eq: ['$b', '$a']}"}) {
        ASSERT_EQ(narrowedDebugString(expr, input, false), "{a: null, b: int, ...}") << expr;
    }
    auto mixed =
        openObject({{"a", unionType(allValues(BSONType::null), allValues(BSONType::numberInt))},
                    {"b", unionType(allValues(BSONType::null), allValues(BSONType::numberInt))}});
    ASSERT_EQ(narrowedDebugString("{$eq: ['$a', '$b']}", mixed, false),
              "{a: null|int, b: null|int, ...}");
    ASSERT_EQ(narrowedDebugString("{$eq: ['$a', 5]}", Type::anyObject(), false), "object");
    ASSERT_EQ(narrowedDebugString("{$eq: ['$a', 'lalala']}", Type::anyObject(), false), "object");
    ASSERT_EQ(narrowedDebugString("{$eq: ['$a', {$or: [true]}]}", Type::anyObject(), false),
              "object");
}

TEST(ExpressionTypingTest, EqualityNarrowingDoesNotEvaluateArrayOrObjectOperands) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto assertType = [&](const std::string& spec, BSONType bsonType) {
        auto expr = parseExpr(fromjson(spec), *expCtx);
        const auto trueType = narrowType(Type::anyObject(), expr.get(), true);
        const auto falseType = narrowType(Type::anyObject(), expr.get(), false);
        ASSERT_EQ(trueType.getField("a").toDebugString(),
                  Type(bsonType, Extent::kSubset).toDebugString())
            << spec;
        ASSERT_TRUE(falseType.getField("a").hasType(bsonType))
            << spec << ", inferred " << falseType.getField("a").toDebugString();
    };
    assertType("{$eq: ['$a', ['$b', {$divide: [1, 0]}]]}", BSONType::array);
    assertType("{$eq: [['$b', {$divide: [1, 0]}], '$a']}", BSONType::array);
    assertType("{$eq: ['$a', {x: '$b', y: {$divide: [1, 0]}}]}", BSONType::object);
    assertType("{$eq: [{x: '$b', y: {$divide: [1, 0]}}, '$a']}", BSONType::object);
}

TEST(ExpressionTypingTest, EqualityNarrowingLeavesUnsupportedOperandsUnchanged) {
    auto input = openObject({{"a", allValues(BSONType::string)}});
    for (const auto& expr : {"{$eq: ['$$ROOT', 1]}",
                             "{$eq: ['$$NOW', 1]}",
                             "{$eq: [1, 1]}",
                             "{$eq: ['$a', {$add: [1, 1]}]}",
                             "{$eq: ['$a', '$$NOW']}",
                             "{$eq: [{$add: ['$a', 1]}, 1]}",
                             "{$gt: ['$a', 1]}"}) {
        for (bool assumeTrue : {false, true}) {
            ASSERT_EQ(narrowedDebugString(expr, input, assumeTrue), input.toDebugString()) << expr;
        }
    }
}

/// Returns the type the field path expression evaluates to.
std::string fieldPathOutputDebugString(const std::string& rawPath,
                                       Type inputType = Type::anyObject()) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto expr = ExpressionFieldPath::parse(expCtx.get(), rawPath, expCtx->variablesParseState);
    return getOutputType(std::move(inputType), expr.get()).toDebugString();
}

TEST(ExpressionTypingTest, FieldPathOutputTypeReadsTheField) {
    auto input = openObject({{"a", allValues(BSONType::numberInt)}});
    ASSERT_EQ(fieldPathOutputDebugString("$a", input), "int");
    ASSERT_EQ(fieldPathOutputDebugString("$a.b", input), "missing");
    ASSERT_EQ(fieldPathOutputDebugString("$b", input), "any");
}

TEST(ExpressionTypingTest, FieldPathOutputTypeOfRoot) {
    auto input = openObject({{"a", allValues(BSONType::numberInt)}});
    ASSERT_EQ(fieldPathOutputDebugString("$$ROOT", input), input.toDebugString());
    ASSERT_EQ(fieldPathOutputDebugString("$$CURRENT", input), input.toDebugString());
    ASSERT_EQ(fieldPathOutputDebugString("$$ROOT.a", input), "int");
}

TEST(ExpressionTypingTest, FieldPathOutputTypeOfVariableIsAny) {
    ASSERT_EQ(fieldPathOutputDebugString("$$NOW"), "any");
    ASSERT_EQ(fieldPathOutputDebugString("$$NOW.x"), "any");
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
