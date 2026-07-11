// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * Provides resources for opening/closing of cursors on data shards and the config server, and
 * inquiring the current state of the change stream.
 */
class [[MONGO_MOD_PUBLIC]] ChangeStreamReaderContext {
public:
    virtual ~ChangeStreamReaderContext() = default;

    /**
     * Triggers opening of change stream cursors on a set of data shards for the change stream
     * associated with this context.
     *
     * @param atClusterTime minimum cluster time of the first change event from any of the shards to
     *                      be tracked.
     * @param shardSet set of shard identifiers to open the change stream cursors on.
     *
     * Preconditions:
     * - atClusterTime > cluster time of the control event that is being processed.
     *
     * Postconditions:
     * - The cursors are open and the results from them are merged into the change stream before
     *   retrieving the next change event by the change stream reader.
     */
    virtual void openCursorsOnDataShards(Timestamp atClusterTime,
                                         const stdx::unordered_set<ShardId>& shardSet) = 0;

    /**
     * Triggers closing of change stream cursors on a set of data shards for the change stream
     * associated with this context.
     *
     * @param shardSet set of data shard identifiers to close the change stream cursors on.
     *
     * Preconditions:
     * - shardSet is a subset of or equal to the set of data shards currently tracked.
     *
     * Postconditions:
     * - The cursors are ultimately closed.
     */
    virtual void closeCursorsOnDataShards(const stdx::unordered_set<ShardId>& shardSet) = 0;

    /**
     * Triggers opening of a change stream cursor on the config server for the change stream
     * associated with this context.
     *
     * @param atClusterTime the minimum cluster time of the first change event from the config
     * server.
     *
     * Preconditions:
     * - atClusterTime > cluster time of the control event that is being processed.
     * - There is no cursor open on the config server.
     *
     * Postconditions:
     * - The cursor is open and the results from it are merged into the change stream before
     *   retrieving the next change event by the change stream reader.
     */
    virtual void openCursorOnConfigServer(Timestamp atClusterTime) = 0;

    /**
     * Triggers closing of a change stream cursor on the config server for the change stream
     * associated with this context.
     *
     * Postconditions:
     * - The cursor is ultimately closed.
     */
    virtual void closeCursorOnConfigServer() = 0;

    /**
     * Determines if a change stream cursor on the config server for the change stream associated
     * with this context is open. Note that opening and closing of cursors happens after
     * ChangeStreamShardTargeter is invoked.
     */
    virtual bool isCursorOnConfigServerOpen() const = 0;

    /**
     * Returns a set of data shards that are currently targeted, that is, shards that have change
     * stream cursors opened on for the change stream associated with this context.
     */
    virtual const stdx::unordered_set<ShardId>& getCurrentlyTargetedDataShards() const = 0;

    /**
     * Returns a change stream to which this context is associated with.
     */
    virtual const ChangeStream& getChangeStream() const = 0;

    /**
     * Returns true if the change stream is read in 'ChangeStreamReadMode::kIgnoreRemovedShards'
     * mode and the current change stream segment is read in the degraded mode.
     */
    virtual bool inDegradedMode() const = 0;
};

}  // namespace mongo
