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

#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "boost/container/detail/std_fwd.hpp"
#include "mongo/bson/json.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/expression/evaluate_test_helpers.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"


namespace mongo {
namespace expression_evaluation_test {
using namespace std::literals::string_view_literals;

using boost::intrusive_ptr;

/* ----------------------------- ExpressionArray ------------------------------ */

namespace {
intrusive_ptr<Expression> parseArrayLiteral(ExpressionContextForTest* expCtx,
                                            const BSONArray& elems) {
    BSONObj wrapper = BSON("" << elems);
    return Expression::parseOperand(expCtx, wrapper.firstElement(), expCtx->variablesParseState);
}
}  // namespace

TEST(ExpressionArrayTest, TracksOutputMemoryAndReleasesAfterEvaluation) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = parseArrayLiteral(&expCtx, BSON_ARRAY("$a"sv << "$b"sv));

    SimpleMemoryUsageTracker tracker{4096};
    EvaluationContext ctx{.tracker = &tracker};

    Document doc{{"a", "hello"sv}, {"b", "world"sv}};
    ASSERT_VALUE_EQ(expr->evaluate(doc, &expCtx.variables, ctx),
                    Value(BSON_ARRAY("hello"sv << "world"sv)));

    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
    ASSERT_GT(tracker.peakTrackedMemoryBytes(), 0);
}

TEST(ExpressionArrayTest, ThrowsExceededMemoryLimitWhenOverLimit) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = parseArrayLiteral(&expCtx, BSON_ARRAY("$a"sv << "$b"sv));

    SimpleMemoryUsageTracker tracker{8};
    EvaluationContext ctx{.tracker = &tracker};

    Document doc{{"a", std::string(100, 'x')}, {"b", std::string(100, 'y')}};
    try {
        expr->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$array");
    }

    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 0);
}

TEST(ExpressionArrayTest, NoTrackerDoesNotThrow) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = parseArrayLiteral(&expCtx, BSON_ARRAY("$a"sv << "$b"sv));

    Document doc{{"a", std::string(100, 'x')}, {"b", std::string(100, 'y')}};
    ASSERT_VALUE_EQ(expr->evaluate(doc, &expCtx.variables),
                    Value(BSON_ARRAY(std::string(100, 'x') << std::string(100, 'y'))));
}

/* ------------------------- ExpressionArrayToObject -------------------------- */

TEST(ExpressionArrayToObjectTest, KVFormatSimple) {
    assertExpectedResults("$arrayToObject",
                          {{{Value(BSON_ARRAY(BSON("k" << "key1"
                                                       << "v" << 2)
                                              << BSON("k" << "key2"
                                                          << "v" << 3)))},
                            Value(BSON("key1" << 2 << "key2" << 3))}});
}

TEST(ExpressionArrayToObjectTest, KVFormatWithDuplicates) {
    assertExpectedResults("$arrayToObject",
                          {{{Value(BSON_ARRAY(BSON("k" << "hi"
                                                       << "v" << 2)
                                              << BSON("k" << "hi"
                                                          << "v" << 3)))},
                            Value(BSON("hi" << 3))}});
}

TEST(ExpressionArrayToObjectTest, ListFormatSimple) {
    assertExpectedResults("$arrayToObject",
                          {{{Value(BSON_ARRAY(BSON_ARRAY("key1" << 2) << BSON_ARRAY("key2" << 3)))},
                            Value(BSON("key1" << 2 << "key2" << 3))}});
}

TEST(ExpressionArrayToObjectTest, ListFormWithDuplicates) {
    assertExpectedResults("$arrayToObject",
                          {{{Value(BSON_ARRAY(BSON_ARRAY("key1" << 2) << BSON_ARRAY("key1" << 3)))},
                            Value(BSON("key1" << 3))}});
}

/* ------------------------ ExpressionReverseArray -------------------- */

TEST(ExpressionReverseArrayTest, ReversesNormalArray) {
    assertExpectedResults("$reverseArray",
                          {{{Value(BSON_ARRAY(1 << 2 << 3))}, Value(BSON_ARRAY(3 << 2 << 1))}});
}

TEST(ExpressionReverseArrayTest, ReversesEmptyArray) {
    assertExpectedResults("$reverseArray",
                          {{{Value(std::vector<Value>())}, Value(std::vector<Value>())}});
}

TEST(ExpressionReverseArrayTest, ReversesOneElementArray) {
    assertExpectedResults("$reverseArray", {{{Value(BSON_ARRAY(1))}, Value(BSON_ARRAY(1))}});
}

TEST(ExpressionReverseArrayTest, ReturnsNullWithNullishInput) {
    assertExpectedResults(
        "$reverseArray",
        {{{Value(BSONNULL)}, Value(BSONNULL)}, {{Value(BSONUndefined)}, Value(BSONNULL)}});
}

/* ------------------------ ExpressionSortArray -------------------- */

TEST(ExpressionSortArrayTest, SortsNormalArrayForwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: [ 2, 1, 3 ] }, sortBy: 1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 2 << 3)));
}

TEST(ExpressionSortArrayTest, SortsNormalArrayBackwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: [ 2, 1, 3 ] }, sortBy: -1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(3 << 2 << 1)));
}

TEST(ExpressionSortArrayTest, SortsEmptyArray) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: [ ] }, sortBy: -1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(std::vector<Value>()));
}

TEST(ExpressionSortArrayTest, SortsOneElementArray) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: [ 1 ] }, sortBy: -1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1)));
}

TEST(ExpressionSortArrayTest, ReturnsNullWithNullInput) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: null }, sortBy: -1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionSortArrayTest, ReturnsNullWithUndefinedInput) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $sortArray: { input: { $literal: undefined }, sortBy: -1 } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionSortArrayTest, SortsObjectsByFieldAscending) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $sortArray: { input: { $literal: [ {x: 3}, {x: 1}, {x: 2} ] }, sortBy: { x: 1 } } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(BSON("x" << 1) << BSON("x" << 2) << BSON("x" << 3))));
}

