// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/database_change_stream_db_present_state_event_handler.h"

#include "mongo/s/change_streams/database_change_stream_db_absent_state_event_handler.h"

namespace mongo {

ShardTargeterDecision DatabaseChangeStreamShardTargeterDbPresentStateEventHandler::handleMoveChunk(
    OperationContext* opCtx,
    const MoveChunkControlEvent& event,
    ChangeStreamShardTargeterStateEventHandlingContext& ctx,
    ChangeStreamReaderContext& readerCtx) {
    return handlePlacementRefresh(opCtx, event.clusterTime, ctx, readerCtx);
}

std::unique_ptr<ChangeStreamShardTargeterStateEventHandler>
DatabaseChangeStreamShardTargeterDbPresentStateEventHandler::buildDbAbsentStateEventHandler()
    const {
    return std::make_unique<DatabaseChangeStreamShardTargeterDbAbsentStateEventHandler>();
}

std::string DatabaseChangeStreamShardTargeterDbPresentStateEventHandler::toString() const {
    return "DatabaseChangeStreamShardTargeterDbPresentStateEventHandler";
}

}  // namespace mongo
