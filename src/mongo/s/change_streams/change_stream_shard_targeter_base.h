// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/pipeline/change_stream_shard_targeter.h"
#include "mongo/db/pipeline/historical_placement_fetcher.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler.h"
#include "mongo/util/modules.h"

#include <memory>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Abstract base class for change stream shard targeters, providing common functionality to manage
 * the targeter's historical placement fetcher and state event handlers.
 */
class ChangeStreamShardTargeterBase : public ChangeStreamShardTargeter,
                                      public ChangeStreamShardTargeterStateEventHandlingContext {
public:
    explicit ChangeStreamShardTargeterBase(std::unique_ptr<HistoricalPlacementFetcher> fetcher)
        : _fetcher(std::move(fetcher)) {}

    HistoricalPlacementFetcher& getHistoricalPlacementFetcher() const override;

    void setEventHandler(
        std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> eventHandler) override;

    ChangeStreamShardTargeterStateEventHandler& getEventHandler_forTest() const;

    /**
     * Common implementation of shard targeter initialization functionality, shared by subclasses.
     */
    ShardTargeterDecision initialize(OperationContext* opCtx,
                                     Timestamp atClusterTime,
                                     ChangeStreamReaderContext& readerContext) override;

    /**
     * Common implementation of functionality to start a new change stream segment, shared by
     * subclasses.
     */
    std::pair<ShardTargeterDecision, boost::optional<Timestamp>> startChangeStreamSegment(
        OperationContext* opCtx,
        Timestamp atClusterTime,
        ChangeStreamReaderContext& readerContext) override;

    /**
     * Common implementation of functionality to handle a control event, shared by subclasses.
     */
    ShardTargeterDecision handleEvent(OperationContext* opCtx,
                                      const Document& event,
                                      ChangeStreamReaderContext& readerContext) override;

protected:
    /**
     * Method to create a new 'ChangeStreamShardTargeterStateEventHandler' instance for the state in
     * which the database is absent.
     */
    virtual std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> createDbAbsentHandler()
        const = 0;

    /**
     * Method to create a new 'ChangeStreamShardTargeterStateEventHandler' instance for the state in
     * which the database is present.
     */
    virtual std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> createDbPresentHandler()
        const = 0;

private:
    /**
     * Creates / overwrites the '_eventHandler' instance, based on the amount of shards that are
     * targeted. For an empty shard set, a DbAbsent handler will be created. Otherwise, a DbPresent
     * handler will be created.
     */
    void createStateEventHandler(const std::vector<ShardId>& shards);

    /**
     * The current shard targeter state event handler used by the shard targeter. Created
     * dynamically when calling 'initialize()' and 'startChangeStreamSegment()', which in turn call
     * 'createStateEventHandler()' with the correct event handler type (absent/present).
     */

    std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> _eventHandler;

    /**
     * Historical placement fetcher instance used to query the placement history information for the
     * underlying collection/database. Set by the constructor and always present.
     */
    std::unique_ptr<HistoricalPlacementFetcher> _fetcher;
};

}  // namespace mongo
