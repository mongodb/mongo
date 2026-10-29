// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/pipeline/change_stream_shard_targeter.h"
#include "mongo/db/pipeline/historical_placement_fetcher.h"
#include "mongo/s/change_streams/control_events.h"
#include "mongo/util/modules.h"

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
    enum class DbPresenceState {
        // Unknown state.
        kUnknown,

        // Database is absent.
        kDbAbsent,

        // Database is present.
        kDbPresent,
    };

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

    /**
     * Returns the name of the event handler.
     */
    virtual std::string toString() const = 0;


    /**
     * Returns the db presence state (absent/present/unknown) for this event handler.
     */
    virtual DbPresenceState getDbPresenceState() const = 0;
};
}  // namespace mongo
