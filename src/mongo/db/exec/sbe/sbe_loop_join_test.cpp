// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for sbe::LoopJoinStage.
 */

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/sbe_unittest_assert.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>
#include <vector>

namespace mongo::sbe {

using LoopJoinStageTest = PlanStageTestFixture;

TEST_F(LoopJoinStageTest, LoopJoinNoPredicate) {
    auto ctx = makeCompileCtx();

    // Build a scan for the outer loop.
    auto [outerScanSlot, outerScanStage] = generateVirtualScan(BSON_ARRAY(1 << 2));

    // Build a scan for the inner loop.
    auto [innerScanSlot, innerScanStage] = generateVirtualScan(BSON_ARRAY(3 << 4 << 5));

    // Build and prepare for execution loop join of the two scan stages.
    auto loopJoin = makeS<LoopJoinStage>(std::move(outerScanStage),
                                         std::move(innerScanStage),
                                         makeSV(outerScanSlot) /*outerProjects*/,
                                         makeSV() /*outerCorrelated*/,
                                         nullptr /*predicate*/,
                                         kEmptyPlanNodeId);

    prepareTree(ctx.get(), loopJoin.get());
    auto outer = loopJoin->getAccessor(*ctx, outerScanSlot);
    auto inner = loopJoin->getAccessor(*ctx, innerScanSlot);

    // Expected output: cartesian product of the two scans.
    std::vector<std::pair<int, int>> expected{{1, 3}, {1, 4}, {1, 5}, {2, 3}, {2, 4}, {2, 5}};
    int i = 0;
    for (auto st = loopJoin->getNext(); st == PlanState::ADVANCED; st = loopJoin->getNext(), i++) {
        ASSERT_LT(i, expected.size());

        auto [outerTag, outerVal] = outer->getViewOfValue();
        ASSERT_SBE_VALUE_EQ(outerTag, outerVal, value::TypeTags::NumberInt32, expected[i].first);

        auto [innerTag, innerVal] = inner->getViewOfValue();
        ASSERT_SBE_VALUE_EQ(innerTag, innerVal, value::TypeTags::NumberInt32, expected[i].second);
    }
    ASSERT_EQ(i, expected.size());
}

TEST_F(LoopJoinStageTest, LoopJoinConstTruePredicate) {
    auto ctx = makeCompileCtx();

    // Build a scan for the outer loop.
    auto [outerScanSlot, outerScanStage] = generateVirtualScan(BSON_ARRAY(1 << 2));

    // Build a scan for the inner loop.
    auto [innerScanSlot, innerScanStage] = generateVirtualScan(BSON_ARRAY(3 << 4 << 5));

    // Build and prepare for execution loop join of the two scan stages.
    auto loopJoin = makeS<LoopJoinStage>(std::move(outerScanStage),
                                         std::move(innerScanStage),
                                         makeSV(outerScanSlot) /*outerProjects*/,
                                         makeSV() /*outerCorrelated*/,
                                         makeBoolConstant(true) /*predicate*/,
                                         kEmptyPlanNodeId);
    prepareTree(ctx.get(), loopJoin.get());
    auto outer = loopJoin->getAccessor(*ctx, outerScanSlot);
    auto inner = loopJoin->getAccessor(*ctx, innerScanSlot);

    // Expected output: cartesian product of the two scans.
    std::vector<std::pair<int, int>> expected{{1, 3}, {1, 4}, {1, 5}, {2, 3}, {2, 4}, {2, 5}};
    int i = 0;
    for (auto st = loopJoin->getNext(); st == PlanState::ADVANCED; st = loopJoin->getNext(), i++) {
        ASSERT_LT(i, expected.size());

        auto [outerTag, outerVal] = outer->getViewOfValue();
        ASSERT_SBE_VALUE_EQ(outerTag, outerVal, value::TypeTags::NumberInt32, expected[i].first);

        auto [innerTag, innerVal] = inner->getViewOfValue();
        ASSERT_SBE_VALUE_EQ(innerTag, innerVal, value::TypeTags::NumberInt32, expected[i].second);
    }
    ASSERT_EQ(i, expected.size());
}

TEST_F(LoopJoinStageTest, LoopJoinConstFalsePredicate) {
    auto ctx = makeCompileCtx();

    // Build a scan for the outer loop.
    auto [outerScanSlot, outerScanStage] = generateVirtualScan(BSON_ARRAY(1 << 2));

    // Build a scan for the inner loop.
    auto [innerScanSlot, innerScanStage] = generateVirtualScan(BSON_ARRAY(3 << 4 << 5));

    // Build and prepare for execution loop join of the two scan stages.
    auto loopJoin = makeS<LoopJoinStage>(std::move(outerScanStage),
                                         std::move(innerScanStage),
                                         makeSV(outerScanSlot) /*outerProjects*/,
                                         makeSV() /*outerCorrelated*/,
                                         makeBoolConstant(false) /*predicate*/,
                                         kEmptyPlanNodeId);
    prepareTree(ctx.get(), loopJoin.get());

    // Executing the stage should produce no results because of the predicate filter.
    ASSERT(PlanState::IS_EOF == loopJoin->getNext());
}

TEST_F(LoopJoinStageTest, LeftLoopJoinConstFalsePredicate) {
    auto ctx = makeCompileCtx();

    // Build a scan for the outer loop.
    auto [outerScanSlot, outerScanStage] = generateVirtualScan(BSON_ARRAY(1 << 2));

    // Build a scan for the inner loop.
    auto [innerScanSlot, innerScanStage] = generateVirtualScan(BSON_ARRAY(3 << 4 << 5));

    // Build and prepare for execution loop join of the two scan stages.
    auto loopJoin = makeS<LoopJoinStage>(std::move(outerScanStage),
                                         std::move(innerScanStage),
                                         makeSV(outerScanSlot) /*outerProjects*/,
                                         makeSV() /*outerCorrelated*/,
                                         makeSV(innerScanSlot) /*innerProjects*/,
                                         makeBoolConstant(false) /*predicate*/,
                                         JoinType::Left,
                                         kEmptyPlanNodeId);
    prepareTree(ctx.get(), loopJoin.get());
    auto outer = loopJoin->getAccessor(*ctx, outerScanSlot);
    auto inner = loopJoin->getAccessor(*ctx, innerScanSlot);

    // Expected output: each outer doc has no match, but it's part of the results due to left join
    // being used.
    std::vector<int> expected{1, 2};
    int i = 0;
    for (auto st = loopJoin->getNext(); st == PlanState::ADVANCED; st = loopJoin->getNext(), i++) {
        ASSERT_LT(i, expected.size());

        auto [outerTag, outerVal] = outer->getViewOfValue();
        ASSERT_SBE_VALUE_EQ(outerTag, outerVal, value::TypeTags::NumberInt32, expected[i]);

        auto [innerTag, innerVal] = inner->getViewOfValue();
        ASSERT_SBE_VALUE_EQ(innerTag, innerVal, value::TypeTags::Nothing, 0);
    }
    ASSERT_EQ(i, expected.size());
}

TEST_F(LoopJoinStageTest, LoopJoinEqualityPredicate) {
    auto ctx = makeCompileCtx();

    // Build a scan for the outer loop.
    auto [outerScanSlot, outerScanStage] = generateVirtualScan(BSON_ARRAY(1 << 2 << 3 << 4 << 1));

    // Build a scan for the inner loop.
    auto [innerScanSlot, innerScanStage] = generateVirtualScan(BSON_ARRAY(3 << 1 << 5 << 3 << 3));

    // Build and prepare for execution loop join of the two scan stages.
    auto predicate = makeE<EPrimBinary>(
        EPrimBinary::eq, makeE<EVariable>(outerScanSlot), makeE<EVariable>(innerScanSlot));
    auto loopJoin = makeS<LoopJoinStage>(std::move(outerScanStage),
                                         std::move(innerScanStage),
                                         makeSV(outerScanSlot) /*outerProjects*/,
                                         makeSV() /*outerCorrelated*/,
                                         std::move(predicate),
                                         kEmptyPlanNodeId);
    prepareTree(ctx.get(), loopJoin.get());
    auto inner = loopJoin->getAccessor(*ctx, innerScanSlot);

    // Expected output should filter from the inner array elements that exist in the outer.
    std::vector<int> expected{1, 3, 3, 3, 1};
    int i = 0;
    for (auto st = loopJoin->getNext(); st == PlanState::ADVANCED; st = loopJoin->getNext(), i++) {
        ASSERT_LT(i, expected.size());

        auto [innerTag, innerVal] = inner->getViewOfValue();
        ASSERT_SBE_VALUE_EQ(innerTag, innerVal, value::TypeTags::NumberInt32, expected[i]);
    }
    ASSERT_EQ(i, expected.size());
}
}  // namespace mongo::sbe
