// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for sbe::AggProjectStage.
 */

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/agg_project.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo::sbe {

using AggProjectStageTest = PlanStageTestFixture;

TEST_F(AggProjectStageTest, SimpleCountTest) {
    value::TagValueOwned input = value::TagValueOwned::fromRaw(
        stage_builder::makeValue(BSON_ARRAY(10LL << 20LL << 30LL << 40LL << 50LL)));

    value::TagValueOwned expected = value::TagValueOwned::fromRaw(
        stage_builder::makeValue(BSON_ARRAY(1LL << 2LL << 3LL << 4LL << 5LL)));

    auto makeStageFn = [&](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto outSlot = generateSlotId();
        auto aggProject = makeS<AggProjectStage>(
            std::move(scanStage),
            makeAggExprVector(outSlot,
                              nullptr,
                              makeFunction(EFn::kSum,
                                           makeE<EConstant>(value::TypeTags::NumberInt64,
                                                            value::bitcastFrom<int64_t>(1)))),
            kEmptyPlanNodeId);

        return std::make_pair(outSlot, std::move(aggProject));
    };

    runTest(std::move(input), std::move(expected), makeStageFn);
}

TEST_F(AggProjectStageTest, SimpleSumTest) {
    value::TagValueOwned input = value::TagValueOwned::fromRaw(
        stage_builder::makeValue(BSON_ARRAY(10LL << 20LL << 30LL << 40LL << 50LL)));

    value::TagValueOwned expected = value::TagValueOwned::fromRaw(
        stage_builder::makeValue(BSON_ARRAY(10LL << 30LL << 60LL << 100LL << 150LL)));

    auto makeStageFn = [&](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto outSlot = generateSlotId();
        auto aggProject = makeS<AggProjectStage>(
            std::move(scanStage),
            makeAggExprVector(outSlot, nullptr, makeFunction(EFn::kSum, makeVariable(scanSlot))),
            kEmptyPlanNodeId);
        return std::make_pair(outSlot, std::move(aggProject));
    };

    runTest(std::move(input), std::move(expected), makeStageFn);
}

TEST_F(AggProjectStageTest, SumWithInitTest) {
    value::TagValueOwned input = value::TagValueOwned::fromRaw(
        stage_builder::makeValue(BSON_ARRAY(10LL << 20LL << 30LL << 40LL << 50LL)));

    value::TagValueOwned expected = value::TagValueOwned::fromRaw(
        stage_builder::makeValue(BSON_ARRAY(110LL << 130LL << 160LL << 200LL << 250LL)));

    auto makeStageFn = [&](value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
        auto outSlot = generateSlotId();
        auto aggProject = makeS<AggProjectStage>(
            std::move(scanStage),
            makeAggExprVector(
                outSlot,
                makeE<EConstant>(value::TypeTags::NumberInt64, value::bitcastFrom<int64_t>(100)),
                makeFunction(EFn::kSum, makeVariable(scanSlot))),
            kEmptyPlanNodeId);
        return std::make_pair(outSlot, std::move(aggProject));
    };

    runTest(std::move(input), std::move(expected), makeStageFn);
}

}  // namespace mongo::sbe