TEST(ExpressionSortArrayTest, SortsObjectsByFieldDescending) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $sortArray: { input: { $literal: [ {x: 1}, {x: 3}, {x: 2} ] }, sortBy: { x: -1 } } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(BSON("x" << 3) << BSON("x" << 2) << BSON("x" << 1))));
}

TEST(ExpressionSortArrayTest, SortsObjectsByDottedPath) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $sortArray: { input: { $literal: [ {a: {b: 3}}, {a: {b: 1}}, {a: {b: 2}} ] },"
        "  sortBy: { 'a.b': 1 } } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val,
                    Value(BSON_ARRAY(BSON("a" << BSON("b" << 1)) << BSON("a" << BSON("b" << 2))
                                                                 << BSON("a" << BSON("b" << 3)))));
}

TEST(ExpressionSortArrayTest, NonObjectElementsSortAsNullKeyByPattern) {
    auto expCtx = ExpressionContextForTest{};
    // Non-object elements get a null sort key and sort before objects with a defined key.
    BSONObj expr = fromjson(
        "{ $sortArray: { input: { $literal: [ {x: 2}, 42, {x: 1} ] }, sortBy: { x: 1 } } }");

    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    const auto& arr = val.getArray();
    ASSERT_EQ(arr.size(), 3U);
    // Non-object (42) gets null key; null sorts before 1 and 2.
    ASSERT_VALUE_EQ(arr[0], Value(42));
    ASSERT_VALUE_EQ(arr[1], Value(BSON("x" << 1)));
    ASSERT_VALUE_EQ(arr[2], Value(BSON("x" << 2)));
}

TEST(ExpressionSortArrayTest, TrackerIsDeductedAfterPatternSort) {
    // After a successful sort, the tracker should return to 5 — the ScopeGuard
    // must have fired and deducted the key bytes.
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $sortArray: { input: { $literal: [ {x: 3}, {x: 1}, {x: 2} ] }, sortBy: { x: 1 } } }");
    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    SimpleMemoryUsageTracker tracker{1024 * 1024};
    tracker.set(5);
    EvaluationContext ctx{&tracker};
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables, ctx);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(BSON("x" << 1) << BSON("x" << 2) << BSON("x" << 3))));
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 5);
}

TEST(ExpressionSortArrayTest, TrackerDeductedAfterMemoryLimitException) {
    // When the limit is exceeded, assertWithinMemoryLimit throws. The ScopeGuard must still
    // fire and deduct the bytes so the tracker is not left in an inflated state.
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $sortArray: { input: { $literal: [ {x: 3}, {x: 1}, {x: 2} ] }, sortBy: { x: 1 } } }");
    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    SimpleMemoryUsageTracker tracker{6};  // 6-byte limit; always exceeded by any key
    tracker.set(5);
    EvaluationContext ctx{&tracker};

    ASSERT_THROWS(expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables, ctx),
                  AssertionException);
    ASSERT_EQ(tracker.inUseTrackedMemoryBytes(), 5);
}

TEST(ExpressionSortArrayTest, NoMemoryTrackerNoProblems) {
    // Memory tracker in eval ctx is optional, so make sure the code tolerates it missing.
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $sortArray: { input: { $literal: [ {x: 3}, {x: 1}, {x: 2} ] }, sortBy: { x: 1 } } }");
    auto expressionSortArray =
        ExpressionSortArray::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);

    EvaluationContext ctx{nullptr};
    Value val = expressionSortArray->evaluate(MutableDocument().freeze(), &expCtx.variables, ctx);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(BSON("x" << 1) << BSON("x" << 2) << BSON("x" << 3))));
}

/* ------------------------ ExpressionTopN -------------------- */

TEST(ExpressionTopNTest, ReturnsTopNElementsForwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $topN: { n: 2, input: [ 2, 1, 3 ], sortBy: 1 } }");

    auto expressionTopN =
        ExpressionTopN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTopN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 2)));
}

TEST(ExpressionTopNTest, ReturnsTopNElementsBackwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $topN: { n: 2, input: [ 2, 1, 3 ], sortBy: -1 } }");

    auto expressionTopN =
        ExpressionTopN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTopN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(3 << 2)));
}

TEST(ExpressionTopNTest, ReturnsAllElementsWhenNGreaterThanArraySize) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $topN: { n: 10, input: [ 2, 1, 3 ], sortBy: 1 } }");

    auto expressionTopN =
        ExpressionTopN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTopN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 2 << 3)));
}

TEST(ExpressionTopNTest, ReturnsEmptyArrayWhenNIsZero) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $topN: { n: 0, input: [ 2, 1, 3 ], sortBy: 1 } }");

    auto expressionTopN =
        ExpressionTopN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTopN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(std::vector<Value>()));
}

TEST(ExpressionTopNTest, ThrowsWhenNIsNegative) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $topN: { n: -1, input: [ 2, 1, 3 ], sortBy: 1 } }");

    ASSERT_THROWS(
        [&] {
            auto expressionTopN =
                ExpressionTopN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
            expressionTopN->evaluate(MutableDocument().freeze(), &expCtx.variables);
        }(),
        AssertionException);
}

