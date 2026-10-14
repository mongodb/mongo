// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

/**
 * This file contains tests for sbe::ProjectStage.
 */

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/size_estimator.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/stage_builder/sbe/gen_helpers.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <utility>

namespace mongo::sbe {
namespace {

std::unique_ptr<EExpression> makeInt32(int32_t num) {
    return makeE<EConstant>(value::TypeTags::NumberInt32, value::bitcastFrom<int32_t>(num));
}

std::unique_ptr<EExpression> makeAdd(std::unique_ptr<EExpression> lhs,
                                     std::unique_ptr<EExpression> rhs) {
    return makeE<EPrimBinary>(EPrimBinary::add, std::move(lhs), std::move(rhs));
}

std::unique_ptr<EExpression> makeMul(std::unique_ptr<EExpression> lhs,
                                     std::unique_ptr<EExpression> rhs) {
    return makeE<EPrimBinary>(EPrimBinary::mul, std::move(lhs), std::move(rhs));
}

std::unique_ptr<EExpression> makeVar(value::SlotId slot) {
    return makeE<EVariable>(slot);
}

template <typename... Ts>
std::unique_ptr<PlanStage> makeProject(ProjectStage::Binding binding,
                                       std::unique_ptr<PlanStage> input,
                                       Ts&&... pack) {
    if (binding == ProjectStage::Binding::kSequential) {
        return makeSequentialProjectStage(
            std::move(input), kEmptyPlanNodeId, std::forward<Ts>(pack)...);
    }
    return makeProjectStage(std::move(input), kEmptyPlanNodeId, std::forward<Ts>(pack)...);
}

// Each input row is a single-element array, so the scan produces one slot.
const BSONArray kInput =
    BSON_ARRAY(BSON_ARRAY(1) << BSON_ARRAY(2) << BSON_ARRAY(3) << BSON_ARRAY(4));

}  // namespace

using ProjectStageTest = PlanStageTestFixture;

TEST_F(ProjectStageTest, StageName) {
    auto [scanSlot, scanStage] = generateVirtualScan(BSON_ARRAY(1));
    auto project = makeProjectStage(
        std::move(scanStage), kEmptyPlanNodeId, generateSlotId(), makeVar(scanSlot));
    ASSERT_EQ(project->getCommonStats()->stageType, "project");
    ASSERT_EQ(project->clone()->getCommonStats()->stageType, "project");

    auto [seqScanSlot, seqScanStage] = generateVirtualScan(BSON_ARRAY(1));
    auto seqProject = makeSequentialProjectStage(
        std::move(seqScanStage), kEmptyPlanNodeId, generateSlotId(), makeVar(seqScanSlot));
    ASSERT_EQ(seqProject->getCommonStats()->stageType, "project*");
    ASSERT_EQ(seqProject->clone()->getCommonStats()->stageType, "project*");
}

TEST_F(ProjectStageTest, IndependentExpressions) {
    // Independent expressions produce the same results with either binding mode.
    for (auto binding : {ProjectStage::Binding::kParallel, ProjectStage::Binding::kSequential}) {
        // project [s2 = s1 + 1, s3 = s1 * 10]
        auto makeStageFn = [&](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
            auto s1 = scanSlots[0];
            auto s2 = generateSlotId();
            auto s3 = generateSlotId();
            auto project = makeProject(binding,
                                       std::move(scanStage),
                                       s2,
                                       makeAdd(makeVar(s1), makeInt32(1)),
                                       s3,
                                       makeMul(makeVar(s1), makeInt32(10)));
            return std::make_pair(value::SlotVector{s2, s3}, std::move(project));
        };

        auto input = value::TagValueOwned::fromRaw(stage_builder::makeValue(kInput));
        auto expected = value::TagValueOwned::fromRaw(stage_builder::makeValue(
            BSON_ARRAY(BSON_ARRAY(2 << 10)
                       << BSON_ARRAY(3 << 20) << BSON_ARRAY(4 << 30) << BSON_ARRAY(5 << 40))));
        auto [inputTag, inputVal] = input.releaseToRaw();
        auto [expectedTag, expectedVal] = expected.releaseToRaw();
        runTestMulti(1, inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
    }
}

TEST_F(ProjectStageTest, SequentialBindingRefersToEarlierSlot) {
    // project* [s2 = s1 + 1, s3 = s2 * 10]
    auto makeStageFn = [this](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
        auto s1 = scanSlots[0];
        auto s2 = generateSlotId();
        auto s3 = generateSlotId();
        auto project = makeSequentialProjectStage(std::move(scanStage),
                                                  kEmptyPlanNodeId,
                                                  s2,
                                                  makeAdd(makeVar(s1), makeInt32(1)),
                                                  s3,
                                                  makeMul(makeVar(s2), makeInt32(10)));
        return std::make_pair(value::SlotVector{s2, s3}, std::move(project));
    };

    // 's3' must be computed from the value of 's2' for the same row, not the previous row.
    auto input = value::TagValueOwned::fromRaw(stage_builder::makeValue(kInput));
    auto expected = value::TagValueOwned::fromRaw(stage_builder::makeValue(BSON_ARRAY(
        BSON_ARRAY(2 << 20) << BSON_ARRAY(3 << 30) << BSON_ARRAY(4 << 40) << BSON_ARRAY(5 << 50))));
    auto [inputTag, inputVal] = input.releaseToRaw();
    auto [expectedTag, expectedVal] = expected.releaseToRaw();
    runTestMulti(1, inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(ProjectStageTest, SequentialBindingChainOfDependentExpressions) {
    // project* [s2 = s1 + 1, s3 = s2 * 10, s4 = s3 + s2 + s1]
    auto makeStageFn = [this](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
        auto s1 = scanSlots[0];
        auto s2 = generateSlotId();
        auto s3 = generateSlotId();
        auto s4 = generateSlotId();
        auto project =
            makeSequentialProjectStage(std::move(scanStage),
                                       kEmptyPlanNodeId,
                                       s2,
                                       makeAdd(makeVar(s1), makeInt32(1)),
                                       s3,
                                       makeMul(makeVar(s2), makeInt32(10)),
                                       s4,
                                       makeAdd(makeAdd(makeVar(s3), makeVar(s2)), makeVar(s1)));
        return std::make_pair(value::SlotVector{s1, s2, s3, s4}, std::move(project));
    };

    auto input = value::TagValueOwned::fromRaw(stage_builder::makeValue(kInput));
    auto expected = value::TagValueOwned::fromRaw(stage_builder::makeValue(
        BSON_ARRAY(BSON_ARRAY(1 << 2 << 20 << 23)
                   << BSON_ARRAY(2 << 3 << 30 << 35) << BSON_ARRAY(3 << 4 << 40 << 47)
                   << BSON_ARRAY(4 << 5 << 50 << 59))));
    auto [inputTag, inputVal] = input.releaseToRaw();
    auto [expectedTag, expectedVal] = expected.releaseToRaw();
    runTestMulti(1, inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
}

TEST_F(ProjectStageTest, CompileTimeSizeIncludesSlotIndex) {
    // The SBE plan cache budgets entries using estimateCompileTimeSize(), so the estimate must
    // account for the per-projection slot index in addition to the child and the expressions.
    constexpr size_t kNumProjects = 100;
    auto [scanSlot, scanStage] = generateVirtualScan(BSON_ARRAY(1));
    const size_t childSize = scanStage->estimateCompileTimeSize();

    SlotExprPairVector projects;
    value::SlotMap<size_t> expectedSlotIndex;
    for (size_t idx = 0; idx < kNumProjects; ++idx) {
        auto slot = generateSlotId();
        projects.emplace_back(slot, makeVar(scanSlot));
        expectedSlotIndex.emplace(slot, idx);
    }
    const size_t projectsSize = size_estimator::estimate(projects);

    auto project = makeS<ProjectStage>(std::move(scanStage), std::move(projects), kEmptyPlanNodeId);
    ASSERT_GTE(project->estimateCompileTimeSize(),
               sizeof(ProjectStage) + childSize + projectsSize +
                   size_estimator::estimate(expectedSlotIndex));
}

TEST_F(ProjectStageTest, ParentStageSeesAllOutputSlots) {
    // In both binding modes, a parent stage can read every output slot of its child project.
    for (auto binding : {ProjectStage::Binding::kParallel, ProjectStage::Binding::kSequential}) {
        // project [s4 = s3 + s2] (project(*) [s2 = s1 + 1, s3 = s1 * 10] scan)
        auto makeStageFn = [&](value::SlotVector scanSlots, std::unique_ptr<PlanStage> scanStage) {
            auto s1 = scanSlots[0];
            auto s2 = generateSlotId();
            auto s3 = generateSlotId();
            auto s4 = generateSlotId();
            auto inner = makeProject(binding,
                                     std::move(scanStage),
                                     s2,
                                     makeAdd(makeVar(s1), makeInt32(1)),
                                     s3,
                                     makeMul(makeVar(s1), makeInt32(10)));
            auto outer = makeProjectStage(
                std::move(inner), kEmptyPlanNodeId, s4, makeAdd(makeVar(s3), makeVar(s2)));
            return std::make_pair(value::SlotVector{s4}, std::move(outer));
        };

        auto input = value::TagValueOwned::fromRaw(stage_builder::makeValue(kInput));
        auto expected = value::TagValueOwned::fromRaw(stage_builder::makeValue(
            BSON_ARRAY(BSON_ARRAY(12) << BSON_ARRAY(23) << BSON_ARRAY(34) << BSON_ARRAY(45))));
        auto [inputTag, inputVal] = input.releaseToRaw();
        auto [expectedTag, expectedVal] = expected.releaseToRaw();
        runTestMulti(1, inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
    }
}

using ProjectStageDeathTest = ProjectStageTest;

DEATH_TEST_REGEX_F(ProjectStageDeathTest,
                   WithoutSequentialBindingCannotReferToEarlierSlot,
                   "Tripwire assertion.*4946301") {
    // project [s2 = s1 + 1, s3 = s2] is invalid because 's3' refers to another output slot of the
    // same stage.
    auto ctx = makeCompileCtx();
    auto [scanSlot, scanStage] = generateVirtualScan(BSON_ARRAY(1 << 2));
    auto s2 = generateSlotId();
    auto s3 = generateSlotId();
    auto project = makeProjectStage(std::move(scanStage),
                                    kEmptyPlanNodeId,
                                    s2,
                                    makeAdd(makeVar(scanSlot), makeInt32(1)),
                                    s3,
                                    makeVar(s2));
    prepareTree(ctx.get(), project.get(), s3);
}

DEATH_TEST_REGEX_F(ProjectStageDeathTest,
                   SequentialBindingCannotReferToLaterSlot,
                   "Tripwire assertion.*4946301") {
    // project* [s2 = s3 + 1, s3 = s1] is invalid because 's2' refers to a slot defined after it.
    auto ctx = makeCompileCtx();
    auto [scanSlot, scanStage] = generateVirtualScan(BSON_ARRAY(1 << 2));
    auto s2 = generateSlotId();
    auto s3 = generateSlotId();
    auto project = makeSequentialProjectStage(std::move(scanStage),
                                              kEmptyPlanNodeId,
                                              s2,
                                              makeAdd(makeVar(s3), makeInt32(1)),
                                              s3,
                                              makeVar(scanSlot));
    prepareTree(ctx.get(), project.get(), s2);
}

DEATH_TEST_REGEX_F(ProjectStageDeathTest,
                   SlotCannotBeDefinedTwice,
                   "Tripwire assertion.*13572200") {
    auto [scanSlot, scanStage] = generateVirtualScan(BSON_ARRAY(1 << 2));
    auto s2 = generateSlotId();
    auto project = makeProjectStage(std::move(scanStage),
                                    kEmptyPlanNodeId,
                                    s2,
                                    makeAdd(makeVar(scanSlot), makeInt32(1)),
                                    s2,
                                    makeVar(scanSlot));
}

}  // namespace mongo::sbe
