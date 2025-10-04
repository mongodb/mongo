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

#include "mongo/db/query/compiler/rewrites/matcher/expression_simplifier.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_array.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
inline void assertExpr(const MatchExpression* expected, MatchExpression* actual) {
    ASSERT_TRUE(expected->equivalent(actual))
        << expected->debugString() << " != " << actual->debugString();
}

inline void assertSimplification(const MatchExpression& expr,
                                 const MatchExpression& expected,
                                 const ExpressionSimplifierSettings& settings) {
    auto result = simplifyMatchExpression(&expr, settings);
    ASSERT_TRUE(result);
    assertExpr(&expected, result->get());
}

inline void assertSimplification(const MatchExpression& expr, const MatchExpression& expected) {
    ExpressionSimplifierSettings settings{};
    settings.doNotOpenContainedOrs = true;
    assertSimplification(expr, expected, settings);
}

inline void assertSimplification(const MatchExpression& expr) {
    assertSimplification(expr, expr);
}

inline std::unique_ptr<MatchExpression> parse(BSONObj bsonExpr) {
    QueryTestServiceContext serviceContext{};
    auto opCtx = serviceContext.makeOperationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get());

    auto expr = MatchExpressionParser::parse(bsonExpr,
                                             expCtx,
                                             ExtensionsCallbackNoop(),
                                             MatchExpressionParser::kAllowAllSpecialFeatures);

    ASSERT_OK(expr);
    return std::move(expr.getValue());
}

inline void assertSimplification(BSONObj bsonExpr, BSONObj bsonExpected) {
    QueryTestServiceContext serviceContext{};
    auto opCtx = serviceContext.makeOperationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get());

    auto expr = MatchExpressionParser::parse(bsonExpr,
                                             expCtx,
                                             ExtensionsCallbackNoop(),
                                             MatchExpressionParser::kAllowAllSpecialFeatures);

    ASSERT_OK(expr);

    auto expected = MatchExpressionParser::parse(bsonExpected,
                                                 expCtx,
                                                 ExtensionsCallbackNoop(),
                                                 MatchExpressionParser::kAllowAllSpecialFeatures);
    ASSERT_OK(expected);

    assertSimplification(*expr.getValue(), *expected.getValue());
}
inline void assertSimplification(BSONObj bsonExpr) {
    assertSimplification(bsonExpr, bsonExpr);
}

inline void assertDNFTransformation(const MatchExpression& expr, const MatchExpression& expected) {
    ExpressionSimplifierSettings settings{};
    settings.applyQuineMcCluskey = false;
    auto result = simplifyMatchExpression(&expr, settings);
    ASSERT_TRUE(result);
    assertExpr(&expected, result->get());
}

TEST(ExpressionSimplifierTests, SimpleEq) {
    BSONObj operand = BSON("a" << 5);
    EqualityMatchExpression eq("a"_sd, operand["a"]);
    assertSimplification(eq);
}

TEST(ExpressionSimplifierTests, SimpleNe) {
    auto baseOperand = BSON("$eq" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>("a"_sd, baseOperand["$eq"]);
    auto expr = NotMatchExpression{eq.release()};
    assertSimplification(expr);
}

TEST(ExpressionSimplifierTests, SimpleLt) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a"_sd, baseOperand["$lt"]);
    auto expr = NotMatchExpression{lt.release()};
    assertSimplification(expr);
}

TEST(ExpressionSimplifierTests, MultikeyLt) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a"_sd, baseOperand["$lt"]);
    auto expr = NotMatchExpression{lt.release()};
    assertSimplification(expr);
}

// a > 10 | b <= 5
TEST(ExpressionSimplifierTests, Or) {
    auto firstOperand = BSON("$gt" << 10);
    auto secondOperand = BSON("$lte" << 5);
    auto firstExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto secondExpr = std::make_unique<LTEMatchExpression>("b"_sd, secondOperand["$lte"]);
    OrMatchExpression expr{};
    expr.add(std::move(firstExpr));
    expr.add(std::move(secondExpr));
    assertSimplification(expr);
}

