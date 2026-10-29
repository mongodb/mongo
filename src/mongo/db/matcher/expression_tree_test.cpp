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
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

//
// Dynamic predicate reordering.
//

namespace {
/**
 * Builds an $or with three distinct children, and returns the child pointers in their original
 * order so that tests can identify them after a reorder.
 */
struct ReorderingFixture {
    ReorderingFixture() {
        auto sub0 = std::make_unique<LTMatchExpression>("a"sv, operands[0]["$lt"]);
        auto sub1 = std::make_unique<LTMatchExpression>("b"sv, operands[1]["$lt"]);
        auto sub2 = std::make_unique<LTMatchExpression>("c"sv, operands[2]["$lt"]);

        children = {sub0.get(), sub1.get(), sub2.get()};

        orOp.add(std::move(sub0));
        orOp.add(std::move(sub1));
        orOp.add(std::move(sub2));
    }

    /**
     * Records 'count' short-circuits attributed to the child currently at position 'index'.
     */
    void recordAt(size_t index, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i) {
            orOp.recordMatch(orOp.getChildren().begin() + index);
        }
    }

    /**
     * Asserts which child is evaluated first. Only the front position is checked: the children are
     * ordered with 'std::sort', which is not stable, so the relative order of children with equal
     * counts is unspecified.
     */
    void assertFirst(MatchExpression* expected) const {
        ASSERT_EQ(static_cast<const void*>(orOp.getChild(0)), static_cast<const void*>(expected));
    }

    /**
     * Asserts that the children currently appear in the given order. Compares element-wise so that
     * a failure reports which position differs. Only safe when every child has a distinct count, or
     * when no reorder has happened.
     */
    void assertOrder(std::vector<MatchExpression*> expected) const {
        ASSERT_EQ(orOp.numChildren(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
            ASSERT_EQ(static_cast<const void*>(orOp.getChild(i)),
                      static_cast<const void*>(expected[i]))
                << " at position " << i;
        }
    }

    BSONObj operands[3] = {BSON("$lt" << 1), BSON("$lt" << 2), BSON("$lt" << 3)};
    std::vector<MatchExpression*> children;
    OrMatchExpression orOp;
};
}  // namespace

TEST(MatchExpressionReordering, DoesNotRecordWhenNotEnabled) {
    ReorderingFixture fixture;

    // Reordering was never enabled, so recording is a no-op even well past the threshold.
    fixture.recordAt(2, ListOfMatchExpression::kReorderIterations * 2);

    fixture.assertOrder(fixture.children);
    ASSERT_EQ(fixture.children[2]->getShortCircuitCounter(), 0u);
}

TEST(MatchExpressionReordering, DoesNotReorderBeforeTheThresholdIsReached) {
    ReorderingFixture fixture;
    fixture.orOp.allowReordering();

    fixture.recordAt(2, ListOfMatchExpression::kReorderIterations - 1);

    // Counts have accumulated on the child, but one short-circuit short of the threshold the order
    // is still untouched.
    fixture.assertOrder(fixture.children);
    ASSERT_EQ(fixture.children[2]->getShortCircuitCounter(),
              ListOfMatchExpression::kReorderIterations - 1);
}

TEST(MatchExpressionReordering, MovesMostFrequentlyShortCircuitingChildToFrontAtTheThreshold) {
    ReorderingFixture fixture;
    fixture.orOp.allowReordering();

    // The last child short-circuits every time, the others never do, so it must end up first. The
    // order of the two children tied at zero is unspecified.
    fixture.recordAt(2, ListOfMatchExpression::kReorderIterations);

    fixture.assertFirst(fixture.children[2]);
}

TEST(MatchExpressionReordering, ResetsCountersAfterReordering) {
    ReorderingFixture fixture;
    fixture.orOp.allowReordering();

    fixture.recordAt(2, ListOfMatchExpression::kReorderIterations);

    for (auto* child : fixture.children) {
        ASSERT_EQ(child->getShortCircuitCounter(), 0u);
    }
}

