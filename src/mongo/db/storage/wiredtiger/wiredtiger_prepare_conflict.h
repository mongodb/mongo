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

#include "mongo/db/storage/prepare_conflict_tracker.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

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