TEST(ExpressionTopNTest, ReturnsNullWithNullInput) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $topN: { n: 2, input: null, sortBy: 1 } }");

    auto expressionTopN =
        ExpressionTopN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTopN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionTopNTest, ReturnsNullWithNullN) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $topN: { n: null, input: [ 2, 1, 3 ], sortBy: 1 } }");

    auto expressionTopN =
        ExpressionTopN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTopN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionTopNTest, ReturnsMixedTypesWithNumericSort) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $topN: { n: 5, input: [ \"string\", null, 3, {a: 1}, [1, 2] ], "
        "sortBy: 1 } }");

    auto expressionTopN =
        ExpressionTopN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTopN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    // BSON type ordering: null < numbers < strings < objects < arrays
    ASSERT_VALUE_EQ(
        val,
        Value(BSON_ARRAY(BSONNULL << 3 << "string"sv << BSON("a" << 1) << BSON_ARRAY(1 << 2))));
}

TEST(ExpressionTopNTest, ReturnsTopNWithNestedArrayElements) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $topN: { n: 2, input: [ [3, 1], [1, 2], [2, 0] ], sortBy: 1 } }");

    auto expressionTopN =
        ExpressionTopN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTopN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(BSON_ARRAY(1 << 2) << BSON_ARRAY(2 << 0))));
}

/* ------------------------ ExpressionTop -------------------- */

TEST(ExpressionTopTest, ReturnsTopElementForwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $top: { input: [ 2, 1, 3 ], sortBy: 1 } }");

    auto expressionTop =
        ExpressionTop::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTop->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(1));
}

TEST(ExpressionTopTest, ReturnsTopElementBackwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $top: { input: [ 2, 1, 3 ], sortBy: -1 } }");

    auto expressionTop =
        ExpressionTop::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTop->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(3));
}

TEST(ExpressionTopTest, ReturnsNullWithEmptyArray) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $top: { input: [], sortBy: 1 } }");

    auto expressionTop =
        ExpressionTop::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTop->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionTopTest, ReturnsNullWithNullInput) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $top: { input: null, sortBy: 1 } }");

    auto expressionTop =
        ExpressionTop::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTop->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionTopTest, ReturnsNullWithUndefinedInput) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $top: { input: undefined, sortBy: 1 } }");

    auto expressionTop =
        ExpressionTop::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTop->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionTopTest, ReturnsTopWithComplexSortBy) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $top: { input: [ {a: 2, b: 1}, {a: 1, b: 2}, {a: 3, b: 0} ], "
        "sortBy: {a: 1, b: -1} } }");

    auto expressionTop =
        ExpressionTop::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTop->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(fromjson("{a: 1, b: 2}")));
}

TEST(ExpressionTopTest, ReturnsTopWithMixedTypesWithNumericSort) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr =
        fromjson("{ $top: { input: [ 1, \"string\", 2, 3, {a: 1}, [1, 2] ], sortBy: 1 } }");

    auto expressionTop =
        ExpressionTop::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTop->evaluate(MutableDocument().freeze(), &expCtx.variables);

    // BSON type ordering: null < numbers < strings < objects < arrays
    ASSERT_VALUE_EQ(val, Value(1));
}

TEST(ExpressionTopTest, ReturnsTopWithNestedArrayElements) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $top: { input: [ [3, 1], [1, 2], [2, 0] ], sortBy: 1 } }");

    auto expressionTop =
        ExpressionTop::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionTop->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 2)));
}

/* ------------------------ ExpressionBottomN -------------------- */

TEST(ExpressionBottomNTest, ReturnsBottomNElementsForwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottomN: { n: 2, input: [ 2, 1, 3 ], sortBy: 1 } }");

    auto expressionBottomN =
        ExpressionBottomN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottomN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(2 << 3)));
}

TEST(ExpressionBottomNTest, ReturnsBottomNElementsBackwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottomN: { n: 2, input: [ 2, 1, 3 ], sortBy: -1 } }");

    auto expressionBottomN =
        ExpressionBottomN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottomN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(2 << 1)));
}

TEST(ExpressionBottomNTest, ReturnsAllElementsWhenNGreaterThanArraySize) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottomN: { n: 10, input: [ 2, 1, 3 ], sortBy: 1 } }");

    auto expressionBottomN =
        ExpressionBottomN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottomN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 2 << 3)));
}

TEST(ExpressionBottomNTest, ReturnsEmptyArrayWhenNIsZero) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottomN: { n: 0, input: [ 2, 1, 3 ], sortBy: 1 } }");

    auto expressionBottomN =
        ExpressionBottomN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottomN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(std::vector<Value>()));
}

TEST(ExpressionBottomNTest, ThrowsWhenNIsNegative) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottomN: { n: -1, input: [ 2, 1, 3 ], sortBy: 1 } }");

    ASSERT_THROWS(
        [&] {
            auto expressionBottomN =
                ExpressionBottomN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
            expressionBottomN->evaluate(MutableDocument().freeze(), &expCtx.variables);
        }(),
        AssertionException);
}

TEST(ExpressionBottomNTest, ReturnsNullWithNullInput) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottomN: { n: 2, input: null, sortBy: 1 } }");

    auto expressionBottomN =
        ExpressionBottomN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottomN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionBottomNTest, ReturnsNullWithNullN) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottomN: { n: null, input: [ 2, 1, 3 ], sortBy: 1 } }");

    auto expressionBottomN =
        ExpressionBottomN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottomN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionBottomNTest, ReturnsMixedTypesWithNumericSort) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $bottomN: { n: 5, input: [ \"string\", null, 3, {a: 1}, [1, 2] ], "
        "sortBy: 1 } }");

    auto expressionBottomN =
        ExpressionBottomN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottomN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    // BSON type ordering: null < numbers < strings < objects < arrays
    ASSERT_VALUE_EQ(
        val,
        Value(BSON_ARRAY(BSONNULL << 3 << "string"sv << BSON("a" << 1) << BSON_ARRAY(1 << 2))));
}

