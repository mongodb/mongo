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

#pragma once

#include "mongo/db/curop.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/util/modules.h"

#include <cstdint>
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
class OperationMemoryUsageTracker : public SimpleMemoryUsageTracker {
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
        int64_t maxMemoryUsageBytes = std::numeric_limits<int64_t>::max());

    static SimpleMemoryUsageTracker createSimpleMemoryUsageTrackerForSBE(
        OperationContext* opCtx, int64_t maxMemoryUsageBytes = std::numeric_limits<int64_t>::max());

    static DeduplicatorReporter createDeduplicatorReporter(
        std::function<void(int64_t, int64_t)> callback, int64_t chunkSize);

    /**
     * Rate-limited memory tracker. Chunking refers to the fact that memory usage reporting will be
     * done in discrete chunks (0, chunkSize, 2*chunkSize, etc.) rather than exact values.
     */
    static SimpleMemoryUsageTracker createChunkedSimpleMemoryUsageTrackerForStage(
        const ExpressionContext& expCtx,
        int64_t maxMemoryUsageBytes = std::numeric_limits<int64_t>::max());

    static SimpleMemoryUsageTracker createChunkedSimpleMemoryUsageTrackerForSBE(
        OperationContext* opCtx, int64_t maxMemoryUsageBytes = std::numeric_limits<int64_t>::max());

    /**
     * When constructing a stage containing a MemoryUsageTracker, use this method to ensure that we
     * aggregate operation-wide memory stats.
     */
    static MemoryUsageTracker createMemoryUsageTrackerForStage(
        const ExpressionContext& expCtx,
        bool allowDiskUse = false,
        int64_t maxMemoryUsageBytes = std::numeric_limits<int64_t>::max());

    /**
     * Rate-limited memory tracker. Chunking refers to the fact that memory usage reporting will be
     * done in discrete chunks (0, chunkSize, 2*chunkSize, etc.) rather than exact values.
     */
    static MemoryUsageTracker createChunkedMemoryUsageTrackerForStage(
        const ExpressionContext& expCtx,
        bool allowDiskUse = false,
        int64_t maxMemoryUsageBytes = std::numeric_limits<int64_t>::max());

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
     * Returns the operation's tracker if one exists, otherwise nullptr. Never creates one.
     */
    static OperationMemoryUsageTracker* getIfExists(OperationContext* opCtx);

    explicit OperationMemoryUsageTracker(OperationContext* opCtx) : _opCtx(opCtx) {}

private:
    friend class RunAggregateTest;
    friend class ClusterAggregateMemoryTrackingTest;

    static OperationMemoryUsageTracker* getOperationMemoryUsageTracker(OperationContext* opCtx);

    static SimpleMemoryUsageTracker createSimpleMemoryUsageTrackerImpl(
        OperationContext* opCtx,
        int64_t maxMemoryUsageBytes,
        int64_t chunkSize = 0,
        bool excludeOperationMemoryTracking = false);
    static MemoryUsageTracker createMemoryUsageTrackerImpl(const ExpressionContext& expCtx,
                                                           bool allowDiskUse,
                                                           int64_t maxMemoryUsageBytes,
                                                           int64_t chunkSize = 0);

    OperationContext* _opCtx;
};
}  // namespace mongo
