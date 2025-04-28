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
#include <cstdint>

#include "mongo/db/curop.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"

namespace mongo {

class ClientCursor;
class ClusterClientCursor;

/**
 * A memory usage tracker class that aggregates memory statistics for the entire life of a query,
 * including the initial request and subsequent getMore() invocations. Stages that track memory will
 * report to an instance of this class, which in turn will update statistics in CurOp of the current
 * operation.
 *
 * Between commands, the tracker is stashed on the cursor (ClientCursor on mongod, or
 * ClusterClientCursor if we are on a router) if there is more data. Command classes call
 * moveToCursorIfAvailable() or moveToOpCtxIfAvailable() to stash and unstash the tracker as needed.
 * When the tracker is stashed, its OperationContext pointer is null.
 *
 * This class is instantiated on demand when building the stages, and its add() method is invoked as
 * early as the constructor (accumulators call 'add(sizeof(*this))' at construction time) and as
 * late as when destructors fire. We need a valid OperationContext whenever we do memory accounting,
 *
 * TODO SERVER-104309 Update this comment if we refactor how the tracker is managed
 * Therefore, the lifetimes of the stages and when we stash need to be carefully managed:
 * - If we forget to stash, the opCtx pointer could be stale, and an ASAN build will show
 *   use-after-free. Example:
 *     1. An aggregate() request comes in, and the memory trackers in the created pipeline report
 *       upstream to the OperationMemoryUsageTracker, which contains a pointer to the current
 *       opCtx.
 *     2. The OperationMemoryUsageTracker is never transferred to the cursor, so when getMore() is
 *       called, the OperationMemoryUsageTracker still points to the opCtx for the initial request,
 *       which has been deleted.
 *     3. When add() is called, OperationMemoryUsageTracker attempts to update CurOp through a
 *       stale OpCtx pointer.
 * - If we forget to unstash, the opCtx pointer will be null, because invoking
 *     moveToOpCtxIfAvailable() sets the tracker's opCtx for the current operation. If add() is
 *     called when the tracker is in this state, a tassert will fire.
 * For examples of correct usage of this class, see the C++ unit tests:
 * - RunAggregateTest: TransferOperationMemoryUsageTracker
 * - ClusterAggregateMemoryTrackingTest: MemoryTrackingWorksOnRouter
 */
class OperationMemoryUsageTracker : public SimpleMemoryUsageTracker {
    OperationMemoryUsageTracker() = delete;

public:
    /**
     * When constructing a stage containing a SimpleMemoryUsageTracker, use this method to ensure
     * that we aggregate operation-wide memory stats.
     */
    static SimpleMemoryUsageTracker createSimpleMemoryUsageTrackerForStage(
        const ExpressionContext& expCtx,
        int64_t maxMemoryUsageBytes = std::numeric_limits<int64_t>::max());

    /**
     * When constructing a stage containing a MemoryUsageTracker, use this method to ensure that we
     * aggregate operation-wide memory stats.
     */
    static MemoryUsageTracker createMemoryUsageTrackerForStage(
        const ExpressionContext& expCtx,
        bool allowDiskUse = false,
        int64_t maxMemoryUsageBytes = std::numeric_limits<int64_t>::max());

    /**
     * Look for an OperationMemoryUsageTracker on the given OperationContext. If there is one, move
     * it to the cursor.
     *
     * Note: this method is not threadsafe and should not be called concurrently on the same cursor.
     */
    static void moveToCursorIfAvailable(OperationContext* opCtx, ClientCursor* cursor);

    /**
     * Look for an OperationMemoryUsageTracker on the given OperationContext. If there is one, move
     * it to the cursor.
     *
     * Note: this method is not threadsafe and should not be called concurrently on the same cursor.
     */
    static void moveToCursorIfAvailable(OperationContext* opCtx, ClusterClientCursor* cursor);

    /**
     * Look for an OperationMemoryUsageTracker on the given ClientCursor. If there is one, move it
     * to the OperationContext.
     *
     * Note: this method is not threadsafe and should not be called concurrently on the same cursor.
     */
    static void moveToOpCtxIfAvailable(ClientCursor* cursor, OperationContext* opCtx);

    /**
     * Look for an OperationMemoryUsageTracker on the given ClusterClientCursor. If there is one,
     * move it to the OperationContext.
     *
     * Note: this method is not threadsafe and should not be called concurrently on the same cursor.
     */
    static void moveToOpCtxIfAvailable(ClusterClientCursor* cursor, OperationContext* opCtx);

    explicit OperationMemoryUsageTracker(OperationContext* opCtx) : _opCtx(opCtx) {}

    static OperationMemoryUsageTracker* getFromClientCursor_forTest(ClientCursor* clientCursor);
    static OperationMemoryUsageTracker* getFromClientCursor_forTest(
        ClusterClientCursor* clientCursor);

private:
    friend class RunAggregateTest;

    template <class C>
    static void _moveToCursorIfAvailable(OperationContext* opCtx, C* cursor);

    template <class C>
    static void _moveToOpCtxIfAvailable(C* cursor, OperationContext* opCtx);

    static OperationMemoryUsageTracker* getOperationMemoryUsageTracker(OperationContext* opCtx);

    OperationContext* _opCtx;
};
}  // namespace mongo
