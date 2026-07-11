// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/change_stream_db_present_state_event_handler.h"

#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/logv2/log.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/s/change_streams/shard_targeter_helper.h"
#include "mongo/util/assert_util.h"

#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#define STAGE_LOG_PREFIX "ChangeStreamShardTargeterDbPresentStateEventHandler: "

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

    // For 'NamespacePlacementChanged' events, check if the placement history can provide
    // information for the oplog time embedded in the event. If this is not the case, this was
    // likely due to an FCV downgrade, which clears the placement history information. In this case,
    // the change stream reader needs to downgrade to v1.
    if (std::holds_alternative<NamespacePlacementChangedControlEvent>(event)) {
        const NamespacePlacementChangedControlEvent& placementChangedEvent =
            std::get<NamespacePlacementChangedControlEvent>(event);
        if (placementChangedEvent.nss.isEmpty() &&
            change_streams::getDataToShardsAllocationQueryService(opCtx)
                    ->getAllocationToShardsStatus(opCtx, placementChangedEvent.clusterTime) ==
                AllocationToShardsStatus::kNotAvailable) {
            // No shard placement information is available. Use v1 change stream reader.
            return ShardTargeterDecision::kSwitchToV1;
        }
    }

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
    auto placement =
        ctx.getHistoricalPlacementFetcher().fetch(opCtx,
                                                  readerCtx.getChangeStream().getNamespace(),
                                                  clusterTime,
                                                  false /* checkIfPointInTimeIsInFuture */,
                                                  false /* ignoreRemovedShards */);
    if (placement.getStatus() == HistoricalPlacementStatus::NotAvailable) {
        LOGV2_DEBUG(12321700,
                    3,
                    STAGE_LOG_PREFIX "Placement history not available, switching to v1",
                    "atClusterTime"_attr = clusterTime,
                    "namespace"_attr = readerCtx.getChangeStream().getNamespace());
        return ShardTargeterDecision::kSwitchToV1;
    }

    const auto& shardRefs = placement.getShards();
    // TODO(SERVER-127411): once change-stream routing is UUID-aware via ShardHandle, remove this
    // conversion and route directly by ShardRef.
    const std::vector<ShardId> shards(shardRefs.begin(), shardRefs.end());

    LOGV2_DEBUG(10922912,
                3,
                STAGE_LOG_PREFIX "Handling placement refresh",
                "atClusterTime"_attr = clusterTime,
                "anyRemovedShardDetected"_attr =
                    placement.getAnyRemovedShardDetected().value_or(false),
                "openCursorAt"_attr = placement.getOpenCursorAt(),
                "nextPlacementChangedAt"_attr = placement.getNextPlacementChangedAt(),
                "isCursorOnConfigServerOpen"_attr = readerCtx.isCursorOnConfigServerOpen(),
                "currentActiveShards"_attr = readerCtx.getCurrentlyTargetedDataShards(),
                "shards"_attr = shards);

    // Validate status and other fields of historical placement result.
    change_streams::assertHistoricalPlacementStatusOK(placement);
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
