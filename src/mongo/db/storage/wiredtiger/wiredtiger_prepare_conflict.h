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
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/util/fail_point_service.h"

namespace mongo {

// When set, simulates returning WT_PREPARE_CONFLICT on WT cursor read operations.
MONGO_FAIL_POINT_DECLARE(WTPrepareConflictForReads);

// When set, WT_ROLLBACK is returned in place of retrying on WT_PREPARE_CONFLICT errors.
MONGO_FAIL_POINT_DECLARE(WTSkipPrepareConflictRetries);

/**
 * Logs a message with the number of prepare conflict retry attempts.
 */
void wiredTigerPrepareConflictLog(int attempt);

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
    // If the failpoint is enabled, don't call the function, just simulate a conflict.
    int ret =
        MONGO_FAIL_POINT(WTPrepareConflictForReads) ? WT_PREPARE_CONFLICT : WT_READ_CHECK(f());
    if (ret != WT_PREPARE_CONFLICT)
        return ret;
    CurOp::get(opCtx)->debug().additiveMetrics.incrementPrepareReadConflicts(1);
    wiredTigerPrepareConflictLog(attempts);

    if (MONGO_FAIL_POINT(WTSkipPrepareConflictRetries)) {
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
        ret =
            MONGO_FAIL_POINT(WTPrepareConflictForReads) ? WT_PREPARE_CONFLICT : WT_READ_CHECK(f());

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
