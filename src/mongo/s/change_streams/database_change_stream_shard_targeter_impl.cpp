// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/database_change_stream_shard_targeter_impl.h"

#include "mongo/s/change_streams/database_change_stream_db_absent_state_event_handler.h"
#include "mongo/s/change_streams/database_change_stream_db_present_state_event_handler.h"

namespace mongo {

std::unique_ptr<ChangeStreamShardTargeterStateEventHandler>
DatabaseChangeStreamShardTargeterImpl::createDbAbsentHandler() const {
    return std::make_unique<DatabaseChangeStreamShardTargeterDbAbsentStateEventHandler>();
}

std::unique_ptr<ChangeStreamShardTargeterStateEventHandler>
DatabaseChangeStreamShardTargeterImpl::createDbPresentHandler() const {
    return std::make_unique<DatabaseChangeStreamShardTargeterDbPresentStateEventHandler>();
}

}  // namespace mongo
