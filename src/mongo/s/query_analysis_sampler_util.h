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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <set>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_id.h"
#include "mongo/platform/basic.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/analyze_shard_key_role.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/query_analysis_sampler.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/testing_proctor.h"
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
boost::optional<UUID> tryGenerateSampleId(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          SampledCommandNameEnum cmdName);
boost::optional<UUID> tryGenerateSampleId(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          StringData cmdName);

/**
 * Similar to 'tryGenerateSampleId()' but assigns the sample id to a random shard out of the given
 * ones.
 */
boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              SampledCommandNameEnum cmdName,
                                                              const std::set<ShardId>& shardIds);

boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(OperationContext* opCtx,
                                                              const NamespaceString& nss,
                                                              StringData cmdName,
                                                              const std::set<ShardId>& shardIds);

boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(
    OperationContext* opCtx,
    const NamespaceString& nss,
    SampledCommandNameEnum cmdName,
    const std::vector<ShardEndpoint>& endpoints);

boost::optional<TargetedSampleId> tryGenerateTargetedSampleId(
    OperationContext* opCtx,
    const NamespaceString& nss,
    BatchedCommandRequest::BatchType cmdName,
    const std::vector<ShardEndpoint>& endpoints);

ShardId getRandomShardId(const std::set<ShardId>& shardIds);
ShardId getRandomShardId(const std::vector<ShardEndpoint>& endpoints);

BSONObj appendSampleId(const BSONObj& cmdObj, const UUID& sampleId);
void appendSampleId(BSONObjBuilder* bob, const UUID& sampleId);

/**
 * To be invoked by commands on mongod. On a mongod that does not support persisting sampled
 * queries, returns none. On a mongod in a shardsvr replica set, returns the sample id attached in
 * the request. On a mongod in a standalone replica set, returns the sample id as generated by
 * 'tryGenerateSampleId'.
 */
template <typename RequestType>
boost::optional<UUID> getOrGenerateSampleId(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const SampledCommandNameEnum cmdName,
                                            const RequestType& request) {
    if (!supportsPersistingSampledQueries(opCtx)) {
        return boost::none;
    }
    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        const auto isInternalThreadOrClient =
            !opCtx->getClient()->session() || opCtx->getClient()->isInternalClient();
        uassert(ErrorCodes::InvalidOptions,
                "Cannot specify 'sampleId' since it is an internal field",
                !request.getSampleId() || isInternalThreadOrClient ||
                    TestingProctor::instance().isEnabled());
        return request.getSampleId();
    }
    if (serverGlobalParams.clusterRole.has(ClusterRole::None)) {
        uassert(ErrorCodes::InvalidOptions,
                "Cannot specify 'sampleId' since it is an internal field",
                !request.getSampleId());
        return QueryAnalysisSampler::get(opCtx).tryGenerateSampleId(opCtx, nss, cmdName);
    }
    MONGO_UNREACHABLE;
}

}  // namespace analyze_shard_key
}  // namespace mongo
