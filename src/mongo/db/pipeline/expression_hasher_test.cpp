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

#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/expression_hasher.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"

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

    absl::Hash<Expression> hash{};
    ExpressionContextForTest ctx{};
};

TEST_F(ExpressionHasherTest, ExpressionConstant) {
    ExpressionConstant expr1{&ctx, Value(1)};
    ExpressionConstant expr2{&ctx, Value(1)};
    ExpressionConstant expr3{&ctx, Value("abc"_sd)};

    ASSERT_EQ(hash(expr1), hash(expr2));
    ASSERT_NE(hash(expr1), hash(expr3));
}

TEST_F(ExpressionHasherTest, ExpressionAbs) {
    ExpressionAbs expr1{&ctx, {constE(1)}};
    ExpressionAbs expr2{&ctx, {constE(1)}};
    ExpressionAbs expr3{&ctx, {constE(10)}};

    ASSERT_EQ(hash(expr1), hash(expr2));
    ASSERT_NE(hash(expr1), hash(expr3));
}

TEST_F(ExpressionHasherTest, ExpressionAdd) {
    ExpressionAdd expr1{&ctx, {constE(1), constE("abc"_sd)}};
    ExpressionAdd expr2{&ctx, {constE(1), constE("abc"_sd)}};
    ExpressionAdd expr3{&ctx, {constE("abc"_sd), constE(1)}};

    ASSERT_EQ(hash(expr1), hash(expr2));
    ASSERT_NE(hash(expr1), hash(expr3));
}

TEST_F(ExpressionHasherTest, ExpressionCompare) {
    ExpressionCompare expr1{&ctx, ExpressionCompare::EQ, {constE(1), constE("abc"_sd)}};
    ExpressionCompare expr2{&ctx, ExpressionCompare::EQ, {constE(1), constE("abc"_sd)}};
    ExpressionCompare expr3{&ctx, ExpressionCompare::NE, {constE(1), constE("abc"_sd)}};
    ExpressionCompare expr4{&ctx, ExpressionCompare::EQ, {constE("abc"_sd), constE(1)}};

    ASSERT_EQ(hash(expr1), hash(expr2));
    ASSERT_NE(hash(expr1), hash(expr3));
    ASSERT_NE(hash(expr1), hash(expr4));
}

TEST_F(ExpressionHasherTest, DifferentTypesAreNotEqual) {
    std::vector<boost::intrusive_ptr<Expression>> expressions{
        makeExpr<ExpressionAbs>(1),
        makeExpr<ExpressionAdd>(1),
        makeExpr<ExpressionAllElementsTrue>(1),
        makeExpr<ExpressionAnd>(1),
        makeExpr<ExpressionAnyElementTrue>(1),
        makeExpr<ExpressionArray>(1),
        makeExpr<ExpressionBitAnd>(1),
        makeExpr<ExpressionBitOr>(1),
        makeExpr<ExpressionBitXor>(1),
        makeExpr<ExpressionBitNot>(1),
        makeExpr<ExpressionFirst>(1),
        makeExpr<ExpressionArrayToObject>(1),
    };

    stdx::unordered_set<size_t> uniqueHashes{};
    for (const auto& expr : expressions) {
        uniqueHashes.insert(hash(*expr));
    }

    ASSERT_EQ(expressions.size(), uniqueHashes.size());
}
}  // namespace mongo
