// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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
