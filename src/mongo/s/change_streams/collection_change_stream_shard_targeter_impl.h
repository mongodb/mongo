// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_reader_context.h"
#include "mongo/db/pipeline/change_stream_shard_targeter.h"
#include "mongo/db/pipeline/historical_placement_fetcher.h"
#include "mongo/s/change_streams/change_stream_shard_targeter_base.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {

class CollectionChangeStreamShardTargeterImpl : public ChangeStreamShardTargeterBase {
public:
    explicit CollectionChangeStreamShardTargeterImpl(
        std::unique_ptr<HistoricalPlacementFetcher> fetcher)
        : ChangeStreamShardTargeterBase(std::move(fetcher)) {}

    std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> createDbAbsentHandler()
        const override;

    std::unique_ptr<ChangeStreamShardTargeterStateEventHandler> createDbPresentHandler()
        const override;
};

}  // namespace mongo
