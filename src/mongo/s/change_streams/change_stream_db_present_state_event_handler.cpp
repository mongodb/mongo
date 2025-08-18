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

#include "mongo/s/change_streams/change_stream_db_present_state_event_handler.h"

#include "mongo/util/assert_util.h"

#include <algorithm>

namespace mongo {
namespace {
void updateActiveShardCursors(Timestamp atClusterTime,
                              const stdx::unordered_set<ShardId>& newActiveShardSet,
                              ChangeStreamReaderContext& readerCtx) {
    const auto& currentActiveShardSet = readerCtx.getCurrentlyTargetedDataShards();
    stdx::unordered_set<ShardId> shardsToCloseCursors;
    std::set_difference(currentActiveShardSet.begin(),
                        currentActiveShardSet.end(),
                        newActiveShardSet.begin(),
                        newActiveShardSet.end(),
                        std::inserter(shardsToCloseCursors, shardsToCloseCursors.begin()));

    if (!shardsToCloseCursors.empty()) {
        readerCtx.closeCursorsOnDataShards(shardsToCloseCursors);
    }

    stdx::unordered_set<ShardId> shardsToOpenCursors;
    std::set_difference(newActiveShardSet.begin(),
                        newActiveShardSet.end(),
                        currentActiveShardSet.begin(),
                        currentActiveShardSet.end(),
                        std::inserter(shardsToOpenCursors, shardsToOpenCursors.begin()));

    if (!shardsToOpenCursors.empty()) {
        readerCtx.openCursorsOnDataShards(atClusterTime, shardsToOpenCursors);
    }
}
}  // namespace

ShardTargeterDecision ChangeStreamShardTargeterDbPresentStateEventHandler::handleEvent(
    OperationContext* opCtx,
    const ControlEvent& event,
    ChangeStreamShardTargeterStateEventHandlingContext& ctx,
    ChangeStreamReaderContext& readerCtx) {
    return std::visit(
        OverloadedVisitor{
            [&](const MovePrimaryControlEvent& e) {
                return handlePlacementRefresh(opCtx, e.clusterTime, ctx, readerCtx);
            },
            [&](const NamespacePlacementChangedControlEvent& e) {
                return handlePlacementRefresh(opCtx, e.clusterTime, ctx, readerCtx);
            },
            [&](const MoveChunkControlEvent& e) {
                return handleMoveChunk(opCtx, e, ctx, readerCtx);
            },
            [&](const DatabaseCreatedControlEvent&) {
                tasserted(ErrorCodes::IllegalOperation,
                          "DatabaseCreatedControlEvent can not be processed in DbPresent state");
                return ShardTargeterDecision::kContinue;
            }},
        event);
}

ShardTargeterDecision
ChangeStreamShardTargeterDbPresentStateEventHandler::handleEventInDegradedMode(
    OperationContext* opCtx,
    const ControlEvent& event,
    ChangeStreamShardTargeterStateEventHandlingContext& ctx,
    ChangeStreamReaderContext& readerCtx) {
    MONGO_UNIMPLEMENTED_TASSERT(10917000);
}

ShardTargeterDecision ChangeStreamShardTargeterDbPresentStateEventHandler::handlePlacementRefresh(
    OperationContext* opCtx,
    Timestamp clusterTime,
    ChangeStreamShardTargeterStateEventHandlingContext& ctx,
    ChangeStreamReaderContext& readerCtx) {
    auto placement = ctx.historicalPlacementFetcher().fetch(
        opCtx, readerCtx.getChangeStream().getNamespace(), clusterTime);
    if (placement.getStatus() == HistoricalPlacementStatus::NotAvailable) {
        return ShardTargeterDecision::kSwitchToV1;
    }
    tassert(10917001,
            "HistoricalPlacementStatus can not be in the future",
            placement.getStatus() != HistoricalPlacementStatus::FutureClusterTime);

    const auto& shards = placement.getShards();
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
    updateActiveShardCursors(clusterTime + 1, shardSet, readerCtx);

    // In case the set of shards is empty, it means the underlying database is no longer present.
    // We open a cursor on the configsvr and change the event handler to the database absent state.
    if (shards.empty()) {
        tassert(10917002,
                "Opened cursors set must be empty",
                readerCtx.getCurrentlyTargetedDataShards().empty());
        readerCtx.openCursorOnConfigServer(clusterTime + 1);
        ctx.setEventHandler(buildDbAbsentStateEventHandler());
    }

    return ShardTargeterDecision::kContinue;
}

}  // namespace mongo
