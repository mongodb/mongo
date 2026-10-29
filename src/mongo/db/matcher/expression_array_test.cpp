// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
using namespace std::literals::string_view_literals;

DEATH_TEST_REGEX(ElemMatchObjectMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanOne,
                 "Tripwire assertion.*6400204") {
    auto baseOperand = BSON("c" << 6);
    auto eq = std::make_unique<EqualityMatchExpression>("c"sv, baseOperand["c"]);
    auto op = ElemMatchObjectMatchExpression{"a.b"sv, std::move(eq)};

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

DEATH_TEST_REGEX(ElemMatchValueMatchExpressionDeathTest,
                 GetChildFailsOnIndexLargerThanChildSet,
                 "Tripwire assertion.*6400205") {
    auto baseOperand = BSON("$gt" << 6);
    auto gt = std::make_unique<GTMatchExpression>(""sv, baseOperand["$gt"]);
    auto op =
        ElemMatchValueMatchExpression{"a.b"sv, std::unique_ptr<MatchExpression>{std::move(gt)}};

    const size_t numChildren = 1;
    ASSERT_EQ(op.numChildren(), numChildren);
    ASSERT_THROWS_CODE(op.getChild(numChildren), AssertionException, 6400205);
}

TEST(ElemMatchValueMatchExpression, IsReducedToAlwaysFalseIfContainsIt) {
    auto baseOperand = BSON("$gt" << 6);
    auto gt = std::make_unique<GTMatchExpression>(""sv, baseOperand["$gt"]);
    auto expr = std::make_unique<ElemMatchValueMatchExpression>("a"sv, std::move(gt));
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
    auto e1 = SizeMatchExpression{"a"sv, 5};
    auto e2 = SizeMatchExpression{"a"sv, 6};
    auto e3 = SizeMatchExpression{"v"sv, 5};

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
    ASSERT(!e1.equivalent(&e3));
}

DEATH_TEST_REGEX(SizeMatchExpressionDeathTest,
                 GetChildFailsIndexGreaterThanZero,
                 "Tripwire assertion.*6400206") {
    auto e1 = SizeMatchExpression{"a"sv, 5};

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
