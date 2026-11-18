// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_dead_slot_elimination.h"

#include "mongo/db/exec/sbe/util/debug_print.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_blueprint_slot_usage.h"
#include "mongo/db/query/stage_builder/sbe/sbe_stage_lowering.h"
#include "mongo/db/query/stage_builder/sbe/sbexpr_helpers.h"
#include "mongo/db/query/stage_builder/sbe/tests/sbe_builder_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <functional>
#include <string>

namespace mongo::stage_builder {
namespace {
class SbBlueprintDeadSlotEliminationTest : public GoldenSbeExprBuilderTestFixture {
protected:
    struct Plan {
        SbStage root;
        // The slots that the root's consumer reads.
        sbe::value::SlotVector liveSlots;
    };

    SbBlueprint& blueprint() {
        return _state->blueprint();
    }

    SbHashAggAccumulator sum(SbBuilder& b, SbExpr input) {
        auto spillSlot = SbSlot{_state->slotId()};
        return {.spillSlot = spillSlot,
                .implementation = SbHashAggCompiledAccumulator{
                    {},
                    b.makeFunction(sbe::EFn::kAggDoubleDoubleSum, std::move(input)),
                    b.makeFunction(sbe::EFn::kAggMergeDoubleDoubleSums, spillSlot)}};
    }

    // Runs dead slot elimination over the tree rooted at 'root', treating 'liveSlots' as read by
    // the root's consumer, until it converges or 'maxFirings' rules have fired. Checks that the
    // rule left the slot summary up to date.
    SbBlueprintRewriteResult eliminateDeadSlots(SbStage root,
                                                const sbe::value::SlotVector& liveSlots,
                                                size_t maxFirings = 10000) {
        auto usage = SbBlueprintSlotUsage::build(blueprint(), root.idx(), liveSlots);
        SbBlueprintRewriteRules rules;
        rules.push_back(makeDeadSlotEliminationRule());
        auto result = runSbeBlueprintRewrites(
            _state->opCtx, &blueprint(), &usage, root.release(), rules, maxFirings);
        ASSERT_TRUE(usage == SbBlueprintSlotUsage::build(blueprint(), result.root, liveSlots));
        return result;
    }

    // Builds the plan with 'build' with slot and frame numbering restarted, so that every build
    // numbers its slots the same way.
    Plan buildPlan(const std::function<Plan(SbBuilder&)>& build) {
        _slotIdGenerator = sbe::value::SlotIdGenerator{};
        _frameIdGenerator = sbe::value::FrameIdGenerator{};
        SbBuilder b{*_state, 1};
        return build(b);
    }

    std::string print(SbBlueprintNodeIdx root) {
        auto stage = lowerSbeBlueprint(blueprint(), root, *_state);
        sbe::DebugPrintInfo debugPrintInfo{};
        return replaceUuid(sbe::DebugPrinter().print(*stage, debugPrintInfo), _uuid);
    }

    // Prints the plan that 'build' makes, then the plan after each firing of dead slot
    // elimination, and checks the output against the golden file. The plan after 'i' firings
    // comes from a fresh plan rewritten with a budget of 'i' firings.
    void runGoldenTest(const std::function<Plan(SbBuilder&)>& build) {
        _gctx->printTestHeader(unittest::GoldenTestContext::HeaderFormat::Text);
        auto& out = _gctx->outStream();

        auto before = buildPlan(build);
        out << "Live slots:";
        for (auto slot : before.liveSlots) {
            out << " s" << slot;
        }
        out << std::endl
            << std::endl
            << "Before:" << std::endl
            << print(before.root.release()) << std::endl;

        auto plan = buildPlan(build);
        auto full = eliminateDeadSlots(std::move(plan.root), plan.liveSlots);
        ASSERT_TRUE(full.converged);
        const auto finalPlan = print(full.root);
        for (size_t i = 1; i < full.firings; ++i) {
            auto partialPlan = buildPlan(build);
            auto partial =
                eliminateDeadSlots(std::move(partialPlan.root), partialPlan.liveSlots, i);
            out << std::endl
                << "After firing " << i << ":" << std::endl
                << print(partial.root) << std::endl;
        }
        out << std::endl
            << "After firing " << full.firings << ":" << std::endl
            << finalPlan << std::endl;

        _gctx->verifyOutput();
    }

