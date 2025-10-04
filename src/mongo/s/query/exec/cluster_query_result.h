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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/sharding_environment/shard_id.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * Holds a single result from a mongos find command shard request and the shard the request
 * originated from. The result can either contain collection data, stored in '_resultObj'; or be
 * EOF, and isEOF() returns true.
 */
class ClusterQueryResult {
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
