// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"

#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/query/query_execution_knobs_gen.h"
#include "mongo/db/query/query_knob_descriptors_execution.h"
#include "mongo/logv2/log.h"

#include <limits>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {
namespace {

const OperationContext::Decoration<std::unique_ptr<OperationMemoryUsageTracker>> _getFromOpCtx =
    OperationContext::declareDecoration<std::unique_ptr<OperationMemoryUsageTracker>>();

}  // namespace

OperationMemoryUsageTracker::OperationMemoryUsageTracker(OperationContext* opCtx)
    : SimpleMemoryUsageTracker(MemoryUsageLimit{query_knobs::kMaxMemoryUsageBytesPerOperation}),
      _opCtx(opCtx) {}

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
    const ExpressionContext& expCtx, MemoryUsageLimit maxMemoryUsageBytes) {
    return createSimpleMemoryUsageTrackerImpl(expCtx.getOperationContext(),
                                              maxMemoryUsageBytes,
                                              0 /* chunkSize */,
                                              expCtx.getExcludeOperationMemoryTracking());
}

SimpleMemoryUsageTracker OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerForSBE(
    OperationContext* opCtx, MemoryUsageLimit maxMemoryUsageBytes) {
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
    const ExpressionContext& expCtx, MemoryUsageLimit maxMemoryUsageBytes) {
    return createSimpleMemoryUsageTrackerImpl(
        expCtx.getOperationContext(),
        maxMemoryUsageBytes,
        internalQueryMaxWriteToCurOpMemoryUsageBytes.loadRelaxed(),
        expCtx.getExcludeOperationMemoryTracking());
}

SimpleMemoryUsageTracker OperationMemoryUsageTracker::createChunkedSimpleMemoryUsageTrackerForSBE(
    OperationContext* opCtx, MemoryUsageLimit maxMemoryUsageBytes) {
    return createSimpleMemoryUsageTrackerImpl(
        opCtx, maxMemoryUsageBytes, internalQueryMaxWriteToCurOpMemoryUsageBytes.loadRelaxed());
}

SimpleMemoryUsageTracker OperationMemoryUsageTracker::createSimpleMemoryUsageTrackerImpl(
    OperationContext* opCtx,
    MemoryUsageLimit maxMemoryUsageBytes,
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
    const ExpressionContext& expCtx, bool allowDiskUse, MemoryUsageLimit maxMemoryUsageBytes) {
    return createMemoryUsageTrackerImpl(expCtx, allowDiskUse, maxMemoryUsageBytes);
}

MemoryUsageTracker OperationMemoryUsageTracker::createChunkedMemoryUsageTrackerForStage(
    const ExpressionContext& expCtx, bool allowDiskUse, MemoryUsageLimit maxMemoryUsageBytes) {
    return createMemoryUsageTrackerImpl(expCtx,
                                        allowDiskUse,
                                        maxMemoryUsageBytes,
                                        internalQueryMaxWriteToCurOpMemoryUsageBytes.loadRelaxed());
}

MemoryUsageTracker OperationMemoryUsageTracker::createMemoryUsageTrackerImpl(
    const ExpressionContext& expCtx,
    bool allowDiskUse,
    MemoryUsageLimit maxMemoryUsageBytes,
    int64_t chunkSize) {
    if (!feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled() ||
        expCtx.getExcludeOperationMemoryTracking()) {
        return MemoryUsageTracker{allowDiskUse, maxMemoryUsageBytes};
    }

    OperationContext* opCtx = expCtx.getOperationContext();
    OperationMemoryUsageTracker* opTracker = getOperationMemoryUsageTracker(opCtx);
    return MemoryUsageTracker{opTracker, allowDiskUse, maxMemoryUsageBytes, chunkSize};
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

bool OperationMemoryUsageTracker::hasTrackerOnOpCtx(OperationContext* opCtx) {
    return _getFromOpCtx(opCtx) != nullptr;
}

void OperationMemoryUsageTracker::rebindToOperation(SimpleMemoryUsageTracker& tracker,
                                                    OperationContext* opCtx) {
    if (!feature_flags::gFeatureFlagQueryMemoryTracking.isEnabled()) {
        return;
    }
    tracker.resetBase(getOperationMemoryUsageTracker(opCtx));
}

}  // namespace mongo
