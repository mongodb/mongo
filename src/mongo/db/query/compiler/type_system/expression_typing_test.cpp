// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/type_system/expression_typing.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/type_system/type_system_test_util.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <string>
#include <tuple>
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

TEST(ExpressionTypingTest, LogicalOutputTypeIsBoolean) {
    ASSERT_EQ(outputTypeDebugString("{$and: []}"), "bool(S)");
    ASSERT_EQ(outputTypeDebugString("{$or: []}"), "bool(S)");
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
    const auto values = extendedRepresentatives();

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

TEST(ExpressionTypingTest, InequalityNarrowingLeavesUntypedOperandsUnchanged) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const Type anyObject = Type::anyObject();
    for (const auto& spec :
         {"{$ne: ['$a', '$b']}", "{$ne: ['$a', '$$NOW']}", "{$ne: ['$a', {$add: [1, 1]}]}"}) {
        auto expr = parseExpr(fromjson(spec), *expCtx);
        for (bool assumeTrue : {false, true}) {
            ASSERT_EQ(narrowType(anyObject, expr.get(), assumeTrue).toDebugString(), "object")
                << spec << ", assumption " << assumeTrue;
        }
    }
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

TEST(ExpressionTypingTest, OrderedComparisonNarrowingUsesCanonicalTypeOrder) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto path =
        ExpressionFieldPath::createPathFromString(expCtx.get(), "a", expCtx->variablesParseState);
    auto one = ExpressionConstant::create(expCtx.get(), Value(1));

    const Type beforeNumbers =
        unionType(unionType(allValues(BSONType::minKey), Type::missing()),
                  unionType(allValues(BSONType::undefined), allValues(BSONType::null)));
    const Type withinNumbers = numbers(Extent::kSubset);
    const Type beforeOrWithinNumbers = unionType(beforeNumbers, withinNumbers);
    const Type afterNumbers =
        unionType(withinNumbers, complement(unionType(beforeNumbers, numbers())));

    for (auto op : {ExpressionCompare::LT, ExpressionCompare::LTE}) {
        for (bool reverse : {false, true}) {
            auto expr = reverse ? ExpressionCompare::create(expCtx.get(), op, one, path)
                                : ExpressionCompare::create(expCtx.get(), op, path, one);
            const Type expectedWhenTrue = reverse ? afterNumbers : beforeOrWithinNumbers;
            const Type expectedWhenFalse = reverse ? beforeOrWithinNumbers : afterNumbers;
            ASSERT_EQ(narrowType(Type::anyObject(), expr.get(), true).getField("a").toDebugString(),
                      expectedWhenTrue.toDebugString())
                << "op " << op << ", reverse " << reverse << ", assuming true";
            ASSERT_EQ(
                narrowType(Type::anyObject(), expr.get(), false).getField("a").toDebugString(),
                expectedWhenFalse.toDebugString())
                << "op " << op << ", reverse " << reverse << ", assuming false";
        }
    }
}

TEST(ExpressionTypingTest, OrderedComparisonNarrowingIntersectsWithExistingFieldType) {
    const Type stringInput = openObject({{"a", allValues(BSONType::string)}});
    for (const auto& op : {"$lt", "$lte"}) {
        ASSERT_EQ(narrowedDebugString("{" + std::string(op) + ": ['$a', 1]}", stringInput), "never")
            << op;
        ASSERT_EQ(narrowedDebugString("{" + std::string(op) + ": ['$a', 1]}", stringInput, false),
                  stringInput.toDebugString())
            << op;
        ASSERT_EQ(narrowedDebugString("{" + std::string(op) + ": [1, '$a']}", stringInput),
                  stringInput.toDebugString())
            << op;
        ASSERT_EQ(narrowedDebugString("{" + std::string(op) + ": [1, '$a']}", stringInput, false),
                  "never")
            << op;
    }
}

TEST(ExpressionTypingTest, OrderedComparisonNarrowingDottedPaths) {
    const Type input = openObject({{"a", openObject({{"b", allValues(BSONType::string)}})}});
    for (const auto& op : {"$lt", "$lte"}) {
        const auto fieldFirst = "{" + std::string(op) + ": ['$a.b', 1]}";
        const auto constantFirst = "{" + std::string(op) + ": [1, '$$ROOT.a.b']}";
        ASSERT_EQ(narrowedDebugString(fieldFirst, input), "never") << op;
        ASSERT_EQ(narrowedDebugString(fieldFirst, input, false), input.toDebugString()) << op;
        ASSERT_EQ(narrowedDebugString(constantFirst, input), input.toDebugString()) << op;
        ASSERT_EQ(narrowedDebugString(constantFirst, input, false), "never") << op;
    }
}