TEST(ExpressionSimplifierTests, SimpleNor) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]);
    auto ltExpr = std::make_unique<LTMatchExpression>("c"_sd, thirdOperand["$lt"]);

    NorMatchExpression expr{};
    expr.add(gtExpr->clone());
    expr.add(eqExpr->clone());
    expr.add(ltExpr->clone());

    AndMatchExpression expected{};
    expected.add(std::make_unique<NotMatchExpression>(gtExpr->clone()));
    expected.add(std::make_unique<NotMatchExpression>(eqExpr->clone()));
    expected.add(std::make_unique<NotMatchExpression>(ltExpr->clone()));

    assertSimplification(expr, expected);
}

/**
 * (a & ~b) <nor> (a & b & c) =
 * ~(a & ~b) & ~(a & b & c) =
 * (~a | b) & (~a | ~b | ~c) =
 * (~a & ~a) | (~a & ~b) | (~a & ~c) | (~a & b) | (b & ~b) | (b & ~c) =
 *  ~a | (~a & ~b) | (~a & ~c) | (~a & b) | (b & ~c) =
 *  ~a | (b & ~c)
 */
TEST(ExpressionSimplifierTests, NorWithNotDNFOnly) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]);
    auto ltExpr = std::make_unique<LTMatchExpression>("c"_sd, thirdOperand["$lt"]);

    // (a & ~b) nor (a & b & c)
    NorMatchExpression expr{};
    // a & ~b
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(gtExpr->clone());                                        // a
        andExpr->add(std::make_unique<NotMatchExpression>(eqExpr->clone()));  // ~b
        expr.add(std::move(andExpr));
    }
    // a & b & c
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(gtExpr->clone());  // a
        andExpr->add(eqExpr->clone());  // b
        andExpr->add(ltExpr->clone());  // c
        expr.add(std::move(andExpr));
    }

    // ~a | (b & ~c)
    OrMatchExpression expected{};
    expected.add(std::make_unique<NotMatchExpression>(gtExpr->clone()));  // ~a
    // b & ~c
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(eqExpr->clone());                                        // b
        andExpr->add(std::make_unique<NotMatchExpression>(ltExpr->clone()));  // ~c
        expected.add(std::move(andExpr));
    }

    assertDNFTransformation(expr, expected);
}

/**
 * (a & ~b) nor (a & b & c) =
 * ~(a & ~b) & ~(a & b & c) =
 * (~a | b) & (~a | ~b | ~c) =
 * (~a & ~a) | (~a & ~b) | (~a & ~c) | (~a & b) | (b & ~b) | (b & ~c) =
 *  ~a | (~a & ~b) | (~a & ~c) | (~a & b) | (b & ~c) =
 *  ~a | (~a & ~c) | (b & ~c) =
 *  ~a | (b & ~c)
 */
TEST(ExpressionSimplifierTests, NorWithNot) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);
    auto gtExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto eqExpr = std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]);
    auto ltExpr = std::make_unique<LTMatchExpression>("c"_sd, thirdOperand["$lt"]);

    // (a & ~b) nor (a & b & c)
    NorMatchExpression expr{};
    // a & ~b
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(gtExpr->clone());                                        // a
        andExpr->add(std::make_unique<NotMatchExpression>(eqExpr->clone()));  // ~b
        expr.add(std::move(andExpr));
    }
    // a & b & c
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(gtExpr->clone());  // a
        andExpr->add(eqExpr->clone());  // b
        andExpr->add(ltExpr->clone());  // c
        expr.add(std::move(andExpr));
    }

    // ~a | (b & ~c)
    OrMatchExpression expected{};
    expected.add(std::make_unique<NotMatchExpression>(gtExpr->clone()));  // ~a
    // b & ~c
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(eqExpr->clone());                                        // b
        andExpr->add(std::make_unique<NotMatchExpression>(ltExpr->clone()));  // ~c
        expected.add(std::move(andExpr));
    }

    assertSimplification(expr, expected);
}

