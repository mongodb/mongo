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
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"

#include <variant>

namespace mongo {

ShardTargeterDecision ChangeStreamShardTargeterDbAbsentStateEventHandler::handleEvent(
    OperationContext* opCtx,
    const ControlEvent& event,
    ChangeStreamShardTargeterStateEventHandlingContext& ctx,
    ChangeStreamReaderContext& readerCtx) {
    tassert(ErrorCodes::IllegalOperation,
            "Only DatabaseCreatedControlEvent can be processed in DbAbsent state.",
            std::holds_alternative<DatabaseCreatedControlEvent>(event));
    Timestamp clusterTime = std::get<DatabaseCreatedControlEvent>(event).clusterTime;

    auto placement = ctx.getHistoricalPlacementFetcher().fetch(
        opCtx, readerCtx.getChangeStream().getNamespace(), clusterTime);
    if (placement.getStatus() == HistoricalPlacementStatus::NotAvailable) {
        return ShardTargeterDecision::kSwitchToV1;
    }
    tassert(10915200,
            "HistoricalPlacementStatus can not be in the future",
            placement.getStatus() != HistoricalPlacementStatus::FutureClusterTime);

    const auto& shards = placement.getShards();
    tassert(10915201,
            "HistoricalPlacementStatus must have at least one shard after DatabaseCreated "
            "control event",
            !shards.empty());

    // Close the cursor on the configsvr and open the cursor(s) on the data shards.
    readerCtx.closeCursorOnConfigServer();
    stdx::unordered_set<ShardId> shardSet(shards.begin(), shards.end());
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
    tasserted(ErrorCodes::IllegalOperation,
              str::stream() << "change stream over collection "
                            << readerCtx.getChangeStream().getNamespace()->toStringForErrorMsg()
                            << " can not be in degraded mode when database is absent");
}

}  // namespace mongo
