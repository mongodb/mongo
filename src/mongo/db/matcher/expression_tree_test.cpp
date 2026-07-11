// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/** Unit tests for MatchMatchExpression operator implementations in match_operators.{h,cpp}. */

#include "mongo/db/matcher/expression_tree.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DEATH_TEST_REGEX(NotMatchExpressionDeathTest,
                 GetChildFailsIndexLargerThanOne,
                 "Tripwire assertion.*6400210") {
    auto baseOperand = BSON("$lt" << 5);
    auto lt = std::make_unique<LTMatchExpression>("a"sv, baseOperand["$lt"]);
    auto notOp = NotMatchExpression{lt.release()};

    ASSERT_EQ(notOp.numChildren(), 1);
    ASSERT_THROWS_CODE(notOp.getChild(1), AssertionException, 6400210);
}

DEATH_TEST_REGEX(AndOpDeathTest,
                 GetChildFailsOnIndexLargerThanChildren,
                 "Tripwire assertion.*6400201") {
    auto baseOperand1 = BSON("$gt" << 1);
    auto baseOperand2 = BSON("$lt" << 10);
    auto baseOperand3 = BSON("$lt" << 100);

    auto sub1 = std::make_unique<GTMatchExpression>("a"sv, baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a"sv, baseOperand2["$lt"]);
    auto sub3 = std::make_unique<LTMatchExpression>("b"sv, baseOperand3["$lt"]);

    auto andOp = AndMatchExpression{};
    andOp.add(std::move(sub1));
    andOp.add(std::move(sub2));
    andOp.add(std::move(sub3));

    const size_t numChildren = 3;
    ASSERT_EQ(andOp.numChildren(), numChildren);
    ASSERT_THROWS_CODE(andOp.getChild(numChildren), AssertionException, 6400201);
}

DEATH_TEST_REGEX(OrOpDeathTest,
                 GetChildFailsOnIndexLargerThanChildren,
                 "Tripwire assertion.*6400201") {
    auto baseOperand1 = BSON("$gt" << 10);
    auto baseOperand2 = BSON("$lt" << 0);
    auto baseOperand3 = BSON("b" << 100);
    auto sub1 = std::make_unique<GTMatchExpression>("a"sv, baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a"sv, baseOperand2["$lt"]);
    auto sub3 = std::make_unique<EqualityMatchExpression>("b"sv, baseOperand3["b"]);

    auto orOp = OrMatchExpression{};
    orOp.add(std::move(sub1));
    orOp.add(std::move(sub2));
    orOp.add(std::move(sub3));

    const size_t numChildren = 3;
    ASSERT_EQ(orOp.numChildren(), numChildren);
    ASSERT_THROWS_CODE(orOp.getChild(numChildren), AssertionException, 6400201);
}

TEST(NorOp, Equivalent) {
    auto baseOperand1 = BSON("a" << 1);
    auto baseOperand2 = BSON("b" << 2);
    auto sub1 = EqualityMatchExpression{"a"sv, baseOperand1["a"]};
    auto sub2 = EqualityMatchExpression{"b"sv, baseOperand2["b"]};

    auto e1 = NorMatchExpression{};
    e1.add(sub1.clone());
    e1.add(sub2.clone());

    auto e2 = NorMatchExpression{};
    e2.add(sub1.clone());

    ASSERT(e1.equivalent(&e1));
    ASSERT(!e1.equivalent(&e2));
}

DEATH_TEST_REGEX(NorOpDeathTest,
                 GetChildFailsOnIndexLargerThanChildren,
                 "Tripwire assertion.*6400201") {
    auto baseOperand1 = BSON("$gt" << 10);
    auto baseOperand2 = BSON("$lt" << 0);
    auto baseOperand3 = BSON("b" << 100);

    auto sub1 = std::make_unique<GTMatchExpression>("a"sv, baseOperand1["$gt"]);
    auto sub2 = std::make_unique<LTMatchExpression>("a"sv, baseOperand2["$lt"]);
    auto sub3 = std::make_unique<EqualityMatchExpression>("b"sv, baseOperand3["b"]);

    auto norOp = NorMatchExpression{};
    norOp.add(std::move(sub1));
    norOp.add(std::move(sub2));
    norOp.add(std::move(sub3));

    const size_t numChildren = 3;
    ASSERT_EQ(norOp.numChildren(), numChildren);
    ASSERT_THROWS_CODE(norOp.getChild(numChildren), AssertionException, 6400201);
}

}  // namespace mongo