TEST(ExpressionBottomNTest, ReturnsBottomNWithNestedArrayElements) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottomN: { n: 2, input: [ [3, 1], [1, 2], [2, 0] ], sortBy: 1 } }");

    auto expressionBottomN =
        ExpressionBottomN::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottomN->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_EQ(val.getType(), BSONType::array);
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(BSON_ARRAY(2 << 0) << BSON_ARRAY(3 << 1))));
}

/* ------------------------ ExpressionBottom -------------------- */

TEST(ExpressionBottomTest, ReturnsBottomElementForwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottom: { input: [ 2, 1, 3 ], sortBy: 1 } }");

    auto expressionBottom =
        ExpressionBottom::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottom->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(3));
}

TEST(ExpressionBottomTest, ReturnsBottomElementBackwards) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottom: { input: [ 2, 1, 3 ], sortBy: -1 } }");

    auto expressionBottom =
        ExpressionBottom::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottom->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(1));
}

TEST(ExpressionBottomTest, ReturnsNullWithEmptyArray) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottom: { input: [], sortBy: 1 } }");

    auto expressionBottom =
        ExpressionBottom::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottom->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionBottomTest, ReturnsNullWithNullInput) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottom: { input: null, sortBy: 1 } }");

    auto expressionBottom =
        ExpressionBottom::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottom->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionBottomTest, ReturnsNullWithUndefinedInput) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottom: { input: undefined, sortBy: 1 } }");

    auto expressionBottom =
        ExpressionBottom::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottom->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSONNULL));
}

TEST(ExpressionBottomTest, ReturnsBottomWithComplexSortBy) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson(
        "{ $bottom: { input: [ {a: 2, b: 1}, {a: 1, b: 2}, {a: 3, b: 0} ], "
        "sortBy: {a: 1, b: -1} } }");

    auto expressionBottom =
        ExpressionBottom::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottom->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(fromjson("{a: 3, b: 0}")));
}

TEST(ExpressionBottomTest, ReturnsBottomWithMixedTypesWithNumericSort) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr =
        fromjson("{ $bottom: { input: [ 1, \"string\", 2, 3, {a: 1}, [1, 2] ], sortBy: 1 } }");

    auto expressionBottom =
        ExpressionBottom::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottom->evaluate(MutableDocument().freeze(), &expCtx.variables);

    // BSON type ordering: null < numbers < strings < objects < arrays
    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(1 << 2)));
}

TEST(ExpressionBottomTest, ReturnsBottomWithNestedArrayElements) {
    auto expCtx = ExpressionContextForTest{};
    BSONObj expr = fromjson("{ $bottom: { input: [ [3, 1], [1, 2], [2, 0] ], sortBy: 1 } }");

    auto expressionBottom =
        ExpressionBottom::parse(&expCtx, expr.firstElement(), expCtx.variablesParseState);
    Value val = expressionBottom->evaluate(MutableDocument().freeze(), &expCtx.variables);

    ASSERT_VALUE_EQ(val, Value(BSON_ARRAY(3 << 1)));
}

namespace set {
Value sortSet(Value set) {
    if (set.nullish()) {
        return Value(BSONNULL);
    }
    std::vector<Value> sortedSet = set.getArray();
    ValueComparator valueComparator;
    std::sort(sortedSet.begin(), sortedSet.end(), valueComparator.getLessThan());
    return Value(sortedSet);
}

void runTest(Document spec) {
    auto expCtx = ExpressionContextForTest{};
    const Value args = spec["input"];
    if (!spec["expected"].missing()) {
        FieldIterator fields(spec["expected"].getDocument());
        while (fields.more()) {
            const Document::FieldPair field(fields.next());
            const Value expected = field.second;
            const BSONObj obj = BSON(field.first << args);
            VariablesParseState vps = expCtx.variablesParseState;
            const intrusive_ptr<Expression> expr = Expression::parseExpression(&expCtx, obj, vps);
            Value result = expr->evaluate({}, &expCtx.variables);
            if (result.getType() == BSONType::array) {
                result = sortSet(result);
            }
            if (ValueComparator().evaluate(result != expected)) {
                std::string errMsg = str::stream()
                    << "for expression " << std::string{field.first} << " with argument "
                    << args.toString() << " full tree: " << expr->serialize().toString()
                    << " expected: " << expected.toString() << " but got: " << result.toString();
                FAIL(errMsg);
            }
            // TODO test optimize here
        }
    }
    if (!spec["error"].missing()) {
        MONGO_COMPILER_DIAGNOSTIC_PUSH
        MONGO_COMPILER_DIAGNOSTIC_IGNORED_TRANSITIONAL("-Wdangling-reference")
        const std::vector<Value>& asserters = spec["error"].getArray();
        MONGO_COMPILER_DIAGNOSTIC_POP
        size_t n = asserters.size();
        for (size_t i = 0; i < n; i++) {
            const BSONObj obj = BSON(asserters[i].getString() << args);
            VariablesParseState vps = expCtx.variablesParseState;
            ASSERT_THROWS(
                [&] {
                    // NOTE: parse and evaluation failures are treated the
                    // same
                    const intrusive_ptr<Expression> expr =
                        Expression::parseExpression(&expCtx, obj, vps);
                    expr->evaluate({}, &expCtx.variables);
                }(),
                AssertionException);
        }
    }
}

TEST(ExpressionSetTest, Same) {
    runTest(
        DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(1 << 2)) << "expected"
                    << DOC("$setIsSubset" << true << "$setEquals" << true << "$setIntersection"
                                          << DOC_ARRAY(1 << 2) << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << std::vector<Value>())));
}

TEST(ExpressionSetTest, Redundant) {
    runTest(
        DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(1 << 2 << 2)) << "expected"
                    << DOC("$setIsSubset" << true << "$setEquals" << true << "$setIntersection"
                                          << DOC_ARRAY(1 << 2) << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << std::vector<Value>())));
}

