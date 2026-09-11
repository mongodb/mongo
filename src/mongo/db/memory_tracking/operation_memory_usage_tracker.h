// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/curop.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <limits>
#include <memory>

namespace mongo {

/**
 * A memory usage tracker class that aggregates memory statistics for the entire life of a query,
 * including the initial request and subsequent getMore() invocations. Stages that track memory will
 * report to an instance of this class, which in turn will update statistics in CurOp of the current
 * operation.
 *
 * Between commands, the tracker is stashed on the cursor (ClientCursor on mongod, or
 * ClusterClientCursor if we are on a router) if there is more data. The respective cursor manager
 * classes take care of the stashing (CursorManager on mongod, or ClusterCursorManager on the
 * router.)
 *
 * This class is instantiated on demand when building the stages, and its add() method is invoked as
 * early as the constructor (accumulators call 'add(sizeof(*this))' at construction time) and as
 * late as when destructors fire. Sometimes, on the router we have memory trackers for pipelines
 * that won't actually be executed, because the pipelines are executed in another process. We may
 * or may not have a valid opCtx pointer for these cases, and so will not report changes in memory
 * usage to the Curop instance. Since the memory amounts involved will be small and not for
 * pipelines that are actually executing, this is acceptable.
 *
 * For examples of correct usage of this class, see the C++ unit tests:
 * - RunAggregateTest: TransferOperationMemoryUsageTracker
 * - ClusterAggregateMemoryTrackingTest: MemoryTrackingWorksOnRouter
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] OperationMemoryUsageTracker
    : public SimpleMemoryUsageTracker {
    OperationMemoryUsageTracker() = delete;

public:
    /**
     * Whether attachToOpCtxIfAvailable() should also point the tracker's CurOp reporting target at
     * the destination opCtx. See that method for details.
     */
    enum class ReportToCurOp { kNo, kYes };

    /**
     * When constructing a stage containing a SimpleMemoryUsageTracker, use this method to ensure
     * that we aggregate operation-wide memory stats.
     */
    static SimpleMemoryUsageTracker createSimpleMemoryUsageTrackerForStage(
        const ExpressionContext& expCtx,
        MemoryUsageLimit maxMemoryUsageBytes = MemoryUsageLimit{
            std::numeric_limits<int64_t>::max()});

    static SimpleMemoryUsageTracker createSimpleMemoryUsageTrackerForSBE(
        OperationContext* opCtx,
        MemoryUsageLimit maxMemoryUsageBytes = MemoryUsageLimit{
            std::numeric_limits<int64_t>::max()});

    static DeduplicatorReporter createDeduplicatorReporter(
        std::function<void(int64_t, int64_t)> callback, int64_t chunkSize);

    /**
     * Rate-limited memory tracker. Chunking refers to the fact that memory usage reporting will be
     * done in discrete chunks (0, chunkSize, 2*chunkSize, etc.) rather than exact values.
     */
    static SimpleMemoryUsageTracker createChunkedSimpleMemoryUsageTrackerForStage(
        const ExpressionContext& expCtx,
        MemoryUsageLimit maxMemoryUsageBytes = MemoryUsageLimit{
            std::numeric_limits<int64_t>::max()});

    static SimpleMemoryUsageTracker createChunkedSimpleMemoryUsageTrackerForSBE(
        OperationContext* opCtx,
        MemoryUsageLimit maxMemoryUsageBytes = MemoryUsageLimit{
            std::numeric_limits<int64_t>::max()});

    /**
     * When constructing a stage containing a MemoryUsageTracker, use this method to ensure that we
     * aggregate operation-wide memory stats.
     */
    static MemoryUsageTracker createMemoryUsageTrackerForStage(
        const ExpressionContext& expCtx,
        bool allowDiskUse = false,
        MemoryUsageLimit maxMemoryUsageBytes = MemoryUsageLimit{
            std::numeric_limits<int64_t>::max()});

