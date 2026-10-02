// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint.h"

#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_lowering.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <variant>

namespace mongo::stage_builder {
namespace {

class SbBlueprintLoweringTest : public GoldenSbeExprBuilderTestFixture {
protected:
    std::string print(const SbStage& stage) {
        sbe::DebugPrintInfo debugPrintInfo{};
        return sbe::DebugPrinter().print(*stage, debugPrintInfo);
    }
};

TEST(SbBlueprintTest, AddReturnsIndexOfNewNode) {
    SbBlueprint blueprint;
    ASSERT_EQ(blueprint.add(SbBlueprintCoScan{}).value, 0u);
    ASSERT_EQ(blueprint.add(SbBlueprintCoScan{}).value, 1u);
    ASSERT_EQ(blueprint.size(), 2u);
    ASSERT_TRUE(std::holds_alternative<SbBlueprintCoScan>(blueprint[SbBlueprintNodeIdx{1}]));
}

TEST_F(SbBlueprintLoweringTest, LowerTreeWithIndexChildren) {
    SbBlueprint blueprint;
    auto left = blueprint.add(SbBlueprintCoScan{.nodeId = 1});
    auto unwind = blueprint.add(
        SbBlueprintUnwind{.child = left, .nodeId = 2, .inSlot = 10, .outSlot = 11, .idxSlot = 12});
    auto right = blueprint.add(SbBlueprintPassthrough{sbe::makeS<sbe::CoScanStage>(3)});
    auto root = blueprint.add(SbBlueprintUnion{
        .nodeId = 4, .children = {unwind, right}, .inputSlots = {{11}, {10}}, .outputSlots = {20}});

    auto stage = lowerSbeBlueprint(blueprint, root, *_state);
    ASSERT_EQ(print(stage),
              "[4] union [s20] \n"
              "    branch0 [s11] \n"
              "        [2] unwind s11 = outField, s12 = outIndex, s10 = inField "
              "!_preserveNullAndEmptyArrays \n"
              "        [1] coscan \n"
              "    branch1 [s10] \n"
              "        [3] coscan \n");
}

TEST_F(SbBlueprintLoweringTest, SbBuilderLowersEachStageImmediately) {
    SbBuilder b{*_state, 5};
    auto [unwound, outSlot, idxSlot] =
        b.makeUnwind(b.makeCoScan(), SbSlot{_state->slotId()}, false);
    auto unique = b.makeUnique(std::move(unwound), outSlot);
    auto [joined, _] = b.makeUnion(sbe::makeSs(std::move(unique), b.makeLimitOneCoScanTree()),
                                   {SbExpr::makeSV(outSlot), SbExpr::makeSV(idxSlot)});

    auto str = print(joined);
    ASSERT_STRING_CONTAINS(str, "[5] union");
    ASSERT_STRING_CONTAINS(str, "[5] unique");
    ASSERT_STRING_CONTAINS(str, "[5] unwind");
    ASSERT_STRING_CONTAINS(str, "[5] limit 1");
}

class SbBlueprintLoweringDeathTest : public SbBlueprintLoweringTest {};

DEATH_TEST_F(SbBlueprintLoweringDeathTest, LowerOutOfRangeIndexFails, "12702000") {
    SbBlueprint blueprint;
    blueprint.add(SbBlueprintCoScan{});
    lowerSbeBlueprint(blueprint, SbBlueprintNodeIdx{1}, *_state);
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
