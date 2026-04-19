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

#include "mongo/s/change_streams/change_stream_db_absent_state_event_handler.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"
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

    const auto& shards = placement.getShards();

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