    /**
     * Rate-limited memory tracker. Chunking refers to the fact that memory usage reporting will be
     * done in discrete chunks (0, chunkSize, 2*chunkSize, etc.) rather than exact values.
     */
    static MemoryUsageTracker createChunkedMemoryUsageTrackerForStage(
        const ExpressionContext& expCtx,
        bool allowDiskUse = false,
        MemoryUsageLimit maxMemoryUsageBytes = MemoryUsageLimit{
            std::numeric_limits<int64_t>::max()});

    void propagateStatsToCurOp() const {
        CurOp::get(_opCtx)->setMemoryTrackingStats(inUseTrackedMemoryBytes(),
                                                   peakTrackedMemoryBytes());
    }

    /**
     * Detach the operation's memory tracker from the operation context. Returns a co-owning
     * reference to it (or nullptr if there was none).
     */
    static std::shared_ptr<OperationMemoryUsageTracker> detachFromOpCtxIfAvailable(
        OperationContext* opCtx);

    /**
     * Attach the given memory tracker to the operation context. A null tracker is a no-op that
     * leaves any tracker already parked on the opCtx by a sibling cursor in place.
     *
     * With ReportToCurOp::kYes, also point the tracker's CurOp reporting target at 'opCtx' and
     * flush current stats to that CurOp. With kNo, the tracker is published for binding only and
     * its CurOp reporting target is cleared.
     */
    static void attachToOpCtxIfAvailable(OperationContext* opCtx,
                                         std::shared_ptr<OperationMemoryUsageTracker> tracker,
                                         ReportToCurOp reportToCurOp = ReportToCurOp::kYes);

    /**
     * Returns a co-owning reference to the operation's memory tracker without removing it from the
     * operation context, or nullptr if there is none. Unlike getOperationMemoryUsageTracker(), does
     * not create one.
     */
    [[nodiscard]] static std::shared_ptr<OperationMemoryUsageTracker> getOwningIfExists(
        OperationContext* opCtx);

    /**
     * Returns true if the given operation context currently holds a memory tracker. Unlike
     * getOperationMemoryUsageTracker(), never creates one.
     */
    static bool hasTrackerOnOpCtx(OperationContext* opCtx);

    /**
     * Returns the operation's tracker if one exists, otherwise nullptr. Never creates one. Lets the
     * load-shedding decision read tracked-memory size without forcing a tracker onto operations
     * that don't track memory.
     */
    static OperationMemoryUsageTracker* getIfExists(OperationContext* opCtx);

    /**
     * Re-point 'tracker' at the operation memory tracker for 'opCtx'. For stages whose lifetime
     * spans getMore opCtx swaps to re-bind after being detached, since the operation tracker lives
     * on the OperationContext. No-op when memory tracking is disabled or when 'expCtx' excludes
     * operation memory tracking, matching the create*ForStage() factories, so a stage built without
     * a base stays standalone.
     *
     * TODO SERVER-131203: this is a stopgap and is NOT for general use -- it exists specifically to
     * let BatchedEnrichmentStage rebind its tracker base across getMore opCtx swaps. Remove it once
     * that stage's memory tracking is properly integrated with the operation memory tracker.
     */
    static void rebindToOperation(SimpleMemoryUsageTracker& tracker,
                                  const ExpressionContext& expCtx,
                                  OperationContext* opCtx);

    explicit OperationMemoryUsageTracker(OperationContext* opCtx);

private:
    friend class RunAggregateTest;
    friend class ClusterAggregateMemoryTrackingTest;

    static OperationMemoryUsageTracker* getOperationMemoryUsageTracker(OperationContext* opCtx);

    static SimpleMemoryUsageTracker createSimpleMemoryUsageTrackerImpl(
        OperationContext* opCtx,
        MemoryUsageLimit maxMemoryUsageBytes,
        int64_t chunkSize = 0,
        bool excludeOperationMemoryTracking = false);
    static MemoryUsageTracker createMemoryUsageTrackerImpl(const ExpressionContext& expCtx,
                                                           bool allowDiskUse,
                                                           MemoryUsageLimit maxMemoryUsageBytes,
                                                           int64_t chunkSize = 0);

    OperationContext* _opCtx;
};

}  // namespace mongo
