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

#include "mongo/db/query/compiler/rewrites/matcher/expression_restorer.h"

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
using boolean_simplification::BitsetTreeNode;
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
    const BitsetTreeTransformResult::ExpressionList& expressions) {
    BitsetTreeNode root = boolean_simplification::convertToBitsetTree(maxterm);
    return restoreMatchExpression(root, expressions);
}
}  // namespace

DEATH_TEST_REGEX(RestoreSingleMatchExpressionTests,
                 AssertOnRestoringNegativeNodes,
                 "Tripwire assertion.*8163020") {
    auto operand = BSON("$gt" << 5);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"_sd, operand["$gt"]);
    BitsetTreeTransformResult::ExpressionList expressions{ExpressionBitInfo{gtExpr.get()}};

    BitsetTreeNode root{BitsetTreeNode::And, /* isNegated */ true};
    root.leafChildren.set(0, true);

    restoreMatchExpression(root, expressions);
}

TEST(RestoreSingleMatchExpressionTests, AlwaysTrue) {
    BitsetTreeTransformResult::ExpressionList expressions{};

    BitsetTreeNode root{BitsetTreeNode::And, /* isNegated */ false};

    AndMatchExpression expectedExpr{};

    auto expr = restoreMatchExpression(root, expressions);
    ASSERT_EXPR(expectedExpr, expr);
}

TEST(RestoreSingleMatchExpressionTests, NotAlwaysTrue) {
    BitsetTreeTransformResult::ExpressionList expressions{};

    BitsetTreeNode root{BitsetTreeNode::And, /* isNegated */ true};

    // We cannot restore negative nodes, so let's transform them first.
    auto dnf = boolean_simplification::convertToDNF(root, 10000);
    ASSERT_TRUE(dnf);
    auto transformedBitsetTree = boolean_simplification::convertToBitsetTree(*dnf);

    AlwaysFalseMatchExpression expectedExpr{};

    auto expr = restoreMatchExpression(transformedBitsetTree, expressions);
    ASSERT_EXPR(expectedExpr, expr);
}

TEST(RestoreSingleMatchExpressionTests, AlwaysFalse) {
    BitsetTreeTransformResult::ExpressionList expressions{};

    BitsetTreeNode root{BitsetTreeNode::Or, /* isNegated */ false};

    AlwaysFalseMatchExpression expectedExpr{};

    auto expr = restoreMatchExpression(root, expressions);
    ASSERT_EXPR(expectedExpr, expr);
}

TEST(RestoreSingleMatchExpressionTests, NotAlwaysFalse) {
    BitsetTreeTransformResult::ExpressionList expressions{};

    BitsetTreeNode root{BitsetTreeNode::Or, /* isNegated */ true};

    // We cannot restore negative nodes, so let's transform them first.
    auto dnf = boolean_simplification::convertToDNF(root, 1000);
    ASSERT_TRUE(dnf);
    auto transformedBitsetTree = boolean_simplification::convertToBitsetTree(*dnf);

    AndMatchExpression expectedExpr{};

    auto expr = restoreMatchExpression(transformedBitsetTree, expressions);
    ASSERT_EXPR(expectedExpr, expr);
}

TEST(RestoreSingleMatchExpressionTests, GtExpression) {
    auto operand = BSON("$gt" << 5);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"_sd, operand["$gt"]);
    BitsetTreeTransformResult::ExpressionList expressions{ExpressionBitInfo{gtExpr.get()}};

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
    auto gtExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]);
    BitsetTreeTransformResult::ExpressionList expressions{
        ExpressionBitInfo{gtExpr.get()},
        ExpressionBitInfo{eqExpr.get()},
    };

    Maxterm maxterm{
        {"01", "11"},
    };

    AndMatchExpression expectedExpr{};
    expectedExpr.add(gtExpr->clone());
    expectedExpr.add(std::make_unique<NotMatchExpression>(eqExpr->clone()));

    auto expr = restoreMatchExpression(maxterm, expressions);
    ASSERT_EXPR(expectedExpr, expr);
}

TEST(RestoreSingleMatchExpressionTests, OrExpression) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]);
    auto ltExpr = std::make_unique<LTMatchExpression>("c"_sd, thirdOperand["$lt"]);
    BitsetTreeTransformResult::ExpressionList expressions{
        ExpressionBitInfo{gtExpr.get()},
        ExpressionBitInfo{eqExpr.get()},
        ExpressionBitInfo{ltExpr.get()},
    };

    Maxterm maxterm{
        {"001", "011"},
        {"111", "111"},
    };

    auto and1 = std::make_unique<AndMatchExpression>();
    and1->add(gtExpr->clone());
    and1->add(std::make_unique<NotMatchExpression>(eqExpr->clone()));

    auto and2 = std::make_unique<AndMatchExpression>();
    and2->add(gtExpr->clone());
    and2->add(eqExpr->clone());
    and2->add(ltExpr->clone());

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
    BitsetTreeTransformResult::ExpressionList expressions{
        ExpressionBitInfo{firstExpr.get()},
        ExpressionBitInfo{secondExpr.get()},
        ExpressionBitInfo{thirdExpr.get()},
    };

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

    BitsetTreeTransformResult::ExpressionList expressions{ExpressionBitInfo{expr.get()}};

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

    BitsetTreeTransformResult::ExpressionList expressions{ExpressionBitInfo{expr.get()}};

    Maxterm maxterm{
        Minterm{"1", "1"},
    };

    auto restored = restoreMatchExpression(maxterm, expressions);
    ASSERT_EXPR(*expr, restored);
}
}  // namespace mongo
