/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"

#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

const OperationContext::Decoration<std::unique_ptr<OperationMemoryUsageTracker>> _getFromOpCtx =
    OperationContext::declareDecoration<std::unique_ptr<OperationMemoryUsageTracker>>();

}

/**
 * Return the OperationMemoryUsageTracker for this operation. If we haven't yet created one, do it
 * now.
 */
OperationMemoryUsageTracker* OperationMemoryUsageTracker::getOperationMemoryUsageTracker(
    OperationContext* opCtx) {
    OperationMemoryUsageTracker* opTracker = _getFromOpCtx(opCtx).get();
    if (!opTracker) {
        auto uniqueTracker = std::make_unique<OperationMemoryUsageTracker>(opCtx);
        opTracker = uniqueTracker.get();
        opTracker->setWriteToCurOp(
            [opTracker](int64_t inUseTrackedMemoryBytes, int64_t peakTrackedMemBytes) {
                if (opTracker->_opCtx) {
                    CurOp::get(opTracker->_opCtx)
                        ->setMemoryTrackingStats(inUseTrackedMemoryBytes, peakTrackedMemBytes);
                } else {
                    LOGV2_DEBUG(10430900, 3, "No OperationContext on OperationMemoryUsageTracker");
                }
            });
        _getFromOpCtx(opCtx) = std::move(uniqueTracker);
    }

    return opTracker;
}

SimpleMemoryUsageTracker OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForStage(
    const ExpressionContext& expCtx, int64_t maxMemoryUsageBytes) {
    if (!feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled() ||
        expCtx.isIncompatibleWithMemoryTracking()) {
        return SimpleMemoryUsageTracker{maxMemoryUsageBytes};
    }

    OperationContext* opCtx = expCtx.getOperationContext();
    OperationMemoryUsageTracker* opTracker = getOperationMemoryUsageTracker(opCtx);
    return SimpleMemoryUsageTracker{opTracker, maxMemoryUsageBytes};
}

SimpleMemoryUsageTracker OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForSBE(
    OperationContext* opCtx, int64_t maxMemoryUsageBytes) {
    if (!feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
        return SimpleMemoryUsageTracker{maxMemoryUsageBytes};
    }
    OperationMemoryUsageTracker* opTracker = getOperationMemoryUsageTracker(opCtx);
    return SimpleMemoryUsageTracker{opTracker, maxMemoryUsageBytes};
}

SimpleMemoryUsageTracker OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForStage(
    const ExpressionContext& expCtx, int64_t maxMemoryUsageBytes) {
    if (!feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled() ||
        expCtx.isIncompatibleWithMemoryTracking()) {
        return SimpleMemoryUsageTracker{maxMemoryUsageBytes, 0 /* chunkSize */};
    }

    OperationContext* opCtx = expCtx.getOperationContext();
    OperationMemoryUsageTracker* opTracker = getOperationMemoryUsageTracker(opCtx);
    return SimpleMemoryUsageTracker{
        opTracker, maxMemoryUsageBytes, internalQueryMaxWriteToCurOpMemoryUsageBytes.loadRelaxed()};
}

MemoryUsageTracker OperationMemoryUsageTracker::createMemoryUsageTrackerForStage(
    const ExpressionContext& expCtx, bool allowDiskUse, int64_t maxMemoryUsageBytes) {
    if (!feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled() ||
        expCtx.isIncompatibleWithMemoryTracking()) {
        return MemoryUsageTracker{allowDiskUse, maxMemoryUsageBytes};
    }

    OperationContext* opCtx = expCtx.getOperationContext();
    OperationMemoryUsageTracker* opTracker = getOperationMemoryUsageTracker(opCtx);
    return MemoryUsageTracker{opTracker, allowDiskUse, maxMemoryUsageBytes};
}

std::unique_ptr<OperationMemoryUsageTracker> OperationMemoryUsageTracker::moveFromOpCtxIfAvailable(
    OperationContext* opCtx) {
    std::unique_ptr<OperationMemoryUsageTracker> tracker = std::move(_getFromOpCtx(opCtx));
    if (tracker) {
        tracker->_opCtx = nullptr;
    }
    return tracker;
}

void OperationMemoryUsageTracker::moveToOpCtxIfAvailable(
    OperationContext* opCtx, std::unique_ptr<OperationMemoryUsageTracker> tracker) {
    invariant(opCtx);
    if (tracker) {
        tracker->_opCtx = opCtx;
        CurOp::get(opCtx)->setMemoryTrackingStats(tracker->inUseTrackedMemoryBytes(),
                                                  tracker->peakTrackedMemoryBytes());
    }
    _getFromOpCtx(opCtx) = std::move(tracker);
}

}  // namespace mongo