// a > 10 & b <= 5
TEST(ExpressionSimplifierTests, And) {
    auto firstOperand = BSON("$gt" << 10);
    auto secondOperand = BSON("$lte" << 5);
    auto firstExpr = std::make_unique<GTMatchExpression>("a"_sd, firstOperand["$gt"]);
    auto secondExpr = std::make_unique<LTEMatchExpression>("b"_sd, secondOperand["$lte"]);
    AndMatchExpression expr{};
    expr.add(std::move(firstExpr));
    expr.add(std::move(secondExpr));
    assertSimplification(expr);
}

/**
 * ~((a > 1 | b > 1) & (a < 2 | b < 2)) = ~(a > 1) & ~(b > 1) | ~(a < 2) & ~ (b < 2)
 * ~((a1 | b1) & (a2 | b2)) = ~a1~b1 | ~a2~b2
 */
TEST(ExpressionSimplifierTests, NotExpression) {
    auto operand1 = BSON("$gt" << 1);
    auto operand2 = BSON("$lt" << 2);

    auto gtA = std::make_unique<GTMatchExpression>("a"_sd, operand1["$gt"]);
    auto gtB = std::make_unique<GTMatchExpression>("b"_sd, operand1["$gt"]);

    auto ltA = std::make_unique<LTMatchExpression>("a"_sd, operand2["$lt"]);
    auto ltB = std::make_unique<LTMatchExpression>("b"_sd, operand2["$lt"]);

    auto or1 = std::make_unique<OrMatchExpression>();
    or1->add(gtA->clone());
    or1->add(gtB->clone());

    auto or2 = std::make_unique<OrMatchExpression>();
    or2->add(ltA->clone());
    or2->add(ltB->clone());

    auto andExpr = std::make_unique<AndMatchExpression>();
    andExpr->add(std::move(or1));
    andExpr->add(std::move(or2));

    auto expr = NotMatchExpression{std::move(andExpr)};

    OrMatchExpression expected;
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(std::make_unique<NotMatchExpression>(gtA->clone()));
        andExpr->add(std::make_unique<NotMatchExpression>(gtB->clone()));
        expected.add(std::move(andExpr));
    }
    {
        auto andExpr = std::make_unique<AndMatchExpression>();
        andExpr->add(std::make_unique<NotMatchExpression>(ltA->clone()));
        andExpr->add(std::make_unique<NotMatchExpression>(ltB->clone()));
        expected.add(std::move(andExpr));
    }

    assertSimplification(expr, expected);
}

/**
 * a | a == a
 */
TEST(ExpressionSimplifierTests, OrOfTheSame) {
    auto operand = BSON("$lt" << 0);
    auto lt1 = std::make_unique<LTMatchExpression>("a"_sd, operand["$lt"]);
    auto lt2 = std::make_unique<LTMatchExpression>("a"_sd, operand["$lt"]);

    OrMatchExpression expr{};
    expr.add(lt1->clone());
    expr.add(std::move(lt2));

    assertSimplification(expr, *lt1);
}

// {a: $elemMatch: {$gt: 5, $eq: 10, $lt: 10}}
TEST(ExpressionSimplifierTests, ElemMatch) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);

    ElemMatchValueMatchExpression expr{"a"_sd};
    expr.add(std::make_unique<GTMatchExpression>(""_sd, firstOperand["$gt"]));
    expr.add(std::make_unique<EqualityMatchExpression>(""_sd, secondOperand["$eq"]));
    expr.add(std::make_unique<LTMatchExpression>(""_sd, thirdOperand["$lt"]));

    assertSimplification(expr);
}

