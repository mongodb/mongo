// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo {

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

    OperationContext* getOperationContext() override;

    /**
     * Records that a single key write actually happened against the storage engine (one document
     * record or one index key). Called from WiredTiger cursor helpers only after a successful
     * write (ret == 0). Rejected writes are not counted; write-conflict retries accumulate as each
     * retry re-applies its successful writes. If this OperationContext did not pass through
     * write-throttler admission, recording is skipped.
     */
    void recordStorageWrite() {
        if (getAdmissions() <= 0) {
            return;
        }
        _storageWrites.fetchAndAddRelaxed(1);
    }

    /**
     * Adds `count` successful storage-engine key writes to the reconciliation accumulator. Used by
     * unit tests to simulate WiredTiger increments without going through the storage layer.
     */
    void recordStorageWrites(int64_t count) {
        if (count <= 0 || getAdmissions() <= 0) {
            return;
        }
        _storageWrites.fetchAndAddRelaxed(count);
    }

    /**
     * Returns the number of storage-engine key writes recorded for this operation.
     */
    int64_t getStorageWrites() const {
        return _storageWrites.loadRelaxed();
    }

    /**
     * Marks the service-entry admission as available to cover the first known child write. This
     * lets mid-flight admission preserve one-token-per-statement accounting without double
     * charging the first child write.
     */
    void markServiceEntryAdmissionCredit() {
        _serviceEntryAdmissionCredit.storeRelaxed(true);
    }

    /**
     * Consumes the service-entry admission credit once. Operation contexts are single-owner for
     * command execution, so relaxed load/store is sufficient here.
     */
    bool consumeServiceEntryAdmissionCredit() {
        if (!_serviceEntryAdmissionCredit.loadRelaxed()) {
            return false;
        }
        _serviceEntryAdmissionCredit.storeRelaxed(false);
        return true;
    }

private:
    // Count of individual key writes that WiredTiger actually applied (document records and index
    // keys). Rejected writes are not counted; write-conflict retries accumulate.
    AtomicWord<int64_t> _storageWrites{0};
    AtomicWord<bool> _serviceEntryAdmissionCredit{false};
};

}  // namespace mongo
