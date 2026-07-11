// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * Holds a single result from a mongos find command shard request and the shard the request
 * originated from. The result can either contain collection data, stored in '_resultObj'; or be
 * EOF, and isEOF() returns true.
 *
 * TODO SERVER-111290 Remove external dependencies on this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ClusterQueryResult {
public:
    ClusterQueryResult() = default;

    ClusterQueryResult(BSONObj resObj, boost::optional<ShardId> shardId = boost::none)
        : _resultObj(std::move(resObj)), _shardId(std::move(shardId)) {}

    bool isEOF() const {
        return !_resultObj;
    }

    const boost::optional<BSONObj>& getResult() const {
        return _resultObj;
    }

    const boost::optional<ShardId>& getShardId() const {
        return _shardId;
    }

private:
    boost::optional<BSONObj> _resultObj;
    boost::optional<ShardId> _shardId;
};

}  // namespace mongo
