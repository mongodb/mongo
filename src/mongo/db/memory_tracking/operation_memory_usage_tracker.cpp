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

const OperationContext::Decoration<std::shared_ptr<OperationMemoryUsageTracker>> _getFromOpCtx =
    OperationContext::declareDecoration<std::shared_ptr<OperationMemoryUsageTracker>>();

}

/**
 * Return the OperationMemoryUsageTracker for this operation. If we haven't yet created one, do it
 * now.
 */
OperationMemoryUsageTracker* OperationMemoryUsageTracker::getOperationMemoryUsageTracker(
    OperationContext* opCtx) {
    OperationMemoryUsageTracker* opTracker = _getFromOpCtx(opCtx).get();
    if (!opTracker) {
        auto sharedTracker = std::make_shared<OperationMemoryUsageTracker>(opCtx);
        opTracker = sharedTracker.get();
        opTracker->setWriteToCurOp(
            [opTracker](int64_t inUseTrackedMemoryBytes, int64_t peakTrackedMemBytes) {
                if (opTracker->_opCtx) {
                    CurOp::get(opTracker->_opCtx)
                        ->setMemoryTrackingStats(inUseTrackedMemoryBytes, peakTrackedMemBytes);
                } else {
                    LOGV2_DEBUG(10430900, 3, "No OperationContext on OperationMemoryUsageTracker");
                }
            });
        _getFromOpCtx(opCtx) = std::move(sharedTracker);
    }

    return opTracker;
}

SimpleMemoryUsageTracker OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForStage(
    const ExpressionContext& expCtx, int64_t maxMemoryUsageBytes) {
    return createSimpleMemoryUsageTrackerImpl(expCtx.getOperationContext(),
                                              maxMemoryUsageBytes,
                                              0 /* chunkSize */,
                                              expCtx.getExcludeOperationMemoryTracking());
}

SimpleMemoryUsageTracker OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForSBE(
    OperationContext* opCtx, int64_t maxMemoryUsageBytes) {
    return createSimpleMemoryUsageTrackerImpl(opCtx, maxMemoryUsageBytes);
}

DeduplicatorReporter OperationMemoryUsageTracker::createDeduplicatorReporter(
    std::function<void(int64_t, int64_t)> callback, int64_t chunkSize) {
    if (!feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
        return DeduplicatorReporter{nullptr, chunkSize};
    }
    return DeduplicatorReporter{std::move(callback), chunkSize};
}

SimpleMemoryUsageTracker OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForStage(
    const ExpressionContext& expCtx, int64_t maxMemoryUsageBytes) {
    return createSimpleMemoryUsageTrackerImpl(
        expCtx.getOperationContext(),
        maxMemoryUsageBytes,
        internalQueryMaxWriteToCurOpMemoryUsageBytes.loadRelaxed(),
        expCtx.getExcludeOperationMemoryTracking());
}

SimpleMemoryUsageTracker OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForSBE(
    OperationContext* opCtx, int64_t maxMemoryUsageBytes) {
    return createSimpleMemoryUsageTrackerImpl(
        opCtx, maxMemoryUsageBytes, internalQueryMaxWriteToCurOpMemoryUsageBytes.loadRelaxed());
}

SimpleMemoryUsageTracker OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerImpl(
    OperationContext* opCtx,
    int64_t maxMemoryUsageBytes,
    int64_t chunkSize,
    bool excludeOperationMemoryTracking) {
    if (!feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled() ||
        excludeOperationMemoryTracking) {
        return SimpleMemoryUsageTracker{maxMemoryUsageBytes, chunkSize};
    }

    OperationMemoryUsageTracker* opTracker = getOperationMemoryUsageTracker(opCtx);
    return SimpleMemoryUsageTracker{opTracker, maxMemoryUsageBytes, chunkSize};
}

MemoryUsageTracker OperationMemoryUsageTracker::createMemoryUsageTrackerForStage(
    const ExpressionContext& expCtx, bool allowDiskUse, int64_t maxMemoryUsageBytes) {
    return createMemoryUsageTrackerImpl(expCtx, allowDiskUse, maxMemoryUsageBytes);
}

MemoryUsageTracker OperationMemoryUsageTracker::createChunkedMemoryUsageTrackerForStage(
    const ExpressionContext& expCtx, bool allowDiskUse, int64_t maxMemoryUsageBytes) {
    return createMemoryUsageTrackerImpl(expCtx,
                                        allowDiskUse,
                                        maxMemoryUsageBytes,
                                        internalQueryMaxWriteToCurOpMemoryUsageBytes.loadRelaxed());
}

MemoryUsageTracker OperationMemoryUsageTracker::createMemoryUsageTrackerImpl(
    const ExpressionContext& expCtx,
    bool allowDiskUse,
    int64_t maxMemoryUsageBytes,
    int64_t chunkSize) {
    if (!feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled() ||
        expCtx.getExcludeOperationMemoryTracking()) {
        return MemoryUsageTracker{allowDiskUse, maxMemoryUsageBytes};
    }

    OperationContext* opCtx = expCtx.getOperationContext();
    OperationMemoryUsageTracker* opTracker = getOperationMemoryUsageTracker(opCtx);
    return MemoryUsageTracker{opTracker, allowDiskUse, maxMemoryUsageBytes, chunkSize};
}

std::shared_ptr<OperationMemoryUsageTracker>
OperationMemoryUsageTracker::detachFromOpCtxIfAvailable(OperationContext* opCtx) {
    invariant(opCtx);
    std::shared_ptr<OperationMemoryUsageTracker> tracker = std::move(_getFromOpCtx(opCtx));
    if (tracker) {
        tracker->_opCtx = nullptr;
    }
    return tracker;
}

void OperationMemoryUsageTracker::attachToOpCtxIfAvailable(
    OperationContext* opCtx,
    std::shared_ptr<OperationMemoryUsageTracker> tracker,
    ReportToCurOp reportToCurOp) {
    invariant(opCtx);
    if (!tracker) {
        // Nothing to publish. Leave any tracker already on the opCtx in place.
        return;
    }
    if (reportToCurOp == ReportToCurOp::kYes) {
        tracker->_opCtx = opCtx;
        CurOp::get(opCtx)->setMemoryTrackingStats(tracker->inUseTrackedMemoryBytes(),
                                                  tracker->peakTrackedMemoryBytes());
    } else {
        // Reset tracker _opCtx to not report to curOp.
        tracker->_opCtx = nullptr;
    }
    _getFromOpCtx(opCtx) = std::move(tracker);
}

std::shared_ptr<OperationMemoryUsageTracker> OperationMemoryUsageTracker::getOwningIfExists(
    OperationContext* opCtx) {
    invariant(opCtx);
    return _getFromOpCtx(opCtx);
}

OperationMemoryUsageTracker* OperationMemoryUsageTracker::getIfExists(OperationContext* opCtx) {
    return _getFromOpCtx(opCtx).get();
}

}  // namespace mongo
