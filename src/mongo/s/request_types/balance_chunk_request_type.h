/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>

#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/migration_secondary_throttle_options.h"

namespace mongo {

class BSONObj;
template <typename T>
class StatusWith;

/**
 * Provides support for parsing and serialization of arguments to the config server moveChunk
 * command, which controls the cluster balancer. If any changes are made to this class, they need to
 * be backwards compatible with older versions of the server.
 */
class BalanceChunkRequest {
public:
    /**
     * Parses the provided BSON content and if it is correct construct a request object with the
     * request parameters.
     */
    static StatusWith<BalanceChunkRequest> parseFromConfigCommand(const BSONObj& obj);

    /**
     * Produces a BSON object for the variant of the command, which requests the balancer to move a
     * chunk to a user-specified shard.
     */
    static BSONObj serializeToMoveCommandForConfig(
        const ChunkType& chunk,
        const ShardId& newShardId,
        int64_t maxChunkSizeBytes,
        const MigrationSecondaryThrottleOptions& secondaryThrottle,
        bool waitForDelete);

    /**
     * Produces a BSON object for the variant of the command, which requests the balancer to pick a
     * better location for a chunk.
     */
    static BSONObj serializeToRebalanceCommandForConfig(const ChunkType& chunk);

    const ChunkType& getChunk() const {
        return _chunk;
    }

    bool hasToShardId() const {
        return _toShardId.is_initialized();
    }

    const ShardId& getToShardId() const {
        return *_toShardId;
    }

    int64_t getMaxChunkSizeBytes() const {
        return _maxChunkSizeBytes;
    }

    const MigrationSecondaryThrottleOptions& getSecondaryThrottle() const {
        return _secondaryThrottle;
    }

    bool getWaitForDelete() const {
        return _waitForDelete;
    }

private:
    BalanceChunkRequest(ChunkType chunk, MigrationSecondaryThrottleOptions secondaryThrottle);

    // Complete description of the chunk to be manipulated
    ChunkType _chunk;

    // Id of the shard to which it should be moved (if specified)
    boost::optional<ShardId> _toShardId;

    // This value is used by the migration source to determine the data size threshold above which a
    // chunk would be considered jumbo and migrations will not proceed.
    int64_t _maxChunkSizeBytes;

    // The parsed secondary throttle options
    MigrationSecondaryThrottleOptions _secondaryThrottle;

    // Whether to block and wait for the range deleter to cleanup the orphaned documents at the end
    // of move.
    bool _waitForDelete;
};

}  // namespace mongo