TEST(MatchExpressionReordering, OrdersChildrenByDescendingShortCircuitCount) {
    ReorderingFixture fixture;
    fixture.orOp.allowReordering();

    // Split the window so that child 1 short-circuits most, then child 2, then child 0.
    constexpr std::uint32_t kTotal = ListOfMatchExpression::kReorderIterations;
    fixture.recordAt(0, kTotal / 8);
    fixture.recordAt(2, kTotal / 4);
    fixture.recordAt(1, kTotal - kTotal / 8 - kTotal / 4);

    fixture.assertOrder({fixture.children[1], fixture.children[2], fixture.children[0]});
}

TEST(MatchExpressionReordering, AdaptsAcrossSuccessiveWindows) {
    ReorderingFixture fixture;
    fixture.orOp.allowReordering();

    fixture.recordAt(2, ListOfMatchExpression::kReorderIterations);
    fixture.assertFirst(fixture.children[2]);

    // In the next window whichever child now sits at the back dominates instead. Because counters
    // were cleared, the previous window does not outvote this one.
    auto* lastChild = fixture.orOp.getChild(fixture.orOp.numChildren() - 1);
    fixture.recordAt(fixture.orOp.numChildren() - 1, ListOfMatchExpression::kReorderIterations);

    fixture.assertFirst(lastChild);
}

TEST(MatchExpressionReordering, IsNotEnabledForNodesWithFewerThanTwoChildren) {
    BSONObj operand = BSON("$lt" << 1);
    OrMatchExpression orOp;
    orOp.add(std::make_unique<LTMatchExpression>("a"sv, operand["$lt"]));

    orOp.allowReordering();

    // Recording must remain a no-op: there is nothing to reorder, so no counters accumulate.
    for (std::uint32_t i = 0; i < ListOfMatchExpression::kReorderIterations; ++i) {
        orOp.recordMatch(orOp.getChildren().begin());
    }
    ASSERT_EQ(orOp.getChild(0)->getShortCircuitCounter(), 0u);
}

TEST(MatchExpressionReordering, CountersAloneDoNotAffectEquivalence) {
    ReorderingFixture withCounts;
    withCounts.orOp.allowReordering();

    ReorderingFixture untouched;

    // Accumulating counts without reaching the threshold leaves the child order alone, and the
    // counters themselves are not part of expression identity.
    withCounts.recordAt(2, 10);
    ASSERT_EQ(withCounts.children[2]->getShortCircuitCounter(), 10u);
    ASSERT(withCounts.orOp.equivalent(&untouched.orOp));
}

TEST(MatchExpressionReordering, ReorderingChangesEquivalenceBecauseChildOrderMatters) {
    ReorderingFixture reordered;
    reordered.orOp.allowReordering();

    ReorderingFixture untouched;
    ASSERT(reordered.orOp.equivalent(&untouched.orOp));

    reordered.recordAt(2, ListOfMatchExpression::kReorderIterations);

    // 'ListOfMatchExpression::equivalent()' compares children pairwise by position, so an actual
    // reorder makes the expression no longer equivalent to its original form. This is why
    // reordering must only ever be enabled on an expression that is not shared with a query shape,
    // a plan cache entry, or the CanonicalQuery that gets serialized out to shards.
    ASSERT_FALSE(reordered.orOp.equivalent(&untouched.orOp));
}

TEST(MatchExpressionReordering, ClonePreservesEnablementButNotCounters) {
    ReorderingFixture fixture;
    fixture.orOp.allowReordering();

    // Accumulate some counts without crossing the reorder threshold.
    fixture.recordAt(2, 10);
    ASSERT_EQ(fixture.children[2]->getShortCircuitCounter(), 10u);

    auto cloned = fixture.orOp.clone();
    auto* clonedList = static_cast<ListOfMatchExpression*>(cloned.get());

    // Counters start fresh in the clone.
    for (size_t i = 0; i < clonedList->numChildren(); ++i) {
        ASSERT_EQ(clonedList->getChild(i)->getShortCircuitCounter(), 0u);
    }

    // But reordering is still enabled, so the clone adapts on its own.
    auto* expectedFront = clonedList->getChild(2);
    for (std::uint32_t i = 0; i < ListOfMatchExpression::kReorderIterations; ++i) {
        clonedList->recordMatch(clonedList->getChildren().begin() + 2);
    }
    ASSERT_EQ(clonedList->getChild(0), expectedFront);
}