TEST(ExpressionTypingTest, OrderedComparisonNarrowingRespectsInclusiveBounds) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto path =
        ExpressionFieldPath::createPathFromString(expCtx.get(), "a", expCtx->variablesParseState);
    const std::vector<Value> candidates{
        Value(), Value(MINKEY), Value(BSONNULL), Value(0), Value(1), Value("x"sv), Value(MAXKEY)};
    const std::vector<std::pair<ExpressionCompare::CmpOp, Value>> cases{
        {ExpressionCompare::LT, Value(MINKEY)},
        {ExpressionCompare::LTE, Value(MINKEY)},
        {ExpressionCompare::LT, Value(MAXKEY)},
        {ExpressionCompare::LTE, Value(MAXKEY)}};

    for (const auto& [op, bound] : cases) {
        auto constant = ExpressionConstant::create(expCtx.get(), bound);
        auto expr = ExpressionCompare::create(expCtx.get(), op, path, constant);
        const auto narrowedTrue = narrowType(Type::anyObject(), expr.get(), true);
        const auto narrowedFalse = narrowType(Type::anyObject(), expr.get(), false);
        for (const auto& candidate : candidates) {
            Document doc = candidate.missing() ? Document{} : Document{{"a", candidate}};
            const bool result = expr->evaluate(doc, &expCtx->variables, {}).getBool();
            ASSERT_TRUE((result ? narrowedTrue : narrowedFalse).hasValue(Value(doc)))
                << "op " << op << ", bound " << bound.toString() << ", candidate "
                << candidate.toString() << ", result " << result << ", type "
                << (result ? narrowedTrue : narrowedFalse).toDebugString();
        }
    }

    auto lessThanMin =
        ExpressionCompare::create(expCtx.get(),
                                  ExpressionCompare::LT,
                                  path,
                                  ExpressionConstant::create(expCtx.get(), Value(MINKEY)));
    ASSERT_EQ(narrowType(Type::anyObject(), lessThanMin.get(), true).toDebugString(), "never");

    auto atMostMin =
        ExpressionCompare::create(expCtx.get(),
                                  ExpressionCompare::LTE,
                                  path,
                                  ExpressionConstant::create(expCtx.get(), Value(MINKEY)));
    ASSERT_EQ(narrowType(Type::anyObject(), atMostMin.get(), true).toDebugString(),
              "{a: minKey, ...}");
    ASSERT_EQ(narrowType(Type::anyObject(), atMostMin.get(), false).toDebugString(),
              "{a: ~minKey, ...}");

    auto lessThanMax =
        ExpressionCompare::create(expCtx.get(),
                                  ExpressionCompare::LT,
                                  path,
                                  ExpressionConstant::create(expCtx.get(), Value(MAXKEY)));
    ASSERT_EQ(narrowType(Type::anyObject(), lessThanMax.get(), true).toDebugString(),
              "{a: ~maxKey, ...}");
    ASSERT_EQ(narrowType(Type::anyObject(), lessThanMax.get(), false).toDebugString(),
              "{a: maxKey, ...}");

    auto atMostMax =
        ExpressionCompare::create(expCtx.get(),
                                  ExpressionCompare::LTE,
                                  path,
                                  ExpressionConstant::create(expCtx.get(), Value(MAXKEY)));
    ASSERT_EQ(narrowType(Type::anyObject(), atMostMax.get(), true).toDebugString(), "object");
    ASSERT_EQ(narrowType(Type::anyObject(), atMostMax.get(), false).toDebugString(), "never");
}

TEST(ExpressionTypingTest, OrderedComparisonNarrowingPreservesEvaluationResults) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto values = extendedRepresentatives();
    values.push_back(Value(2));
    auto path =
        ExpressionFieldPath::createPathFromString(expCtx.get(), "a", expCtx->variablesParseState);
    for (auto op : {ExpressionCompare::LT, ExpressionCompare::LTE}) {
        for (const auto& constant : values) {
            auto literal = ExpressionConstant::create(expCtx.get(), constant);
            for (bool reverse : {false, true}) {
                auto expr = reverse ? ExpressionCompare::create(expCtx.get(), op, literal, path)
                                    : ExpressionCompare::create(expCtx.get(), op, path, literal);
                const auto narrowedTrue = narrowType(Type::anyObject(), expr.get(), true);
                const auto narrowedFalse = narrowType(Type::anyObject(), expr.get(), false);
                if (constant.getType() == BSONType::null) {
                    ASSERT_NE(narrowedTrue.toDebugString(), "object")
                        << "op " << op << ", reverse " << reverse;
                    ASSERT_NE(narrowedFalse.toDebugString(), "object")
                        << "op " << op << ", reverse " << reverse;
                }
                for (const auto& candidate : values) {
                    Document doc{{"a", candidate}};
                    const bool result = expr->evaluate(doc, &expCtx->variables, {}).getBool();
                    ASSERT_TRUE((result ? narrowedTrue : narrowedFalse).hasValue(Value(doc)))
                        << "op " << op << ", constant " << constant.toString() << ", candidate "
                        << candidate.toString() << ", reverse " << reverse << ", result " << result
                        << ", type " << (result ? narrowedTrue : narrowedFalse).toDebugString();
                }
            }
        }
    }
}

