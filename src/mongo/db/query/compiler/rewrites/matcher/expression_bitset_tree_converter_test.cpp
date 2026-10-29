// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/compiler/rewrites/matcher/expression_bitset_tree_converter.h"

#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/compiler/rewrites/boolean_simplification/bitset_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
using namespace std::literals::string_view_literals;
using mongo::boolean_simplification::BitsetTreeNode;
using mongo::boolean_simplification::makeBitsetTerm;
using mongo::boolean_simplification::Minterm;

namespace {
BitsetTreeTransformResult transformToBitsetTreeTest(const MatchExpression* root) {
    auto result = transformToBitsetTree(root, std::numeric_limits<size_t>::max());
    ASSERT_TRUE(result.has_value());
    return std::move(*result);
}

inline void assertExprInfo(const BitsetTreeTransformResult::ExpressionList& expected,
                           const BitsetTreeTransformResult::ExpressionList& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_TRUE(expected[i].expression->equivalent(actual[i].expression))
            << expected[i].expression->debugString()
            << " != " << actual[i].expression->debugString();
    }
}
}  // namespace

TEST(BitsetTreeConverterTests, AlwaysTrue) {
    AndMatchExpression expr{};

    BitsetTreeTransformResult::ExpressionList expectedExpressions{};

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};

    const auto result = transformToBitsetTreeTest(&expr);
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(1, result.expressionSize);
}

TEST(BitsetTreeConverterTests, AlwaysFalse) {
    AlwaysFalseMatchExpression expr{};

    BitsetTreeTransformResult::ExpressionList expectedExpressions{};

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, false};

    const auto result = transformToBitsetTreeTest(&expr);
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(1, result.expressionSize);
}

TEST(BitsetTreeConverterTests, NorOfAlwaysFalse) {
    NorMatchExpression expr{};
    expr.add(std::make_unique<AlwaysFalseMatchExpression>());

    BitsetTreeTransformResult::ExpressionList expectedExpressions{};

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, true};

    const auto result = transformToBitsetTreeTest(&expr);
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(2, result.expressionSize);
}

TEST(BitsetTreeConverterTests, NorOfAlwaysTrue) {
    NorMatchExpression expr{};
    expr.add(std::make_unique<AlwaysTrueMatchExpression>());

    BitsetTreeTransformResult::ExpressionList expectedExpressions{};

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, false};

    const auto result = transformToBitsetTreeTest(&expr);
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(2, result.expressionSize);
}

TEST(BitsetTreeConverterTests, AlwaysTrueNorAlwaysFalse) {
    NorMatchExpression expr{};
    expr.add(std::make_unique<AlwaysTrueMatchExpression>());
    expr.add(std::make_unique<AlwaysFalseMatchExpression>());

    BitsetTreeTransformResult::ExpressionList expectedExpressions{};

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, false};

    const auto result = transformToBitsetTreeTest(&expr);
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(2, result.expressionSize);  // finish earlier
}

// [$nor: {a: 1}, {a: {$ne: 1}}] == always false
TEST(BitsetTreeConverterTests, NeNorEq) {
    auto operand = BSON("$eq" << 1);
    auto eq = std::make_unique<EqualityMatchExpression>("a"sv, operand["$eq"]);
    NorMatchExpression expr{};
    expr.add(eq->clone());
    expr.add(std::make_unique<NotMatchExpression>(eq->clone()));

    BitsetTreeTransformResult::ExpressionList expectedExpressions{ExpressionBitInfo{eq.get()}};

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, false};

    const auto result = transformToBitsetTreeTest(&expr);
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(3, result.expressionSize);
}

TEST(BitsetTreeConverterTests, NotAlwaysTrue) {
    NotMatchExpression expr{std::make_unique<AlwaysTrueMatchExpression>()};

    BitsetTreeTransformResult::ExpressionList expectedExpressions{};

    BitsetTreeNode expectedTree{BitsetTreeNode::Or, false};

    const auto result = transformToBitsetTreeTest(&expr);
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(1, result.expressionSize);
}

TEST(BitsetTreeConverterTests, NotAlwaysFalse) {
    NotMatchExpression expr{std::make_unique<AlwaysFalseMatchExpression>()};

    BitsetTreeTransformResult::ExpressionList expectedExpressions{};

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};

    const auto result = transformToBitsetTreeTest(&expr);
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(1, result.expressionSize);
}

TEST(BitsetTreeConverterTests, GtExpression) {
    auto operand = BSON("$gt" << 5);
    GTMatchExpression expr{"a"sv, operand["$gt"]};

    BitsetTreeTransformResult::ExpressionList expectedExpressions{ExpressionBitInfo{&expr}};

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm("1", "1");

    const auto result = transformToBitsetTreeTest(&expr);
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(1, result.expressionSize);
}

