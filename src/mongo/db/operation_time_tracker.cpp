// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/operation_time_tracker.h"

#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <mutex>
#include <utility>

namespace mongo {
namespace {
struct OperationTimeTrackerHolder {
    OperationTimeTrackerHolder() : opTimeTracker(std::make_shared<OperationTimeTracker>()) {}
    static const OperationContext::Decoration<OperationTimeTrackerHolder> get;
    std::shared_ptr<OperationTimeTracker> opTimeTracker;
};

const OperationContext::Decoration<OperationTimeTrackerHolder> OperationTimeTrackerHolder::get =
    OperationContext::declareDecoration<OperationTimeTrackerHolder>();
}  // namespace

std::shared_ptr<OperationTimeTracker> OperationTimeTracker::get(OperationContext* opCtx) {
    auto timeTrackerHolder = OperationTimeTrackerHolder::get(opCtx);
    invariant(timeTrackerHolder.opTimeTracker);
    return timeTrackerHolder.opTimeTracker;
}

LogicalTime OperationTimeTracker::getMaxOperationTime() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _maxOperationTime;
}

void OperationTimeTracker::updateOperationTime(LogicalTime newTime) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (newTime > _maxOperationTime) {
        _maxOperationTime = std::move(newTime);
    }
}

}  // namespace mongo
