/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/exec/sbe/expressions/expression.h"
#include "mongo/db/exec/sbe/values/slot.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/exec/trial_run_progress_tracker.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/stage_builder.h"

namespace mongo::stage_builder {
/**
 * Creates a new compilation environment and registers global values within the
 * new environment.
 */
std::unique_ptr<sbe::RuntimeEnvironment> makeRuntimeEnvironment(
    OperationContext* opCtx, sbe::value::SlotIdGenerator* slotIdGenerator);

/**
 * Some auxiliary data returned by a 'SlotBasedStageBuilder' along with a PlanStage tree root, which
 * is needed to execute the PlanStage tree.
 */
struct PlanStageData {
    PlanStageData(std::unique_ptr<sbe::RuntimeEnvironment> env)
        : env{env.get()}, ctx{std::move(env)} {}

    std::string debugString() const {
        StringBuilder builder;

        if (resultSlot) {
            builder << "$$RESULT=s" << *resultSlot << " ";
        }
        if (recordIdSlot) {
            builder << "$$RID=s" << *recordIdSlot << " ";
        }
        if (oplogTsSlot) {
            builder << "$$OPLOGTS=s" << *oplogTsSlot << " ";
        }

        env->debugString(&builder);

        return builder.str();
    }

    boost::optional<sbe::value::SlotId> resultSlot;
    boost::optional<sbe::value::SlotId> recordIdSlot;
    boost::optional<sbe::value::SlotId> oplogTsSlot;
    sbe::RuntimeEnvironment* env{nullptr};
    sbe::CompileCtx ctx;
    bool shouldTrackLatestOplogTimestamp{false};
    bool shouldTrackResumeToken{false};
    bool shouldUseTailableScan{false};
    // Used during the trial run of the runtime planner to track progress of the work done so far.
    std::unique_ptr<TrialRunProgressTracker> trialRunProgressTracker;
};

/**
 * A stage builder which builds an executable tree using slot-based PlanStages.
 */
class SlotBasedStageBuilder final : public StageBuilder<sbe::PlanStage> {
public:
    SlotBasedStageBuilder(OperationContext* opCtx,
                          const CollectionPtr& collection,
                          const CanonicalQuery& cq,
                          const QuerySolution& solution,
                          PlanYieldPolicySBE* yieldPolicy,
                          bool needsTrialRunProgressTracker)
        : StageBuilder(opCtx, collection, cq, solution), _yieldPolicy(yieldPolicy) {
        if (needsTrialRunProgressTracker) {
            const auto maxNumResults{trial_period::getTrialPeriodNumToReturn(_cq)};
            const auto maxNumReads{trial_period::getTrialPeriodMaxWorks(_opCtx, _collection)};
            _data.trialRunProgressTracker =
                std::make_unique<TrialRunProgressTracker>(maxNumResults, maxNumReads);
        }
    }

    std::unique_ptr<sbe::PlanStage> build(const QuerySolutionNode* root) final;

    PlanStageData getPlanStageData() {
        return std::move(_data);
    }

private:
    std::unique_ptr<sbe::PlanStage> buildCollScan(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildIndexScan(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildFetch(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildLimit(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildSkip(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildSort(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildSortKeyGeneraror(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildProjectionSimple(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildProjectionCovered(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildProjectionDefault(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildOr(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildText(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildReturnKey(const QuerySolutionNode* root);
    std::unique_ptr<sbe::PlanStage> buildEof(const QuerySolutionNode* root);

    std::unique_ptr<sbe::PlanStage> makeLoopJoinForFetch(
        std::unique_ptr<sbe::PlanStage> inputStage,
        sbe::value::SlotId recordIdKeySlot,
        PlanNodeId planNodeId,
        const sbe::value::SlotVector& slotsToForward = {});

    std::unique_ptr<sbe::PlanStage> makeUnionForTailableCollScan(const QuerySolutionNode* root);

    sbe::value::SlotIdGenerator _slotIdGenerator;
    sbe::value::FrameIdGenerator _frameIdGenerator;
    sbe::value::SpoolIdGenerator _spoolIdGenerator;

    // If we have both limit and skip stages and the skip stage is beneath the limit, then we can
    // combine these two stages into one. So, while processing the LIMIT stage we will save the
    // limit value in this member and will handle it while processing the SKIP stage.
    boost::optional<long long> _limit;

    // A slot here indicates that the plan has a ReturnKeyStage at its root and that any index scans
    // should inflate each index entry into an object and bind it to this slot.
    boost::optional<sbe::value::SlotId> _returnKeySlot;

    // A bitset here indicates that we have a covered projection that is expecting to read values
    // from an index scan.
    boost::optional<sbe::IndexKeysInclusionSet> _indexKeysToInclude;

    // When an index scan produces values for a covered projection, this is where the slots for the
    // produced values are stored.
    boost::optional<sbe::value::SlotVector> _indexKeySlots;

    // These two flags control whether we're in the middle of the process of building a special
    // union sub-tree implementing a tailable cursor collection scan, and if so, whether we're
    // building an anchor or resume branch.
    bool _isBuildingUnionForTailableCollScan{false};
    bool _isTailableCollScanResumeBranch{false};

    PlanYieldPolicySBE* const _yieldPolicy;

    // Apart from generating just an execution tree, this builder will also produce some auxiliary
    // data which is needed to execute the tree, such as a result slot, or a recordId slot.
    PlanStageData _data{makeRuntimeEnvironment(_opCtx, &_slotIdGenerator)};
};
}  // namespace mongo::stage_builder