TEST(ExpressionTypingTest, OrderedComparisonNarrowingPreservesUnionBoundResults) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const Type boundType = unionType(
        Type::null(), unionType(allValues(BSONType::string), allValues(BSONType::boolean)));
    const Type input = openObject(
        {{"a", unionType(unionType(numbers(), Type::anyArray()), boundType)}, {"b", boundType}});
    const std::vector<Value> candidates{Value(BSONNULL),
                                        Value(false),
                                        Value(true),
                                        Value("x"sv),
                                        Value(1),
                                        Value(std::vector<Value>{}),
                                        Value(std::vector<Value>{Value(1)})};
    const std::vector<Value> bounds{Value(BSONNULL), Value("x"sv), Value(false), Value(true)};
    for (const auto& spec : {"{$lt: ['$a', '$b']}",
                             "{$lte: ['$a', '$b']}",
                             "{$lt: ['$b', '$a']}",
                             "{$lte: ['$b', '$a']}",
                             "{$not: [{$lt: ['$a', '$b']}]}",
                             "{$not: [{$lte: ['$a', '$b']}]}",
                             "{$not: [{$lt: ['$b', '$a']}]}",
                             "{$not: [{$lte: ['$b', '$a']}]}"}) {
        auto expr = parseExpr(fromjson(spec), *expCtx);
        const auto narrowedTrue = narrowType(input, expr.get(), true);
        const auto narrowedFalse = narrowType(input, expr.get(), false);
        for (const auto& candidate : candidates) {
            for (const auto& bound : bounds) {
                const Document doc{{"a", candidate}, {"b", bound}};
                const bool result = expr->evaluate(doc, &expCtx->variables, {}).getBool();
                ASSERT_TRUE((result ? narrowedTrue : narrowedFalse).hasValue(Value(doc)))
                    << spec << ", document " << doc.toString() << ", result " << result << ", type "
                    << (result ? narrowedTrue : narrowedFalse).toDebugString();
            }
        }
    }
}

TEST(ExpressionTypingTest, OrderedComparisonNarrowingPreservesFalseUnionBoundResults) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const Type input =
        openObject({{"a", numbers()}, {"b", unionType(Type::null(), allValues(BSONType::string))}});
    const Document numberLessThanNull{{"a", Value(1)}, {"b", Value(BSONNULL)}};
    const Document stringGreaterThanNumber{{"a", Value(1)}, {"b", Value("x"sv)}};

    for (const auto& [spec, nullResult, stringResult] :
         {std::tuple{"{$lt: ['$a', '$b']}", false, true},
          std::tuple{"{$lte: ['$a', '$b']}", false, true},
          std::tuple{"{$lt: ['$b', '$a']}", true, false},
          std::tuple{"{$lte: ['$b', '$a']}", true, false}}) {
        auto expr = parseExpr(fromjson(spec), *expCtx);
        const auto narrowedTrue = narrowType(input, expr.get(), true);
        const auto narrowedFalse = narrowType(input, expr.get(), false);
        for (const auto& [doc, expected] : {std::pair{numberLessThanNull, nullResult},
                                            std::pair{stringGreaterThanNumber, stringResult}}) {
            const bool result = expr->evaluate(doc, &expCtx->variables, {}).getBool();
            ASSERT_EQ(result, expected) << spec << ", document " << doc.toString();
            ASSERT_TRUE((result ? narrowedTrue : narrowedFalse).hasValue(Value(doc)))
                << spec << ", document " << doc.toString() << ", result " << result << ", type "
                << (result ? narrowedTrue : narrowedFalse).toDebugString();
        }
    }
}

TEST(ExpressionTypingTest, LessThanNarrowingHandlesBoundsAndTypedOperands) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto path =
        ExpressionFieldPath::createPathFromString(expCtx.get(), "a", expCtx->variablesParseState);
    auto min = ExpressionConstant::create(expCtx.get(), Value(MINKEY));
    auto max = ExpressionConstant::create(expCtx.get(), Value(MAXKEY));
    auto lessThanMin = ExpressionCompare::create(expCtx.get(), ExpressionCompare::LT, path, min);
    auto lessThanMax = ExpressionCompare::create(expCtx.get(), ExpressionCompare::LT, path, max);
    auto maxLessThan = ExpressionCompare::create(expCtx.get(), ExpressionCompare::LT, max, path);
    ASSERT_EQ(narrowType(Type::anyObject(), lessThanMin.get(), true).toDebugString(), "never");
    ASSERT_EQ(narrowType(Type::anyObject(), lessThanMin.get(), false).toDebugString(), "object");
    ASSERT_EQ(narrowType(Type::anyObject(), lessThanMax.get(), true).toDebugString(),
              "{a: ~maxKey, ...}");
    ASSERT_EQ(narrowType(Type::anyObject(), lessThanMax.get(), false).toDebugString(),
              "{a: maxKey, ...}");
    ASSERT_EQ(narrowType(Type::anyObject(), maxLessThan.get(), true).toDebugString(), "never");
    ASSERT_EQ(narrowType(Type::anyObject(), maxLessThan.get(), false).toDebugString(), "object");

    const Type input = openObject(
        {{"a", Type::anyObject()}, {"b", unionType(Type::null(), allValues(BSONType::string))}});
    ASSERT_EQ(narrowedDebugString("{$lt: ['$a', '$b']}", input), "never");
    ASSERT_EQ(narrowedDebugString("{$lt: ['$b', '$a']}", input, false), "never");
}

TEST(ExpressionTypingTest, LessThanNarrowingPreservesEvaluationResults) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto values = extendedRepresentatives();
    values.insert(values.end(), {Value(), Value(2), Value(MAXKEY)});
    auto path =
        ExpressionFieldPath::createPathFromString(expCtx.get(), "a", expCtx->variablesParseState);
    for (const auto& bound : values) {
        auto literal = ExpressionConstant::create(expCtx.get(), bound);
        for (bool reverse : {false, true}) {
            auto expr = reverse
                ? ExpressionCompare::create(expCtx.get(), ExpressionCompare::LT, literal, path)
                : ExpressionCompare::create(expCtx.get(), ExpressionCompare::LT, path, literal);
            const auto narrowedTrue = narrowType(Type::anyObject(), expr.get(), true);
            const auto narrowedFalse = narrowType(Type::anyObject(), expr.get(), false);
            for (const auto& value : values) {
                Document doc{{"a", value}};
                const bool result = expr->evaluate(doc, &expCtx->variables, {}).getBool();
                const auto& narrowed = result ? narrowedTrue : narrowedFalse;
                ASSERT_TRUE(narrowed.hasValue(Value(doc)))
                    << "bound " << bound.toString() << ", value " << value.toString()
                    << ", reverse " << reverse << ", result " << result << ", type "
                    << narrowed.toDebugString();
            }
        }
    }
}

