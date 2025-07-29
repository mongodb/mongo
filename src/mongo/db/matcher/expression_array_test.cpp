/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/matcher/expression_array.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_always_boolean.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_tree.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/rewrites/matcher/expression_optimizer.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

DEATH_TEST_REGEX(ElemMatchObjectMatchExpression,
                 GetChildFailsIndexGreaterThanOne,
                 "Tripwire assertion.*6400204") {
    auto baseOperand = BSON("c" << 6);
    auto eq = std::make_unique<EqualityMatchExpression>("c"_sd, baseOperand["c"]);
    auto op = ElemMatchObjectMatchExpression{"a.b"_sd, std::move(eq)};

    const size_t numChildren = 1;
    ASSERT_EQ(op.numChildren(), numChildren);
    ASSERT_THROWS_CODE(op.getChild(numChildren), AssertionException, 6400204);
}

/**
TEST(ElemMatchObjectMatchExpression, MatchesIndexKey) {
    auto baseOperand = BSON("b" << 5);
    auto eq = std::make_unique<EqualityMatchExpression>();
    ASSERT(eq->init("b", baseOperand["b"]).isOK());
    auto op = ElemMatchObjectMatchExpression{};
    ASSERT(op.init("a", std::move(eq)).isOK());
    auto indexSpec = IndexSpec{BSON("a.b" << 1)};
    auto indexKey = BSON("" << "5");
    ASSERT(MatchMatchExpression::PartialMatchResult_Unknown ==
           op.matchesIndexKey(indexKey, indexSpec));
}
*/

DEATH_TEST_REGEX(ElemMatchValueMatchExpression,
                 GetChildFailsOnIndexLargerThanChildSet,
                 "Tripwire assertion.*6400205") {
    auto baseOperand = BSON("$gt" << 6);
    auto gt = std::make_unique<GTMatchExpression>(""_sd, baseOperand["$gt"]);
    auto op =
        ElemMatchValueMatchExpression{"a.b"_sd, std::unique_ptr<MatchExpression>{std::move(gt)}};

    const size_t numChildren = 1;
    ASSERT_EQ(op.numChildren(), numChildren);
    ASSERT_THROWS_CODE(op.getChild(numChildren), AssertionException, 6400205);
}

TEST(ElemMatchValueMatchExpression, IsReducedToAlwaysFalseIfContainsIt) {
    auto baseOperand = BSON("$gt" << 6);
    auto gt = std::make_unique<GTMatchExpression>(""_sd, baseOperand["$gt"]);
    auto expr = std::make_unique<ElemMatchValueMatchExpression>("a"_sd, std::move(gt));
    expr->add(std::make_unique<AlwaysFalseMatchExpression>());
    ASSERT_FALSE(expr->isTriviallyFalse());
    auto optimizedExpr = optimizeMatchExpression(std::move(expr), true);
    ASSERT(optimizedExpr->isTriviallyFalse());
}

/**
TEST(ElemMatchValueMatchExpression, MatchesIndexKey) {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LtOp>();
    ASSERT(lt->init("a", baseOperand["$lt"]).isOK());
    auto op = ElemMatchValueMatchExpression{};
    ASSERT(op.init("a", std::move(lt)).isOK());
    auto indexSpec = IndexSpec{BSON("a" << 1)};
    auto indexKey = BSON("" << "3");
    ASSERT(MatchMatchExpression::PartialMatchResult_Unknown ==
           op.matchesIndexKey(indexKey, indexSpec));
}
*/

TEST(SizeMatchExpression, Equivalent) {
    auto e1 = SizeMatchExpression{"a"_sd, 5};
    auto e2 = SizeMatchExpression{"a"_sd, 6};
    auto e3 = SizeMatchExpression{"v"_sd, 5};

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
    ASSERT(!e1.equivalent(&e3));
}

DEATH_TEST_REGEX(SizeMatchExpression,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400206") {
    auto e1 = SizeMatchExpression{"a"_sd, 5};

    const size_t numChildren = 0;
    ASSERT_EQ(e1.numChildren(), numChildren);
    ASSERT_THROWS_CODE(e1.getChild(0), AssertionException, 6400206);
}

/**
   TEST(SizeMatchExpression, MatchesIndexKey) {
   auto operand = BSON("$size" << 4);
   auto size = SizeMatchExpression{};
   ASSERT(size.init("a", operand["$size"]).isOK());
   auto indexSpec = IndexSpec{BSON("a" << 1)};
   auto indexKey = BSON("" << 1);
   ASSERT(MatchMatchExpression::PartialMatchResult_Unknown ==
          size.matchesIndexKey(indexKey, indexSpec));
   }
*/

}  // namespace mongo