TEST(ExpressionSetTest, DoubleRedundant) {
    runTest(
        DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 1 << 2) << DOC_ARRAY(1 << 2 << 2)) << "expected"
                    << DOC("$setIsSubset" << true << "$setEquals" << true << "$setIntersection"
                                          << DOC_ARRAY(1 << 2) << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << std::vector<Value>())));
}

TEST(ExpressionSetTest, Super) {
    runTest(
        DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(1)) << "expected"
                    << DOC("$setIsSubset" << false << "$setEquals" << false << "$setIntersection"
                                          << DOC_ARRAY(1) << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << DOC_ARRAY(2))));
}

TEST(ExpressionSetTest, SuperWithRedundant) {
    runTest(
        DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2 << 2) << DOC_ARRAY(1)) << "expected"
                    << DOC("$setIsSubset" << false << "$setEquals" << false << "$setIntersection"
                                          << DOC_ARRAY(1) << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << DOC_ARRAY(2))));
}

TEST(ExpressionSetTest, Sub) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(1) << DOC_ARRAY(1 << 2)) << "expected"
                        << DOC("$setIsSubset" << true << "$setEquals" << false << "$setIntersection"
                                              << DOC_ARRAY(1) << "$setUnion" << DOC_ARRAY(1 << 2)
                                              << "$setDifference" << std::vector<Value>())));
}

TEST(ExpressionSetTest, SameBackwards) {
    runTest(
        DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(2 << 1)) << "expected"
                    << DOC("$setIsSubset" << true << "$setEquals" << true << "$setIntersection"
                                          << DOC_ARRAY(1 << 2) << "$setUnion" << DOC_ARRAY(1 << 2)
                                          << "$setDifference" << std::vector<Value>())));
}

TEST(ExpressionSetTest, NoOverlap) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(8 << 4)) << "expected"
                        << DOC("$setIsSubset" << false << "$setEquals" << false
                                              << "$setIntersection" << std::vector<Value>()
                                              << "$setUnion" << DOC_ARRAY(1 << 2 << 4 << 8)
                                              << "$setDifference" << DOC_ARRAY(1 << 2))));
}

TEST(ExpressionSetTest, Overlap) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << DOC_ARRAY(8 << 2 << 4)) << "expected"
                        << DOC("$setIsSubset" << false << "$setEquals" << false
                                              << "$setIntersection" << DOC_ARRAY(2) << "$setUnion"
                                              << DOC_ARRAY(1 << 2 << 4 << 8) << "$setDifference"
                                              << DOC_ARRAY(1))));
}

TEST(ExpressionSetTest, LastNull) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << Value(BSONNULL)) << "expected"
                        << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                  << "$setDifference" << BSONNULL)
                        << "error" << DOC_ARRAY("$setEquals"sv << "$setIsSubset"sv)));
}

TEST(ExpressionSetTest, FirstNull) {
    runTest(DOC("input" << DOC_ARRAY(Value(BSONNULL) << DOC_ARRAY(1 << 2)) << "expected"
                        << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                  << "$setDifference" << BSONNULL)
                        << "error" << DOC_ARRAY("$setEquals"sv << "$setIsSubset"sv)));
}

TEST(ExpressionSetTest, LeftNullAndRightEmpty) {
    runTest(DOC("input" << DOC_ARRAY(Value(BSONNULL) << std::vector<Value>()) << "expected"
                        << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                  << "$setDifference" << BSONNULL)
                        << "error" << DOC_ARRAY("$setEquals"sv << "$setIsSubset"sv)));
}

TEST(ExpressionSetTest, RightNullAndLeftEmpty) {
    runTest(DOC("input" << DOC_ARRAY(std::vector<Value>() << Value(BSONNULL)) << "expected"
                        << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                  << "$setDifference" << BSONNULL)
                        << "error" << DOC_ARRAY("$setEquals"sv << "$setIsSubset"sv)));
}

TEST(ExpressionSetTest, NoArg) {
    runTest(DOC("input" << std::vector<Value>() << "expected"
                        << DOC("$setIntersection" << std::vector<Value>() << "$setUnion"
                                                  << std::vector<Value>())
                        << "error"
                        << DOC_ARRAY("$setEquals"sv << "$setIsSubset"sv
                                                    << "$setDifference"sv)));
}

TEST(ExpressionSetTest, OneArg) {
    runTest(DOC(
        "input" << DOC_ARRAY(DOC_ARRAY(1 << 2)) << "expected"
                << DOC("$setIntersection" << DOC_ARRAY(1 << 2) << "$setUnion" << DOC_ARRAY(1 << 2))
                << "error"
                << DOC_ARRAY("$setEquals"sv << "$setIsSubset"sv
                                            << "$setDifference"sv)));
}

TEST(ExpressionSetTest, EmptyArg) {
    runTest(DOC("input" << DOC_ARRAY(std::vector<Value>()) << "expected"
                        << DOC("$setIntersection" << std::vector<Value>() << "$setUnion"
                                                  << std::vector<Value>())
                        << "error"
                        << DOC_ARRAY("$setEquals"sv << "$setIsSubset"sv
                                                    << "$setDifference"sv)));
}

TEST(ExpressionSetTest, LeftArgEmpty) {
    runTest(DOC("input" << DOC_ARRAY(std::vector<Value>() << DOC_ARRAY(1 << 2)) << "expected"
                        << DOC("$setIntersection" << std::vector<Value>() << "$setUnion"
                                                  << DOC_ARRAY(1 << 2) << "$setIsSubset" << true
                                                  << "$setEquals" << false << "$setDifference"
                                                  << std::vector<Value>())));
}

