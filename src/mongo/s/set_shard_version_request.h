/**
 *    Copyright (C) 2015 MongoDB Inc.
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
#include <string>

#include "mongo/client/connection_string.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/shard_id.h"

namespace mongo {

class BSONObj;
template <typename T>
class StatusWith;

/**
 * Encapsulates the parsing and construction logic for the SetShardVersion command.
 */
class SetShardVersionRequest {
public:
    /**
     * Constructs a new set shard version request, which is of the "init" type, meaning it has no
     * namespace or version information associated with it and the init flag is set.
     * The constructed request will not contain the "noConnectionVersioning" field, which means that
     * the entire connection will be marked as "versioned" on the mongod side. DO NOT USE when
     * sending through the TaskExecutor, which pools connections without consideration for which
     * are marked as sharded.
     */
    static SetShardVersionRequest makeForInit(const ConnectionString& configServer,
                                              const ShardId& shardName,
                                              const ConnectionString& shardConnectionString);

    /**
     * Constructs a new set shard version request, which is of the "init" type, meaning it has no
     * namespace or version information associated with it and the init flag is set. In
     * addition, the request will contain the "noConnectionVersioning" field, which means that the
     * connection WILL NOT be marked as "versioned". DO NOT USE except on connections only used
     * with operations that do per-operation versioning, and do not depend on the connection being
     * marked as sharded.
     */
    static SetShardVersionRequest makeForInitNoPersist(
        const ConnectionString& configServer,
        const ShardId& shardName,
        const ConnectionString& shardConnectionString);

    /**
     * Constructs a new set shard version request, which is of the "versioning" type, meaning it has
     * both initialization data and namespace and version information associated with it.
     *
     * The constructed request will not contain the "noConnectionVersioning" field, which means that
     * the entire connection will be marked as "versioned" on the mongod side. DO NOT USE when
     * sending through the TaskExecutor, which pools connections without consideration for which
     * are marked as sharded.
     */
    static SetShardVersionRequest makeForVersioning(const ConnectionString& configServer,
                                                    const ShardId& shardName,
                                                    const ConnectionString& shard,
                                                    const NamespaceString& nss,
                                                    const ChunkVersion& nssVersion,
                                                    bool isAuthoritative);

    /**
     * Constructs a new set shard version request, which is of the "versioning" type, meaning it has
     * both initialization data and namespace and version information associated with it. In
     * addition, the request will contain the "noConnectionVersioning" field, which means that the
     * connection WILL NOT be marked as "versioned". DO NOT USE except on connections only used
     * with operations that do per-operation versioning, and do not depend on the connection being
     * marked as sharded.
     */
    static SetShardVersionRequest makeForVersioningNoPersist(const ConnectionString& configServer,
                                                             const ShardId& shardName,
                                                             const ConnectionString& shard,
                                                             const NamespaceString& nss,
                                                             const ChunkVersion& nssVersion,
                                                             bool isAuthoritative);

    /**
     * Parses an SSV request from a set shard version command.
     */
    static StatusWith<SetShardVersionRequest> parseFromBSON(const BSONObj& cmdObj);

    /**
     * Produces a BSON representation of the request, which can be used for sending as a command.
     */
    BSONObj toBSON() const;

    /**
     * Returns whether this is an "init" type of request, where we only have the config server
     * information and the identity that the targeted shard should assume or it contains namespace
     * version as well. If this value is true, it is illegal to access anything other than the
     * config server, shard name and shard connection string fields.
     */
    bool isInit() const {
        return _init;
    }

    /**
     * Returns whether this request should force the version to be set instead of it being reloaded
     * and recalculated from the metadata.
     */
    bool isAuthoritative() const {
        return _isAuthoritative;
    }

    const ConnectionString& getConfigServer() const {
        return _configServer;
    }

    const ShardId& getShardName() const {
        return _shardName;
    }

    const ConnectionString& getShardConnectionString() const {
        return _shardCS;
    }

    /**
     * Returns the namespace associated with this set shard version request. It is illegal to access
     * this field if isInit() returns true.
     */
    const NamespaceString& getNS() const;

    /**
     * Returns the version of the namespace associated with this set shard version request. It is
     * illegal to access this field if isInit() returns true.
     */
    const ChunkVersion getNSVersion() const;

    /**
     * Returns whether this setShardVersion request should be persisted on the connection or it
     * should only be used to initialize the namespace in the global sharding state.
     */
    bool getNoConnectionVersioning() const {
        return _noConnectionVersioning;
    }

private:
    SetShardVersionRequest(ConnectionString configServer,
                           ShardId shardName,
                           ConnectionString shardConnectionString);

    SetShardVersionRequest(ConnectionString configServer,
                           ShardId shardName,
                           ConnectionString shardConnectionString,
                           NamespaceString nss,
                           ChunkVersion version,
                           bool isAuthoritative);

    SetShardVersionRequest();

    bool _init{false};
    bool _isAuthoritative{false};
    bool _noConnectionVersioning{false};

    ConnectionString _configServer;

    ShardId _shardName;
    ConnectionString _shardCS;

    // These values are only set if _init is false
    boost::optional<NamespaceString> _nss;
    boost::optional<ChunkVersion> _version;
};

}  // namespace mongo
