// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/collection_change_stream_db_absent_state_event_handler.h"

#include "mongo/s/change_streams/collection_change_stream_db_present_state_event_handler.h"

#include <memory>

namespace mongo {
std::unique_ptr<ChangeStreamShardTargeterStateEventHandler>
CollectionChangeStreamShardTargeterDbAbsentStateEventHandler::buildDbPresentStateEventHandler()
    const {
    return std::make_unique<CollectionChangeStreamShardTargeterDbPresentStateEventHandler>();
}

std::string CollectionChangeStreamShardTargeterDbAbsentStateEventHandler::toString() const {
    return "CollectionChangeStreamShardTargeterDbAbsentStateEventHandler";
}
}  // namespace mongo
