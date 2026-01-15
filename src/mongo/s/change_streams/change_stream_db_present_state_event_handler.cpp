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

#include "mongo/db/pipeline/change_stream.h"
#include "mongo/logv2/log.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/s/change_streams/shard_targeter_helper.h"
#include "mongo/util/assert_util.h"

#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

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
                tasserted(11600503,
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
    tassert(
        10922909,
        "Change stream reader must be in degraded mode when calling 'handleEventInDegradedMode()'",
        readerCtx.inDegradedMode());

    tassert(11600501,
            "DatabaseCreatedControlEvent can not be processed in DbPresent state",
            !std::holds_alternative<DatabaseCreatedControlEvent>(event));

    // In degraded mode, requests to open and/or close cursors cannot be made - the set of tracked
    // shards for a bounded change stream segment is fixed.
    // We also cannot assume that a specific event type is fed in here, as we may have missed
    // certain events on removed shards in ignoreRemovedShards mode.
    // There is also no need to set the event handler here, because the only relevant state that can
    // be reached after degraded fetching is to start a new change stream segment, which will always
    // install a new event handler.
    return ShardTargeterDecision::kContinue;
}

ShardTargeterDecision ChangeStreamShardTargeterDbPresentStateEventHandler::handlePlacementRefresh(
    OperationContext* opCtx,
    Timestamp clusterTime,
    ChangeStreamShardTargeterStateEventHandlingContext& ctx,
    ChangeStreamReaderContext& readerCtx) {
    // The placement history query here uses ignoreRemovedShards=false. This should work because the
    // set of cursors will ultimately be opened by the 'ChangeStreamHandleTopologyChangeV2' stage in
    // normal fetching mode state, and this state will catch any 'ShardRemoved' exceptions. In case
    // a cursor is opened on a shard that has been removed, the v2 stage will start a new change
    // stream segment.
    // TODO SERVER-114863 SERVER-115212: verify that this is sensible.
    auto placement =
        ctx.getHistoricalPlacementFetcher().fetch(opCtx,
                                                  readerCtx.getChangeStream().getNamespace(),
                                                  clusterTime,
                                                  false /* checkIfPointInTimeIsInFuture */,
                                                  false /* ignoreRemovedShards */);
    if (placement.getStatus() == HistoricalPlacementStatus::NotAvailable) {
        return ShardTargeterDecision::kSwitchToV1;
    }

    const auto& shards = placement.getShards();

    LOGV2_DEBUG(10922912,
                3,
                "Handling placement refresh",
                "atClusterTime"_attr = clusterTime,
                "anyRemovedShardDetected"_attr =
                    placement.getAnyRemovedShardDetected().value_or(false),
                "openCursorAt"_attr = placement.getOpenCursorAt(),
                "nextPlacementChangedAt"_attr = placement.getNextPlacementChangedAt(),
                "currentActiveShards"_attr = readerCtx.getCurrentlyTargetedDataShards(),
                "shards"_attr = shards);

    // Validate status and other fields of historical placement result.
    change_streams::assertHistoricalPlacementStatusOK(placement);

    // TODO SERVER-114863 SERVER-115212: adjust the invariants checked here if it turns out to be
    // necessary when running more exhaustive tests.
    change_streams::assertHistoricalPlacementHasNoSegment(placement);

    change_streams::updateActiveShardCursors(clusterTime + 1, shards, readerCtx);

    // In case the set of shards is empty, it means the underlying database is no longer present.
    // We open a cursor on the configsvr and change the event handler to the database absent state.
    if (shards.empty()) {
        readerCtx.openCursorOnConfigServer(clusterTime + 1);
        ctx.setEventHandler(buildDbAbsentStateEventHandler());
    }

    return ShardTargeterDecision::kContinue;
}

}  // namespace mongo
