// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler.h"
#include "mongo/util/modules.h"

namespace mongo {

class ChangeStreamShardTargeterDbAbsentStateEventHandler
    : public ChangeStreamShardTargeterStateEventHandler {
public:
    ChangeStreamShardTargeterStateEventHandler::DbPresenceState getDbPresenceState()
        const override {
        return ChangeStreamShardTargeterStateEventHandler::DbPresenceState::kDbAbsent;
    }

    /**
     * Handles DatabaseCreatedControlEvent by fetching the placement history, closing the cursor on
     * the configsvr and opening a cursor on the data shard. Tasserts for other control events, as
     * it violates a design invariant.
     */
    ShardTargeterDecision handleEvent(OperationContext* opCtx,
                                      const ControlEvent& event,
                                      ChangeStreamShardTargeterStateEventHandlingContext& ctx,
                                      ChangeStreamReaderContext& readerCtx) override;

    /**
     * Always tasserts. This method should not be called for this state, as it violates the design
     * invariant.
     */
    ShardTargeterDecision handleEventInDegradedMode(
        OperationContext* opCtx,
        const ControlEvent& event,
        ChangeStreamShardTargeterStateEventHandlingContext& ctx,
        ChangeStreamReaderContext& readerCtx) override;

    /**
     * Builds the corresponding ChangeStreamShardTargeterStateEventHandler for handling control
     * events in DbPresent state.
     */
    virtual std::unique_ptr<ChangeStreamShardTargeterStateEventHandler>
    buildDbPresentStateEventHandler() const = 0;
};

}  // namespace mongo