    const UUID _uuid = UUID::gen();
};

TEST_F(SbBlueprintDeadSlotEliminationTest, ProjectWithOnlyDeadOutputsIsReplacedByChild) {
    // Nothing reads the project's output. The plan's consumer reads only the record slot.
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fieldSlots] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {"a"});
        auto [project, outSlots] = b.makeProject(
            std::move(scan),
            b.makeBinaryOp(abt::Operations::Add, fieldSlots[0], b.makeInt32Constant(1)));
        return Plan{std::move(project), {recordSlot.getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, DeadEntryBetweenLiveOnesIsRemoved) {
    // The live entries on either side of the dead one keep their order.
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fieldSlots] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {"a"});
        auto [project, outSlots] = b.makeProject(
            std::move(scan),
            b.makeBinaryOp(abt::Operations::Add, fieldSlots[0], b.makeInt32Constant(1)),
            b.makeBinaryOp(abt::Operations::Add, fieldSlots[0], b.makeInt32Constant(2)),
            b.makeBinaryOp(abt::Operations::Add, fieldSlots[0], b.makeInt32Constant(3)));
        return Plan{std::move(project), {outSlots[0].getId(), outSlots[2].getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, RemovingAnEntryQueuesTheDefinerTwoStagesBelow) {
    // The upper project reads both outputs of the lower one, but only its first output is live.
    // Removing its second entry leaves the lower project's second output unread. The lower project,
    // two stages below, was rewritten before the upper one, so it is rewritten again only because
    // the rule queues it. The filter between them is left alone.
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fieldSlots] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {"a"});
        auto [lower, lowerSlots] = b.makeProject(
            std::move(scan),
            b.makeBinaryOp(abt::Operations::Add, fieldSlots[0], b.makeInt32Constant(1)),
            b.makeBinaryOp(abt::Operations::Add, fieldSlots[0], b.makeInt32Constant(2)));
        auto filter = b.makeFilter(
            std::move(lower),
            b.makeBinaryOp(abt::Operations::Gt, lowerSlots[0], b.makeInt32Constant(0)));
        auto [upper, upperSlots] = b.makeProject(
            std::move(filter),
            b.makeBinaryOp(abt::Operations::Add, lowerSlots[0], b.makeInt32Constant(1)),
            b.makeBinaryOp(abt::Operations::Add, lowerSlots[1], b.makeInt32Constant(1)));
        return Plan{std::move(upper), {recordSlot.getId(), upperSlots[0].getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, SequentialProjectEntryReadingAnEarlierOne) {
    // In a project*, each entry may read the entries before it. Only the first entry is live.
    // Removing the third leaves the second unread, and since the rule visits the entries last to
    // first, the second goes in the same firing.
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fieldSlots] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {"a"});
        SbSlot first{_state->slotId()};
        SbSlot second{_state->slotId()};
        SbSlot third{_state->slotId()};
        SbExprSlotVector projects;
        projects.emplace_back(
            b.makeBinaryOp(abt::Operations::Add, fieldSlots[0], b.makeInt32Constant(1)), first);
        projects.emplace_back(b.makeBinaryOp(abt::Operations::Add, first, b.makeInt32Constant(1)),
                              second);
        projects.emplace_back(b.makeBinaryOp(abt::Operations::Add, second, b.makeInt32Constant(1)),
                              third);
        auto project =
            blueprint().add(SbBlueprintProject{.child = scan.release(),
                                               .nodeId = 1,
                                               .projects = std::move(projects),
                                               .binding = sbe::ProjectStage::Binding::kSequential});
        return Plan{SbStage{project}, {recordSlot.getId(), first.getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, AddFieldsFieldThatFailsButIsProjectedOutIsRemoved) {
    // The plan the stage builder makes for $addFields {c: {$divide: ['$a', 0]}} followed by
    // $project {b: 1, _id: 0}. Nothing reads 'c', so its entry goes, and its error with it: queries
    // do not promise to raise errors for values that nothing reads. That leaves getField(record,
    // "a") unread, so the rule queues the project that defines it and removes that entry too.
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fieldSlots] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {});
        auto [fields, fieldsOut] =
            b.makeProject(std::move(scan),
                          b.makeFunction(sbe::EFn::kGetField, recordSlot, b.makeStrConstant("a")),
                          b.makeFunction(sbe::EFn::kGetField, recordSlot, b.makeStrConstant("b")));
        auto a = fieldsOut[0];
        auto [addFields, addFieldsOut] = b.makeProject(
            std::move(fields),
            b.makeIf(b.makeFunction(sbe::EFn::kIsNullish, a),
                     b.makeNullConstant(),
                     b.makeIf(b.makeFunction(sbe::EFn::kIsNumber, a),
                              b.makeBinaryOp(abt::Operations::Div, a, b.makeInt32Constant(0)),
                              b.makeFail(ErrorCodes::TypeMismatch,
                                         "$divide only supports numeric types"))));
        auto [project, projectOut] = b.makeProject(
            std::move(addFields),
            b.makeFunction(sbe::EFn::kNewBsonObj, b.makeStrConstant("b"), fieldsOut[1]));
        return Plan{std::move(project), {projectOut[0].getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, UnreadScanFieldsAreRemoved) {
    // Only the record and 'b' are read. 'a' and 'c' go, and 'b' keeps its slot.
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fieldSlots] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {"a", "b", "c"});
        return Plan{std::move(scan), {recordSlot.getId(), fieldSlots[1].getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, ScanWithNoReadFieldsKeepsNone) {
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fieldSlots] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {"a", "b"});
        return Plan{std::move(scan), {recordSlot.getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, BoundedScanUnreadFieldsAreRemoved) {
    // A scan with record id bounds lowers to a ScanStage rather than a GenericScanStage.
    runGoldenTest([&](SbBuilder& b) {
        auto [boundsProject, boundsSlots] = b.makeProject(b.makeCoScan(1), b.makeInt64Constant(0));
        auto [scan, recordSlot, recordIdSlot, fieldSlots] =
            b.makeScan(_uuid,
                       DatabaseName::kAdmin,
                       true,
                       {"a", "b"},
                       SbScanBounds{.minRecordIdSlot = boundsSlots[0]});
        auto loopJoin = b.makeLoopJoin(std::move(boundsProject),
                                       std::move(scan),
                                       /*outerProjects=*/{},
                                       /*outerCorrelated=*/{boundsSlots[0]});
        return Plan{std::move(loopJoin), {recordSlot.getId(), fieldSlots[0].getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, RemovingAnEntryQueuesTheScanThatDefinesItsInput) {
    // Only the project's first entry is live. Removing the second leaves the scan's 'b' unread, so
    // the rule queues the scan and removes 'b' from it.
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fieldSlots] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {"a", "b"});
        auto [project, outSlots] = b.makeProject(
            std::move(scan),
            b.makeBinaryOp(abt::Operations::Add, fieldSlots[0], b.makeInt32Constant(1)),
            b.makeBinaryOp(abt::Operations::Add, fieldSlots[1], b.makeInt32Constant(1)));
        return Plan{std::move(project), {recordSlot.getId(), outSlots[0].getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, GroupWithOnlyDeadAccumulatorsKeepsGrouping) {
    // Even when none of its outputs are read, a group must still produce one row per distinct
    // key. A global group must likewise remain an aggregation, rather than forwarding input rows.
    for (bool global : {false, true}) {
        SbBuilder b{*_state, 1};
        auto [input, slots] = b.makeProject(b.makeCoScan(3), b.makeInt32Constant(1));
        SbHashAggAccumulatorVector accumulators;
        accumulators.push_back(
            {.spillSlot = SbSlot{_state->slotId()},
             .implementation =
                 SbHashAggSinglePurposeScalarAccumulator<sbe::PushHashAggAccumulator>{slots[0]}});
        auto [group, keys, outputs] = b.makeHashAgg(VariableTypes{},
                                                    std::move(input),
                                                    global ? SbSlotVector{} : slots,
                                                    accumulators,
                                                    boost::none,
                                                    false /* compact */);
        auto result = eliminateDeadSlots(std::move(group), {});
        ASSERT_TRUE(result.converged);
        const auto& rewritten = std::get<SbBlueprintHashAgg>(blueprint()[result.root]);
        ASSERT_TRUE(rewritten.accumulators.empty());
        ASSERT_EQ(rewritten.groupBySlots.size(), global ? 0 : 1);
        ASSERT_EQ(rewritten.groupByOutSlots.size(), global ? 0 : 1);
    }
}

TEST_F(SbBlueprintDeadSlotEliminationTest, DeadAccumulatorBetweenLiveOnesIsRemoved) {
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fields] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {"key", "a", "b"});
        SbHashAggAccumulatorVector accumulators;
        accumulators.push_back(sum(b, fields[1]));
        accumulators.push_back(sum(b, fields[2]));
        accumulators.push_back(sum(b, fields[1]));
        auto [group, keys, outputs] = b.makeHashAgg(
            VariableTypes{}, std::move(scan), {fields[0]}, accumulators, boost::none, false);
        return Plan{std::move(group), {outputs[0].getId(), outputs[2].getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, RemovingProjectEntryQueuesGroupAndItsInput) {
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fields] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {"key", "a", "b"});
        auto [input, inputs] =
            b.makeProject(std::move(scan),
                          b.makeBinaryOp(abt::Operations::Add, fields[2], b.makeInt32Constant(1)));
        SbHashAggAccumulatorVector accumulators;
        accumulators.push_back(sum(b, fields[1]));
        accumulators.push_back(sum(b, inputs[0]));
        auto [group, keys, outputs] = b.makeHashAgg(
            VariableTypes{}, std::move(input), {fields[0]}, accumulators, boost::none, false);
        auto [project, finalSlots] =
            b.makeProject(std::move(group),
                          b.makeBinaryOp(abt::Operations::Add, outputs[0], b.makeInt32Constant(1)),
                          b.makeBinaryOp(abt::Operations::Add, outputs[1], b.makeInt32Constant(1)));
        return Plan{std::move(project), {finalSlots[0].getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, RemovingAccumulatorsRemovesAllTheirExpressionUses) {
    SbBuilder b{*_state, 1};
    auto [scan, recordSlot, recordIdSlot, fields] =
        b.makeScan(_uuid,
                   DatabaseName::kAdmin,
                   true,
                   {"key", "init", "agg", "merge", "transform", "size", "live"});
    auto spill = SbSlot{_state->slotId()};
    SbHashAggAccumulatorVector accumulators;
    accumulators.push_back(
        {.spillSlot = spill,
         .implementation = SbHashAggCompiledAccumulator{
             fields[1],
             b.makeFunction(sbe::EFn::kAggDoubleDoubleSum,
                            b.makeBinaryOp(abt::Operations::Add, fields[2], fields[2])),
             b.makeBinaryOp(abt::Operations::Add, spill, fields[3])}});
    accumulators.push_back(
        {.spillSlot = SbSlot{_state->slotId()},
         .implementation =
             SbHashAggSinglePurposeScalarAccumulator<sbe::PushHashAggAccumulator>{fields[4]}});
    accumulators.push_back(
        {.spillSlot = SbSlot{_state->slotId()},
         .implementation =
             SbHashAggMinMaxNAccumulator<sbe::MinNHashAggAccumulator>{fields[4], fields[5], 1024}});
    accumulators.push_back(sum(b, fields[6]));
    auto [group, keys, outputs] = b.makeHashAgg(
        VariableTypes{}, std::move(scan), {fields[0]}, accumulators, boost::none, false);
    auto result = eliminateDeadSlots(std::move(group), {outputs[3].getId()});
    ASSERT_TRUE(result.converged);
    const auto& rewritten = std::get<SbBlueprintHashAgg>(blueprint()[result.root]);
    ASSERT_EQ(rewritten.accumulators.size(), 1);
    ASSERT_EQ(rewritten.accumulators[0].outSlot, outputs[3].getId());
    const auto& rewrittenScan = std::get<SbBlueprintScan>(blueprint()[rewritten.child]);
    ASSERT_EQ(rewrittenScan.info->scanFieldNames, (std::vector<std::string>{"key", "live"}));

    // Nothing changes when all the remaining accumulators are live.
    auto again = eliminateDeadSlots(SbStage{result.root}, {outputs[3].getId()});
    ASSERT_TRUE(again.converged);
    ASSERT_EQ(again.firings, 0);
}

TEST_F(SbBlueprintDeadSlotEliminationTest, CompactGroupRemovesDeadAccumulators) {
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fields] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {"key", "a", "b"});
        SbHashAggAccumulatorVector accumulators;
        for (size_t i : {1, 2}) {
            accumulators.push_back(
                {.spillSlot = SbSlot{_state->slotId()},
                 .implementation =
                     SbHashAggSinglePurposeScalarAccumulator<sbe::CompactPushHashAggAccumulator>{
                         fields[i]}});
        }
        auto [group, keys, outputs] = b.makeHashAgg(
            VariableTypes{}, std::move(scan), {fields[0]}, accumulators, boost::none, true);
        return Plan{std::move(group), {outputs[0].getId()}};
    });
}

TEST_F(SbBlueprintDeadSlotEliminationTest, StreamingGroupKeepsUnreadKeysAndClusterKeys) {
    runGoldenTest([&](SbBuilder& b) {
        auto [scan, recordSlot, recordIdSlot, fields] =
            b.makeScan(_uuid, DatabaseName::kAdmin, true, {"key", "cluster", "a"});
        SbHashAggAccumulatorVector accumulators;
        accumulators.push_back(sum(b, fields[2]));
        auto [group, keys, outputs] = b.makeStreamingAgg(
            VariableTypes{}, std::move(scan), {fields[0]}, {fields[1]}, accumulators, boost::none);
        return Plan{std::move(group), {}};
    });
}
}  // namespace
}  // namespace mongo::stage_builder
