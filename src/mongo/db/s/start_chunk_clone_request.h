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
#include "mongo/db/s/migration_session_id.h"
#include "mongo/s/migration_secondary_throttle_options.h"

namespace mongo {

class BSONObjBuilder;
template <typename T>
class StatusWith;
class ShardId;
/**
 * Parses the arguments for a start chunk clone operation.
 */
class StartChunkCloneRequest {
public:
    /**
     * Parses the input command and produces a request corresponding to its arguments.
     */
    static StatusWith<StartChunkCloneRequest> createFromCommand(NamespaceString nss,
                                                                const BSONObj& obj);

    /**
     * Constructs a start chunk clone command with the specified parameters and writes it to the
     * builder, without closing the builder. The builder must be empty, but callers are free to
     * append more fields once the command has been constructed.
     */
    static void appendAsCommand(BSONObjBuilder* builder,
                                const NamespaceString& nss,
                                const MigrationSessionId& sessionId,
                                const ConnectionString& configServerConnectionString,
                                const ConnectionString& fromShardConnectionString,
                                const ShardId& toShardId,
                                const BSONObj& chunkMinKey,
                                const BSONObj& chunkMaxKey,
                                const BSONObj& shardKeyPattern,
                                const MigrationSecondaryThrottleOptions& secondaryThrottle);

    const NamespaceString& getNss() const {
        return _nss;
    }

    const MigrationSessionId& getSessionId() const {
        return _sessionId;
    }

    const ConnectionString& getConfigServerCS() const {
        return _configServerCS;
    }

    const ConnectionString& getFromShardConnectionString() const {
        return _fromShardCS;
    }

    const std::string& getToShardId() const {
        return _toShardId;
    }

    const BSONObj& getMinKey() const {
        return _minKey;
    }

    const BSONObj& getMaxKey() const {
        return _maxKey;
    }

    const BSONObj& getShardKeyPattern() const {
        return _shardKeyPattern;
    }

    const MigrationSecondaryThrottleOptions& getSecondaryThrottle() const {
        return _secondaryThrottle;
    }

private:
    StartChunkCloneRequest(NamespaceString nss,
                           MigrationSessionId sessionId,
                           MigrationSecondaryThrottleOptions secondaryThrottle);

    // The collection for which this request applies
    NamespaceString _nss;

    // The session id of this migration
    MigrationSessionId _sessionId;

    // Connections string for the config server. This is a legacy field and is used in order to
    // initialize the sharding state on the donor shard in case it doesn't yet know that it is part
    // of a sharded system.
    ConnectionString _configServerCS;

    // The source host and port
    ConnectionString _fromShardCS;

    // The recipient shard id
    std::string _toShardId;

    // Exact min and max key of the chunk being moved
    BSONObj _minKey;
    BSONObj _maxKey;

    // Shard key pattern used by the collection
    BSONObj _shardKeyPattern;

    // The parsed secondary throttle options
    MigrationSecondaryThrottleOptions _secondaryThrottle;
};

}  // namespace mongo
