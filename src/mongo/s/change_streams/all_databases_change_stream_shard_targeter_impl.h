// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/pipeline/change_stream_shard_targeter.h"
#include "mongo/db/pipeline/historical_placement_fetcher.h"
#include "mongo/s/change_streams/all_databases_change_stream_state_event_handler.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_state_event_handler.h"
#include "mongo/util/modules.h"

#include <utility>

#include <boost/optional.hpp>

namespace mongo {

class AllDatabasesChangeStreamShardTargeterImpl
    : public ChangeStreamShardTargeter,
      public ChangeStreamShardTargeterStateEventHandlingContext {
public:
    explicit AllDatabasesChangeStreamShardTargeterImpl(
        std::unique_ptr<HistoricalPlacementFetcher> fetcher)
        : _fetcher(std::move(fetcher)) {
        setEventHandler(std::make_unique<AllDatabasesShardTargeterStateEventHandler>());
    }

    ShardTargeterDecision initialize(OperationContext* opCtx,
                                     Timestamp atClusterTime,
                                     ChangeStreamReaderContext& context) override;

    std::pair<ShardTargeterDecision, boost::optional<Timestamp>> startChangeStreamSegment(
        OperationContext* opCtx,
        Timestamp atClusterTime,
        ChangeStreamReaderContext& context) override;

    ShardTargeterDecision handleEvent(OperationContext* opCtx,
                                      const Document& event,
                                      ChangeStreamReaderContext& context) override;

    HistoricalPlacementFetcher& getHistoricalPlacementFetcher() const override;

    void setEventHandler(
        std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> eventHandler) override;

private:
    /**
     * Historical placement fetcher instance used to query the placement history information for the
     * underlying collection/database. Set by the constructor and always present.
     */
    std::unique_ptr<HistoricalPlacementFetcher> _fetcher;

    /**
     * The current shard targeter state event handler used by the shard targeter.
     */
    std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> _eventHandler;
};

}  // namespace mongo
