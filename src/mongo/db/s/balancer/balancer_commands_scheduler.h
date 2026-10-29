// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/util/modules.h"

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