TEST(ExpressionTypingTest, LessThanNarrowingPreservesDottedAndLogicalEvaluationResults) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const auto corpus = generateDocumentCorpus("a.b");
    for (const auto& spec : {"{$lt: ['$a.b', 1]}",
                             "{$lt: [null, '$a.b']}",
                             "{$not: [{$lt: ['$a.b', 1]}]}",
                             "{$and: [{$lt: ['$a.b', 1]}, {$lt: [null, '$a.b']}]}"}) {
        auto expr = parseExpr(fromjson(spec), *expCtx);
        const auto narrowedTrue = narrowType(Type::anyObject(), expr.get(), true);
        const auto narrowedFalse = narrowType(Type::anyObject(), expr.get(), false);
        ASSERT_TRUE(narrowedTrue.toDebugString() != "object" ||
                    narrowedFalse.toDebugString() != "object")
            << spec;
        for (const auto& bson : corpus) {
            Document doc(bson);
            const bool result = expr->evaluate(doc, &expCtx->variables, {}).coerceToBool();
            const auto& narrowed = result ? narrowedTrue : narrowedFalse;
            ASSERT_TRUE(narrowed.hasValue(Value(doc)))
                << spec << ", document " << doc.toString() << ", result " << result << ", type "
                << narrowed.toDebugString();
        }
    }
}

TEST(ExpressionTypingTest, LogicalEmptyChild) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const Type anyObject = Type::anyObject();
    auto emptyAnd = parseExpr(fromjson("{$and: []}"), *expCtx);
    auto emptyOr = parseExpr(fromjson("{$or: []}"), *expCtx);
    ASSERT_EQ(narrowType(anyObject, emptyAnd.get(), true).toDebugString(), "object");
    ASSERT_EQ(narrowType(anyObject, emptyAnd.get(), false).toDebugString(), "never");
    ASSERT_EQ(narrowType(anyObject, emptyOr.get(), true).toDebugString(), "never");
    ASSERT_EQ(narrowType(anyObject, emptyOr.get(), false).toDebugString(), "object");
}

TEST(ExpressionTypingTest, LogicalSingleChild) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const Type anyObject = Type::anyObject();
    auto child = parseExpr(fromjson("{$eq: ['$a', 1]}"), *expCtx);
    auto andExpr = parseExpr(fromjson("{$and: [{$eq: ['$a', 1]}]}"), *expCtx);
    auto orExpr = parseExpr(fromjson("{$or: [{$eq: ['$a', 1]}]}"), *expCtx);
    for (bool assumption : {false, true}) {
        ASSERT_EQ(narrowType(anyObject, andExpr.get(), assumption).toDebugString(),
                  narrowType(anyObject, child.get(), assumption).toDebugString());
        ASSERT_EQ(narrowType(anyObject, orExpr.get(), assumption).toDebugString(),
                  narrowType(anyObject, child.get(), assumption).toDebugString());
    }
}

TEST(ExpressionTypingTest, LogicalNarrowingCombinesChildrenAnd) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const Type anyObject = Type::anyObject();
    auto a = parseExpr(fromjson("{$eq: ['$a', 1]}"), *expCtx);
    auto b = parseExpr(fromjson("{$eq: ['x', '$b']}"), *expCtx);
    auto andExpr = parseExpr(fromjson("{$and: [{$eq: ['$a', 1]}, {$eq: ['x', '$b']}]}"), *expCtx);
    const auto andTrue = narrowType(anyObject, andExpr.get(), true);
    ASSERT_EQ(andTrue.getField("a").toDebugString(),
              Type(sameCanonicalClass(BSONType::numberInt)).toDebugString());
    ASSERT_EQ(andTrue.getField("b").toDebugString(),
              Type(sameCanonicalClass(BSONType::string)).toDebugString());
    ASSERT_EQ(
        narrowType(anyObject, andExpr.get(), false).toDebugString(),
        unionType(narrowType(anyObject, a.get(), false), narrowType(anyObject, b.get(), false))
            .toDebugString());

    auto incompatible =
        parseExpr(fromjson("{$and: [{$eq: ['$a', 1]}, {$eq: ['$a', 'x']}]}"), *expCtx);
    ASSERT_EQ(narrowType(anyObject, incompatible.get(), true).toDebugString(), "never");
}

