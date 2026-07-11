// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/storage/prepare_conflict_tracker.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/modules.h"

#include <wiredtiger.h>

namespace mongo {

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
int wiredTigerPrepareConflictRetrySlow(Interruptible& interruptible,
                                       PrepareConflictTracker& tracker,
                                       RecoveryUnit& ru,
                                       std::function<int()> func);

template <typename F>
int wiredTigerPrepareConflictRetry(Interruptible& interruptible,
                                   PrepareConflictTracker& tracker,
                                   RecoveryUnit& ru,
                                   F&& f) {
    int ret = WT_READ_CHECK(f());
    if (ret != WT_PREPARE_CONFLICT)
        return ret;

    return wiredTigerPrepareConflictRetrySlow(interruptible, tracker, ru, f);
}

}  // namespace mongo
