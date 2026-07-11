// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

/**
 * TrialStage runs a specified 'trial' plan for a given number of iterations, tracking the number of
 * times the plan advanced over that duration. If the ratio of advances to works falls below a given
 * threshold, then the trial results are discarded and the backup plan is adopted. Otherwise, we
 * consider the trial a success, and return the cached results of the trial phase followed by any
 * further results that the plan produces.
 *
 * If the trial plan hits EOF before the trial phase is complete, then it is considered a successful
 * trial and the cached results are returned. If the trial plan dies during the trial phase, we
 * consider it to have failed and adopt the backup plan instead.
 *
 * This stage differs from other stages that choose between plans in that it does not evaluate all
 * candidates; it evaluates one specific trial plan, and adopts the backup plan if it fails.
 * Currently, results from the trial period are discarded on failure. Future use cases may wish to
 * extend this class to have multiple policies on what to do in this scenario.
 */
class TrialStage final : public PlanStage {
public:
    static constexpr std::string_view kStageType = "TRIAL"sv;

    /**
     * Constructor. Both 'trialPlan' and 'backupPlan' must be non-nullptr; 'maxTrialEWorks' must be
     * greater than 0, and 'minWorkAdvancedRatio' must be in the range (0,1].
     */
    TrialStage(ExpressionContext* expCtx,
               WorkingSet* ws,
               std::unique_ptr<PlanStage> trialPlan,
               std::unique_ptr<PlanStage> backupPlan,
               size_t maxTrialWorks,
               double minWorkAdvancedRatio);

    StageState doWork(WorkingSetID* out) final;

    // Works the trial plan until the evaluation period is complete.
    Status pickBestPlan(PlanYieldPolicy* yieldPolicy);

    bool isTrialPhaseComplete() const {
        return _specificStats.trialCompleted;
    }

    bool pickedBackupPlan() const {
        return (_specificStats.trialCompleted && !_specificStats.trialSucceeded);
    }

    StageType stageType() const final {
        return STAGE_TRIAL;
    }

    bool isEOF() const final {
        return (_specificStats.trialCompleted ? child()->isEOF() : false);
    }

    const SpecificStats* getSpecificStats() const final {
        return &_specificStats;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

protected:
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;

private:
    void _replaceCurrentPlan(std::unique_ptr<PlanStage>& newPlan);

    StageState _workTrialPlan(WorkingSetID* out);
    void _assessTrialAndBuildFinalPlan();

    std::unique_ptr<PlanStage> _backupPlan;
    std::unique_ptr<PlanStage> _queuedData;

    TrialStats _specificStats;

    WorkingSet* _ws;
};

}  // namespace mongo
