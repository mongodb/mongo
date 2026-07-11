// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/all_databases_change_stream_state_event_handler.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/logv2/log.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/s/change_streams/shard_targeter_helper.h"
#include "mongo/util/assert_util.h"

#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

ShardTargeterDecision AllDatabasesShardTargeterStateEventHandler::handleEvent(
    OperationContext* opCtx,
    const ControlEvent& event,
    ChangeStreamShardTargeterStateEventHandlingContext& ctx,
    ChangeStreamReaderContext& readerCtx) {

    return std::visit(
        OverloadedVisitor{
            [&](const MovePrimaryControlEvent& e) {
                return handlePlacementRefresh(opCtx, e, e.clusterTime, ctx, readerCtx);
            },
            [&](const NamespacePlacementChangedControlEvent& e) {
                return handlePlacementRefresh(opCtx, e, e.clusterTime, ctx, readerCtx);
            },
            [&](const MoveChunkControlEvent& e) {
                return handlePlacementRefresh(opCtx, e, e.clusterTime, ctx, readerCtx);
            },
            [&](const DatabaseCreatedControlEvent& e) {
                return handlePlacementRefresh(opCtx, e, e.clusterTime, ctx, readerCtx);
            }},
        event);
}

ShardTargeterDecision AllDatabasesShardTargeterStateEventHandler::handlePlacementRefresh(
    OperationContext* opCtx,
    const ControlEvent& event,
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
        LOGV2_DEBUG(12321704,
                    3,
                    "Placement history not available, switching to v1",
                    "atClusterTime"_attr = clusterTime);
        return ShardTargeterDecision::kSwitchToV1;
    }

    const auto& shardRefs = placement.getShards();
    // TODO(SERVER-127411): once change-stream routing is UUID-aware via ShardHandle, remove this
    // conversion and route directly by ShardRef.
    const std::vector<ShardId> shards(shardRefs.begin(), shardRefs.end());

    LOGV2_DEBUG(11138117,
                3,
                "Handling placement refresh",
                "atClusterTime"_attr = clusterTime,
                "currentActiveShards"_attr = readerCtx.getCurrentlyTargetedDataShards(),
                "shards"_attr = shards);

    // Validate status and other fields of historical placement result.
    change_streams::assertHistoricalPlacementStatusOK(placement);
    change_streams::assertHistoricalPlacementHasNoSegment(placement);

    bool isDatabaseCreatedEvent = std::holds_alternative<DatabaseCreatedControlEvent>(event);
    tassert(11138118,
            "HistoricalPlacementStatus must have at least one shard after DatabaseCreated "
            "control event",
            !isDatabaseCreatedEvent || !shards.empty());

    change_streams::updateActiveShardCursors(clusterTime + 1, shards, readerCtx);

    return ShardTargeterDecision::kContinue;
}

ShardTargeterDecision AllDatabasesShardTargeterStateEventHandler::handleEventInDegradedMode(
    OperationContext* opCtx,
    const ControlEvent& event,
    ChangeStreamShardTargeterStateEventHandlingContext& ctx,
    ChangeStreamReaderContext& readerCtx) {
    tassert(
        11138123,
        "Change stream reader must be in degraded mode when calling 'handleEventInDegradedMode()'",
        readerCtx.inDegradedMode());

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

std::string AllDatabasesShardTargeterStateEventHandler::toString() const {
    return "AllDatabasesShardTargeterStateEventHandler";
}
}  // namespace mongo
