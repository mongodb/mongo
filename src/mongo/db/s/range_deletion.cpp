// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/range_deletion.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/service_context.h"

namespace mongo {

RangeDeletion::RangeDeletion(const RangeDeletionTask& task)
    : _taskId(task.getId()),
      _range(task.getRange()),
      _registrationTime(task.getTimestamp().value_or(
          Timestamp(getGlobalServiceContext()->getFastClockSource()->now()))) {}

RangeDeletion::~RangeDeletion() {
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(Status{ErrorCodes::Interrupted, "Range deletion interrupted"});
    }
}

const UUID& RangeDeletion::getTaskId() const {
    return _taskId;
}

const ChunkRange& RangeDeletion::getRange() const {
    return _range;
}

const Timestamp& RangeDeletion::getRegistrationTime() const {
    return _registrationTime;
}

SharedSemiFuture<void> RangeDeletion::getPendingFuture() {
    return _pendingPromise.getFuture();
}

void RangeDeletion::clearPending() {
    if (!_pendingPromise.getFuture().isReady()) {
        _pendingPromise.emplaceValue();
    }
}

SharedSemiFuture<void> RangeDeletion::getCompletionFuture() const {
    return _completionPromise.getFuture().semi().share();
}

void RangeDeletion::markComplete() {
    _completionPromise.emplaceValue();
}

}  // namespace mongo