TEST(ExpressionTypingTest, LogicalNarrowingCombinesChildrenOr) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const Type anyObject = Type::anyObject();
    auto a = parseExpr(fromjson("{$eq: ['$a', 1]}"), *expCtx);
    auto b = parseExpr(fromjson("{$eq: ['x', '$b']}"), *expCtx);
    auto orExpr = parseExpr(fromjson("{$or: [{$eq: ['$a', 1]}, {$eq: ['x', '$b']}]}"), *expCtx);
    const auto orTrue = narrowType(anyObject, orExpr.get(), true);
    ASSERT_EQ(orTrue.toDebugString(),
              unionType(narrowType(anyObject, a.get(), true), narrowType(anyObject, b.get(), true))
                  .toDebugString());
    ASSERT_EQ(narrowType(anyObject, orExpr.get(), false).toDebugString(),
              narrowType(narrowType(anyObject, a.get(), false), b.get(), false).toDebugString());

    auto sameField = parseExpr(fromjson("{$or: [{$eq: ['$a', 1]}, {$eq: ['$a', 'x']}]}"), *expCtx);
    ASSERT_EQ(narrowType(anyObject, sameField.get(), true).getField("a").toDebugString(),
              unionType(Type(sameCanonicalClass(BSONType::numberInt)),
                        Type(sameCanonicalClass(BSONType::string)))
                  .toDebugString());
}

TEST(ExpressionTypingTest, LogicalNarrowingHonorsConstantOperands) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const Type anyObject = Type::anyObject();
    auto falseAnd = parseExpr(fromjson("{$and: [false, {$eq: ['$a', 1]}]}"), *expCtx);
    auto trueOr = parseExpr(fromjson("{$or: [true, {$eq: ['$a', 1]}]}"), *expCtx);
    auto equality = parseExpr(fromjson("{$eq: ['$a', 1]}"), *expCtx);
    auto trueAnd = parseExpr(fromjson("{$and: [true, {$eq: ['$a', 1]}]}"), *expCtx);
    auto falseOr = parseExpr(fromjson("{$or: [false, {$eq: ['$a', 1]}]}"), *expCtx);

    ASSERT_EQ(narrowType(anyObject, falseAnd.get(), true).toDebugString(), "never");
    ASSERT_EQ(narrowType(anyObject, falseAnd.get(), false).toDebugString(), "object");
    ASSERT_EQ(narrowType(anyObject, trueOr.get(), true).toDebugString(), "object");
    ASSERT_EQ(narrowType(anyObject, trueOr.get(), false).toDebugString(), "never");
    for (bool assumption : {false, true}) {
        const auto expected = narrowType(anyObject, equality.get(), assumption).toDebugString();
        ASSERT_EQ(narrowType(anyObject, trueAnd.get(), assumption).toDebugString(), expected);
        ASSERT_EQ(narrowType(anyObject, falseOr.get(), assumption).toDebugString(), expected);
    }
}

TEST(ExpressionTypingTest, LogicalNarrowingUsesConstantTruthiness) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const Type anyObject = Type::anyObject();
    auto equality = parseExpr(fromjson("{$eq: ['$a', 1]}"), *expCtx);
    auto truthyAnd = parseExpr(fromjson("{$and: ['x', {$eq: ['$a', 1]}]}"), *expCtx);
    auto falsyOr = parseExpr(fromjson("{$or: [0, {$eq: ['$a', 1]}]}"), *expCtx);

    for (bool assumption : {false, true}) {
        const auto expected = narrowType(anyObject, equality.get(), assumption).toDebugString();
        ASSERT_EQ(narrowType(anyObject, truthyAnd.get(), assumption).toDebugString(), expected);
        ASSERT_EQ(narrowType(anyObject, falsyOr.get(), assumption).toDebugString(), expected);
    }
}

TEST(ExpressionTypingTest, LogicalNarrowingExpectedTypes) {
    for (const auto& [spec, expectedTrue, expectedFalse] : {
             std::tuple{"{$and: [{$eq: ['$a', 1]}, {$eq: ['x', '$b']}]}",
                        "{a: number(S), b: string(S)|symbol(S), ...}",
                        "object"},
             std::tuple{"{$or: [{$eq: [1, '$a']}, {$eq: ['$b', 'x']}]}", "object(S)", "object"},
             std::tuple{"{$not: [{$and: [{$eq: ['$a', 1]}, {$eq: ['$b', 'x']}]}]}",
                        "object",
                        "{a: number(S), b: string(S)|symbol(S), ...}"},
             std::tuple{"{$and: [{$or: [false, {$eq: ['$a', 1]}]}, "
                        "{$not: [{$eq: ['x', '$b']}]}]}",
                        "{a: number(S), ...}",
                        "object"},
             std::tuple{"{$or: [{$add: [1, 2]}, {$eq: ['$a', 1]}]}", "object", "object"},
             std::tuple{"{$or: [{$eq: ['$a', null]}, {$eq: ['$b', null]}]}",
                        "object(S)",
                        "{a: ~null, b: ~null, ...}"},
             std::tuple{"{$not: [{$or: [{$eq: ['$a', null]}, {$eq: ['$b', null]}]}]}",
                        "{a: ~null, b: ~null, ...}",
                        "object(S)"},
         }) {
        ASSERT_EQ(narrowedDebugString(spec, Type::anyObject(), true), expectedTrue) << spec;
        ASSERT_EQ(narrowedDebugString(spec, Type::anyObject(), false), expectedFalse) << spec;
    }
}