TEST(BitsetTreeConverterTests, AndExpression) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"sv, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"sv, secondOperand["$eq"]);
    BitsetTreeTransformResult::ExpressionList expectedExpressions{
        ExpressionBitInfo{gtExpr.get()},
        ExpressionBitInfo{eqExpr.get()},
    };

    AndMatchExpression expr{};
    expr.add(gtExpr->clone());
    expr.add(std::make_unique<NotMatchExpression>(eqExpr->clone()));

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm("01", "11");

    const auto result = transformToBitsetTreeTest(&expr);
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(3, result.expressionSize);
}

TEST(BitsetTreeConverterTests, OrExpression) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"sv, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"sv, secondOperand["$eq"]);
    auto ltExpr = std::make_unique<LTMatchExpression>("c"sv, thirdOperand["$lt"]);

    BitsetTreeTransformResult::ExpressionList expectedExpressions{
        ExpressionBitInfo{gtExpr.get()},
        ExpressionBitInfo{eqExpr.get()},
        ExpressionBitInfo{ltExpr.get()},
    };

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

    const auto result = transformToBitsetTreeTest(expr.get());
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(8, result.expressionSize);
}

TEST(BitsetTreeConverterTests, NorExpression) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"sv, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"sv, secondOperand["$eq"]);
    auto ltExpr = std::make_unique<LTMatchExpression>("c"sv, thirdOperand["$lt"]);

    BitsetTreeTransformResult::ExpressionList expectedExpressions{
        ExpressionBitInfo{gtExpr.get()},
        ExpressionBitInfo{eqExpr.get()},
        ExpressionBitInfo{ltExpr.get()},
    };

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

    const auto result = transformToBitsetTreeTest(expr.get());
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(8, result.expressionSize);
}

// {a: $elemMatch: {$gt: 5, $eq: 10, $lt: 10}}
TEST(BitsetTreeConverterTests, ElemMatch) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);

    auto expr = std::make_unique<ElemMatchValueMatchExpression>("a"sv);
    expr->add(std::make_unique<GTMatchExpression>(""sv, firstOperand["$gt"]));
    expr->add(std::make_unique<EqualityMatchExpression>(""sv, secondOperand["$eq"]));
    expr->add(std::make_unique<LTMatchExpression>(""sv, thirdOperand["$lt"]));

    BitsetTreeTransformResult::ExpressionList expectedExpressions{ExpressionBitInfo{expr.get()}};

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm("1", "1");

    const auto result = transformToBitsetTreeTest(expr.get());
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(1, result.expressionSize);
}

// {$and: [{a: {$elemMatch: {$not: {$gt: 21}}}}, {a: {$not: {$elemMatch: {$lt: 21}}}}]}
TEST(BitsetTreeConverterTests, TwoElemMatches) {
    auto operand = BSON("$gt" << 21 << "$lt" << 21);

    auto gt = std::make_unique<GTMatchExpression>(""sv, operand["$gt"]);
    auto notGt = std::make_unique<NotMatchExpression>(gt->clone());
    auto elemMatchGt = std::make_unique<ElemMatchValueMatchExpression>("a"sv);
    elemMatchGt->add(notGt->clone());

    auto lt = std::make_unique<GTMatchExpression>(""sv, operand["$lt"]);
    auto elemMatchLt = std::make_unique<ElemMatchValueMatchExpression>("a"sv);
    elemMatchLt->add(lt->clone());
    auto notElemMatchLt = std::make_unique<NotMatchExpression>(elemMatchLt->clone());

    auto expr = std::make_unique<AndMatchExpression>();
    expr->add(elemMatchGt->clone());
    expr->add(notElemMatchLt->clone());

    BitsetTreeTransformResult::ExpressionList expectedExpressions{
        ExpressionBitInfo{elemMatchGt.get()},
        ExpressionBitInfo{elemMatchLt.get()},
    };

    BitsetTreeNode expectedTree{BitsetTreeNode::And, false};
    expectedTree.leafChildren = makeBitsetTerm("01", "11");

    const auto result = transformToBitsetTreeTest(expr.get());
    ASSERT_EQ(expectedTree, result.bitsetTree);
    assertExprInfo(expectedExpressions, result.expressions);
    ASSERT_EQ(3, result.expressionSize);
}

TEST(BitsetTreeConverterTests, TooManyPredicates) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"sv, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"sv, secondOperand["$eq"]);
    auto ltExpr = std::make_unique<LTMatchExpression>("c"sv, thirdOperand["$lt"]);


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
