// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/collection_change_stream_db_present_state_event_handler.h"

#include "mongo/logv2/log.h"
#include "mongo/s/change_streams/collection_change_stream_db_absent_state_event_handler.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

ShardTargeterDecision
CollectionChangeStreamShardTargeterDbPresentStateEventHandler::handleMoveChunk(
    OperationContext* opCtx,
    const MoveChunkControlEvent& event,
    ChangeStreamShardTargeterStateEventHandlingContext& ctx,
    ChangeStreamReaderContext& readerCtx) {
    // Open a cursor on the data shard 'event.toShard' if not already opened.
    const auto& currentlyOpenedShards = readerCtx.getCurrentlyTargetedDataShards();
    const bool newCursorOnRecipientNeeded = currentlyOpenedShards.count(event.toShard) == 0;
    if (newCursorOnRecipientNeeded) {
        readerCtx.openCursorsOnDataShards(event.clusterTime + 1, {event.toShard});
    }

    // Close cursor on the data shard 'event.fromShard' if all chunks have been migrated from donor.
    const bool cursorClosedOnDonor = event.allCollectionChunksMigratedFromDonor;
    if (cursorClosedOnDonor) {
        tassert(10917003,
                "cursor should have been opened",
                currentlyOpenedShards.count(event.fromShard));
        readerCtx.closeCursorsOnDataShards({event.fromShard});
    }

    LOGV2_DEBUG(10917004,
                3,
                "CollectionChangeStreamShardTargeterDbPresentStateEventHandler: "
                "Handling moveChunk",
                "clusterTime"_attr = event.clusterTime,
                "fromShard"_attr = event.fromShard,
                "toShard"_attr = event.toShard,
                "cursorOpenedOnRecipient"_attr = newCursorOnRecipientNeeded,
                "cursorClosedOnDonor"_attr = cursorClosedOnDonor,
                "currentActiveShards"_attr = readerCtx.getCurrentlyTargetedDataShards());

    return ShardTargeterDecision::kContinue;
}

std::unique_ptr<ChangeStreamShardTargeterStateEventHandler>
CollectionChangeStreamShardTargeterDbPresentStateEventHandler::buildDbAbsentStateEventHandler()
    const {
    return std::make_unique<CollectionChangeStreamShardTargeterDbAbsentStateEventHandler>();
}

std::string CollectionChangeStreamShardTargeterDbPresentStateEventHandler::toString() const {
    return "CollectionChangeStreamShardTargeterDbPresentStateEventHandler";
}

}  // namespace mongo
