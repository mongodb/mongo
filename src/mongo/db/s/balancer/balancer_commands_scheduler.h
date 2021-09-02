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
#include "mongo/executor/task_executor.h"
#include "mongo/s/request_types/move_chunk_request.h"
#include "mongo/s/shard_id.h"

namespace mongo {

class ChunkType;
class ShardKeyPattern;
class MigrationSecondaryThrottleOptions;

/**
 * Set of command-specific aggregations of submission settings
 */
struct MoveChunkSettings {
    MoveChunkSettings(int64_t maxChunkSizeBytes,
                      const MigrationSecondaryThrottleOptions& secondaryThrottle,
                      bool waitForDelete,
                      MoveChunkRequest::ForceJumbo forceJumbo)
        : maxChunkSizeBytes(maxChunkSizeBytes),
          secondaryThrottle(secondaryThrottle),
          waitForDelete(waitForDelete),
          forceJumbo(forceJumbo) {}

    int64_t maxChunkSizeBytes;
    const MigrationSecondaryThrottleOptions secondaryThrottle;
    bool waitForDelete;
    MoveChunkRequest::ForceJumbo forceJumbo;
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
 * Common interface for any response generated from a command issued through the
 * BalancerCommandsScheduler.
 */
class DeferredResponse {
public:
    virtual ~DeferredResponse() = default;

    virtual uint32_t getRequestId() const = 0;

    virtual bool hasFinalised() const = 0;

    /**
     * Blocking as long as hasFinalised() is false.
     */
    virtual Status getOutcome() = 0;
};

/**
 * Set of command-specific subclasses of DeferredResponse,
 * adding helper functions to deserialise the response payload (where this applies).
 */
class MoveChunkResponse : public DeferredResponse {};

class MergeChunksResponse : public DeferredResponse {};

class SplitVectorResponse : public DeferredResponse {
public:
    virtual StatusWith<std::vector<BSONObj>> getSplitKeys() = 0;
};

class SplitChunkResponse : public DeferredResponse {};

class ChunkDataSizeResponse : public DeferredResponse {
public:
    virtual StatusWith<long long> getSize() = 0;
    virtual StatusWith<long long> getNumObjects() = 0;
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
     * Activates the scheduler, which will start accepting and processing
     * request<Command>() invocations.
     */
    virtual void start() = 0;

    /**
     * Stops the scheduler and the processing of any outstanding and incoming request
     * (which will be resolved as cancelled).
     */
    virtual void stop() = 0;

    virtual std::unique_ptr<MoveChunkResponse> requestMoveChunk(
        const NamespaceString& nss,
        const ChunkType& chunk,
        const ShardId& recipient,
        const MoveChunkSettings& commandSettings) = 0;

    virtual std::unique_ptr<MergeChunksResponse> requestMergeChunks(
        const NamespaceString& nss, const ChunkType& lowerBound, const ChunkType& upperBound) = 0;

    virtual std::unique_ptr<SplitVectorResponse> requestSplitVector(
        const NamespaceString& nss,
        const ChunkType& chunk,
        const ShardKeyPattern& shardKeyPattern,
        const SplitVectorSettings& commandSettings) = 0;

    virtual std::unique_ptr<SplitChunkResponse> requestSplitChunk(
        const NamespaceString& nss,
        const ChunkType& chunk,
        const ShardKeyPattern& shardKeyPattern,
        const std::vector<BSONObj>& splitPoints) = 0;

    virtual std::unique_ptr<ChunkDataSizeResponse> requestChunkDataSize(
        const NamespaceString& nss,
        const ChunkType& chunk,
        const ShardKeyPattern& shardKeyPattern,
        bool estimatedValue) = 0;
};

}  // namespace mongo
