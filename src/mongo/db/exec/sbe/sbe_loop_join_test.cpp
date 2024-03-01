/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

/**
 * This file contains tests for sbe::LoopJoinStage.
 */

#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/spool.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

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
        assertValuesEqual(outerTag, outerVal, value::TypeTags::NumberInt32, expected[i].first);

        auto [innerTag, innerVal] = inner->getViewOfValue();
        assertValuesEqual(innerTag, innerVal, value::TypeTags::NumberInt32, expected[i].second);
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
                                         stage_builder::makeBoolConstant(true) /*predicate*/,
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
        assertValuesEqual(outerTag, outerVal, value::TypeTags::NumberInt32, expected[i].first);

        auto [innerTag, innerVal] = inner->getViewOfValue();
        assertValuesEqual(innerTag, innerVal, value::TypeTags::NumberInt32, expected[i].second);
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
                                         stage_builder::makeBoolConstant(false) /*predicate*/,
                                         kEmptyPlanNodeId);
    prepareTree(ctx.get(), loopJoin.get());

    // Executing the stage should produce no results because of the predicate filter.
    ASSERT(PlanState::IS_EOF == loopJoin->getNext());
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
        assertValuesEqual(innerTag, innerVal, value::TypeTags::NumberInt32, expected[i]);
    }
    ASSERT_EQ(i, expected.size());
}

TEST_F(LoopJoinStageTest, LoopJoinInnerBlockingStage) {
    auto ctx = makeCompileCtx();

    // Build a scan for the outer loop.
    auto [outerScanSlot, outerScanStage] = generateVirtualScan(BSON_ARRAY(1 << 2));

    // Build a scan for the inner loop.
    auto [innerScanSlot, innerScanStage] = generateVirtualScan(BSON_ARRAY(3 << 4 << 5));

    auto spoolStage = makeS<SpoolEagerProducerStage>(std::move(innerScanStage),
                                                     generateSpoolId(),
                                                     makeSV(innerScanSlot),
                                                     nullptr /* yieldPolicy */,
                                                     kEmptyPlanNodeId);

    // Build and prepare for execution loop join of the two scan stages.
    auto loopJoin = makeS<LoopJoinStage>(std::move(outerScanStage),
                                         std::move(spoolStage),
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
        assertValuesEqual(outerTag, outerVal, value::TypeTags::NumberInt32, expected[i].first);

        auto [innerTag, innerVal] = inner->getViewOfValue();
        assertValuesEqual(innerTag, innerVal, value::TypeTags::NumberInt32, expected[i].second);
    }
    ASSERT_EQ(i, expected.size());
}
}  // namespace mongo::sbe