TEST(ExpressionTypingTest, InequalityNarrowingIsNegatedEquality) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    for (const auto& operands : {"['$a', 1]",
                                 "[1, '$a']",
                                 "['$a', null]",
                                 "[null, '$a']",
                                 "['$a', '$b']",
                                 "['$b', '$a']",
                                 "['$a', [1]]",
                                 "[{x: 1}, '$a']",
                                 "['$a', {$add: [1, 1]}]"}) {
        auto eq = parseExpr(fromjson(std::string("{$eq: ") + operands + "}"), *expCtx);
        auto ne = parseExpr(fromjson(std::string("{$ne: ") + operands + "}"), *expCtx);
        for (bool assumeTrue : {false, true}) {
            ASSERT_EQ(narrowType(Type::anyObject(), ne.get(), assumeTrue).toDebugString(),
                      narrowType(Type::anyObject(), eq.get(), !assumeTrue).toDebugString())
                << operands << ", assumption " << assumeTrue;
        }
    }

    auto number = parseExpr(fromjson("{$ne: ['$a', 1]}"), *expCtx);
    auto unequalNumbers = narrowType(Type::anyObject(), number.get(), true);
    ASSERT_EQ(unequalNumbers.toDebugString(), "object");
    auto equalNumbers = narrowType(Type::anyObject(), number.get(), false);
    ASSERT_EQ(equalNumbers.toDebugString(), "{a: number(S), ...}");

    auto null = parseExpr(fromjson("{$ne: ['$a', null]}"), *expCtx);
    auto notNull = narrowType(Type::anyObject(), null.get(), true);
    ASSERT_EQ(notNull.toDebugString(), "{a: ~null, ...}");
}

TEST(ExpressionTypingTest, InequalityNarrowingExpectedTypes) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    struct TestCase {
        Value constant;
        const char* ifEqual;
        const char* ifNotEqual;
    };
    const std::vector<TestCase> cases{
        {Value(), "missing|undefined", "~(missing|undefined)"},
        {Value(BSONUndefined), "missing|undefined", "~(missing|undefined)"},
        {Value(BSONNULL), "null", "~null"},
        {Value(MINKEY), "minKey", "~minKey"},
        {Value(MAXKEY), "maxKey", "~maxKey"},
        {Value(0), "number(S)", "any"},
        {Value(1), "number(S)", "any"},
        {Value(2), "number(S)", "any"},
        {Value(1LL), "number(S)", "any"},
        {Value(1.0), "number(S)", "any"},
        {Value(Decimal128(1)), "number(S)", "any"},
        {Value("str"sv), "string(S)|symbol(S)", "any"},
        {Value(BSONSymbol("sym")), "string(S)|symbol(S)", "any"},
        {Value(BSONBinData("", 0, BinDataGeneral)), "binData(S)", "any"},
        {Value(OID("0123456789abcdef01234567")), "objectId(S)", "any"},
        {Value(Date_t()), "date(S)", "any"},
        {Value(Timestamp(1, 1)), "timestamp(S)", "any"},
        {Value(BSONRegEx("a")), "regex(S)", "any"},
        {Value(BSONDBRef("db.coll", OID("0123456789abcdef01234567"))), "dbPointer(S)", "any"},
        {Value(BSONCode("f()")), "javascript(S)", "any"},
        {Value(BSONCodeWScope("f()", BSON("a" << 1))), "javascriptWithScope(S)", "any"},
        {Value(false), "bool(S)", "any"},
        {Value(true), "bool(S)", "any"},
        {Value(BSONObj()), "object(S)", "any"},
        {Value(BSON("x" << 1)), "object(S)", "any"},
        {Value(std::vector<Value>{}), "array(S)", "any"},
        {Value(std::vector<Value>{Value(1)}), "array(S)", "any"},
    };
    auto path =
        ExpressionFieldPath::createPathFromString(expCtx.get(), "a", expCtx->variablesParseState);
    for (const auto& [constant, ifEqual, ifNotEqual] : cases) {
        auto literal = ExpressionConstant::create(expCtx.get(), constant);
        for (bool reverse : {false, true}) {
            auto expr = reverse
                ? ExpressionCompare::create(expCtx.get(), ExpressionCompare::NE, literal, path)
                : ExpressionCompare::create(expCtx.get(), ExpressionCompare::NE, path, literal);
            ASSERT_EQ(
                narrowType(Type::anyObject(), expr.get(), false).getField("a").toDebugString(),
                ifEqual)
                << "constant " << constant.toString() << ", reverse " << reverse;
            ASSERT_EQ(narrowType(Type::anyObject(), expr.get(), true).getField("a").toDebugString(),
                      ifNotEqual)
                << "constant " << constant.toString() << ", reverse " << reverse;
        }
    }
}

TEST(ExpressionTypingTest, InequalityNarrowingDottedPaths) {
    for (const auto& [spec, expectedTrue, expectedFalse] : {
             std::tuple{"{$ne: ['$a.b', 1]}", "object", "{a: {b: number(S), ...}, ...}"},
             std::tuple{"{$ne: [1, '$a.b']}", "object", "{a: {b: number(S), ...}, ...}"},
             std::tuple{"{$ne: ['$a.b', null]}",
                        "{a: ~object|{b: ~null, ...}, ...}",
                        "{a: {b: null, ...}, ...}"},
             std::tuple{"{$ne: [null, '$a.b']}",
                        "{a: ~object|{b: ~null, ...}, ...}",
                        "{a: {b: null, ...}, ...}"},
             std::tuple{"{$ne: ['$$ROOT.a.b', 1]}", "object", "{a: {b: number(S), ...}, ...}"},
             std::tuple{"{$ne: [null, '$$CURRENT.a.b']}",
                        "{a: ~object|{b: ~null, ...}, ...}",
                        "{a: {b: null, ...}, ...}"},
         }) {
        ASSERT_EQ(narrowedDebugString(spec, Type::anyObject(), true), expectedTrue) << spec;
        ASSERT_EQ(narrowedDebugString(spec, Type::anyObject(), false), expectedFalse) << spec;
    }
}

