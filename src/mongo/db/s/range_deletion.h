// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/type_chunk_range.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

namespace mongo {

/*
 * In memory representation of registered range deletion tasks. To each non-pending range
 * deletion task corresponds a registered task on the service.
 */
class RangeDeletion {
public:
    RangeDeletion(const RangeDeletionTask& task);

    ~RangeDeletion();

    const UUID& getTaskId() const;

    const ChunkRange& getRange() const;

    const Timestamp& getRegistrationTime() const;

    SharedSemiFuture<void> getPendingFuture();

    void clearPending();

    SharedSemiFuture<void> getCompletionFuture() const;

    void markComplete();

private:
    UUID _taskId;
    ChunkRange _range;
    Timestamp _registrationTime;

    // Marked ready once the range deletion has been fully processed
    SharedPromise<void> _completionPromise;

    SharedPromise<void> _pendingPromise;
};

}  // namespace mongo
