// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_slot_usage.h"

#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/query/record_id_range_list.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <variant>

namespace mongo::stage_builder {
namespace {
using sbe::value::SlotId;
using sbe::value::SlotVector;

/**
 * Tests for the slots each blueprint node type reports through def()/use() and for the
 * SbBlueprintSlotUsage summary built from them over a whole tree.
 */
class SbBlueprintSlotUsageTest : public GoldenSbeExprBuilderTestFixture {
protected:
    SbBlueprint& blueprint() {
        return _state->blueprint();
    }

    SbSlot slot() {
        return SbSlot{_state->slotId()};
    }

    static SlotVector sorted(SlotVector v) {
        std::sort(v.begin(), v.end());
        return v;
    }

    SlotVector defs(const SbStage& stage) {
        SbSlotCollector out;
        std::visit([&](const auto& n) { n.def(out); }, blueprint()[stage.idx()]);
        return sorted(out.slots());
    }

    SlotVector uses(const SbStage& stage) {
        SbSlotCollector out;
        std::visit([&](const auto& n) { n.use(out); }, blueprint()[stage.idx()]);
        return sorted(out.slots());
    }
};

TEST_F(SbBlueprintSlotUsageTest, VisitReferencedSlotsReportsSlotsButNotLocalVariables) {
    SbExprBuilder b{*_state};
    auto s1 = slot();
    auto s2 = slot();
    auto frameId = _state->frameId();

    // let [l = s1 + 1] in l * s2
    auto expr =
        b.makeLet(frameId,
                  SbExpr::makeSeq(b.makeBinaryOp(abt::Operations::Add, s1, b.makeInt32Constant(1))),
                  b.makeBinaryOp(abt::Operations::Mult, SbLocalVar{frameId, 0}, s2));

    SlotVector referenced;
    expr.visitReferencedSlots([&](SbSlot s) { referenced.push_back(s.getId()); });
    ASSERT_EQ(sorted(referenced), sbe::makeSV(s1.getId(), s2.getId()));

    // A bare slot expression reports the slot itself; a null expression reports nothing.
    referenced.clear();
    SbExpr{s1}.visitReferencedSlots([&](SbSlot s) { referenced.push_back(s.getId()); });
    ASSERT_EQ(referenced, sbe::makeSV(s1.getId()));
    SbExpr{}.visitReferencedSlots([&](SbSlot) { FAIL("null expression has no slots"); });
}

TEST_F(SbBlueprintSlotUsageTest, NodesReportTheirDefsAndUses) {
    SbBuilder b{*_state, 1};
    auto [scan, recordSlot, recordIdSlot, fieldSlots] =
        b.makeScan(UUID::gen(), DatabaseName::kAdmin, true, {"a", "b"});
    ASSERT_EQ(defs(scan),
              sorted(sbe::makeSV(recordSlot.getId(),
                                 recordIdSlot.getId(),
                                 fieldSlots[0].getId(),
                                 fieldSlots[1].getId())));
    ASSERT_TRUE(uses(scan).empty());

    auto [project, projectOut] = b.makeProject(
        std::move(scan), b.makeFunction(sbe::EFn::kGetField, recordSlot, b.makeStrConstant("c")));
    ASSERT_EQ(defs(project), sbe::makeSV(projectOut[0].getId()));
    ASSERT_EQ(uses(project), sbe::makeSV(recordSlot.getId()));

    // A sort re-exposes its forwarded slots under the same ids: they are uses, not defs.
    auto sort = b.makeSort(std::move(project),
                           SbExpr::makeSV(fieldSlots[0]),
                           {sbe::value::SortDirection::Ascending},
                           SbExpr::makeSV(recordSlot, projectOut[0]),
                           SbExpr{},
                           0);
    ASSERT_TRUE(defs(sort).empty());
    ASSERT_EQ(
        uses(sort),
        sorted(sbe::makeSV(recordSlot.getId(), fieldSlots[0].getId(), projectOut[0].getId())));

    auto [unwind, unwindOut, unwindIdx] = b.makeUnwind(std::move(sort), fieldSlots[1], false);
    ASSERT_EQ(defs(unwind), sorted(sbe::makeSV(unwindOut.getId(), unwindIdx.getId())));
    ASSERT_EQ(uses(unwind), sbe::makeSV(fieldSlots[1].getId()));

    // A multi-range scan over a defaulted SbIndexInfoSlots defines its outputs and uses nothing.
    auto [mrScan, mrRecordSlot, mrRecordIdSlot, mrFieldSlots] =
        b.makeScan(UUID::gen(), DatabaseName::kAdmin, true, {"c"}, RecordIdRangeList{});
    ASSERT_EQ(
        defs(mrScan),
        sorted(sbe::makeSV(mrRecordSlot.getId(), mrRecordIdSlot.getId(), mrFieldSlots[0].getId())));
    ASSERT_TRUE(uses(mrScan).empty());
}

TEST_F(SbBlueprintSlotUsageTest, CountsUsesAcrossTheTreeAndTheRootConsumer) {
    // filter [p > 0]
    //   sort [f0] forwarded [record, p]
    //     project [p = getField(record, "c")]
    //       scan [record, recordId, f0, f1]
    SbBuilder b{*_state, 1};
    auto [scan, recordSlot, recordIdSlot, fieldSlots] =
        b.makeScan(UUID::gen(), DatabaseName::kAdmin, true, {"a", "b"});
    auto scanIdx = scan.idx();
    auto [project, projectOut] = b.makeProject(
        std::move(scan), b.makeFunction(sbe::EFn::kGetField, recordSlot, b.makeStrConstant("c")));
    auto projectIdx = project.idx();
    auto sort = b.makeSort(std::move(project),
                           SbExpr::makeSV(fieldSlots[0]),
                           {sbe::value::SortDirection::Ascending},
                           SbExpr::makeSV(recordSlot, projectOut[0]),
                           SbExpr{},
                           0);
    auto root =
        b.makeFilter(std::move(sort),
                     b.makeBinaryOp(abt::Operations::Gt, projectOut[0], b.makeInt32Constant(0)));

    auto usage =
        SbBlueprintSlotUsage::build(blueprint(), root.idx(), sbe::makeSV(projectOut[0].getId()));

    ASSERT_EQ(*usage.getDefiningNode(recordSlot.getId()), scanIdx);
    ASSERT_EQ(*usage.getDefiningNode(fieldSlots[1].getId()), scanIdx);
    ASSERT_EQ(*usage.getDefiningNode(projectOut[0].getId()), projectIdx);
    ASSERT_FALSE(usage.getDefiningNode(12345));

    // 'record': the project's expression and the sort. 'p': the sort, the filter and the root
    // consumer. 'f0': the sort key. 'recordId' and 'f1' are dead.
    ASSERT_EQ(usage.numUses(recordSlot.getId()), 2);
    ASSERT_EQ(usage.numUses(projectOut[0].getId()), 3);
    ASSERT_EQ(usage.numUses(fieldSlots[0].getId()), 1);
    ASSERT_EQ(usage.numUses(recordIdSlot.getId()), 0);
    ASSERT_EQ(usage.numUses(fieldSlots[1].getId()), 0);
    ASSERT_EQ(usage.numUses(12345), 0);
}

TEST_F(SbBlueprintSlotUsageTest, ReportsUsesWithNoDefiningNode) {
    SbBuilder b{*_state, 1};
    auto outside = slot();
    auto [scan, recordSlot, recordIdSlot, fieldSlots] =
        b.makeScan(UUID::gen(), DatabaseName::kAdmin, true, {});
    auto root =
        b.makeFilter(std::move(scan), b.makeBinaryOp(abt::Operations::Eq, recordSlot, outside));

    auto usage = SbBlueprintSlotUsage::build(blueprint(), root.idx(), {});
    SlotVector undefined;
    usage.forEachUndefinedUse([&](SlotId s) { undefined.push_back(s); });
    ASSERT_EQ(undefined, sbe::makeSV(outside.getId()));
}

using SbBlueprintSlotUsageDeathTest = SbBlueprintSlotUsageTest;

DEATH_TEST_F(SbBlueprintSlotUsageDeathTest, SlotDefinedTwiceFails, "13627003") {
    SbBuilder b{*_state, 1};
    auto [scan, recordSlot, recordIdSlot, fieldSlots] =
        b.makeScan(UUID::gen(), DatabaseName::kAdmin, true, {});
    // A project that redefines the scan's record slot.
    SbExprOptSlotVector projects;
    projects.emplace_back(b.makeInt32Constant(1), recordSlot);
    auto [root, _] = b.makeProject(std::move(scan), std::move(projects));
    SbBlueprintSlotUsage::build(blueprint(), root.idx(), {});
}
}  // namespace
}  // namespace mongo::stage_builder
