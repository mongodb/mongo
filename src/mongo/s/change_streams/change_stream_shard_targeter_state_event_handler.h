/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/pipeline/change_stream_shard_targeter.h"
#include "mongo/db/pipeline/historical_placement_fetcher.h"
#include "mongo/s/change_streams/control_events.h"

namespace mongo {

class ChangeStreamShardTargeterStateEventHandler;

/**
 * Provides a context for event handling operations within the ShardTargeter subsystem.
 *
 * This interface corresponds to the "context" participant in the state design pattern for
 * ChangeStreamShardTargeter event handling. The context is passed to ShardTargeterStateEventHandler
 * methods, giving handlers access to resources such as the HistoricalPlacementFetcher, as well as
 * the ability to trigger a state transition via setEventHandler().
 */
class ChangeStreamShardTargeterStateEventHandlingContext {
public:
    virtual ~ChangeStreamShardTargeterStateEventHandlingContext() = default;

    /**
     * Returns a reference to the HistoricalPlacementFetcher associated with this context.
     */
    virtual HistoricalPlacementFetcher& getHistoricalPlacementFetcher() const = 0;

    /**
     * Sets a new event handler to respond to ChangeStreamShardTargeter control events.
     */
    virtual void setEventHandler(
        std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> eventHandler) = 0;
};

/**
 * Interface for handling ChangeStreamShardTargeter control events.
 *
 * As the "state" participant in the state pattern, this event handler encapsulates
 * the logic for processing control events in different operational modes of
 * ChangeStreamShardTargeter. ChangeStreamShardTargeter will set the appropriate
 * event handler corresponding to its current state (e.g., when the underlying
 * collection or database is present vs. absent).
 */
class ChangeStreamShardTargeterStateEventHandler {
public:
    virtual ~ChangeStreamShardTargeterStateEventHandler() = default;

    /**
     * Handles a ChangeStreamShardTargeter control events when the system is in normal mode.
     */
    virtual ShardTargeterDecision handleEvent(
        OperationContext* opCtx,
        const ControlEvent& event,
        ChangeStreamShardTargeterStateEventHandlingContext& context,
        ChangeStreamReaderContext& readerContext) = 0;

    /**
     * Handles a ChangeStreamShardTargeter control events when the system is in degraded mode.
     */
    virtual ShardTargeterDecision handleEventInDegradedMode(
        OperationContext* opCtx,
        const ControlEvent& event,
        ChangeStreamShardTargeterStateEventHandlingContext& context,
        ChangeStreamReaderContext& readerContext) = 0;
};
}  // namespace mongo
