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

#include "mongo/db/pipeline/expression_hasher.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/expression_visitor.h"
#include "mongo/unittest/unittest.h"

#include <string>

namespace mongo {

class ExpressionHasherTest : public mongo::unittest::Test {
public:
    /**
     * Makes ExpressionConstant.
     */
    template <typename T>
    boost::intrusive_ptr<Expression> constE(T value) {
        return ExpressionConstant::create(&ctx, Value{value});
    }

    /**
     * Makes Expression of `Expr` type and add one ExpressionConstant with the value 'value' to its
     * children.
     */
    template <typename Expr, typename T>
    boost::intrusive_ptr<Expression> makeExpr(T value) {
        return make_intrusive<Expr>(
            &ctx, std::vector<boost::intrusive_ptr<Expression>>{constE(std::move(value))});
    }

    template <typename Expr, typename T>
    boost::intrusive_ptr<Expression> makeExprWithIterable(T value) {
        std::vector<boost::intrusive_ptr<Expression>> expressions;
        for (const auto& v : value) {
            expressions.push_back(constE(v));
        }
        return boost::intrusive_ptr<Expression>(
            new Expr(&ctx, std::vector<boost::intrusive_ptr<Expression>>{expressions}));
    }

    absl::Hash<Expression> hash{};
    ExpressionContextForTest ctx{};