TEST(ExpressionSetTest, RightArgEmpty) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2) << std::vector<Value>()) << "expected"
                        << DOC("$setIntersection" << std::vector<Value>() << "$setUnion"
                                                  << DOC_ARRAY(1 << 2) << "$setIsSubset" << false
                                                  << "$setEquals" << false << "$setDifference"
                                                  << DOC_ARRAY(1 << 2))));
}

TEST(ExpressionSetTest, ManyArgs) {
    runTest(
        DOC("input" << DOC_ARRAY(DOC_ARRAY(8 << 3) << DOC_ARRAY("asdf"sv << "foo"sv)
                                                   << DOC_ARRAY(80.3 << 34) << std::vector<Value>()
                                                   << DOC_ARRAY(80.3 << "foo"sv << 11 << "yay"sv))
                    << "expected"
                    << DOC("$setIntersection" << std::vector<Value>() << "$setEquals" << false
                                              << "$setUnion"
                                              << DOC_ARRAY(3 << 8 << 11 << 34 << 80.3 << "asdf"sv
                                                             << "foo"sv
                                                             << "yay"sv))
                    << "error" << DOC_ARRAY("$setIsSubset"sv << "$setDifference"sv)));
}

TEST(ExpressionSetTest, ManyArgsEqual) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2 << 4)
                                     << DOC_ARRAY(1 << 2 << 2 << 4) << DOC_ARRAY(4 << 1 << 2)
                                     << DOC_ARRAY(2 << 1 << 1 << 4))
                        << "expected"
                        << DOC("$setIntersection" << DOC_ARRAY(1 << 2 << 4) << "$setEquals" << true
                                                  << "$setUnion" << DOC_ARRAY(1 << 2 << 4))
                        << "error" << DOC_ARRAY("$setIsSubset"sv << "$setDifference"sv)));
}

}  // namespace set

/* ----------------------- ExpressionConcatArrays memory tracking ----------------------- */

TEST(ExpressionConcatArraysTest, MemoryTrackerAccumulatesOutputSize) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = Expression::parseExpression(
        &expCtx,
        BSON("$concatArrays" << BSON_ARRAY(BSON_ARRAY(1 << 2 << 3) << BSON_ARRAY(4 << 5))),
        expCtx.variablesParseState);

    SimpleMemoryUsageTracker tracker{1024 * 1024};
    EvaluationContext ctx{.tracker = &tracker};
    ASSERT_DOES_NOT_THROW(expr->evaluate({}, &expCtx.variables, ctx));
    // The token tracking $concatArrays's transient memory is released when evaluate() returns, so
    // inUseTrackedMemoryBytes() is back to 0 here. Assert on the retained high-water mark instead.
    ASSERT_GT(tracker.peakTrackedMemoryBytes(), 0);
}

TEST(ExpressionConcatArraysTest, MemoryTrackerThrowsWhenLimitExceeded) {
    auto expCtx = ExpressionContextForTest{};
    std::vector<Value> arr1, arr2;
    for (int i = 0; i < 10; ++i)
        arr1.push_back(Value(i));
    for (int i = 10; i < 20; ++i)
        arr2.push_back(Value(i));
    auto expr = Expression::parseExpression(
        &expCtx, BSON("$concatArrays" << BSON_ARRAY("$a"sv << "$b"sv)), expCtx.variablesParseState);
    Document doc{{"a", Value(arr1)}, {"b", Value(arr2)}};

    SimpleMemoryUsageTracker tracker{100};
    EvaluationContext ctx{.tracker = &tracker};
    try {
        expr->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$concatArrays");
    }
}

TEST(ExpressionConcatArraysTest, MemoryTrackerThrowsWhenQueryLimitExceeded) {
    auto expCtx = ExpressionContextForTest{};
    std::vector<Value> arr1, arr2;
    for (int i = 0; i < 10; ++i)
        arr1.push_back(Value(i));
    for (int i = 10; i < 20; ++i)
        arr2.push_back(Value(i));
    auto expr = Expression::parseExpression(
        &expCtx, BSON("$concatArrays" << BSON_ARRAY("$a"sv << "$b"sv)), expCtx.variablesParseState);
    Document doc{{"a", Value(arr1)}, {"b", Value(arr2)}};

    // Root (operation-wide) tracker carries the 100-byte cap; the stage tracker reports into it but
    // has a large local limit of its own. Usage rolls up via the base chain.
    SimpleMemoryUsageTracker operationTracker{100};
    SimpleMemoryUsageTracker stageTracker{&operationTracker, 100 * 1024 * 1024};
    EvaluationContext ctx{.tracker = &stageTracker};
    try {
        expr->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$concatArrays");
    }
}

TEST(ExpressionConcatArraysTest, NoTrackerDoesNotThrow) {
    auto expCtx = ExpressionContextForTest{};
    BSONArrayBuilder arr1, arr2;
    for (int i = 0; i < 10; ++i)
        arr1.append(i);
    for (int i = 10; i < 20; ++i)
        arr2.append(i);
    auto expr =
        Expression::parseExpression(&expCtx,
                                    BSON("$concatArrays" << BSON_ARRAY(arr1.arr() << arr2.arr())),
                                    expCtx.variablesParseState);

    // No tracker, must evaluate normally regardless of output size.
    ASSERT_DOES_NOT_THROW(expr->evaluate({}, &expCtx.variables));
}


/* ----------------------- ExpressionSetUnion memory tracking ----------------------- */

TEST(ExpressionSetUnionTest, MemoryTrackerAccumulatesOutputSize) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = Expression::parseExpression(
        &expCtx,
        BSON("$setUnion" << BSON_ARRAY(BSON_ARRAY(1 << 2 << 3) << BSON_ARRAY(4 << 5))),
        expCtx.variablesParseState);

    SimpleMemoryUsageTracker tracker{1024 * 1024};
    EvaluationContext ctx{.tracker = &tracker};
    ASSERT_DOES_NOT_THROW(expr->evaluate({}, &expCtx.variables, ctx));
    // The token tracking $setUnion's transient memory is released when evaluate() returns, so
    // inUseTrackedMemoryBytes() is back to 0 here. Assert on the retained high-water mark instead.
    ASSERT_GT(tracker.peakTrackedMemoryBytes(), 0);
}

