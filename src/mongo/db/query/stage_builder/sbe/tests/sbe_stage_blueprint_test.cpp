// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint.h"

#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/exec/trial_run_tracker.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_lowering.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <variant>

#include <boost/optional/optional.hpp>

namespace mongo::stage_builder {
namespace {

class SbBlueprintLoweringTest : public GoldenSbeExprBuilderTestFixture {
protected:
    std::string print(const sbe::PlanStage& stage) {
        sbe::DebugPrintInfo debugPrintInfo{};
        return sbe::DebugPrinter().print(stage, debugPrintInfo);
    }

    std::string lowerAndPrint(SbStage stage) {
        auto lowered = lowerSbeBlueprint(_state->blueprint(), stage.release(), *_state);
        return print(*lowered);
    }
};

TEST(SbBlueprintTest, AddReturnsIndexOfNewNode) {
    SbBlueprint blueprint;
    ASSERT_EQ(blueprint.add(SbBlueprintCoScan{}).value, 0u);
    ASSERT_EQ(blueprint.add(SbBlueprintCoScan{}).value, 1u);
    ASSERT_EQ(blueprint.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<SbBlueprintCoScan>(blueprint[SbBlueprintNodeIdx{1}]));
}

TEST(SbStageTest, HandleIsEmptyAfterMoveAndRelease) {
    SbStage empty;
    ASSERT_FALSE(empty);

    SbStage stage{SbBlueprintNodeIdx{7}};
    ASSERT_TRUE(stage);
    ASSERT_EQ(stage.idx().value, 7u);

    SbStage moved = std::move(stage);
    ASSERT_FALSE(stage);  // NOLINT(bugprone-use-after-move)
    ASSERT_TRUE(moved);

    ASSERT_EQ(moved.release().value, 7u);
    ASSERT_FALSE(moved);
}

TEST_F(SbBlueprintLoweringTest, LowerTreeWithIndexChildren) {
    SbBlueprint blueprint;
    auto left = blueprint.add(SbBlueprintCoScan{.nodeId = 1});
    auto unwind = blueprint.add(
        SbBlueprintUnwind{.child = left, .nodeId = 2, .inSlot = 10, .outSlot = 11, .idxSlot = 12});
    auto right = blueprint.add(SbBlueprintCoScan{.nodeId = 3});
    auto root = blueprint.add(SbBlueprintUnion{
        .nodeId = 4, .children = {unwind, right}, .inputSlots = {{11}, {10}}, .outputSlots = {20}});

    auto stage = lowerSbeBlueprint(blueprint, root, *_state);
    ASSERT_EQ(print(*stage),
              "[4] union [s20]\n"
              "    branch0 [s11]\n"
              "        [2] unwind s11 = outField, s12 = outIndex, s10 = inField "
              "!_preserveNullAndEmptyArrays\n"
              "        [1] coscan\n"
              "    branch1 [s10]\n"
              "        [3] coscan");
}

TEST_F(SbBlueprintLoweringTest, SbBuilderRetainsBlueprintUntilLowered) {
    SbBuilder b{*_state, 5};
    auto& blueprint = _state->blueprint();

    auto [unwound, outSlot, idxSlot] =
        b.makeUnwind(b.makeCoScan(), SbSlot{_state->slotId()}, false);
    auto unique = b.makeUnique(std::move(unwound), outSlot);
    auto [joined, _] =
        b.makeUnion(SbStage::makeVector(std::move(unique), b.makeLimitOneCoScanTree()),
                    {SbExpr::makeSV(outSlot), SbExpr::makeSV(idxSlot)});

    // Nothing has been lowered yet: every stage built above is a node of the blueprint, and the
    // root handle refers to the union node.
    ASSERT_EQ(blueprint.size(), 6u);
    ASSERT_TRUE(std::holds_alternative<SbBlueprintUnion>(blueprint[joined.idx()]));
    for (uint32_t i = 0; i < blueprint.size(); ++i) {
        ASSERT_EQ(getNodeId(blueprint[SbBlueprintNodeIdx{i}]), 5);
    }
    ASSERT_TRUE(std::holds_alternative<SbBlueprintCoScan>(blueprint[SbBlueprintNodeIdx{0}]));
    ASSERT_TRUE(std::holds_alternative<SbBlueprintUnwind>(blueprint[SbBlueprintNodeIdx{1}]));
    ASSERT_TRUE(std::holds_alternative<SbBlueprintUnique>(blueprint[SbBlueprintNodeIdx{2}]));
    ASSERT_TRUE(std::holds_alternative<SbBlueprintCoScan>(blueprint[SbBlueprintNodeIdx{3}]));
    ASSERT_TRUE(std::holds_alternative<SbBlueprintLimitSkip>(blueprint[SbBlueprintNodeIdx{4}]));

    auto str = lowerAndPrint(std::move(joined));
    ASSERT_STRING_CONTAINS(str, "[5] union");
    ASSERT_STRING_CONTAINS(str, "[5] unique");
    ASSERT_STRING_CONTAINS(str, "[5] unwind");
    ASSERT_STRING_CONTAINS(str, "[5] limit 1");
}

TEST(SbBlueprintTest, TrialRunTrackingGuardMarksNodesAddedInScope) {
    SbBlueprint blueprint;
    blueprint.add(SbBlueprintCoScan{});
    {
        NoTrialRunTrackingGuard guard{blueprint};
        blueprint.add(SbBlueprintCoScan{});
    }
    blueprint.add(SbBlueprintCoScan{});
    ASSERT_FALSE(blueprint.isTrialRunTrackingDisabled(SbBlueprintNodeIdx{0}));
    ASSERT_TRUE(blueprint.isTrialRunTrackingDisabled(SbBlueprintNodeIdx{1}));
    ASSERT_FALSE(blueprint.isTrialRunTrackingDisabled(SbBlueprintNodeIdx{2}));
}

TEST_F(SbBlueprintLoweringTest, DisableTrialRunTrackingAppliesToLoweredStage) {
    // Builds limit(nodeId 2) -> limit 1(nodeId 1) -> coscan(nodeId 1). With node 1 as the runtime
    // planning root, attaching a trial run tracker reports TrackResults for the inner limit
    // unless its trial run tracking has been disabled.
    auto build = [&](bool disableInner) {
        boost::optional<NoTrialRunTrackingGuard> guard;
        if (disableInner) {
            guard.emplace(_state->blueprint());
        }
        SbBuilder innerBuilder{*_state, 1};
        auto inner = innerBuilder.makeLimitOneCoScanTree();
        guard.reset();
        SbBuilder rootBuilder{*_state, 2};
        auto root = rootBuilder.makeLimit(std::move(inner), rootBuilder.makeInt64Constant(2));
        return lowerSbeBlueprint(_state->blueprint(), root.release(), *_state);
    };

    auto tracker =
        std::make_unique<TrialRunTracker>(boost::none, boost::none, boost::none, boost::none);

    auto enabled = build(false /* disableInner */);
    ASSERT_EQ(enabled->attachToTrialRunTracker(tracker.get(), PlanNodeId{1}),
              sbe::PlanStage::TrialRunTrackingType::TrackResults);
    enabled->detachFromTrialRunTracker();

    auto disabled = build(true /* disableInner */);
    ASSERT_EQ(disabled->attachToTrialRunTracker(tracker.get(), PlanNodeId{1}),
              sbe::PlanStage::TrialRunTrackingType::NoTracking);
    disabled->detachFromTrialRunTracker();
}

TEST_F(SbBlueprintLoweringTest, OutOfLineNodeLowersAndReadsBack) {
    SbBuilder b{*_state, 7};
    auto [scan, recordSlot, recordIdSlot, fieldSlots] =
        b.makeScan(UUID::gen(), DatabaseName::kAdmin, true /* forward */, {"a"});
    ASSERT_TRUE(std::holds_alternative<SbBlueprintScan>(_state->blueprint()[scan.idx()]));
    ASSERT_EQ(getNodeId(_state->blueprint()[scan.idx()]), 7);
}

class SbBlueprintLoweringDeathTest : public SbBlueprintLoweringTest {};

DEATH_TEST_F(SbBlueprintLoweringDeathTest, LowerOutOfRangeIndexFails, "12702000") {
    SbBlueprint blueprint;
    blueprint.add(SbBlueprintCoScan{});
    lowerSbeBlueprint(blueprint, SbBlueprintNodeIdx{1}, *_state);
}

DEATH_TEST_F(SbBlueprintLoweringDeathTest, EmptyHandleIdxFails, "13621600") {
    SbStage empty;
    empty.idx();
}

DEATH_TEST_F(SbBlueprintLoweringDeathTest, LowerSharedChildFails, "12702001") {
    SbBlueprint blueprint;
    auto child = blueprint.add(SbBlueprintCoScan{});
    auto root = blueprint.add(
        SbBlueprintUnion{.children = {child, child}, .inputSlots = {{}, {}}, .outputSlots = {}});
    lowerSbeBlueprint(blueprint, root, *_state);
}

DEATH_TEST_F(SbBlueprintLoweringDeathTest, LowerWithUnreachableNodeFails, "12702002") {
    SbBlueprint blueprint;
    blueprint.add(SbBlueprintCoScan{});
    auto root = blueprint.add(SbBlueprintCoScan{});
    lowerSbeBlueprint(blueprint, root, *_state);
}

}  // namespace
}  // namespace mongo::stage_builder
