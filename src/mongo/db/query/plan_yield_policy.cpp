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

#include "mongo/db/query/plan_yield_policy.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

namespace mongo {

PlanYieldPolicy::PlanYieldPolicy(YieldPolicy policy,
                                 ClockSource* cs,
                                 int yieldIterations,
                                 Milliseconds yieldPeriod)
    : _policy(policy), _elapsedTracker(cs, yieldIterations, yieldPeriod) {}

bool PlanYieldPolicy::shouldYieldOrInterrupt(OperationContext* opCtx) {
    if (_policy == YieldPolicy::INTERRUPT_ONLY) {
        return _elapsedTracker.intervalHasElapsed();
    }
    if (!canAutoYield())
        return false;
    invariant(!opCtx->lockState()->inAWriteUnitOfWork());
    if (_forceYield)
        return true;
    return _elapsedTracker.intervalHasElapsed();
}

void PlanYieldPolicy::resetTimer() {
    _elapsedTracker.resetLastTime();
}

Status PlanYieldPolicy::yieldOrInterrupt(OperationContext* opCtx,
                                         std::function<void()> whileYieldingFn) {
    invariant(opCtx);

    if (_policy == YieldPolicy::INTERRUPT_ONLY) {
        ON_BLOCK_EXIT([this]() { resetTimer(); });
        invariant(opCtx);
        preCheckInterruptOnly(opCtx);
        return opCtx->checkForInterruptNoAssert();
    }


    invariant(!opCtx->lockState()->inAWriteUnitOfWork());

    // After we finish yielding (or in any early return), call resetTimer() to prevent yielding
    // again right away. We delay the resetTimer() call so that the clock doesn't start ticking
    // until after we return from the yield.
    ON_BLOCK_EXIT([this]() { resetTimer(); });
    _forceYield = false;

    return yield(opCtx, whileYieldingFn);
}

}  // namespace mongo
