/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_restorer.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/boolean_simplification/bitset_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
using boolean_simplification::BitsetTreeNode;
using boolean_simplification::makeBitsetTerm;
using boolean_simplification::Maxterm;
using boolean_simplification::Minterm;

namespace {
inline void ASSERT_EXPR(const MatchExpression& expected,
                        const std::unique_ptr<MatchExpression>& actual) {
    ASSERT_TRUE(expected.equivalent(actual.get()))
        << expected.debugString() << " != " << actual->debugString();
}

std::unique_ptr<MatchExpression> restoreMatchExpression(
    const boolean_simplification::Maxterm& maxterm,
    const std::vector<ExpressionBitInfo>& expressions) {
    BitsetTreeNode root = boolean_simplification::convertToBitsetTree(maxterm);
    return restoreMatchExpression(root, expressions);
}
}  // namespace

TEST(RestoreSingleMatchExpressionTests, AlwaysTrue) {
    std::vector<ExpressionBitInfo> expressions{};
    Maxterm maxterm{Minterm{expressions.size()}};
    AndMatchExpression expectedExpr{};

    auto expr = restoreMatchExpression(maxterm, expressions);
    ASSERT_EXPR(expectedExpr, expr);
}

TEST(RestoreSingleMatchExpressionTests, AlwaysFalse) {
    std::vector<ExpressionBitInfo> expressions{};
    Maxterm maxterm{expressions.size()};
    AlwaysFalseMatchExpression expectedExpr{};

    auto expr = restoreMatchExpression(maxterm, expressions);
    ASSERT_EXPR(expectedExpr, expr);
}

TEST(RestoreSingleMatchExpressionTests, GtExpression) {
    auto operand = BSON("$gt" << 5);
    std::vector<ExpressionBitInfo> expressions{};
    expressions.emplace_back(std::make_unique<GTMatchExpression>("a"_sd, operand["$gt"]));

    Maxterm maxterm{
        {"1", "1"},
    };

    GTMatchExpression expectedExpr{"a"_sd, operand["$gt"]};

    auto expr = restoreMatchExpression(maxterm, expressions);
    ASSERT_EXPR(expectedExpr, expr);
}

TEST(RestoreSingleMatchExpressionTests, AndExpression) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    std::vector<ExpressionBitInfo> expressions{};
    expressions.emplace_back(
        ExpressionBitInfo{std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"])});
    expressions.emplace_back(
        ExpressionBitInfo{std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"])});

    Maxterm maxterm{
        {"01", "11"},
    };

    AndMatchExpression expectedExpr{};
    expectedExpr.add(std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]));
    expectedExpr.add(std::make_unique<NotMatchExpression>(
        std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"])));

    auto expr = restoreMatchExpression(maxterm, expressions);
    ASSERT_EXPR(expectedExpr, expr);
}

TEST(RestoreSingleMatchExpressionTests, OrExpression) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);
    std::vector<ExpressionBitInfo> expressions{};
    expressions.emplace_back(
        ExpressionBitInfo{std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"])});
    expressions.emplace_back(
        ExpressionBitInfo{std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"])});
    expressions.emplace_back(
        ExpressionBitInfo{std::make_unique<LTMatchExpression>("c"_sd, thirdOperand["$lt"])});

    Maxterm maxterm{
        {"001", "011"},
        {"111", "111"},
    };

    auto and1 = std::make_unique<AndMatchExpression>();
    and1->add(std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]));
    and1->add(std::make_unique<NotMatchExpression>(
        std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"])));

    auto and2 = std::make_unique<AndMatchExpression>();
    and2->add(std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]));
    and2->add(std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]));
    and2->add(std::make_unique<LTMatchExpression>("c"_sd, thirdOperand["$lt"]));

    OrMatchExpression expectedExpr{};
    expectedExpr.add(std::move(and1));
    expectedExpr.add(std::move(and2));

    auto expr = restoreMatchExpression(maxterm, expressions);
    ASSERT_EXPR(expectedExpr, expr);
}

TEST(RestoreSingleMatchExpressionTests, NorExpression) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);

    auto firstExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto secondExpr = std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]);
    auto thirdExpr = std::make_unique<LTMatchExpression>("c"_sd, thirdOperand["$lt"]);
    std::vector<ExpressionBitInfo> expressions{};
    expressions.emplace_back(ExpressionBitInfo{firstExpr->clone()});
    expressions.emplace_back(ExpressionBitInfo{secondExpr->clone()});
    expressions.emplace_back(ExpressionBitInfo{thirdExpr->clone()});

    Maxterm maxterm{
        Minterm{"000", "001"},
        Minterm{"000", "011"},
        Minterm{"000", "101"},
        Minterm{"010", "011"},
        Minterm{"010", "110"},
    };

    OrMatchExpression expectedExpr{};
    expectedExpr.add(std::make_unique<NotMatchExpression>(firstExpr->clone()));
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(std::make_unique<NotMatchExpression>(firstExpr->clone()));
        andExpr->add(std::make_unique<NotMatchExpression>(secondExpr->clone()));
        expectedExpr.add(std::move(andExpr));
    }
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(std::make_unique<NotMatchExpression>(firstExpr->clone()));
        andExpr->add(std::make_unique<NotMatchExpression>(thirdExpr->clone()));
        expectedExpr.add(std::move(andExpr));
    }
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(std::make_unique<NotMatchExpression>(firstExpr->clone()));
        andExpr->add(secondExpr->clone());
        expectedExpr.add(std::move(andExpr));
    }
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(secondExpr->clone());
        andExpr->add(std::make_unique<NotMatchExpression>(thirdExpr->clone()));
        expectedExpr.add(std::move(andExpr));
    }

    auto expr = restoreMatchExpression(maxterm, expressions);
    ASSERT_EXPR(expectedExpr, expr);
}

// {a: $elemMatch: {$gt: 5, $eq: 10, $lt: 10}}
TEST(RestoreSingleMatchExpressionTests, ElemMatch) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);

    auto expr = std::make_unique<ElemMatchValueMatchExpression>("a"_sd);
    expr->add(std::make_unique<GTMatchExpression>(""_sd, firstOperand["$gt"]));
    expr->add(std::make_unique<EqualityMatchExpression>(""_sd, secondOperand["$eq"]));
    expr->add(std::make_unique<LTMatchExpression>(""_sd, thirdOperand["$lt"]));

    std::vector<ExpressionBitInfo> expressions{};
    expressions.emplace_back(ExpressionBitInfo{expr->clone()});

    Maxterm maxterm{
        Minterm{"1", "1"},
    };

    auto restored = restoreMatchExpression(maxterm, expressions);
    ASSERT_EXPR(*expr, restored);
}

// {a: $elemMatch: {b: {$gt: 5, $eq: 10, $lt: 10}}}
TEST(RestoreSingleMatchExpressionTests, ElemMatchObject) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);

    auto child = std::make_unique<AndMatchExpression>();
    child->add(std::make_unique<GTMatchExpression>("b"_sd, firstOperand["$gt"]));
    child->add(std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]));
    child->add(std::make_unique<LTMatchExpression>("b"_sd, thirdOperand["$lt"]));

    auto expr = std::make_unique<ElemMatchObjectMatchExpression>("a"_sd, std::move(child));

    std::vector<ExpressionBitInfo> expressions{};
    expressions.emplace_back(ExpressionBitInfo{expr->clone()});

    Maxterm maxterm{
        Minterm{"1", "1"},
    };

    auto restored = restoreMatchExpression(maxterm, expressions);
    ASSERT_EXPR(*expr, restored);
}
}  // namespace mongo
