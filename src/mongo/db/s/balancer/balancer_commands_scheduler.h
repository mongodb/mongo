/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/util/future.h"

#include <cstdint>

namespace mongo {

class ChunkType;
class KeyPattern;
class MigrationSecondaryThrottleOptions;

/**
 * Interface for the asynchronous submission of chunk-related commands.
 * Every method assumes that the ChunkType input parameter is filled up with information about
 * version and location (shard ID).
 **/
class BalancerCommandsScheduler {
public:
    virtual ~BalancerCommandsScheduler() = default;

    /**
     * Triggers an asynchronous self-initialisation of the component,
     * which will start accepting request<Command>() invocations.
     */
    virtual void start(OperationContext* opCtx) = 0;

    /**
     * Stops the scheduler and the processing of any outstanding and incoming request
     * (which will be resolved as cancelled).
     */
    virtual void stop() = 0;

    virtual void disableBalancerForCollection(OperationContext* opCtx,
                                              const NamespaceString& nss) = 0;

    virtual SemiFuture<void> requestMergeChunks(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const ShardId& shardId,
                                                const ChunkRange& chunkRange,
                                                const ChunkVersion& version) = 0;

    virtual SemiFuture<DataSizeResponse> requestDataSize(OperationContext* opCtx,
                                                         const NamespaceString& nss,
                                                         const ShardId& shardId,
                                                         const ChunkRange& chunkRange,
                                                         const ShardVersion& version,
                                                         const KeyPattern& keyPattern,
                                                         bool estimatedValue,
                                                         int64_t maxSize) = 0;

    virtual SemiFuture<void> requestMoveRange(OperationContext* opCtx,
                                              const ShardsvrMoveRange& request,
                                              const WriteConcernOptions& secondaryThrottleWC,
                                              bool issuedByRemoteUser) = 0;

    virtual SemiFuture<NumMergedChunks> requestMergeAllChunksOnShard(OperationContext* opCtx,
                                                                     const NamespaceString& nss,
                                                                     const ShardId& shardId) = 0;

    virtual SemiFuture<void> requestMoveCollection(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const ShardId& toShardId,
                                                   const ShardId& dbPrimaryShardId,
                                                   const DatabaseVersion& dbVersion) = 0;
};

}  // namespace mongo
