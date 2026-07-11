// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/change_stream_db_absent_state_event_handler.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_ref.h"
#include "mongo/logv2/log.h"
#include "mongo/s/change_streams/shard_targeter_helper.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"

#include <variant>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#define STAGE_LOG_PREFIX "ChangeStreamShardTargeterDbAbsentStateEventHandler: "

namespace mongo {

ShardTargeterDecision ChangeStreamShardTargeterDbAbsentStateEventHandler::handleEvent(
    OperationContext* opCtx,
    const ControlEvent& event,
    ChangeStreamShardTargeterStateEventHandlingContext& ctx,
    ChangeStreamReaderContext& readerCtx) {
    // For NamespacePlacementChanged events with empty namespace (FCV downgrade),
    // check if placement history is still available. If not, switch to v1.
    if (std::holds_alternative<NamespacePlacementChangedControlEvent>(event)) {
        const auto& placementChangedEvent = std::get<NamespacePlacementChangedControlEvent>(event);
        tassert(12321702,
                "Only cluster-level (empty namespace) NamespacePlacementChanged events are "
                "expected in DbAbsent state",
                placementChangedEvent.nss.isEmpty());
        auto allocationToShardsStatus =
            change_streams::getDataToShardsAllocationQueryService(opCtx)
                ->getAllocationToShardsStatus(opCtx, placementChangedEvent.clusterTime);
        if (allocationToShardsStatus == AllocationToShardsStatus::kNotAvailable) {
            LOGV2_DEBUG(12321703,
                        3,
                        STAGE_LOG_PREFIX "Placement history not available, switching to v1",
                        "atClusterTime"_attr = placementChangedEvent.clusterTime,
                        "namespace"_attr = readerCtx.getChangeStream().getNamespace());
            return ShardTargeterDecision::kSwitchToV1;
        }
        return ShardTargeterDecision::kContinue;
    }

    tassert(11600502,
            "Only DatabaseCreatedControlEvent or NamespacePlacementChangedControlEvent can be "
            "processed in DbAbsent state.",
            std::holds_alternative<DatabaseCreatedControlEvent>(event));
    Timestamp clusterTime = std::get<DatabaseCreatedControlEvent>(event).clusterTime;

    auto placement =
        ctx.getHistoricalPlacementFetcher().fetch(opCtx,
                                                  readerCtx.getChangeStream().getNamespace(),
                                                  clusterTime,
                                                  false /* checkIfPointInTimeIsInFuture */,
                                                  false /* ignoreRemovedShards */);
    if (placement.getStatus() == HistoricalPlacementStatus::NotAvailable) {
        return ShardTargeterDecision::kSwitchToV1;
    }

    const auto& shardRefs = placement.getShards();
    // TODO(SERVER-127411): once change-stream routing is UUID-aware via ShardHandle, remove this
    // conversion and route directly by ShardRef.
    std::vector<ShardId> shards(shardRefs.begin(), shardRefs.end());

    LOGV2_DEBUG(12013809,
                3,
                STAGE_LOG_PREFIX "Handling event",
                "changeStream"_attr = readerCtx.getChangeStream(),
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

    tassert(10915201,
            "HistoricalPlacementStatus must have at least one shard after DatabaseCreated "
            "control event",
            !shards.empty());

    // Close the cursor on the configsvr and open the cursor(s) on the data shards.
    readerCtx.closeCursorOnConfigServer();
    stdx::unordered_set<ShardId> shardSet(std::make_move_iterator(shards.begin()),
                                          std::make_move_iterator(shards.end()));
    readerCtx.openCursorsOnDataShards(clusterTime + 1, shardSet);

    // Since the database is now present, change the state event handler.
    ctx.setEventHandler(buildDbPresentStateEventHandler());

    return ShardTargeterDecision::kContinue;
}

ShardTargeterDecision ChangeStreamShardTargeterDbAbsentStateEventHandler::handleEventInDegradedMode(
    OperationContext* opCtx,
    const ControlEvent& event,
    ChangeStreamShardTargeterStateEventHandlingContext& ctx,
    ChangeStreamReaderContext& readerCtx) {
    tasserted(10922908, "Cannot call 'handleEventInDegradedMode()' with DbAbsentStateEventHandler");
}

}  // namespace mongo