// {a: $elemMatch: {b: {$gt: 5, $eq: 10, $lt: 10}}}
TEST(ExpressionSimplifierTests, ElemMatchObject) {
    auto firstOperand = BSON("$gt" << 5);
    auto secondOperand = BSON("$eq" << 10);
    auto thirdOperand = BSON("$lt" << 10);

    auto child = std::make_unique<AndMatchExpression>();
    child->add(std::make_unique<GTMatchExpression>("b"_sd, firstOperand["$gt"]));
    child->add(std::make_unique<EqualityMatchExpression>("b"_sd, secondOperand["$eq"]));
    child->add(std::make_unique<LTMatchExpression>("b"_sd, thirdOperand["$lt"]));

    ElemMatchObjectMatchExpression expr{"a"_sd, std::move(child)};

    assertSimplification(expr);
}

// {$and: [{a: {$elemMatch: {$not: {$gt: 21}}}}, {a: {$not: {$elemMatch: {$lt: 21}}}}]}
TEST(ExpressionSimplifierTests, TwoElemMatches) {
    auto operand = BSON("$gt" << 21 << "$lt" << 21);

    auto gt = std::make_unique<GTMatchExpression>(""_sd, operand["$gt"]);
    auto notGt = std::make_unique<NotMatchExpression>(gt->clone());
    auto elemMatchGt = std::make_unique<ElemMatchValueMatchExpression>("a"_sd);
    elemMatchGt->add(notGt->clone());

    auto lt = std::make_unique<GTMatchExpression>(""_sd, operand["$lt"]);
    auto elemMatchLt = std::make_unique<ElemMatchValueMatchExpression>("a"_sd);
    elemMatchLt->add(lt->clone());
    auto notElemMatchLt = std::make_unique<NotMatchExpression>(elemMatchLt->clone());

    AndMatchExpression expr{};
    expr.add(elemMatchGt->clone());
    expr.add(notElemMatchLt->clone());

    assertSimplification(expr);
}

TEST(ExpressionSimplifierTests, ContainedOr) {
    auto expr = fromjson("{$and: [{a: 10}, {$or: [{b: 10}, {c: 10}]}]}");
    assertSimplification(expr);
}

TEST(ExpressionSimplifierTests, AlwaysFalseContainedOrs) {
    auto expr = fromjson(
        "{$and: ["
        "{a: 11},"
        "{$or: [{b: 15}, {c: 15}]},"
        "{a: {$ne: 11}, b: 11, c: 11},"
        "{$or: [{c: 10}, {c: {$ne: 10}}]}"
        "]}");
    auto expected = fromjson("{$alwaysFalse: 1}");

    assertSimplification(expr, expected);
}

TEST(ExpressionSimplifierTests, ContainedOrsOnWithoutTop) {
    auto expr = fromjson(
        "{$and: ["
        "{$or: [{b: 15}, {c: 15}]},"
        "{$or: [{b: 11, c: 11}, {b: 11, c: {$ne: 11}}]},"
        "{$or: [{c: 10}, {c: {$ne: 10}}]}"
        "]}");
    auto expected = fromjson(
        "{$and: ["
        "{b: 11},"
        "{$or: [{b: 15}, {c: 15}]}"
        "]}");

    assertSimplification(expr, expected);
}

TEST(ExpressionSimplifierTests, StopOnTooManyPredicates) {
    BSONObj query = fromjson(
        "{$and: ["
        "{$or: [{b: 15}, {c: 15}]},"
        "{$or: [{b: 11, c: 11}, {b: 11, c: {$ne: 11}}]},"
        "{$or: [{c: 10}, {c: {$ne: 10}}]}"
        "]}");
    auto expr = parse(query);

    ExpressionSimplifierSettings settings{};

    settings.maximumNumberOfUniquePredicates = 10;
    auto result = simplifyMatchExpression(expr.get(), settings);
    ASSERT_TRUE(result);

    settings.maximumNumberOfUniquePredicates = 4;
    result = simplifyMatchExpression(expr.get(), settings);
    ASSERT_FALSE(result);
}

