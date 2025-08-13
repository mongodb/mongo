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

#include "mongo/bson/timestamp.h"
#include "mongo/db/pipeline/change_stream.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/unordered_set.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * Provides resources for opening/closing of cursors on data shards and the config server, and
 * inquiring the current state of the change stream.
 */
class ChangeStreamReaderContext {
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
