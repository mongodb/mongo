// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/collection_change_stream_shard_targeter_impl.h"

#include "mongo/s/change_streams/collection_change_stream_db_absent_state_event_handler.h"
#include "mongo/s/change_streams/collection_change_stream_db_present_state_event_handler.h"

#include <memory>

namespace mongo {

std::unique_ptr<ChangeStreamShardTargeterStateEventHandler>
CollectionChangeStreamShardTargeterImpl::createDbAbsentHandler() const {
    return std::make_unique<CollectionChangeStreamShardTargeterDbAbsentStateEventHandler>();
}

std::unique_ptr<ChangeStreamShardTargeterStateEventHandler>
CollectionChangeStreamShardTargeterImpl::createDbPresentHandler() const {
    return std::make_unique<CollectionChangeStreamShardTargeterDbPresentStateEventHandler>();
}

}  // namespace mongo
