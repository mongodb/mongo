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
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/unittest.h"

#include <climits>
#include <cmath>
#include <limits>

namespace mongo {
namespace expression_evaluation_test {

using boost::intrusive_ptr;

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
                        << "error" << DOC_ARRAY("$setEquals"_sd << "$setIsSubset"_sd)));
}

TEST(ExpressionSetTest, FirstNull) {
    runTest(DOC("input" << DOC_ARRAY(Value(BSONNULL) << DOC_ARRAY(1 << 2)) << "expected"
                        << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                  << "$setDifference" << BSONNULL)
                        << "error" << DOC_ARRAY("$setEquals"_sd << "$setIsSubset"_sd)));
}

TEST(ExpressionSetTest, LeftNullAndRightEmpty) {
    runTest(DOC("input" << DOC_ARRAY(Value(BSONNULL) << std::vector<Value>()) << "expected"
                        << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                  << "$setDifference" << BSONNULL)
                        << "error" << DOC_ARRAY("$setEquals"_sd << "$setIsSubset"_sd)));
}

TEST(ExpressionSetTest, RightNullAndLeftEmpty) {
    runTest(DOC("input" << DOC_ARRAY(std::vector<Value>() << Value(BSONNULL)) << "expected"
                        << DOC("$setIntersection" << BSONNULL << "$setUnion" << BSONNULL
                                                  << "$setDifference" << BSONNULL)
                        << "error" << DOC_ARRAY("$setEquals"_sd << "$setIsSubset"_sd)));
}

TEST(ExpressionSetTest, NoArg) {
    runTest(DOC("input" << std::vector<Value>() << "expected"
                        << DOC("$setIntersection" << std::vector<Value>() << "$setUnion"
                                                  << std::vector<Value>())
                        << "error"
                        << DOC_ARRAY("$setEquals"_sd << "$setIsSubset"_sd
                                                     << "$setDifference"_sd)));
}

TEST(ExpressionSetTest, OneArg) {
    runTest(DOC(
        "input" << DOC_ARRAY(DOC_ARRAY(1 << 2)) << "expected"
                << DOC("$setIntersection" << DOC_ARRAY(1 << 2) << "$setUnion" << DOC_ARRAY(1 << 2))
                << "error"
                << DOC_ARRAY("$setEquals"_sd << "$setIsSubset"_sd
                                             << "$setDifference"_sd)));
}

TEST(ExpressionSetTest, EmptyArg) {
    runTest(DOC("input" << DOC_ARRAY(std::vector<Value>()) << "expected"
                        << DOC("$setIntersection" << std::vector<Value>() << "$setUnion"
                                                  << std::vector<Value>())
                        << "error"
                        << DOC_ARRAY("$setEquals"_sd << "$setIsSubset"_sd
                                                     << "$setDifference"_sd)));
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
        DOC("input" << DOC_ARRAY(DOC_ARRAY(8 << 3) << DOC_ARRAY("asdf"_sd << "foo"_sd)
                                                   << DOC_ARRAY(80.3 << 34) << std::vector<Value>()
                                                   << DOC_ARRAY(80.3 << "foo"_sd << 11 << "yay"_sd))
                    << "expected"
                    << DOC("$setIntersection" << std::vector<Value>() << "$setEquals" << false
                                              << "$setUnion"
                                              << DOC_ARRAY(3 << 8 << 11 << 34 << 80.3 << "asdf"_sd
                                                             << "foo"_sd
                                                             << "yay"_sd))
                    << "error" << DOC_ARRAY("$setIsSubset"_sd << "$setDifference"_sd)));
}

TEST(ExpressionSetTest, ManyArgsEqual) {
    runTest(DOC("input" << DOC_ARRAY(DOC_ARRAY(1 << 2 << 4)
                                     << DOC_ARRAY(1 << 2 << 2 << 4) << DOC_ARRAY(4 << 1 << 2)
                                     << DOC_ARRAY(2 << 1 << 1 << 4))
                        << "expected"
                        << DOC("$setIntersection" << DOC_ARRAY(1 << 2 << 4) << "$setEquals" << true
                                                  << "$setUnion" << DOC_ARRAY(1 << 2 << 4))
                        << "error" << DOC_ARRAY("$setIsSubset"_sd << "$setDifference"_sd)));
}

}  // namespace set

}  // namespace expression_evaluation_test
}  // namespace mongo
