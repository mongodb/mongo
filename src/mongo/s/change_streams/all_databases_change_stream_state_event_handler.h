// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler.h"
#include "mongo/util/modules.h"

namespace mongo {

class AllDatabasesShardTargeterStateEventHandler
    : public ChangeStreamShardTargeterStateEventHandler {
public:
    ChangeStreamShardTargeterStateEventHandler::DbPresenceState getDbPresenceState()
        const override {
        return ChangeStreamShardTargeterStateEventHandler::DbPresenceState::kUnknown;
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
    std::string toString() const override;

private:
    ShardTargeterDecision handlePlacementRefresh(
        OperationContext* opCtx,
        const ControlEvent& event,
        Timestamp clusterTime,
        ChangeStreamShardTargeterStateEventHandlingContext& ctx,
        ChangeStreamReaderContext& readerCtx);
};

}  // namespace mongo
