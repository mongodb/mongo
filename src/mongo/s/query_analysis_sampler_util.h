/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace analyze_shard_key {

struct TargetedSampleId {
public:
    TargetedSampleId(UUID sampleId, ShardId shardId) : _sampleId(sampleId), _shardId(shardId){};

    bool isFor(ShardEndpoint endpoint) const {
        return _shardId == endpoint.shardName;
    }

    bool isFor(ShardId shardId) const {
        return _shardId == shardId;
    }

    UUID getId() const {
        return _sampleId;
    }

private:
    UUID _sampleId;
    ShardId _shardId;
};

/**
 * Returns a unique sample id for a query if it should be sampled, and none otherwise. Can only be
 * invoked once for each query since generating a sample id causes the number of remaining queries
 * to sample to get decremented.
 */
boost::optional<UUID> tryGenerateSampleId(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Similar to 'tryGenerateSampleId()' but assigns the sample id to a random shard out of the given
 * ones.
 */
boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              const std::set<ShardId>& shardIds);
boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<ShardEndpoint>& endpoints);

ShardId getRandomShardId(const std::set<ShardId>& shardIds);
ShardId getRandomShardId(const std::vector<ShardEndpoint>& endpoints);

BSONObj appendSampleId(const BSONObj& cmdObj, const UUID& sampleId);
void appendSampleId(BSONObjBuilder* bob, const UUID& sampleId);

}  // namespace analyze_shard_key
}  // namespace mongo
