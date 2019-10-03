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

#include "mongo/db/curop.h"
#include "mongo/db/prepare_conflict_tracker.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/util/fail_point.h"

namespace mongo {

// When set, simulates returning WT_PREPARE_CONFLICT on WT cursor read operations.
extern FailPoint WTPrepareConflictForReads;

// When set, WT_ROLLBACK is returned in place of retrying on WT_PREPARE_CONFLICT errors.
extern FailPoint WTSkipPrepareConflictRetries;

extern FailPoint WTPrintPrepareConflictLog;

/**
 * Logs a message with the number of prepare conflict retry attempts.
 */
void wiredTigerPrepareConflictLog(int attempt);

/**
 * Logs a message to confirm we've hit the WTPrintPrepareConflictLog fail point.
 */
void wiredTigerPrepareConflictFailPointLog();

/**
 * Runs the argument function f as many times as needed for f to return an error other than
 * WT_PREPARE_CONFLICT. Each time f returns WT_PREPARE_CONFLICT we wait until the current unit of
 * work commits or aborts, and then try f again. Imposes no upper limit on the number of times to
 * re-try f, so any required timeout behavior must be enforced within f.
 * The function f must return a WiredTiger error code.
 */
template <typename F>
int wiredTigerPrepareConflictRetry(OperationContext* opCtx, F&& f) {
    invariant(opCtx);

    auto recoveryUnit = WiredTigerRecoveryUnit::get(opCtx);
    int attempts = 1;
    // If we return from this function, we have either returned successfully or we've returned an
    // error other than WT_PREPARE_CONFLICT. Reset PrepareConflictTracker accordingly.
    ON_BLOCK_EXIT([opCtx] { PrepareConflictTracker::get(opCtx).endPrepareConflict(); });
    // If the failpoint is enabled, don't call the function, just simulate a conflict.
    int ret = MONGO_unlikely(WTPrepareConflictForReads.shouldFail()) ? WT_PREPARE_CONFLICT
                                                                     : WT_READ_CHECK(f());
    if (ret != WT_PREPARE_CONFLICT)
        return ret;

    PrepareConflictTracker::get(opCtx).beginPrepareConflict();

    // It is contradictory to be running into a prepare conflict when we are ignoring interruptions,
    // particularly when running code inside an
    // OperationContext::runWithoutInterruptionExceptAtGlobalShutdown block. Operations executed in
    // this way are expected to be set to ignore prepare conflicts.
    invariant(!opCtx->isIgnoringInterrupts());

    if (MONGO_unlikely(WTPrintPrepareConflictLog.shouldFail())) {
        wiredTigerPrepareConflictFailPointLog();
    }

    CurOp::get(opCtx)->debug().additiveMetrics.incrementPrepareReadConflicts(1);
    wiredTigerPrepareConflictLog(attempts);

    const auto lockerInfo = opCtx->lockState()->getLockerInfo(boost::none);
    invariant(lockerInfo);
    for (const auto& lock : lockerInfo->locks) {
        const auto type = lock.resourceId.getType();
        // If a user operation on secondaries acquires a lock in MODE_S and then blocks on a prepare
        // conflict with a prepared transaction, deadlock will occur at the commit time of the
        // prepared transaction when it attempts to reacquire (since locks were yielded on
        // secondaries) an IX lock that conflicts with the MODE_S lock held by the user operation.
        // User operations that acquire MODE_X locks and block on prepare conflicts could lead to
        // the same problem. However, user operations on secondaries should never hold MODE_X locks.
        // Since prepared transactions will not reacquire RESOURCE_MUTEX / RESOURCE_METADATA locks
        // at commit time, these lock types are safe. Therefore, invariant here that we do not get a
        // prepare conflict while holding a global, database, or collection MODE_S lock (or MODE_X
        // lock for completeness).
        if (type == RESOURCE_GLOBAL || type == RESOURCE_DATABASE || type == RESOURCE_COLLECTION)
            invariant(lock.mode != MODE_S && lock.mode != MODE_X,
                      str::stream() << lock.resourceId.toString() << " in " << modeName(lock.mode));
    }

    if (MONGO_unlikely(WTSkipPrepareConflictRetries.shouldFail())) {
        // Callers of wiredTigerPrepareConflictRetry() should eventually call wtRCToStatus() via
        // invariantWTOK() and have the WT_ROLLBACK error bubble up as a WriteConflictException.
        // Enabling the "skipWriteConflictRetries" failpoint in conjunction with the
        // "WTSkipPrepareConflictRetries" failpoint prevents the higher layers from retrying the
        // entire operation.
        return WT_ROLLBACK;
    }

    while (true) {
        attempts++;
        auto lastCount = recoveryUnit->getSessionCache()->getPrepareCommitOrAbortCount();
        // If the failpoint is enabled, don't call the function, just simulate a conflict.
        ret = MONGO_unlikely(WTPrepareConflictForReads.shouldFail()) ? WT_PREPARE_CONFLICT
                                                                     : WT_READ_CHECK(f());

        if (ret != WT_PREPARE_CONFLICT)
            return ret;

        CurOp::get(opCtx)->debug().additiveMetrics.incrementPrepareReadConflicts(1);
        wiredTigerPrepareConflictLog(attempts);

        // Wait on the session cache to signal that a unit of work has been committed or aborted.
        recoveryUnit->getSessionCache()->waitUntilPreparedUnitOfWorkCommitsOrAborts(opCtx,
                                                                                    lastCount);
    }
}
}  // namespace mongo
