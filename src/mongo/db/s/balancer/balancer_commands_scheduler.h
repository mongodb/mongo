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
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/s/shard_id.h"

namespace mongo {

class ChunkType;
class KeyPattern;
class MigrationSecondaryThrottleOptions;

/**
 * Default values for MoveChunkRequests that the Scheduler might recover from a prior
 * step-down or crash as part of its self-initialisation
 */
struct MigrationsRecoveryDefaultValues {
    MigrationsRecoveryDefaultValues(int64_t maxChunkSizeBytes,
                                    const MigrationSecondaryThrottleOptions& secondaryThrottle)
        : maxChunkSizeBytes(maxChunkSizeBytes), secondaryThrottle(secondaryThrottle) {}
    const int64_t maxChunkSizeBytes;
    const MigrationSecondaryThrottleOptions secondaryThrottle;
};

/**
 * Set of command-specific aggregations of submission settings
 */
struct MoveChunkSettings {
    MoveChunkSettings(int64_t maxChunkSizeBytes,
                      const MigrationSecondaryThrottleOptions& secondaryThrottle,
                      bool waitForDelete)
        : maxChunkSizeBytes(maxChunkSizeBytes),
          secondaryThrottle(secondaryThrottle),
          waitForDelete(waitForDelete) {}

    int64_t maxChunkSizeBytes;
    const MigrationSecondaryThrottleOptions secondaryThrottle;
    bool waitForDelete;
};

struct SplitVectorSettings {
    SplitVectorSettings()
        : force(false),
          maxSplitPoints(boost::none),
          maxChunkObjects(boost::none),
          maxChunkSizeBytes(boost::none) {}
    SplitVectorSettings(boost::optional<long long> maxSplitPoints,
                        boost::optional<long long> maxChunkObjects,
                        boost::optional<long long> maxChunkSizeBytes,
                        bool force)
        : force(force),
          maxSplitPoints(maxSplitPoints),
          maxChunkObjects(maxChunkObjects),
          maxChunkSizeBytes(maxChunkSizeBytes) {}

    bool force;
    boost::optional<long long> maxSplitPoints;
    boost::optional<long long> maxChunkObjects;
    boost::optional<long long> maxChunkSizeBytes;
};

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
    virtual void start(OperationContext* opCtx,
                       const MigrationsRecoveryDefaultValues& defaultValues) = 0;

    /**
     * Stops the scheduler and the processing of any outstanding and incoming request
     * (which will be resolved as cancelled).
     */
    virtual void stop() = 0;

    virtual SemiFuture<void> requestMoveChunk(OperationContext* opCtx,
                                              const MigrateInfo& migrateInfo,
                                              const MoveChunkSettings& commandSettings,
                                              bool issuedByRemoteUser = false) = 0;

    virtual SemiFuture<void> requestMergeChunks(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const ShardId& shardId,
                                                const ChunkRange& chunkRange,
                                                const ChunkVersion& version) = 0;

    virtual SemiFuture<AutoSplitVectorResponse> requestAutoSplitVector(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ShardId& shardId,
        const BSONObj& keyPattern,
        const BSONObj& minKey,
        const BSONObj& maxKey,
        int64_t maxChunkSizeBytes) = 0;

    virtual SemiFuture<void> requestSplitChunk(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               const ShardId& shardId,
                                               const ChunkVersion& collectionVersion,
                                               const KeyPattern& keyPattern,
                                               const BSONObj& minKey,
                                               const BSONObj& maxKey,
                                               const SplitPoints& splitPoints) = 0;

    virtual SemiFuture<DataSizeResponse> requestDataSize(OperationContext* opCtx,
                                                         const NamespaceString& nss,
                                                         const ShardId& shardId,
                                                         const ChunkRange& chunkRange,
                                                         const ChunkVersion& version,
                                                         const KeyPattern& keyPattern,
                                                         bool estimatedValue) = 0;

    virtual SemiFuture<void> requestMoveRange(OperationContext* opCtx,
                                              const ShardsvrMoveRange& request,
                                              const WriteConcernOptions& secondaryThrottleWC,
                                              bool issuedByRemoteUser) = 0;
};

}  // namespace mongo