TEST(ExpressionTypingTest, InequalityNarrowingIntersectsWithExistingFieldType) {
    for (const auto& [spec, input, expectedTrue, expectedFalse] : {
             std::tuple{"{$ne: ['$a', 1]}",
                        openObject({{"a", allValues(BSONType::string)}}),
                        "{a: string, ...}",
                        "never"},
             std::tuple{"{$ne: [1, '$a']}",
                        openObject({{"a", allValues(BSONType::numberDouble)}}),
                        "{a: double, ...}",
                        "{a: double(S), ...}"},
             std::tuple{"{$ne: ['$a', null]}",
                        openObject({{"a", allValues(BSONType::null)}}),
                        "never",
                        "{a: null, ...}"},
             std::tuple{
                 "{$ne: [null, '$a']}",
                 openObject(
                     {{"a", unionType(allValues(BSONType::null), allValues(BSONType::numberInt))}}),
                 "{a: int, ...}",
                 "{a: null, ...}"},
         }) {
        ASSERT_EQ(narrowedDebugString(spec, input, true), expectedTrue) << spec;
        ASSERT_EQ(narrowedDebugString(spec, input, false), expectedFalse) << spec;
    }
}

TEST(ExpressionTypingTest, InequalityNarrowingUsesSingletonFieldOperand) {
    const auto input =
        openObject({{"a", allValues(BSONType::null)},
                    {"b", unionType(allValues(BSONType::null), allValues(BSONType::numberInt))}});
    for (const auto& spec : {"{$ne: ['$a', '$b']}", "{$ne: ['$b', '$a']}"}) {
        ASSERT_EQ(narrowedDebugString(spec, input, true), "{a: null, b: int, ...}") << spec;
    }

    const auto mixed =
        openObject({{"a", unionType(allValues(BSONType::null), allValues(BSONType::numberInt))},
                    {"b", unionType(allValues(BSONType::null), allValues(BSONType::numberInt))}});
    for (const auto& spec : {"{$ne: ['$a', '$b']}", "{$ne: ['$b', '$a']}"}) {
        ASSERT_EQ(narrowedDebugString(spec, mixed, true), "{a: null|int, b: null|int, ...}")
            << spec;
    }
}

TEST(ExpressionTypingTest, InequalityNarrowingDoesNotEvaluateArrayOrObjectOperands) {
    for (const auto& [spec, expectedFalse] : {
             std::pair{"{$ne: ['$a', ['$b', {$divide: [1, 0]}]]}", "{a: array(S), ...}"},
             std::pair{"{$ne: [['$b', {$divide: [1, 0]}], '$a']}", "{a: array(S), ...}"},
             std::pair{"{$ne: ['$a', {x: '$b', y: {$divide: [1, 0]}}]}", "{a: object(S), ...}"},
             std::pair{"{$ne: [{x: '$b', y: {$divide: [1, 0]}}, '$a']}", "{a: object(S), ...}"},
         }) {
        ASSERT_EQ(narrowedDebugString(spec, Type::anyObject(), true), "object") << spec;
        ASSERT_EQ(narrowedDebugString(spec, Type::anyObject(), false), expectedFalse) << spec;
    }
}

TEST(ExpressionTypingTest, InequalityNarrowingLogicalExpectedTypes) {
    for (const auto& [spec, expectedTrue, expectedFalse] : {
             std::tuple{"{$and: [{$ne: ['$a', 1]}, {$ne: ['$b', 'x']}]}", "object", "object(S)"},
             std::tuple{
                 "{$or: [{$ne: ['$a', 1]}, {$eq: ['$b', 'x']}]}", "object", "{a: number(S), ...}"},
             std::tuple{"{$not: [{$ne: ['$a', 1]}]}", "{a: number(S), ...}", "object"},
             std::tuple{"{$and: [{$or: [{$ne: ['$a', 1]}, {$eq: ['$b', 'x']}]}, "
                        "{$not: [{$ne: ['$b', 2]}]}]}",
                        "{b: number(S), ...}",
                        "object"},
             std::tuple{"{$and: [{$ne: ['$a', null]}, {$ne: ['$b', null]}]}",
                        "{a: ~null, b: ~null, ...}",
                        "object(S)"},
             std::tuple{"{$or: [{$ne: ['$a', null]}, {$ne: ['$b', null]}]}",
                        "object(S)",
                        "{a: null, b: null, ...}"},
         }) {
        ASSERT_EQ(narrowedDebugString(spec, Type::anyObject(), true), expectedTrue) << spec;
        ASSERT_EQ(narrowedDebugString(spec, Type::anyObject(), false), expectedFalse) << spec;
    }
}

