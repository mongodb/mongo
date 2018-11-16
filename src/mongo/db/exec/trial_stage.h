/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/query/plan_yield_policy.h"

namespace mongo {

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
    static const char* kStageType;

    /**
     * Constructor. Both 'trialPlan' and 'backupPlan' must be non-nullptr; 'maxTrialEWorks' must be
     * greater than 0, and 'minWorkAdvancedRatio' must be in the range (0,1].
     */
    TrialStage(OperationContext* opCtx,
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

    bool isEOF() final {
        return (_specificStats.trialCompleted ? child()->isEOF() : false);
    }

    const SpecificStats* getSpecificStats() const final {
        return &_specificStats;
    }

    std::unique_ptr<PlanStageStats> getStats() final;

protected:
    void doDetachFromOperationContext() final;
    void doReattachToOperationContext() final;
    void doDispose() final;

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
