/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo {

class CurOp;
class OperationContext;

/**
 * Stores state and statistics related to write-throttler admission for a given operation. Recorded
 * admissions and queue-wait time are surfaced through the shared TicketHolderQueueStats registry
 * (the "writeThrottle" queue) so curOp/serverStatus report write-throttle waits like any other
 * admission queue.
 */
class [[MONGO_MOD_PUBLIC]] WriteThrottlerAdmissionContext : public AdmissionContext {
public:
    WriteThrottlerAdmissionContext() = default;

    /**
     * Retrieve the WriteThrottlerAdmissionContext decoration on the provided OperationContext.
     */
    static WriteThrottlerAdmissionContext& get(OperationContext* opCtx);

    /**
     * Adds successful written documents to the command-level reconciliation accumulator. If this
     * OperationContext did not pass through write-throttler admission, recording is skipped so
     * internal/direct writes cannot create orphan reconciliation work.
     */
    void recordWriteCostForReconciliation(int64_t docsWritten) {
        if (docsWritten <= 0 || getAdmissions() <= 0) {
            return;
        }
        _writeCostForReconciliation.fetchAndAddRelaxed(docsWritten);
    }

    /**
     * Returns the accumulated successful document cost and clears it. This supports one
     * command-level reconciliation from HandleRequest::completeOperation().
     */
    int64_t consumeWriteCostForReconciliation() {
        const int64_t current = _writeCostForReconciliation.loadRelaxed();
        _writeCostForReconciliation.storeRelaxed(0);
        return current;
    }

private:
    AtomicWord<int64_t> _writeCostForReconciliation{0};
};

/**
 * Records successful written documents from a CurOp's additive metrics into the operation's
 * write-throttler reconciliation accumulator. If the operation did not pass through write-throttler
 * admission, recording is skipped by WriteThrottlerAdmissionContext.
 */
[[MONGO_MOD_PUBLIC]] void recordWriteThrottlerCostForReconciliation(OperationContext* opCtx,
                                                                    CurOp* curOp);

}  // namespace mongo