    std::vector<boost::intrusive_ptr<Expression>> makeExprList() {
        std::vector<boost::intrusive_ptr<Expression>> expressions{
            // Test different constants.
            constE(1),
            constE("abc"_sd),
            constE(""_sd),
            // Test arithmetic expressions.
            makeExpr<ExpressionAbs>(1),
            makeExpr<ExpressionAbs>(10),
            makeExpr<ExpressionAdd>(1),
            makeExprWithIterable<ExpressionAdd>(std::vector<int>{1, 5}),
            makeExprWithIterable<ExpressionAdd>(std::vector<int>{5, 1}),
            // Test boolean function expressions
            makeExpr<ExpressionAllElementsTrue>(1),
            makeExpr<ExpressionAllElementsTrue>(200),
            makeExpr<ExpressionAllElementsTrue>("hi"_sd),
            makeExpr<ExpressionAnd>(1),
            makeExpr<ExpressionAnd>(300),
            makeExpr<ExpressionAnyElementTrue>(1),
            makeExpr<ExpressionAnyElementTrue>(400),
            // Test array and object expressions.
            makeExpr<ExpressionArray>(1),
            makeExprWithIterable<ExpressionArray>(std::vector<int>{}),
            makeExprWithIterable<ExpressionArray>(std::vector<int>{1, 2, 3}),
            makeExprWithIterable<ExpressionArray>(std::vector<int>{1, 2, 4}),
            makeExprWithIterable<ExpressionArray>(std::vector<int>{1, 2, 3, 4}),
            makeExpr<ExpressionArrayToObject>(1),
            makeExprWithIterable<ExpressionArrayToObject>(std::vector<int>{}),
            makeExprWithIterable<ExpressionArrayToObject>(std::vector<int>{1, 2, 3}),
            makeExprWithIterable<ExpressionArrayToObject>(std::vector<int>{1, 2, 4}),
            makeExprWithIterable<ExpressionArrayToObject>(std::vector<int>{1, 2, 3, 4}),
            // Test bit operator expressions.
            makeExpr<ExpressionBitAnd>(1),
            makeExpr<ExpressionBitAnd>(2),
            makeExpr<ExpressionBitOr>(1),
            makeExpr<ExpressionBitOr>(0),
            makeExpr<ExpressionBitXor>(1),
            makeExpr<ExpressionBitXor>(""_sd),
            makeExpr<ExpressionBitNot>(1),
            makeExpr<ExpressionBitNot>("o-o"_sd),
            // Test comparator expressions.
            ExpressionCompare::create(&ctx, ExpressionCompare::EQ, constE(1), constE("abc"_sd)),
            ExpressionCompare::create(&ctx, ExpressionCompare::NE, constE(1), constE("abc"_sd)),
            ExpressionCompare::create(&ctx, ExpressionCompare::EQ, constE("abc"_sd), constE(1)),
            ExpressionCompare::create(&ctx,
                                      ExpressionCompare::EQ,
                                      makeExprWithIterable<ExpressionAdd>(std::vector<int>{1, 5}),
                                      makeExprWithIterable<ExpressionAdd>(std::vector<int>{3, 2})),
            // Test date expressions/
            ExpressionDateFromString::parseExpression(
                &ctx,
                BSON("$dateFromString" << BSON("dateString" << "2017-07"
                                                            << "format"
                                                            << "%Y-%m-%d")),
                ctx.variablesParseState),
            ExpressionDateFromString::parseExpression(
                &ctx,
                BSON("$dateFromString" << BSON("dateString" << "2017-07-14 -0400"
                                                            << "timezone"
                                                            << "GMT")),
                ctx.variablesParseState),
            ExpressionDateFromString::parseExpression(
                &ctx,
                BSON("$dateFromString" << BSON("dateString" << "Day 7 Week 53 Year 2017"
                                                            << "format"
                                                            << "Day %u Week %V Year %G")),
                ctx.variablesParseState),
            ExpressionDateFromString::parseExpression(
                &ctx,
                BSON("$dateFromString" << BSON("dateString" << "2017-07-14 -0400"
                                                            << "timezone"
                                                            << "-08:00")),
                ctx.variablesParseState),
            // Test field path expressions.
            ExpressionFieldPath::createPathFromString(&ctx, "foo.bar", ctx.variablesParseState),
            ExpressionFieldPath::createPathFromString(&ctx, "foo.bar.a", ctx.variablesParseState),
            ExpressionFieldPath::createPathFromString(&ctx, "HI.foo", ctx.variablesParseState),
            // Test first expressions.
            makeExpr<ExpressionFirst>(1),
            makeExpr<ExpressionFirst>(0),
            makeExpr<ExpressionFirst>(""_sd),
            makeExpr<ExpressionFirst>("Hello"_sd),
        };
        return expressions;
    }
};

TEST_F(ExpressionHasherTest, ExpressionsAreNotEqual) {
    auto exprList1{makeExprList()};
    auto exprList2{makeExprList()};

    std::set<size_t> list1Hashes{};
    std::set<size_t> list2Hashes{};

    for (const auto& expr : exprList1) {
        list1Hashes.insert(hash(*expr));
    }
    for (const auto& expr : exprList2) {
        list2Hashes.insert(hash(*expr));
    }

    // Test that each element in exprList 1 and 2 are hashed to unique values.
    ASSERT_EQ(exprList1.size(), list1Hashes.size());
    ASSERT_EQ(exprList2.size(), list2Hashes.size());
    // Test that exprList 1 and 2 hashed to the same number of values.
    ASSERT_EQ(list1Hashes.size(), list2Hashes.size());

    // Test that two identically initialised lists (1 and 2) have the same expressions hashed to
    // the same values
    for (const auto& hash : list1Hashes) {
        ASSERT(list2Hashes.contains(hash));
    }
}

/**
 * Here we are testing the following scenario: imagine that we have two identical $expr with let
 * inside. This will create two identical variable definitions, which are the same except for
 * variable id field inside ExpressionFieldPath which represents user defined variable (among other
 * things). We need to make sure that the hash of the both ExpressionFieldPaths are the same and
 * don't take into account variable id.
 */
TEST_F(ExpressionHasherTest, UserVariables) {
    ctx.variablesParseState.defineVariable("oplogField");
    auto fp1 = ExpressionFieldPath::parse(&ctx, "$$oplogField", ctx.variablesParseState);
    ctx.variablesParseState.defineVariable("oplogField");
    auto fp2 = ExpressionFieldPath::parse(&ctx, "$$oplogField", ctx.variablesParseState);

    ASSERT_EQ(fp1->serialize().getString(), fp2->serialize().getString());
    ASSERT_EQ(hash(*fp1), hash(*fp2));
}
}  // namespace mongo
