// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
