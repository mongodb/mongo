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

#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/operation_context.h"

namespace mongo {

class BSONObj;
template <typename T>
class StatusWith;

/**
 * Specifies a representation of the external mongos and internal config versions of the addShard
 * command, and provides methods to convert the representation to and from BSON.
 */
class AddShardRequest {
public:
    // Field names and types in the addShard request.
    static const BSONField<std::string> mongosAddShard;
    static const BSONField<std::string> mongosAddShardDeprecated;
    static const BSONField<std::string> configsvrAddShard;
    static const BSONField<std::string> shardName;
    static const BSONField<long long> maxSizeMB;

    /**
     * Parses the provided BSON content as the external addShard command, and if it is correct,
     * constructs an AddShardRequest object from it.
     */
    static StatusWith<AddShardRequest> parseFromMongosCommand(const BSONObj& obj);

    /**
     * Parses the provided BSON content as the internal _configsvrAddShard command, and if it
     * contains the correct types, constructs an AddShardRequest object from it.
     */
    static StatusWith<AddShardRequest> parseFromConfigCommand(const BSONObj& obj);

    /**
     * Creates a serialized BSONObj of the internal _configsvrAddShard command from this
     * AddShardRequest instance.
     */
    BSONObj toCommandForConfig();


    /**
     * Verifies that the request parameters are valid and consistent with each other.
     */
    Status validate(bool allowLocalHost);

    std::string toString() const;

    const ConnectionString& getConnString() const {
        return _connString;
    }

    bool hasName() const {
        return _name.is_initialized();
    }

    const std::string& getName() const {
        invariant(_name.is_initialized());
        return *_name;
    }

    bool hasMaxSize() const {
        return _maxSizeMB.is_initialized();
    }

    long long getMaxSize() const {
        invariant(_maxSizeMB.is_initialized());
        return *_maxSizeMB;
    }

private:
    explicit AddShardRequest(ConnectionString connString);

    /**
     * Parses the fields from the provided BSON content into an AddShardRequest object.
     */
    static StatusWith<AddShardRequest> parseInternalFields(const BSONObj& obj);

    // If the shard to be added is standalone, then the hostname and port of the mongod instance to
    // be added. If the shard to be added is a replica set, the name of the replica set and the
    // hostname and port of at least one member of the replica set.
    ConnectionString _connString;

    // A name for the shard. If not specified, a unique name is automatically generated.
    boost::optional<std::string> _name;

    // The maximum size in megabytes of the shard. If set to 0, the size is not limited.
    boost::optional<long long> _maxSizeMB;
};

}  // namespace mongo
