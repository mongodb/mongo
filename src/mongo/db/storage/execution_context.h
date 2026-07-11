// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/prepare_conflict_tracker.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

/**
 * This class encompasses the storage state needed for an operation.
 */
class StorageExecutionContext {
public:
    /**
     * Retrieves a reference to the StorageExecutionContext decorating the OperationContext.
     */
    static StorageExecutionContext* get(OperationContext* opCtx);

    /**
     * Retrieve metrics from the storage layer.
     */
    AtomicStorageMetrics& getStorageMetrics() {
        return _storageMetrics;
    }

    PrepareConflictTracker& getPrepareConflictTracker() {
        return _prepareConflictTracker;
    }

private:
    AtomicStorageMetrics _storageMetrics;
    PrepareConflictTracker _prepareConflictTracker;
};

}  // namespace mongo
