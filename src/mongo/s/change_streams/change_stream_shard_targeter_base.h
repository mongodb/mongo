/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
