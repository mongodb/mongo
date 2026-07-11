// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/expression/evaluate_test_helpers.h"
#include "mongo/db/pipeline/aggregation_context_fixture.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace expression_evaluation_test {

using boost::intrusive_ptr;
using EvaluateFieldPathTest = AggregationContextFixture;

TEST_F(EvaluateFieldPathTest, Missing) {
    /** Field path target field is missing. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{}"), toBson(expression->evaluate({}, &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, Present) {
    /** Simple case where the target field is present. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        fromjson("{'':123}"),
        toBson(expression->evaluate(fromBson(BSON("a" << 123)), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, NestedBelowNull) {
    /** Target field parent is null. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        fromjson("{}"),
        toBson(expression->evaluate(fromBson(fromjson("{a:null}")), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, NestedBelowUndefined) {
    /** Target field parent is undefined. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        fromjson("{}"),
        toBson(expression->evaluate(fromBson(fromjson("{a:undefined}")), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, NestedBelowMissing) {
    /** Target field parent is missing. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        fromjson("{}"),
        toBson(expression->evaluate(fromBson(fromjson("{z:1}")), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, NestedBelowInt) {
    /** Target field parent is an integer. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        fromjson("{}"), toBson(expression->evaluate(fromBson(BSON("a" << 2)), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, NestedValue) {
    /** A value in a nested object. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        BSON("" << 55),
        toBson(expression->evaluate(fromBson(BSON("a" << BSON("b" << 55))), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, NestedBelowEmptyObject) {
    /** Target field within an empty object. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        fromjson("{}"),
        toBson(expression->evaluate(fromBson(BSON("a" << BSONObj())), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, NestedBelowEmptyArray) {
    /** Target field within an empty array. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        BSON("" << BSONArray()),
        toBson(expression->evaluate(fromBson(BSON("a" << BSONArray())), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, NestedBelowArrayWithNull) {
    /** Target field within an array containing null. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        fromjson("{'':[]}"),
        toBson(expression->evaluate(fromBson(fromjson("{a:[null]}")), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, NestedBelowArrayWithUndefined) {
    /** Target field within an array containing undefined. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        fromjson("{'':[]}"),
        toBson(expression->evaluate(fromBson(fromjson("{a:[undefined]}")), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, NestedBelowArrayWithInt) {
    /** Target field within an array containing an integer. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        fromjson("{'':[]}"),
        toBson(expression->evaluate(fromBson(fromjson("{a:[1]}")), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, NestedWithinArray) {
    /** Target field within an array. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        fromjson("{'':[9]}"),
        toBson(expression->evaluate(fromBson(fromjson("{a:[{b:9}]}")), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, MultipleArrayValues) {
    /** Multiple value types within an array. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(
        fromjson("{'':[9,20]}"),
        toBson(expression->evaluate(
            fromBson(fromjson("{a:[{b:9},null,undefined,{g:4},{b:20},{}]}")), &expCtx.variables)));
}

TEST_F(EvaluateFieldPathTest, ExpandNestedArrays) {
    /** Expanding values within nested arrays. */
    auto expCtx = ExpressionContextForTest{};
    intrusive_ptr<Expression> expression =
        ExpressionFieldPath::createPathFromString(&expCtx, "a.b.c", expCtx.variablesParseState);
    ASSERT_BSONOBJ_BINARY_EQ(fromjson("{'':[[1,2],3,[4],[[5]],[6,7]]}"),
                             toBson(expression->evaluate(fromBson(fromjson("{a:[{b:[{c:1},{c:2}]},"
                                                                           "{b:{c:3}},"
                                                                           "{b:[{c:4}]},"
                                                                           "{b:[{c:[5]}]},"
                                                                           "{b:{c:[6,7]}}]}")),
                                                         &expCtx.variables)));
}

}  // namespace expression_evaluation_test
}  // namespace mongo