TEST(MatchExpressionReordering, CloneOfNonEnabledExpressionIsNotEnabled) {
    ReorderingFixture fixture;

    auto cloned = fixture.orOp.clone();
    auto* clonedList = static_cast<ListOfMatchExpression*>(cloned.get());

    for (std::uint32_t i = 0; i < ListOfMatchExpression::kReorderIterations; ++i) {
        clonedList->recordMatch(clonedList->getChildren().begin() + 2);
    }
    ASSERT_EQ(clonedList->getChild(2)->getShortCircuitCounter(), 0u);
}

TEST(MatchExpressionReordering, AndAndNorSupportReordering) {
    BSONObj operands[2] = {BSON("$lt" << 1), BSON("$lt" << 2)};

    for (bool isAnd : {true, false}) {
        auto sub0 = std::make_unique<LTMatchExpression>("a"sv, operands[0]["$lt"]);
        auto sub1 = std::make_unique<LTMatchExpression>("b"sv, operands[1]["$lt"]);
        auto* second = sub1.get();

        std::unique_ptr<ListOfMatchExpression> expr;
        if (isAnd) {
            expr = std::make_unique<AndMatchExpression>();
        } else {
            expr = std::make_unique<NorMatchExpression>();
        }
        expr->add(std::move(sub0));
        expr->add(std::move(sub1));
        expr->allowReordering();

        for (std::uint32_t i = 0; i < ListOfMatchExpression::kReorderIterations; ++i) {
            expr->recordMatch(expr->getChildren().begin() + 1);
        }
        ASSERT_EQ(expr->getChild(0), second);
    }
}

TEST(MatchExpressionReordering, EachNodeHasItsOwnMeasurementWindow) {
    // Two sibling $or nodes under an $and. Driving one to its threshold must not reorder the other,
    // because the trigger counter lives per node.
    BSONObj operands[4] = {BSON("$lt" << 1), BSON("$lt" << 2), BSON("$lt" << 3), BSON("$lt" << 4)};

    auto makeOr = [&](int lo, int hi, std::string_view lhs, std::string_view rhs) {
        auto o = std::make_unique<OrMatchExpression>();
        o->add(std::make_unique<LTMatchExpression>(lhs, operands[lo]["$lt"]));
        o->add(std::make_unique<LTMatchExpression>(rhs, operands[hi]["$lt"]));
        o->allowReordering();
        return o;
    };

    auto driven = makeOr(0, 1, "a"sv, "b"sv);
    auto quiet = makeOr(2, 3, "c"sv, "d"sv);
    auto* drivenSecond = driven->getChild(1);
    auto* quietFirst = quiet->getChild(0);

    for (std::uint32_t i = 0; i < ListOfMatchExpression::kReorderIterations; ++i) {
        driven->recordMatch(driven->getChildren().begin() + 1);
    }

    ASSERT_EQ(driven->getChild(0), drivenSecond);
    ASSERT_EQ(quiet->getChild(0), quietFirst);
}

TEST(MatchExpressionReordering, ReorderingSurvivesChildVectorMutation) {
    ReorderingFixture fixture;
    fixture.orOp.allowReordering();

    // Per-child counters live on the children, so mutating the child vector directly - as the
    // expression optimizer does during AND/OR absorption - cannot desynchronize the accounting.
    fixture.recordAt(2, 10);
    auto* survivor = fixture.children[2];

    auto* childVector = fixture.orOp.getChildVector();
    childVector->erase(childVector->begin());

    // The count followed the child through the erase.
    ASSERT_EQ(fixture.orOp.numChildren(), 2u);
    ASSERT_EQ(survivor->getShortCircuitCounter(), 10u);

    // And a newly added child simply starts at zero.
    BSONObj operand = BSON("$lt" << 4);
    fixture.orOp.add(std::make_unique<LTMatchExpression>("d"sv, operand["$lt"]));
    ASSERT_EQ(fixture.orOp.getChild(2)->getShortCircuitCounter(), 0u);
}

}  // namespace mongo
