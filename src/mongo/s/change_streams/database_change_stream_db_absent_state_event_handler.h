// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/s/change_streams/change_stream_db_absent_state_event_handler.h"

#include <memory>

namespace mongo {

class DatabaseChangeStreamShardTargeterDbAbsentStateEventHandler
    : public ChangeStreamShardTargeterDbAbsentStateEventHandler {
public:
    /**
     * Builds DatabaseChangeStreamShardTargeterDbPresentStateEventHandler.
     */
    std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> buildDbPresentStateEventHandler()
        const override;

    std::string toString() const override;
};

}  // namespace mongo
