// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/change_stream_shard_targeter.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

#include <memory>
#include <set>
#include <string>

namespace mongo {

class [[MONGO_MOD_OPEN]] ChangeStreamReaderBuilder {
public:
    virtual ~ChangeStreamReaderBuilder() = default;

    /**
     * Builds a shard targeter that makes decisions about what shards to open the change stream
     * cursors on, what shards to close the cursors on when responding to control events or when
     * opening the change stream.
     */
    virtual std::unique_ptr<ChangeStreamShardTargeter> buildShardTargeter(
        OperationContext* opCtx, const ChangeStream& changeStream) = 0;

    /**
     * Builds a match expression that selects oplog entries on a data shard that correspond to
     * control events needed for proper shard targeting. The expression uses Match Expression syntax
     * and semantics.
     * The returned BSONObj instance is expected to be owned.
     */
    virtual BSONObj buildControlEventFilterForDataShard(OperationContext* opCtx,
                                                        const ChangeStream& changeStream) = 0;

    /**
     * Returns a set of names of control events on data shards used by 'ChangeStreamShardTargeter'.
     * These and only these control events will be delivered to the 'ChangeStreamShardTargeter'.
     */
    virtual std::set<std::string> getControlEventTypesOnDataShard(
        OperationContext* opCtx, const ChangeStream& changeStream) = 0;

    /**
     * Builds a match expression that selects oplog entries on the config server that correspond to
     * control events needed for proper shard targeting. The expression uses Match Expression syntax
     * and semantics.
     * The returned BSONObj instance is expected to be owned.
     */
    virtual BSONObj buildControlEventFilterForConfigServer(OperationContext* opCtx,
                                                           const ChangeStream& changeStream) = 0;

    /**
     * Returns a set of names of control events on the config server used by
     * 'ChangeStreamShardTargeter'. Otherwise the contract is the same as for
     * 'getControlEventTypesOnDataShard()'.
     */
    virtual std::set<std::string> getControlEventTypesOnConfigServer(
        OperationContext* opCtx, const ChangeStream& changeStream) = 0;

    /**
     * Service context bindings.
     */
    static ChangeStreamReaderBuilder* get(ServiceContext* serviceContext);
    static void set(ServiceContext* serviceContext,
                    std::unique_ptr<ChangeStreamReaderBuilder> builder);

private:
    /**
     * Swaps the current 'ChangeStreamReaderBuilder' with the 'replacement', returning the original
     * builder.
     */
    static std::unique_ptr<ChangeStreamReaderBuilder> swap_forTest(
        ServiceContext* serviceContext, std::unique_ptr<ChangeStreamReaderBuilder> replacement);

    friend struct ScopedChangeStreamReaderBuilderMock;
};

}  // namespace mongo
