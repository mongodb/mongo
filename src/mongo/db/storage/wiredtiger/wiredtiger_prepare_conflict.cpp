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

#include "mongo/db/storage/wiredtiger/wiredtiger_prepare_conflict.h"

#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/prepare_conflict_tracker.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <wiredtiger.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

MONGO_FAIL_POINT_DEFINE(WTSkipPrepareConflictRetries);

MONGO_FAIL_POINT_DEFINE(WTPrintPrepareConflictLog);

void wiredTigerPrepareConflictLog(int attempts) {
    LOGV2_DEBUG(22379,
                1,
                "Caught WT_PREPARE_CONFLICT, attempt {attempts}. Waiting for unit of work to "
                "commit or abort.",
                "attempts"_attr = attempts);
}

void wiredTigerPrepareConflictFailPointLog() {
    LOGV2(22380, "WTPrintPrepareConflictLog fail point enabled.");
}

int wiredTigerPrepareConflictRetrySlow(Interruptible& interruptible,
                                       PrepareConflictTracker& tracker,
                                       RecoveryUnit& ru,
                                       std::function<int()> func) {
    int attempts = 1;
    wiredTigerPrepareConflictLog(attempts);

    if (!ru.getBlockingAllowed()) {
        throwWriteConflictException(
            str::stream() << "Hit a prepare conflict when in a non-blocking state. Timestamped: "
                          << ru.isTimestamped());
    }

    // If we return from this function, we have either returned successfully or we've returned an
    // error other than WT_PREPARE_CONFLICT. Reset PrepareConflictTracker accordingly.
    ON_BLOCK_EXIT([&tracker] { tracker.endPrepareConflict(*globalSystemTickSource()); });
    tracker.beginPrepareConflict(*globalSystemTickSource());

    if (MONGO_unlikely(WTPrintPrepareConflictLog.shouldFail())) {
        wiredTigerPrepareConflictFailPointLog();
    }

    if (MONGO_unlikely(WTSkipPrepareConflictRetries.shouldFail())) {
        // Callers of wiredTigerPrepareConflictRetry() should eventually call wtRCToStatus() via
        // invariantWTOK() and have the WT_ROLLBACK error bubble up as a WriteConflictException.
        // Enabling the "skipWriteConflictRetries" failpoint in conjunction with the
        // "WTSkipPrepareConflictRetries" failpoint prevents the higher layers from retrying the
        // entire operation.
        return WT_ROLLBACK;
    }

    auto& recoveryUnit = WiredTigerRecoveryUnit::get(ru);
    while (true) {
        attempts++;
        auto lastCount = recoveryUnit.getConnection()->getPrepareCommitOrAbortCount();
        int ret = WT_READ_CHECK(func());

        if (ret != WT_PREPARE_CONFLICT)
            return ret;
        tracker.updatePrepareConflict(*globalSystemTickSource());
        wiredTigerPrepareConflictLog(attempts);

        // Wait on the session cache to signal that a unit of work has been committed or aborted.
        recoveryUnit.getConnection()->waitUntilPreparedUnitOfWorkCommitsOrAborts(interruptible,
                                                                                 lastCount);
    }
}

}  // namespace mongo
