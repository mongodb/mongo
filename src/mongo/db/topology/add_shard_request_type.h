// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

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
};

}  // namespace mongo
