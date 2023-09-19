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

#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_dnf_converter.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

using boolean_simplification::Maxterm;
using boolean_simplification::Minterm;

namespace {
inline void assertExprInfo(const std::vector<ExpressionBitInfo>& expected,
                           const std::vector<ExpressionBitInfo>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_TRUE(expected[i].expression->equivalent(actual[i].expression.get()))
            << expected[i].expression->debugString()
            << " != " << actual[i].expression->debugString();
    }
}

inline void assertDNF(const std::pair<Maxterm, std::vector<ExpressionBitInfo>>& expected,
                      const std::pair<Maxterm, std::vector<ExpressionBitInfo>>& actual) {
    ASSERT_EQ(expected.first, actual.first);
    assertExprInfo(expected.second, actual.second);
}

std::vector<ExpressionBitInfo> makeExpressions(const MatchExpression& expr) {
    std::vector<ExpressionBitInfo> expressions{};
    expressions.emplace_back(expr.clone());
    return expressions;
}

std::vector<ExpressionBitInfo> makeExpressions(std::unique_ptr<MatchExpression> expr) {
    std::vector<ExpressionBitInfo> expressions{};
    expressions.emplace_back(std::move(expr));
    return expressions;
}
}  // namespace

TEST(ExpressionDNFConverterTests, SimpleEq) {
    BSONObj operand = BSON("a" << 5);
    EqualityMatchExpression eq("a"_sd, operand["a"]);

    auto expected = std::make_pair(Maxterm{Minterm{"1", "1"}}, makeExpressions(eq));

    auto dnfs = transformToDNF(&eq);

    assertDNF(expected, dnfs);
}

TEST(ExpressionDNFConverterTests, SimpleNe) {
    auto baseOperand = BSON("$eq" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("a"_sd, baseOperand["$eq"]);
    auto expr = NotMatchExpression{eq.release()};

    auto dnfs = transformToDNF(&expr);

    auto expected = std::make_pair(Maxterm{Minterm{"0", "1"}}, makeExpressions(*expr.getChild(0)));

    assertDNF(expected, dnfs);
}

TEST(ExpressionDNFConverterTests, SimpleLt) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a"_sd, baseOperand["$lt"]);
    auto expr = NotMatchExpression{lt.release()};

    auto expected = std::make_pair(
        Maxterm{Minterm{"0", "1"}},
        makeExpressions(std::make_unique<LTMatchExpression>("a"_sd, baseOperand["$lt"])));

    auto dnfs = transformToDNF(&expr);
    assertDNF(expected, dnfs);
}

TEST(ExpressionDNFConverterTests, MultikeyLt) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a"_sd, baseOperand["$lt"]);
    auto expr = NotMatchExpression{lt.release()};

    auto expected = std::make_pair(Maxterm{Minterm{"0", "1"}}, makeExpressions(*expr.getChild(0)));

    auto dnfs = transformToDNF(&expr);
    assertDNF(expected, dnfs);
}

