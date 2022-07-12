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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/trial_stage.h"

#include <algorithm>
#include <memory>

#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/exec/or.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/exec/working_set_common.h"

namespace mongo {

const char* TrialStage::kStageType = "TRIAL";

TrialStage::TrialStage(ExpressionContext* expCtx,
                       WorkingSet* ws,
                       std::unique_ptr<PlanStage> trialPlan,
                       std::unique_ptr<PlanStage> backupPlan,
                       size_t maxTrialWorks,
                       double minWorkAdvancedRatio)
    : PlanStage(kStageType, expCtx), _ws(ws) {
    invariant(minWorkAdvancedRatio > 0);
    invariant(minWorkAdvancedRatio <= 1);
    invariant(maxTrialWorks > 0);
    invariant(trialPlan);
    invariant(backupPlan);

    // Set the trial plan as our only child, and keep the backup plan in reserve.
    _children.push_back(std::move(trialPlan));
    _backupPlan = std::move(backupPlan);

    // We need to cache results during the trial phase in case it succeeds.
    _queuedData = std::make_unique<QueuedDataStage>(expCtx, _ws);

    // Set up stats tracking specific to this stage.
    _specificStats.successThreshold = minWorkAdvancedRatio;
    _specificStats.trialPeriodMaxWorks = maxTrialWorks;
}

Status TrialStage::pickBestPlan(PlanYieldPolicy* yieldPolicy) {
    // Work the trial plan until the evaluation is complete.
    while (!_specificStats.trialCompleted) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        const bool mustYield = (work(&id) == PlanStage::NEED_YIELD);
        if (mustYield) {
            // Run-time plan selection occurs before a WriteUnitOfWork is opened and it's not
            // subject to TemporarilyUnavailableException's.
            invariant(!expCtx()->getTemporarilyUnavailableException());
        }
        if (mustYield || yieldPolicy->shouldYieldOrInterrupt(expCtx()->opCtx)) {
            if (mustYield && !yieldPolicy->canAutoYield()) {
                throwWriteConflictException();
            }
            auto yieldStatus = yieldPolicy->yieldOrInterrupt(expCtx()->opCtx);
            if (!yieldStatus.isOK()) {
                return yieldStatus;
            }
        }
    }
    return Status::OK();
}

PlanStage::StageState TrialStage::doWork(WorkingSetID* out) {
    // Whether or not we have completed the trial phase, we should have exactly one active child.
    invariant(_children.size() == 1);
    invariant(_children.front());

    // If the trial period isn't complete, run through a single step of the trial.
    if (!_specificStats.trialCompleted) {
        return _workTrialPlan(out);
    }

    // If we reach this point, the final plan has been set as our only child.
    return child()->work(out);
}

PlanStage::StageState TrialStage::_workTrialPlan(WorkingSetID* out) {
    // We should never call this method after the trial phase has completed.
    invariant(!_specificStats.trialCompleted);

    PlanStage::StageState state = child()->work(out);

    switch (state) {
        case PlanStage::ADVANCED: {
            // We need to cache results until the trial is complete. Ensure the BSONObj underlying
            // the WorkingSetMember is owned, and set the return state to NEED_TIME so that we do
            // not also try to return it.
            WorkingSetMember* member = _ws->get(*out);
            member->makeObjOwnedIfNeeded();
            static_cast<QueuedDataStage*>(_queuedData.get())->pushBack(*out);
            *out = WorkingSet::INVALID_ID;
            state = NEED_TIME;
            // Increment the 'advanced' count and fall through into NEED_TIME so that we check for
            // the end of the trial period and assess the results for both NEED_TIME and ADVANCED.
            ++_specificStats.trialAdvanced;
            [[fallthrough]];
        }
        case PlanStage::NEED_TIME:
            // Check whether we have completed the evaluation phase.
            _specificStats.trialCompleted =
                (++_specificStats.trialWorks == _specificStats.trialPeriodMaxWorks);
            // If we've reached the end of the trial phase, attempt to build the final plan.
            if (_specificStats.trialCompleted) {
                _assessTrialAndBuildFinalPlan();
            }
            return state;
        case PlanStage::NEED_YIELD:
            // Run-time plan selection occurs before a WriteUnitOfWork is opened and it's not
            // subject to TemporarilyUnavailableException's.
            invariant(!expCtx()->getTemporarilyUnavailableException());
            // Nothing to update here.
            return state;
        case PlanStage::IS_EOF:
            // EOF always marks the successful end of the trial phase. Swap in the queued data as
            // the active plan and return NEED_TIME so that the caller will consume the cache.
            _specificStats.trialCompleted = _specificStats.trialSucceeded = true;
            _replaceCurrentPlan(_queuedData);
            return NEED_TIME;
    }

    MONGO_UNREACHABLE;
}

void TrialStage::_assessTrialAndBuildFinalPlan() {
    // We should only ever reach here when the trial period ran to completion.
    invariant(_specificStats.trialWorks == _specificStats.trialPeriodMaxWorks);
    invariant(_specificStats.trialCompleted);

    // If we ADVANCED a sufficient number of times over the trial, then the trial succeeded.
    _specificStats.trialSucceeded = _specificStats.trialAdvanced >=
        (_specificStats.trialPeriodMaxWorks * _specificStats.successThreshold);

    // If the trial failed, all we need do is adopt the backup plan.
    if (!_specificStats.trialSucceeded) {
        _replaceCurrentPlan(_backupPlan);
        return;
    }

    // The trial plan succeeded, but we need to build a plan that includes the queued data. Create a
    // final plan which UNIONs across the QueuedDataStage and the trial plan.
    std::unique_ptr<PlanStage> unionPlan = std::make_unique<OrStage>(expCtx(), _ws, false, nullptr);
    static_cast<OrStage*>(unionPlan.get())->addChild(std::move(_queuedData));
    static_cast<OrStage*>(unionPlan.get())->addChild(std::move(_children.front()));
    _replaceCurrentPlan(unionPlan);
}

void TrialStage::_replaceCurrentPlan(std::unique_ptr<PlanStage>& newPlan) {
    invariant(_children.size() == 1);
    _children.front().swap(newPlan);
}

std::unique_ptr<PlanStageStats> TrialStage::getStats() {
    _commonStats.isEOF = isEOF();

    auto ret = std::make_unique<PlanStageStats>(_commonStats, STAGE_TRIAL);
    ret->specific = std::make_unique<TrialStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());

    return ret;
}

void TrialStage::doDetachFromOperationContext() {
    if (_backupPlan) {
        _backupPlan->detachFromOperationContext();
    }
    if (_queuedData) {
        _queuedData->detachFromOperationContext();
    }
}

void TrialStage::doReattachToOperationContext() {
    if (_backupPlan) {
        _backupPlan->reattachToOperationContext(opCtx());
    }
    if (_queuedData) {
        _queuedData->reattachToOperationContext(opCtx());
    }
}

}  // namespace mongo
