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

#include "mongo/db/catalog/collection.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

class ClockSource;

class PlanYieldPolicy {
public:
    virtual ~PlanYieldPolicy() {}

    PlanYieldPolicy(PlanExecutor* exec, PlanExecutor::YieldPolicy policy);

    /**
     * Only used in dbtests since we don't have access to a PlanExecutor. Since we don't have
     * access to the PlanExecutor to grab a ClockSource from, we pass in a ClockSource directly
     * in the constructor instead.
     */
    PlanYieldPolicy(PlanExecutor::YieldPolicy policy, ClockSource* cs);

    /**
     * Periodically returns true to indicate that it is time to check for interrupt (in the case of
     * YIELD_AUTO and INTERRUPT_ONLY) or release locks or storage engine state (in the case of
     * auto-yielding plans).
     */
    virtual bool shouldYieldOrInterrupt();

    /**
     * Resets the yield timer so that we wait for a while before yielding/interrupting again.
     */
    void resetTimer();

    /**
     * Used to cause a plan executor to check for interrupt (in the case of YIELD_AUTO and
     * INTERRUPT_ONLY) or release locks or storage engine state (in the case of auto-yielding
     * plans). The PlanExecutor must *not* be in saved state. Handles calls to save/restore state
     * internally.
     *
     * Returns Status::OK() if the executor was restored successfully and is still alive. Returns
     * ErrorCodes::QueryPlanKilled if the executor got killed during yield, and
     * ErrorCodes::ExceededTimeLimit if the operation has exceeded the time limit.
     *
     * Calls 'whileYieldingFn' after relinquishing locks and before reacquiring the locks that have
     * been relinquished.
     */
    virtual Status yieldOrInterrupt(stdx::function<void()> whileYieldingFn = nullptr);

    /**
     * All calls to shouldYieldOrInterrupt() will return true until the next call to
     * yieldOrInterrupt(). This must only be called for auto-yielding plans, to force a yield. It
     * cannot be used to force an interrupt for INTERRUPT_ONLY plans.
     */
    void forceYield() {
        dassert(canAutoYield());
        _forceYield = true;
    }

    /**
     * Returns true if there is a possibility that a collection lock will be yielded at some point
     * during this PlanExecutor's lifetime.
     */
    bool canReleaseLocksDuringExecution() const {
        switch (_policy) {
            case PlanExecutor::YIELD_AUTO:
            case PlanExecutor::YIELD_MANUAL:
            case PlanExecutor::ALWAYS_TIME_OUT:
            case PlanExecutor::ALWAYS_MARK_KILLED: {
                return true;
            }
            case PlanExecutor::NO_YIELD:
            case PlanExecutor::WRITE_CONFLICT_RETRY_ONLY:
            case PlanExecutor::INTERRUPT_ONLY: {
                return false;
            }
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Returns true if this yield policy performs automatic yielding. Note 'yielding' here refers to
     * either releasing storage engine resources via abandonSnapshot() OR yielding LockManager
     * locks.
     */
    bool canAutoYield() const {
        switch (_policy) {
            case PlanExecutor::YIELD_AUTO:
            case PlanExecutor::WRITE_CONFLICT_RETRY_ONLY:
            case PlanExecutor::ALWAYS_TIME_OUT:
            case PlanExecutor::ALWAYS_MARK_KILLED: {
                return true;
            }
            case PlanExecutor::NO_YIELD:
            case PlanExecutor::YIELD_MANUAL:
            case PlanExecutor::INTERRUPT_ONLY:
                return false;
        }
        MONGO_UNREACHABLE;
    }

    PlanExecutor::YieldPolicy getPolicy() const {
        return _policy;
    }

private:
    const PlanExecutor::YieldPolicy _policy;

    bool _forceYield;
    ElapsedTracker _elapsedTracker;

    // The plan executor which this yield policy is responsible for yielding. Must
    // not outlive the plan executor.
    PlanExecutor* const _planYielding;

    // Returns true to indicate it's time to release locks or storage engine state.
    bool shouldYield();

    // Releases locks or storage engine state.
    Status yield(stdx::function<void()> whileYieldingFn);
};

}  // namespace mongo