TEST(ExpressionSimplifierTests, StopOnTooManyTerms) {
    BSONObj query = fromjson(
        "{$and: [{$or: [{a: 1}, {b: 1}]}, {$or: [{a: 2}, {b: 2}]}, {$or: [{a: 3}, "
        "{b: 3}]}, {$or: [{a: 4}, {b: 4}]}]}");
    auto expr = parse(query);

    ExpressionSimplifierSettings settings{};
    settings.doNotOpenContainedOrs = false;

    settings.maxSizeFactor = 10;
    auto result = simplifyMatchExpression(expr.get(), settings);
    ASSERT_TRUE(result);

    settings.maxSizeFactor = 1;
    result = simplifyMatchExpression(expr.get(), settings);
    ASSERT_FALSE(result);
}

TEST(ExpressionSimplifierTests, StopOnTooManyMinTerms) {
    BSONObj query = fromjson(
        "{$and: [{$or: [{a: 1}, {b: 1}]}, {$or: [{a: 2}, {b: 2}]}, {$or: [{a: 3}, "
        "{b: 3}]}, {$or: [{a: 4}, {b: 4}]}]}");
    auto expr = parse(query);

    ExpressionSimplifierSettings settings{};
    settings.doNotOpenContainedOrs = false;

    settings.maximumNumberOfMinterms = 1000;
    auto result = simplifyMatchExpression(expr.get(), settings);
    ASSERT_TRUE(result);

    settings.maxSizeFactor = 2;
    result = simplifyMatchExpression(expr.get(), settings);
    ASSERT_FALSE(result);
}

TEST(ExpressionSimplifierTests, AbsorbedTerm) {
    auto expr = fromjson("{$or: [{a: 1, b: 1}, {a: 1, b: 1, c: 2}]}");
    auto expected = fromjson("{a: 1, b: 1}");
    assertSimplification(expr, expected);
}

TEST(ExpressionSimplifierTests, NorAlwaysBoolean) {
    AndMatchExpression alwaysTrue{};
    AlwaysFalseMatchExpression alwaysFalse{};

    BSONObj query = fromjson("{$nor: [{$alwaysFalse: 1}]}");
    assertSimplification(*parse(query), alwaysTrue);
    query = fromjson("{$nor: [{$alwaysTrue: 1}]}");
    assertSimplification(*parse(query), alwaysFalse);
    query = fromjson("{$nor: [{a: 1}, {a: {$ne: 1}}]}");
    assertSimplification(*parse(query), alwaysFalse);
    query = fromjson("{$nor: [{$and: [{a: 1}, {a: {$ne: 1}}]}]}");
    assertSimplification(*parse(query), alwaysTrue);
    query = fromjson("{$nor: [{$or: [{a: 1}, {a: {$ne: 1}}]}]}");
    assertSimplification(*parse(query), alwaysFalse);
    query = fromjson("{$nor: [{$alwaysFalse: 1}, {$alwaysTrue: 1}]}");
    assertSimplification(*parse(query), alwaysFalse);
    query = fromjson("{$nor: [{$nor: [{$alwaysTrue:1}]}]}");
    assertSimplification(*parse(query), alwaysTrue);
    query = fromjson("{$nor: [{$nor: [{$alwaysFalse:1}]}]}");
    assertSimplification(*parse(query), alwaysFalse);
    query = fromjson("{$and: [{$alwaysTrue:1}, {$nor: [{$alwaysTrue : 1}]}]}");
    assertSimplification(*parse(query), alwaysFalse);
    query = fromjson("{$or: [{$nor: [{$nor: [{$nor: [{$alwaysTrue : 1}]}]}]}, {$alwaysFalse: 1}]}");
    assertSimplification(*parse(query), alwaysFalse);
}

}  // namespace
}  // namespace mongo
