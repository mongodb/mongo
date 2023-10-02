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
#include "mongo/db/matcher/expression_bitset_tree_converter.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/boolean_simplification/bitset_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
using mongo::boolean_simplification::BitsetTreeNode;
using mongo::boolean_simplification::makeBitsetTerm;
using mongo::boolean_simplification::Minterm;

namespace {
std::pair<boolean_simplification::BitsetTreeNode, std::vector<ExpressionBitInfo>>
transformToBitsetTree(const MatchExpression* root) {
    auto result = transformToBitsetTree(root, std::numeric_limits<size_t>::max());
    ASSERT_TRUE(result.has_value());
    return std::move(*result);
}

inline void assertExprInfo(const std::vector<ExpressionBitInfo>& expected,
                           const std::vector<ExpressionBitInfo>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_TRUE(expected[i].expression->equivalent(actual[i].expression.get()))
            << expected[i].expression->debugString()
            << " != " << actual[i].expression->debugString();
    }
}
}  // namespace

TEST(BitsetTreeConverterTests, AlwaysTrue) {
    AndMatchExpression expr{};

    std::vector<ExpressionBitInfo> expectedExpressions{};

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren.resize(expectedExpressions.size());

    const auto& [tree, expressions] = transformToBitsetTree(&expr);
    ASSERT_EQ(expectedTree, tree);
    assertExprInfo(expectedExpressions, expressions);
}

TEST(BitsetTreeConverterTests, AlwaysFalse) {
    AlwaysFalseMatchExpression expr{};

    std::vector<ExpressionBitInfo> expectedExpressions{};

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, false};

    const auto& [tree, expressions] = transformToBitsetTree(&expr);
    ASSERT_EQ(expectedTree, tree);
    assertExprInfo(expectedExpressions, expressions);
}

TEST(BitsetTreeConverterTests, GtExpression) {
    auto operand = BSON("$gt" << 5);
    GTMatchExpression expr{"a"_sd, operand["$gt"]};

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(expr.clone());

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm("1", "1");

    const auto& [tree, expressions] = transformToBitsetTree(&expr);
    ASSERT_EQ(expectedTree, tree);
    assertExprInfo(expectedExpressions, expressions);
}

TEST(BitsetTreeConverterTests, AndExpression) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]);
    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(gtExpr->clone());
    expectedExpressions.emplace_back(eqExpr->clone());

    AndMatchExpression expr{};
    expr.add(std::move(gtExpr));
    expr.add(std::make_unique<NotMatchExpression>(std::move(eqExpr)));

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm("01", "11");

    const auto& [tree, expressions] = transformToBitsetTree(&expr);
    ASSERT_EQ(expectedTree, tree);
    assertExprInfo(expectedExpressions, expressions);
}

TEST(BitsetTreeConverterTests, OrExpression) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]);
    auto ltExpr = std::make_unique<LTMatchExpression>("c"_sd, thirdOperand["$lt"]);

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(gtExpr->clone());
    expectedExpressions.emplace_back(eqExpr->clone());
    expectedExpressions.emplace_back(ltExpr->clone());

    auto expr = std::make_unique<OrMatchExpression>();
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(gtExpr->clone());
        andExpr->add(std::make_unique<NotMatchExpression>(eqExpr->clone()));
        expr->add(std::move(andExpr));
    }
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(gtExpr->clone());
        andExpr->add(eqExpr->clone());
        andExpr->add(ltExpr->clone());
        expr->add(std::move(andExpr));
    }

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, false};
    expectedTree.leafChildren = makeBitsetTerm("000", "000");
    {
        BitsetTreeNode orOperand{BitsetTreeNode::And, false};
        orOperand.leafChildren = makeBitsetTerm("001", "011");
        expectedTree.internalChildren.emplace_back(std::move(orOperand));
    }
    {
        BitsetTreeNode orOperand{BitsetTreeNode::And, false};
        orOperand.leafChildren = makeBitsetTerm("111", "111");
        expectedTree.internalChildren.emplace_back(std::move(orOperand));
    }

    const auto& [tree, expressions] = transformToBitsetTree(expr.get());
    ASSERT_EQ(expectedTree, tree);
    assertExprInfo(expectedExpressions, expressions);
}