// a > 10 | b <= 5
TEST(ExpressionDNFConverterTests, Or) {
    auto firstOperand = BSON("$gt" << 10);
    auto secondOperand = BSON("$lte" << 5);
    auto firstExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto secondExpr = std::make_unique<LTEMatchExpression>("b"_sd, secondOperand["$lte"]);
    OrMatchExpression expr{};
    expr.add(std::move(firstExpr));
    expr.add(std::move(secondExpr));

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(
        ExpressionBitInfo{std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"])});
    expectedExpressions.emplace_back(
        ExpressionBitInfo{std::make_unique<LTEMatchExpression>("b"_sd, secondOperand["$lte"])});

    auto expected = std::make_pair(Maxterm{{
                                       Minterm{"01", "01"},
                                       Minterm{"10", "10"},
                                   }},
                                   std::move(expectedExpressions));

    auto dnfs = transformToDNF(&expr);
    assertDNF(expected, dnfs);
}

TEST(ExpressionDNFConverterTests, Nor) {
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

    auto expected = std::make_pair(Maxterm{{
                                       Minterm{"000", "001"},
                                       Minterm{"000", "011"},
                                       Minterm{"000", "101"},
                                       Minterm{"010", "011"},
                                       Minterm{"010", "110"},
                                   }},
                                   std::move(expectedExpressions));
    auto dnfs = transformToDNF(expr.get());
    assertDNF(expected, dnfs);
}

// a > 10 & b <= 5
TEST(ExpressionDNFConverterTests, And) {
    auto firstOperand = BSON("$gt" << 10);
    auto secondOperand = BSON("$lte" << 5);
    auto firstExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto secondExpr = std::make_unique<LTEMatchExpression>("b"_sd, secondOperand["$lte"]);
    AndMatchExpression expr{};
    expr.add(std::move(firstExpr));
    expr.add(std::move(secondExpr));

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(
        ExpressionBitInfo{std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"])});
    expectedExpressions.emplace_back(
        ExpressionBitInfo{std::make_unique<LTEMatchExpression>("b"_sd, secondOperand["$lte"])});

    auto expected = std::make_pair(Maxterm{{Minterm{"11", "11"}}}, std::move(expectedExpressions));

    auto dnfs = transformToDNF(&expr);
    assertDNF(expected, dnfs);
}

// ~((a > 1 | b > 1) & (a < 2 | b < 2))
TEST(ExpressionDNFConverterTests, MultikeyNotExpression) {
    auto operand1 = BSON("$gt" << 1);
    auto operand2 = BSON("$lt" << 2);

    auto gtA = std::make_unique<GTMatchExpression>("a"_sd, operand1["$gt"]);
    auto gtB = std::make_unique<GTMatchExpression>("b"_sd, operand1["$gt"]);

    auto ltA = std::make_unique<LTMatchExpression>("a"_sd, operand2["$lt"]);
    auto ltB = std::make_unique<LTMatchExpression>("b"_sd, operand2["$lt"]);

    auto or1 = std::make_unique<OrMatchExpression>();
    or1->add(std::move(gtA));
    or1->add(std::move(gtB));

    auto or2 = std::make_unique<OrMatchExpression>();
    or2->add(std::move(ltA));
    or2->add(std::move(ltB));

    auto andExpr = std::make_unique<AndMatchExpression>();
    andExpr->add(std::move(or1));
    andExpr->add(std::move(or2));

    auto expr = NotMatchExpression{andExpr.release()};

    Maxterm expectedMaxterm{
        {"0000", "0011"},
        {"0000", "1011"},
        {"0000", "0111"},
        {"0000", "1101"},
        {"0000", "1111"},
        {"0000", "1110"},
        {"0000", "1100"},
    };

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(
        ExpressionBitInfo{std::make_unique<GTMatchExpression>("a"_sd, operand1["$gt"])});
    expectedExpressions.emplace_back(
        ExpressionBitInfo{std::make_unique<GTMatchExpression>("b"_sd, operand1["$gt"])});
    expectedExpressions.emplace_back(
        ExpressionBitInfo{std::make_unique<LTMatchExpression>("a"_sd, operand2["$lt"])});
    expectedExpressions.emplace_back(
        ExpressionBitInfo{std::make_unique<LTMatchExpression>("b"_sd, operand2["$lt"])});

    auto expected = std::make_pair(expectedMaxterm, std::move(expectedExpressions));

    auto dnfs = transformToDNF(&expr);
    assertDNF(expected, dnfs);
}

TEST(ExpressionDNFConverterTests, OrOfTheSame) {
    auto operand = BSON("$lt" << 0);
    auto lt1 = std::make_unique<LTMatchExpression>("a"_sd, operand["$lt"]);
    auto lt2 = std::make_unique<LTMatchExpression>("a"_sd, operand["$lt"]);

    OrMatchExpression expr{};
    expr.add(std::move(lt1));
    expr.add(std::move(lt2));

    Maxterm expectedMaxterm{{"1", "1"}};

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(
        ExpressionBitInfo{std::make_unique<LTMatchExpression>("a"_sd, operand["$lt"])});

    auto expected = std::make_pair(expectedMaxterm, std::move(expectedExpressions));

    auto dnfs = transformToDNF(&expr);
    assertDNF(expected, dnfs);
}

// {a: $elemMatch: {$gt: 5, $eq: 10, $lt: 10}}
TEST(ExpressionDNFConverterTests, ElemMatch) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);

    auto expr = std::make_unique<ElemMatchValueMatchExpression>("a"_sd);
    expr->add(std::make_unique<GTMatchExpression>(""_sd, firstOperand["$gt"]));
    expr->add(std::make_unique<EqualityMatchExpression>(""_sd, secondOperand["$eq"]));
    expr->add(std::make_unique<LTMatchExpression>(""_sd, thirdOperand["$lt"]));

    Maxterm expectedMaxterm{{"1", "1"}};

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(expr->clone());

    auto expected = std::make_pair(expectedMaxterm, std::move(expectedExpressions));

    auto dnfs = transformToDNF(expr.get());
    assertDNF(expected, dnfs);
}

// {a: $elemMatch: {b: {$gt: 5, $eq: 10, $lt: 10}}}
TEST(ExpressionDNFConverterTests, ElemMatchObject) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);

    auto child = std::make_unique<AndMatchExpression>();
    child->add(std::make_unique<GTMatchExpression>("b"_sd, firstOperand["$gt"]));
    child->add(std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]));
    child->add(std::make_unique<LTMatchExpression>("b"_sd, thirdOperand["$lt"]));

    auto expr = std::make_unique<ElemMatchObjectMatchExpression>("a"_sd, std::move(child));

    Maxterm expectedMaxterm{{"1", "1"}};

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(expr->clone());

    auto expected = std::make_pair(expectedMaxterm, std::move(expectedExpressions));

    auto dnfs = transformToDNF(expr.get());
    assertDNF(expected, dnfs);
}

// {$and: [{a: {$elemMatch: {$not: {$gt: 21}}}}, {a: {$not: {$elemMatch: {$lt: 21}}}}]}
TEST(ExpressionDNFConverterTests, TwoElemMatches) {
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

    Maxterm expectedMaxterm{
        Minterm{"01", "11"},
    };

    std::vector<ExpressionBitInfo> expectedExpressions{};
    expectedExpressions.emplace_back(elemMatchGt->clone());
    expectedExpressions.emplace_back(elemMatchLt->clone());

    auto expected = std::make_pair(expectedMaxterm, std::move(expectedExpressions));

    auto dnfs = transformToDNF(expr.get());
    assertDNF(expected, dnfs);
}
}  // namespace mongo
