/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
    static constexpr StringData kVersion = "version"_sd;

    SetShardVersionRequest(ConnectionString configServer,
                           ShardId shardName,
                           ConnectionString shardConnectionString,
                           NamespaceString nss,
                           ChunkVersion version,
                           bool isAuthoritative,
                           bool forceRefresh = false);

    /**
     * Parses an SSV request from a set shard version command.
     */
    static StatusWith<SetShardVersionRequest> parseFromBSON(const BSONObj& cmdObj);

    /**
     * Produces a BSON representation of the request, which can be used for sending as a command.
     */
    BSONObj toBSON() const;

    /**
     * Returns whether this request should force the version to be set instead of it being reloaded
     * and recalculated from the metadata.
     */
    bool isAuthoritative() const {
        return _isAuthoritative;
    }

    /**
     * Returns whether the set shard version catalog refresh is allowed to join
     * an in-progress refresh triggered by an other thread, or whether it's
     * required to either a) trigger its own refresh or b) wait for a refresh
     * to be started after it has entered the getCollectionRoutingInfoWithRefresh function
     */
    bool shouldForceRefresh() const {
        return _forceRefresh;
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

private:
    SetShardVersionRequest();

    bool _isAuthoritative{false};
    bool _forceRefresh{false};

    // TODO (SERVER-47440): Remove this parameter once the v4.4 SetShardVersion command stops
    // parsing it.
    ConnectionString _configServer;

    ShardId _shardName;
    ConnectionString _shardCS;

    boost::optional<NamespaceString> _nss;
    boost::optional<ChunkVersion> _version;
};

}  // namespace mongo