TEST(ExpressionSetUnionTest, MemoryTrackerThrowsWhenLimitExceeded) {
    auto expCtx = ExpressionContextForTest{};
    std::vector<Value> arr1, arr2;
    for (int i = 0; i < 10; ++i)
        arr1.push_back(Value(i));
    for (int i = 10; i < 20; ++i)
        arr2.push_back(Value(i));
    auto expr = Expression::parseExpression(
        &expCtx, BSON("$setUnion" << BSON_ARRAY("$a"sv << "$b"sv)), expCtx.variablesParseState);
    Document doc{{"a", Value(arr1)}, {"b", Value(arr2)}};

    SimpleMemoryUsageTracker tracker{256};
    EvaluationContext ctx{.tracker = &tracker};
    try {
        expr->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$setUnion");
    }
}

TEST(ExpressionSetUnionTest, MemoryTrackerThrowsWhenQueryLimitExceeded) {
    auto expCtx = ExpressionContextForTest{};
    std::vector<Value> arr1, arr2;
    for (int i = 0; i < 10; ++i)
        arr1.push_back(Value(i));
    for (int i = 10; i < 20; ++i)
        arr2.push_back(Value(i));
    auto expr = Expression::parseExpression(
        &expCtx, BSON("$setUnion" << BSON_ARRAY("$a"sv << "$b"sv)), expCtx.variablesParseState);
    Document doc{{"a", Value(arr1)}, {"b", Value(arr2)}};

    // Root (operation-wide) tracker carries the cap; the stage tracker reports into it but
    // has a large local limit of its own. Usage rolls up via the base chain.
    SimpleMemoryUsageTracker operationTracker{256};
    SimpleMemoryUsageTracker stageTracker{&operationTracker, 100 * 1024 * 1024};
    EvaluationContext ctx{.tracker = &stageTracker};
    try {
        expr->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$setUnion");
    }
}

TEST(ExpressionSetUnionTest, NoTrackerDoesNotThrow) {
    auto expCtx = ExpressionContextForTest{};
    BSONArrayBuilder arr1, arr2;
    for (int i = 0; i < 10; ++i)
        arr1.append(i);
    for (int i = 10; i < 20; ++i)
        arr2.append(i);
    auto expr =
        Expression::parseExpression(&expCtx,
                                    BSON("$setUnion" << BSON_ARRAY(arr1.arr() << arr2.arr())),
                                    expCtx.variablesParseState);

    ASSERT_DOES_NOT_THROW(expr->evaluate({}, &expCtx.variables));
}

/* ----------------------- ExpressionZip memory tracking ----------------------- */

TEST(ExpressionZipTest, MemoryTrackerAccumulatesOutputSize) {
    auto expCtx = ExpressionContextForTest{};
    auto expr = Expression::parseExpression(
        &expCtx,
        BSON("$zip" << BSON("inputs"
                            << BSON_ARRAY(BSON_ARRAY(1 << 2 << 3) << BSON_ARRAY(4 << 5 << 6)))),
        expCtx.variablesParseState);

    SimpleMemoryUsageTracker tracker{1024 * 1024};
    EvaluationContext ctx{.tracker = &tracker};
    ASSERT_DOES_NOT_THROW(expr->evaluate({}, &expCtx.variables, ctx));
    // The token tracking $zip's transient memory is released when evaluate() returns, so
    // inUseTrackedMemoryBytes() is back to 0 here. Assert on the retained high-water mark instead.
    ASSERT_GT(tracker.peakTrackedMemoryBytes(), 0);
}

TEST(ExpressionZipTest, MemoryTrackerThrowsWhenInputExceedsLimit) {
    auto expCtx = ExpressionContextForTest{};
    // Two 10-element integer arrays; each exceeds the limit on its own.
    std::vector<Value> arr1, arr2;
    for (int i = 0; i < 10; ++i)
        arr1.push_back(Value(i));
    for (int i = 10; i < 20; ++i)
        arr2.push_back(Value(i));
    auto expr =
        Expression::parseExpression(&expCtx,
                                    BSON("$zip" << BSON("inputs" << BSON_ARRAY("$a"sv << "$b"sv))),
                                    expCtx.variablesParseState);
    Document doc{{"a", Value(arr1)}, {"b", Value(arr2)}};

    SimpleMemoryUsageTracker tracker{100};
    EvaluationContext ctx{.tracker = &tracker};
    try {
        expr->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$zip");
    }
}

TEST(ExpressionZipTest, MemoryTrackerThrowsWhenQueryLimitExceeded) {
    auto expCtx = ExpressionContextForTest{};
    // Two 10-element integer arrays; each exceeds the operation-wide limit on its own.
    std::vector<Value> arr1, arr2;
    for (int i = 0; i < 10; ++i)
        arr1.push_back(Value(i));
    for (int i = 10; i < 20; ++i)
        arr2.push_back(Value(i));
    auto expr =
        Expression::parseExpression(&expCtx,
                                    BSON("$zip" << BSON("inputs" << BSON_ARRAY("$a"sv << "$b"sv))),
                                    expCtx.variablesParseState);
    Document doc{{"a", Value(arr1)}, {"b", Value(arr2)}};

    // Root (operation-wide) tracker carries the limited cap; the stage tracker reports into it but
    // has a large local limit of its own. Usage rolls up via the base chain.
    SimpleMemoryUsageTracker operationTracker{100};
    SimpleMemoryUsageTracker stageTracker{&operationTracker, 100 * 1024 * 1024};
    EvaluationContext ctx{.tracker = &stageTracker};
    try {
        expr->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$zip");
    }
}

