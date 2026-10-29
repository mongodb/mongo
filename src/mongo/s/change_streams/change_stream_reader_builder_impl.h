// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/change_stream_reader_builder.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] ChangeStreamReaderBuilderImpl
    : public ChangeStreamReaderBuilder {
public:
    /**
     * Builds CollectionChangeStreamShardTargeterImpl, DatabaseChangeStreamShardTargeterImpl or
     * AllDatabasesChangeStreamShardTargeterImpl depending on the type of the 'changeStream'.
     */
    std::unique_ptr<ChangeStreamShardTargeter> buildShardTargeter(
        OperationContext* opCtx, const ChangeStream& changeStream) override;

    /**
     * Builds a match expression that selects oplog entries on a data shard that correspond to the
     * following events:
     * - moveChunk
     * - movePrimary
     * - namespacePlacementChanged
     */
    BSONObj buildControlEventFilterForDataShard(OperationContext* opCtx,
                                                const ChangeStream& changeStream) override;

    /**
     * Returns control event types that correspond to the following events:
     * - moveChunk
     * - movePrimary
     * - namespacePlacementChanged
     */
    std::set<std::string> getControlEventTypesOnDataShard(
        OperationContext* opCtx, const ChangeStream& changeStream) override;

    /**
     * Builds a match expression that selects oplog entries on the config server that correspond to
     * dabatase creation.
     */
    BSONObj buildControlEventFilterForConfigServer(OperationContext* opCtx,
                                                   const ChangeStream& changeStream) override;

    /**
     * Returns control event type that corresponds to DatabaseCreated event.
     */
    std::set<std::string> getControlEventTypesOnConfigServer(
        OperationContext* opCtx, const ChangeStream& changeStream) override;
};

}  // namespace mongo
