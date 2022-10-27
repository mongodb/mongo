/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression_parameterization.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {
void walkExpression(MatchExpressionParameterizationVisitorContext* context,
                    MatchExpression* expression) {
    MatchExpressionParameterizationVisitor visitor{context};
    MatchExpressionParameterizationWalker walker{&visitor};
    tree_walker::walk<false, MatchExpression>(expression, &walker);
}
}  // namespace

TEST(MatchExpressionParameterizationVisitor, AlwaysFalseMatchExpressionSetsNoParamIds) {
    AlwaysFalseMatchExpression expr{};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, AlwaysTrueMatchExpressionSetsNoParamIds) {
    AlwaysTrueMatchExpression expr{};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, BitsAllClearMatchExpressionSetsTwoParamIds) {
    std::vector<uint32_t> bitPositions;
    BitsAllClearMatchExpression expr{"a"_sd, bitPositions};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, BitsAllSetMatchExpressionSetsTwoParamIds) {
    std::vector<uint32_t> bitPositions;
    BitsAllSetMatchExpression expr{"a"_sd, bitPositions};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, BitsAnyClearMatchExpressionSetsTwoParamIds) {
    std::vector<uint32_t> bitPositions{0, 1, 8};
    BitsAnyClearMatchExpression expr{"a"_sd, bitPositions};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, BitsAnySetMatchExpressionSetsTwoParamIds) {
    std::vector<uint32_t> bitPositions{0, 1, 8};
    BitsAnySetMatchExpression expr{"a"_sd, bitPositions};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor,
     EqualityMatchExpressionWithScalarParameterSetsOneParamId) {
    BSONObj query = BSON("a" << 5);
    EqualityMatchExpression eq("a"_sd, query["a"]);
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    eq.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, EqualityMatchExpressionWithNullSetsNoParamIds) {
    BSONObj query = BSON("a" << BSONNULL);
    EqualityMatchExpression eq{"a"_sd, query["a"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    eq.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, EqualityMatchExpressionWithArraySetsNoParamIds) {
    BSONObj query = BSON("a" << BSON_ARRAY(1 << 2));
    EqualityMatchExpression eq{"a"_sd, query["a"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    eq.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, EqualityMatchExpressionWithMinKeySetsNoParamIds) {
    BSONObj query = BSON("a" << MINKEY);
    EqualityMatchExpression eq{"a"_sd, query["a"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    eq.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, EqualityMatchExpressionWithMaxKeySetsNoParamIds) {
    BSONObj query = BSON("a" << MAXKEY);
    EqualityMatchExpression eq{"a"_sd, query["a"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    eq.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, EqualityMatchExpressionWithUndefinedThrows) {
    BSONObj query = BSON("a" << BSONUndefined);
    ASSERT_THROWS((EqualityMatchExpression{"a"_sd, query["a"]}), DBException);
}

TEST(MatchExpressionParameterizationVisitor, GTEMatchExpressionWithScalarParameterSetsOneParamId) {
    BSONObj query = BSON("$gte" << 5);
    GTEMatchExpression expr{"a"_sd, query["$gte"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, GTEMatchExpressionWithUndefinedThrows) {
    BSONObj query = BSON("a" << BSONUndefined);
    ASSERT_THROWS((EqualityMatchExpression{"a"_sd, query["a"]}), DBException);
}

TEST(MatchExpressionParameterizationVisitor, GTMatchExpressionWithScalarParameterSetsOneParamId) {
    BSONObj query = BSON("$gte" << 5);
    GTMatchExpression expr{"a"_sd, query["$gte"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, LTEMatchExpressionWithScalarParameterSetsOneParamId) {
    BSONObj query = BSON("$lte" << 5);
    LTEMatchExpression expr("a"_sd, query["$lte"]);
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, LTMatchExpressionWithScalarParameterSetsOneParamId) {
    BSONObj query = BSON("$lt" << 5);
    LTMatchExpression expr{"a"_sd, query["$lt"]};
    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, ComparisonMatchExpressionsWithNaNSetsNoParamIds) {
    std::vector<std::unique_ptr<MatchExpression>> expressions;

    BSONObj doubleNaN = BSON("$lt" << std::numeric_limits<double>::quiet_NaN());
    expressions.emplace_back(std::make_unique<LTMatchExpression>("a"_sd, doubleNaN["$lt"]));

    BSONObj decimalNegativeNaN = BSON("$gt" << Decimal128::kNegativeNaN);
    expressions.emplace_back(
        std::make_unique<GTMatchExpression>("b"_sd, decimalNegativeNaN["$gt"]));

    BSONObj decimalPositiveNaN = BSON("c" << Decimal128::kPositiveNaN);
    expressions.emplace_back(
        std::make_unique<EqualityMatchExpression>("c"_sd, decimalPositiveNaN["c"]));

    OrMatchExpression expr{std::move(expressions)};

    MatchExpressionParameterizationVisitorContext context{};
    walkExpression(&context, &expr);

    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, InMatchExpressionWithScalarsSetsOneParamId) {
    BSONObj operand = BSON_ARRAY(1 << "r" << true << 1.1);
    InMatchExpression expr{"a"_sd};
    std::vector<BSONElement> equalities{operand[0], operand[1], operand[2], operand[3]};
    ASSERT_OK(expr.setEqualities(std::move(equalities)));

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, InMatchExpressionWithNullSetsNoParamIds) {
    BSONObj operand = BSON_ARRAY(1 << "r" << true << BSONNULL);
    InMatchExpression expr{"a"_sd};
    std::vector<BSONElement> equalities{operand[0], operand[1], operand[2], operand[3]};
    ASSERT_OK(expr.setEqualities(std::move(equalities)));

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, InMatchExpressionWithRegexSetsNoParamIds) {
    BSONObj query = BSON("a" << BSON("$in" << BSON_ARRAY(BSONRegEx("/^regex/i"))));

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    MatchExpressionParameterizationVisitorContext context{};
    walkExpression(&context, result.getValue().get());
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, ModMatchExpressionSetsTwoParamIds) {
    ModMatchExpression expr{"a"_sd, 1, 2};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, RegexMatchExpressionSetsTwoParamIds) {
    RegexMatchExpression expr{""_sd, "b", ""};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(2, context.inputParamIdToExpressionMap.size());
    ASSERT_EQ(2, context.nextInputParamId(nullptr));
}

TEST(MatchExpressionParameterizationVisitor, SizeMatchExpressionSetsOneParamId) {
    SizeMatchExpression expr{"a"_sd, 2};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(1, context.inputParamIdToExpressionMap.size());
    ASSERT_EQ(1, context.nextInputParamId(nullptr));
}

TEST(MatchExpressionParameterizationVisitor, TypeMatchExpressionWithStringSetsOneParamId) {
    TypeMatchExpression expr{"a"_sd, BSONType::String};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    // TODO SERVER-64776: fix the test case
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, TypeMatchExpressionWithArraySetsNoParamIds) {
    TypeMatchExpression expr{"a"_sd, BSONType::Array};

    MatchExpressionParameterizationVisitorContext context{};
    MatchExpressionParameterizationVisitor visitor{&context};
    expr.acceptVisitor(&visitor);
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor, ExprMatchExpressionSetsNoParamsIds) {
    BSONObj query = BSON("$expr" << BSON("$gte" << BSON_ARRAY("$a"
                                                              << "$b")));

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    MatchExpressionParameterizationVisitorContext context{};
    walkExpression(&context, result.getValue().get());
    ASSERT_EQ(0, context.inputParamIdToExpressionMap.size());
}

TEST(MatchExpressionParameterizationVisitor,
     AutoParametrizationWalkerSetsCorrectNumberOfParamsIds) {
    BSONObj equalityExpr = BSON("x" << 1);
    BSONObj gtExpr = BSON("y" << BSON("$gt" << 2));
    BSONObj inExpr = BSON("$in" << BSON_ARRAY("a"
                                              << "b"
                                              << "c"));
    BSONObj regexExpr = BSON("m" << BSONRegEx("/^regex/i"));
    BSONObj sizeExpr = BSON("n" << BSON("$size" << 1));

    BSONObj query = BSON("$or" << BSON_ARRAY(equalityExpr
                                             << gtExpr << BSON("z" << inExpr)
                                             << BSON("$and" << BSON_ARRAY(regexExpr << sizeExpr))));

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    StatusWithMatchExpression result = MatchExpressionParser::parse(query, expCtx);
    ASSERT_TRUE(result.isOK());

    MatchExpressionParameterizationVisitorContext context{};
    walkExpression(&context, result.getValue().get());
    ASSERT_EQ(6, context.inputParamIdToExpressionMap.size());
}
}  // namespace mongo
