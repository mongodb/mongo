// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler.h"
#include "mongo/util/modules.h"

namespace mongo {

class ChangeStreamShardTargeterDbPresentStateEventHandler
    : public ChangeStreamShardTargeterStateEventHandler {
public:
    ChangeStreamShardTargeterStateEventHandler::DbPresenceState getDbPresenceState()
        const override {
        return ChangeStreamShardTargeterStateEventHandler::DbPresenceState::kDbPresent;
    }

    ShardTargeterDecision handleEvent(OperationContext* opCtx,
                                      const ControlEvent& event,
                                      ChangeStreamShardTargeterStateEventHandlingContext& ctx,
                                      ChangeStreamReaderContext& readerCtx) override;

    ShardTargeterDecision handleEventInDegradedMode(
        OperationContext* opCtx,
        const ControlEvent& event,
        ChangeStreamShardTargeterStateEventHandlingContext& ctx,
        ChangeStreamReaderContext& readerCtx) override;

protected:
    virtual ShardTargeterDecision handleMoveChunk(
        OperationContext* opCtx,
        const MoveChunkControlEvent& e,
        ChangeStreamShardTargeterStateEventHandlingContext& ctx,
        ChangeStreamReaderContext& readerCtx) = 0;

    /**
     * Returns new event handler instance representing the state when the database is absent.
     */
    virtual std::unique_ptr<ChangeStreamShardTargeterStateEventHandler>
    buildDbAbsentStateEventHandler() const = 0;

    /**
     * Fetches HistoricalPlacement from the configsvr and updates the set of active cursors on the
     * data shards. In case there are no active shards, opens the cursor on the configsvr and sets
     * the event handler for DbAbsent state.
     */
    ShardTargeterDecision handlePlacementRefresh(
        OperationContext* opCtx,
        Timestamp clusterTime,
        ChangeStreamShardTargeterStateEventHandlingContext& ctx,
        ChangeStreamReaderContext& readerCtx);
};

}  // namespace mongo
