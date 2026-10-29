// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/s/change_streams/change_stream_db_present_state_event_handler.h"
#include "mongo/util/modules.h"

namespace mongo {
class CollectionChangeStreamShardTargeterDbPresentStateEventHandler
    : public ChangeStreamShardTargeterDbPresentStateEventHandler {
protected:
    /**
     * Opens a cursor on the destination shard if needed and closes the cursor on the donor shard
     * if all its chunks have been migrated away.
     */
    ShardTargeterDecision handleMoveChunk(OperationContext* opCtx,
                                          const MoveChunkControlEvent& event,
                                          ChangeStreamShardTargeterStateEventHandlingContext& ctx,
                                          ChangeStreamReaderContext& readerCtx) override;

    /**
     * Returns CollectionChangeStreamShardTargeterDbAbsentStateEventHandler.
     */
    std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> buildDbAbsentStateEventHandler()
        const override;

    std::string toString() const override;
};

}  // namespace mongo