TEST(BitsetTreeConverterTests, NorExpression) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]);
    auto ltExpr = std::make_unique<LTMatchExpression>("c"_sd, thirdOperand["$lt"]);

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(gtExpr->clone());
    expectedExpressions.emplace_back(eqExpr->clone());
    expectedExpressions.emplace_back(ltExpr->clone());

    auto expr = std::make_unique<NorMatchExpression>();
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(gtExpr->clone());
        andExpr->add(std::make_unique<NotMatchExpression>(eqExpr->clone()));
        expr->add(std::move(andExpr));
    }
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(gtExpr->clone());
        andExpr->add(eqExpr->clone());
        andExpr->add(ltExpr->clone());
        expr->add(std::move(andExpr));
    }

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, true};
    expectedTree.leafChildren = makeBitsetTerm("000", "000");
    {
        BitsetTreeNode orOperand{BitsetTreeNode::And, false};
        orOperand.leafChildren = makeBitsetTerm("001", "011");
        expectedTree.internalChildren.emplace_back(std::move(orOperand));
    }
    {
        BitsetTreeNode orOperand{BitsetTreeNode::And, false};
        orOperand.leafChildren = makeBitsetTerm("111", "111");
        expectedTree.internalChildren.emplace_back(std::move(orOperand));
    }

    const auto& [tree, expressions] = transformToBitsetTree(expr.get());
    ASSERT_EQ(expectedTree, tree);
    assertExprInfo(expectedExpressions, expressions);
}

// {a: $elemMatch: {$gt: 5, $eq: 10, $lt: 10}}
TEST(BitsetTreeConverterTests, ElemMatch) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);

    auto expr = std::make_unique<ElemMatchValueMatchExpression>("a"_sd);
    expr->add(std::make_unique<GTMatchExpression>(""_sd, firstOperand["$gt"]));
    expr->add(std::make_unique<EqualityMatchExpression>(""_sd, secondOperand["$eq"]));
    expr->add(std::make_unique<LTMatchExpression>(""_sd, thirdOperand["$lt"]));

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(expr->clone());

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm("1", "1");

    const auto& [tree, expressions] = transformToBitsetTree(expr.get());
    ASSERT_EQ(expectedTree, tree);
    assertExprInfo(expectedExpressions, expressions);
}

// {$and: [{a: {$elemMatch: {$not: {$gt: 21}}}}, {a: {$not: {$elemMatch: {$lt: 21}}}}]}
TEST(BitsetTreeConverterTests, TwoElemMatches) {
    auto operand = BSON("$gt" << 21 << "$lt" << 21);

    auto gt = std::make_unique<GTMatchExpression>(""_sd, operand["$gt"]);
    auto notGt = std::make_unique<NotMatchExpression>(gt->clone());
    auto elemMatchGt = std::make_unique<ElemMatchValueMatchExpression>("a"_sd);
    elemMatchGt->add(notGt->clone());

    auto lt = std::make_unique<GTMatchExpression>(""_sd, operand["$lt"]);
    auto elemMatchLt = std::make_unique<ElemMatchValueMatchExpression>("a"_sd);
    elemMatchLt->add(lt->clone());
    auto notElemMatchLt = std::make_unique<NotMatchExpression>(elemMatchLt->clone());

    auto expr = std::make_unique<AndMatchExpression>();
    expr->add(elemMatchGt->clone());
    expr->add(notElemMatchLt->clone());

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(elemMatchGt->clone());
    expectedExpressions.emplace_back(elemMatchLt->clone());

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm("01", "11");

    const auto& [tree, expressions] = transformToBitsetTree(expr.get());
    ASSERT_EQ(expectedTree, tree);
    assertExprInfo(expectedExpressions, expressions);
}

TEST(BitsetTreeConverterTests, TooManyPredicates) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]);
    auto ltExpr = std::make_unique<LTMatchExpression>("c"_sd, thirdOperand["$lt"]);


    auto expr = std::make_unique<OrMatchExpression>();
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(gtExpr->clone());
        andExpr->add(std::make_unique<NotMatchExpression>(eqExpr->clone()));
        expr->add(std::move(andExpr));
    }
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(gtExpr->clone());
        andExpr->add(eqExpr->clone());
        andExpr->add(ltExpr->clone());
        expr->add(std::move(andExpr));
    }

    const auto result = transformToBitsetTree(expr.get(), 2);
    ASSERT_FALSE(result.has_value());
}
}  // namespace mongo
