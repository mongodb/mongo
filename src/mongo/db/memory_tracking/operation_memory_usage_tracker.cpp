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

#include "mongo/db/memory_tracking/op_memory_use.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/logv2/log.h"
#include "mongo/s/query/exec/cluster_client_cursor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {

const ClientCursor::Decoration<std::unique_ptr<OperationMemoryUsageTracker>> _getFromClientCursor =
    ClientCursor::declareDecoration<std::unique_ptr<OperationMemoryUsageTracker>>();

const ClusterClientCursor::Decoration<std::unique_ptr<OperationMemoryUsageTracker>>
    _getFromClusterClientCursor =
        ClusterClientCursor::declareDecoration<std::unique_ptr<OperationMemoryUsageTracker>>();

/**
 * Return the memory tracker that is decorating the given cursor. The 'cursor' parameter may be
 * either a ClientCursor or a ClusterClientCursor. Returns an empty unique pointer if the cursor is
 * not decorated. Note that this returns a reference to a unique_ptr so that it may be modified.
 */
std::unique_ptr<OperationMemoryUsageTracker>& getFromClientCursor(auto* cursor) {
    if constexpr (std::is_same_v<decltype(cursor), ClientCursor*>) {
        return _getFromClientCursor(cursor);
    } else {
        static_assert(std::is_same_v<decltype(cursor), ClusterClientCursor*>);
        return _getFromClusterClientCursor(cursor);
    }
}

}  // namespace

/**
 * Return the OperationMemoryUsageTracker for this operation. If we haven't yet created one, do it
 * now.
 */
OperationMemoryUsageTracker* OperationMemoryUsageTracker::getOperationMemoryUsageTracker(
    OperationContext* opCtx) {
    OperationMemoryUsageTracker* opTracker = OpMemoryUse::operationMemoryAggregator(opCtx).get();
    if (!opTracker) {
        auto uniqueTracker = std::make_unique<OperationMemoryUsageTracker>(opCtx);
        opTracker = uniqueTracker.get();
        opTracker->setDoExtraBookkeeping(
            [opTracker](int64_t currentMemoryBytes, int64_t maxUsedMemoryBytes) {
                tassert(10076202,
                        "unable to report memory tracking stats with missing OperationContext",
                        opTracker->_opCtx);
                CurOp::get(opTracker->_opCtx)
                    ->setMemoryTrackingStats(currentMemoryBytes, maxUsedMemoryBytes);
            });
        OpMemoryUse::operationMemoryAggregator(opCtx) = std::move(uniqueTracker);
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

void OperationMemoryUsageTracker::moveToCursorIfAvailable(OperationContext* opCtx,
                                                          ClientCursor* cursor) {
    OperationMemoryUsageTracker::_moveToCursorIfAvailable(opCtx, cursor);
}

void OperationMemoryUsageTracker::moveToCursorIfAvailable(OperationContext* opCtx,
                                                          ClusterClientCursor* cursor) {
    OperationMemoryUsageTracker::_moveToCursorIfAvailable(opCtx, cursor);
}

template <class C>
void OperationMemoryUsageTracker::_moveToCursorIfAvailable(OperationContext* opCtx, C* cursor) {
    std::unique_ptr<OperationMemoryUsageTracker> opCtxTracker{
        std::move(OpMemoryUse::operationMemoryAggregator(opCtx))};
    if (opCtxTracker) {
        std::unique_ptr<OperationMemoryUsageTracker>& cursorTracker = getFromClientCursor(cursor);
        tassert(10076200,
                "OperationMemoryUsageTracker already attached to ClientCursor",
                !cursorTracker);
        cursorTracker = std::move(opCtxTracker);
        cursorTracker->_opCtx = nullptr;
    }
}

void OperationMemoryUsageTracker::moveToOpCtxIfAvailable(ClientCursor* cursor,
                                                         OperationContext* opCtx) {
    OperationMemoryUsageTracker::_moveToOpCtxIfAvailable(cursor, opCtx);
}

void OperationMemoryUsageTracker::moveToOpCtxIfAvailable(ClusterClientCursor* cursor,
                                                         OperationContext* opCtx) {
    OperationMemoryUsageTracker::_moveToOpCtxIfAvailable(cursor, opCtx);
}

template <class C>
void OperationMemoryUsageTracker::_moveToOpCtxIfAvailable(C* cursor, OperationContext* opCtx) {
    std::unique_ptr<OperationMemoryUsageTracker> cursorTracker{
        std::move(getFromClientCursor(cursor))};
    if (cursorTracker) {
        std::unique_ptr<OperationMemoryUsageTracker>& opCtxTracker =
            OpMemoryUse::operationMemoryAggregator(opCtx);
        tassert(10076201,
                "OperationMemoryUsageTracker already attached to OperationContext",
                !opCtxTracker);
        opCtxTracker = std::move(cursorTracker);
        opCtxTracker->_opCtx = opCtx;

        // Propagate stats from the previous operation to this one.
        CurOp::get(opCtx)->setMemoryTrackingStats(opCtxTracker->currentMemoryBytes(),
                                                  opCtxTracker->maxMemoryBytes());
    }
}

OperationMemoryUsageTracker* OperationMemoryUsageTracker::getFromClientCursor_forTest(
    ClientCursor* clientCursor) {
    return getFromClientCursor(clientCursor).get();
}

OperationMemoryUsageTracker* OperationMemoryUsageTracker::getFromClientCursor_forTest(
    ClusterClientCursor* clientCursor) {
    return getFromClientCursor(clientCursor).get();
}

}  // namespace mongo