TEST(ExpressionTypingTest, NotNarrowingUsesConstantTruthiness) {
    const auto input = openObject({{"a", allValues(BSONType::string)}});
    for (const auto& [spec, expectedTrue, expectedFalse] : {
             std::tuple{"{$not: [false]}", "{a: string, ...}", "never"},
             std::tuple{"{$not: [true]}", "never", "{a: string, ...}"},
             std::tuple{"{$not: [0]}", "{a: string, ...}", "never"},
             std::tuple{"{$not: [1]}", "never", "{a: string, ...}"},
             std::tuple{"{$not: [null]}", "{a: string, ...}", "never"},
             std::tuple{"{$not: ['']}", "never", "{a: string, ...}"},
             std::tuple{"{$not: ['x']}", "never", "{a: string, ...}"},
             std::tuple{"{$not: [{$literal: []}]}", "never", "{a: string, ...}"},
             std::tuple{"{$not: [{$literal: {}}]}", "never", "{a: string, ...}"},
         }) {
        ASSERT_EQ(narrowedDebugString(spec, input, true), expectedTrue) << spec;
        ASSERT_EQ(narrowedDebugString(spec, input, false), expectedFalse) << spec;
    }
}

TEST(ExpressionTypingTest, NotNarrowingLeavesUnsupportedChildUnchanged) {
    const auto input = openObject({{"a", allValues(BSONType::string)}});
    for (const auto& spec : {"{$not: [{$add: [1, 2]}]}", "{$not: ['$a']}", "{$not: ['$$NOW']}"}) {
        ASSERT_EQ(narrowedDebugString(spec, input, true), "{a: string, ...}") << spec;
        ASSERT_EQ(narrowedDebugString(spec, input, false), "{a: string, ...}") << spec;
    }
}

TEST(ExpressionTypingTest, DoubleNotNarrowingPreservesChildConstraints) {
    for (const auto& [spec, expectedTrue, expectedFalse] : {
             std::tuple{
                 "{$not: [{$not: [{$eq: ['$a', null]}]}]}", "{a: null, ...}", "{a: ~null, ...}"},
             std::tuple{"{$not: [{$not: [{$ne: ['$a', 1]}]}]}", "object", "{a: number(S), ...}"},
             std::tuple{"{$not: [{$not: [{$and: [{$eq: ['$a', 1]}, "
                        "{$eq: ['$a', 'x']}]}]}]}",
                        "never",
                        "object"},
             std::tuple{"{$not: [{$not: [{$add: [1, 2]}]}]}", "object", "object"},
         }) {
        ASSERT_EQ(narrowedDebugString(spec, Type::anyObject(), true), expectedTrue) << spec;
        ASSERT_EQ(narrowedDebugString(spec, Type::anyObject(), false), expectedFalse) << spec;
    }
}

TEST(ExpressionTypingTest, ConstantNarrowingUsesTruthiness) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    auto values = scalarRepresentatives();
    values.insert(values.end(),
                  {Value(),
                   Value(BSONNULL),
                   Value(false),
                   Value(true),
                   Value(0),
                   Value(1),
                   Value(0LL),
                   Value(0.0),
                   Value(Decimal128(0)),
                   Value(""sv),
                   Value(BSONObj()),
                   Value(std::vector<Value>{})});
    const Type input = openObject({{"a", allValues(BSONType::string)}});
    for (const auto& value : values) {
        auto expr = ExpressionConstant::create(expCtx.get(), value);
        for (bool assumeTrue : {false, true}) {
            const auto narrowed = narrowType(input, expr.get(), assumeTrue);
            ASSERT_EQ(narrowed.toDebugString(),
                      value.coerceToBool() == assumeTrue ? "{a: string, ...}" : "never")
                << "value " << value.toString() << ", assumption " << assumeTrue;
        }
    }
}

TEST(ExpressionTypingTest, LogicalNarrowingWithConstantsAndUnsupportedChildren) {
    auto expCtx = make_intrusive<ExpressionContextForTest>();
    const Type input = Type::anyObject();
    auto eq = parseExpr(fromjson("{$eq: ['$a', 1]}"), *expCtx);
    auto orFalse = parseExpr(fromjson("{$or: [false, {$eq: ['$a', 1]}]}"), *expCtx);
    auto andTrue = parseExpr(fromjson("{$and: [true, {$eq: ['$a', 1]}]}"), *expCtx);
    auto andFalse = parseExpr(fromjson("{$and: [false, {$eq: ['$a', 1]}]}"), *expCtx);
    auto orTrue = parseExpr(fromjson("{$or: [true, {$eq: ['$a', 1]}]}"), *expCtx);
    for (bool assumption : {false, true}) {
        ASSERT_EQ(narrowType(input, orFalse.get(), assumption).toDebugString(),
                  narrowType(input, eq.get(), assumption).toDebugString());
        ASSERT_EQ(narrowType(input, andTrue.get(), assumption).toDebugString(),
                  narrowType(input, eq.get(), assumption).toDebugString());
    }
    ASSERT_EQ(narrowType(input, andFalse.get(), true).toDebugString(), "never");
    ASSERT_EQ(narrowType(input, orTrue.get(), false).toDebugString(), "never");

    auto unknownOr = parseExpr(fromjson("{$or: [{$add: [1, 2]}, {$eq: ['$a', 1]}]}"), *expCtx);
    auto unknownAnd = parseExpr(fromjson("{$and: [{$add: [1, 2]}, {$eq: ['$a', 1]}]}"), *expCtx);
    ASSERT_EQ(narrowType(input, unknownOr.get(), true).toDebugString(), "object");
    ASSERT_EQ(narrowType(input, unknownAnd.get(), false).toDebugString(), "object");
    ASSERT_EQ(narrowType(input, unknownAnd.get(), true).toDebugString(), "{a: number(S), ...}");
    ASSERT_EQ(narrowType(input, unknownOr.get(), false).toDebugString(), "object");
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