TEST(ExpressionZipTest, NoTrackerDoesNotThrow) {
    auto expCtx = ExpressionContextForTest{};
    BSONArrayBuilder arr1, arr2;
    for (int i = 0; i < 10; ++i)
        arr1.append(i);
    for (int i = 10; i < 20; ++i)
        arr2.append(i);
    auto expr = Expression::parseExpression(
        &expCtx,
        BSON("$zip" << BSON("inputs" << BSON_ARRAY(arr1.arr() << arr2.arr()))),
        expCtx.variablesParseState);

    // No tracker, must evaluate normally regardless of output size.
    ASSERT_DOES_NOT_THROW(expr->evaluate({}, &expCtx.variables));
}

TEST(ExpressionZipTest, MemoryTrackerThrowsWhenDefaultExceedsLimit) {
    auto expCtx = ExpressionContextForTest{};
    // arr1 has 1 element, arr2 has 5 (different lengths force a default to be evaluated).
    // The default is a 10 000-char string; the inputs (6 integers) fit within the limit
    // but the default does not.
    std::string largeDefault(10000, 'x');
    auto expr = Expression::parseExpression(
        &expCtx,
        BSON("$zip" << BSON("inputs" << BSON_ARRAY("$a"sv << "$b"sv) << "defaults"
                                     << BSON_ARRAY(largeDefault << 0) << "useLongestLength"
                                     << true)),
        expCtx.variablesParseState);
    Document doc{
        {"a", Value(std::vector<Value>{Value(1)})},
        {"b", Value(std::vector<Value>{Value(1), Value(2), Value(3), Value(4), Value(5)})}};

    SimpleMemoryUsageTracker tracker{2000};
    EvaluationContext ctx{.tracker = &tracker};
    try {
        expr->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$zip");
    }
}

TEST(ExpressionZipTest, MemoryTrackerThrowsWhenInputAndDefaultCombinationExceedsLimit) {
    auto expCtx = ExpressionContextForTest{};
    // arr1 has 3 elements, arr2 has 5 (different lengths force a default to be evaluated).
    // Compute the exact limit so that inputs and default each fit individually but not together.
    std::string moderateDefault(300, 'x');

    int64_t inputsSize =
        static_cast<int64_t>(
            Value(std::vector<Value>{Value(1), Value(2), Value(3)}).getApproximateSize()) +
        static_cast<int64_t>(
            Value(std::vector<Value>{Value(1), Value(2), Value(3), Value(4), Value(5)})
                .getApproximateSize());
    int64_t defaultSize = static_cast<int64_t>(Value(moderateDefault).getApproximateSize());
    int64_t nullSize = static_cast<int64_t>(Value(BSONNULL).getApproximateSize());

    // token at the failing assert = inputsSize + nullSize + defaultSize (one null replaced by
    // default)
    int64_t limit = inputsSize + nullSize + defaultSize - 1;
    ASSERT_GT(limit, inputsSize + 2 * nullSize);  // inputs + placeholder nulls fit
    ASSERT_GT(limit, defaultSize);                // default alone fits

    BSONArrayBuilder bArr1, bArr2;
    bArr1.append(1).append(2).append(3);
    bArr2.append(1).append(2).append(3).append(4).append(5);
    auto expr = Expression::parseExpression(
        &expCtx,
        BSON("$zip" << BSON("inputs" << BSON_ARRAY(bArr1.arr() << bArr2.arr()) << "defaults"
                                     << BSON_ARRAY(moderateDefault << 0) << "useLongestLength"
                                     << true)),
        expCtx.variablesParseState);

    SimpleMemoryUsageTracker tracker{limit};
    EvaluationContext ctx{.tracker = &tracker};
    try {
        expr->evaluate({}, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$zip");
    }
}

TEST(ExpressionZipTest, MemoryTrackerThrowsWhenOutputExceedsLimit) {
    auto expCtx = ExpressionContextForTest{};
    // Three equal-length arrays of integers: inputs and placeholder defaults fit under the limit,
    // but the output allocation (outputChild scratch buffer + per-row RCVector<Value> header +
    // element slots) pushes the total over it.
    const int N = 100;
    std::vector<Value> vArr1, vArr2, vArr3;
    for (int i = 0; i < N; ++i) {
        vArr1.push_back(Value(i));
        vArr2.push_back(Value(i + N));
        vArr3.push_back(Value(i + 2 * N));
    }
    auto expr = Expression::parseExpression(
        &expCtx,
        BSON("$zip" << BSON("inputs" << BSON_ARRAY("$a"sv << "$b"sv << "$c"sv))),
        expCtx.variablesParseState);

    // Compute input cost: three N-element integer arrays.
    int64_t oneArraySize =
        static_cast<int64_t>(Value(std::vector<Value>(N, Value(0))).getApproximateSize());
    int64_t inputsSize = 3 * oneArraySize;
    int64_t nullSize = static_cast<int64_t>(Value(BSONNULL).getApproximateSize());
    // Set limit to exactly inputs + placeholder defaults: these pass, then output allocation fails.
    int64_t limit = inputsSize + 3 * nullSize;

    Document doc{{"a", Value(vArr1)}, {"b", Value(vArr2)}, {"c", Value(vArr3)}};
    SimpleMemoryUsageTracker tracker{limit};
    EvaluationContext ctx{.tracker = &tracker};
    try {
        expr->evaluate(doc, &expCtx.variables, ctx);
        FAIL("Expected ExceededMemoryLimit to be thrown");
    } catch (const AssertionException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::ExceededMemoryLimit);
        ASSERT_STRING_CONTAINS(ex.reason(), "$zip");
    }
}

}  // namespace expression_evaluation_test
}  // namespace mongo
