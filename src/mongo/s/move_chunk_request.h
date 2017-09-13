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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <string>

#include "mongo/client/connection_string.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/migration_secondary_throttle_options.h"

namespace mongo {

class BSONObjBuilder;
template <typename T>
class StatusWith;

/**
 * Parses the arguments for a move chunk operation.
 */
class MoveChunkRequest {
public:
    /**
     * Parses the input command and produces a request corresponding to its arguments. The parsing
     * ignores arguments, which are processed at the command level, in particular shardVersion and
     * maxTimeMS.
     */
    static StatusWith<MoveChunkRequest> createFromCommand(NamespaceString nss, const BSONObj& obj);

    /**
     * Constructs a moveChunk command with the specified parameters and writes it to the builder,
     * without closing the builder. The builder must be empty, but callers are free to append more
     * fields once the command has been constructed.
     *
     * The chunkVersion argument is appended as 'chunkVersion', but not parsed by the
     * createFromCommand method above, because it is needed for backwards compatibility with 3.4.
     * However, the 'epoch' field created from the chunkVersion argument is parsed.
     */
    static void appendAsCommand(BSONObjBuilder* builder,
                                const NamespaceString& nss,
                                ChunkVersion chunkVersion,
                                const ConnectionString& configServerConnectionString,
                                const ShardId& fromShardId,
                                const ShardId& toShardId,
                                const ChunkRange& range,
                                int64_t maxChunkSizeBytes,
                                const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                bool waitForDelete);

    const NamespaceString& getNss() const {
        return _nss;
    }

    const ShardId& getFromShardId() const {
        return _fromShardId;
    }

    const ShardId& getToShardId() const {
        return _toShardId;
    }

    const BSONObj& getMinKey() const {
        return _range.getMin();
    }

    const BSONObj& getMaxKey() const {
        return _range.getMax();
    }

    const OID getVersionEpoch() const {
        return _versionEpoch;
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

    /**
     * Returns true if the requests match exactly in terms of the field values and the order of
     * elements within the BSON-typed fields.
     */
    bool operator==(const MoveChunkRequest& other) const;
    bool operator!=(const MoveChunkRequest& other) const;

    /**
     *  Print logging info for the request.
     */
    std::string toString() const;

private:
    MoveChunkRequest(NamespaceString nss,
                     ChunkRange range,
                     MigrationSecondaryThrottleOptions secondaryThrottle);

    // The collection for which this request applies
    NamespaceString _nss;

    // The source shard id
    ShardId _fromShardId;

    // The recipient shard id
    ShardId _toShardId;

    // Range of the chunk being moved
    ChunkRange _range;

    // Assures the collection has not been dropped and recreated since the moveChunk was sent.
    OID _versionEpoch;

    // This value is used by the migration source to determine the data size threshold above which a
    // chunk would be considered jumbo and migrations will not proceed.
    int64_t _maxChunkSizeBytes{0};

    // The parsed secondary throttle options
    MigrationSecondaryThrottleOptions _secondaryThrottle;

    // Whether to block and wait for the range deleter to cleanup the orphaned documents at the end
    // of move.
    bool _waitForDelete;
};

}  // namespace mongo
