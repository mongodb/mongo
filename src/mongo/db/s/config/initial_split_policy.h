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

#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/string_map.h"

namespace mongo {

struct SplitPolicyParams {
    NamespaceString nss;
    // TODO SERVER-53105 update this comment explaining that just (nss || uuid) field will be
    // persisted If boost::none, the resulting config.chunks document(s) are not going to include
    // the collection UUID field
    boost::optional<CollectionUUID> collectionUUID;
    ShardId primaryShardId;
};

class InitialSplitPolicy {
public:
    /**
     * Returns the optimization strategy for building initial chunks based on the input parameters
     * and the collection state.
     */
    static std::unique_ptr<InitialSplitPolicy> calculateOptimizationStrategy(
        OperationContext* opCtx,
        const ShardKeyPattern& shardKeyPattern,
        const ShardsvrShardCollectionRequest& request,
        const std::vector<TagsType>& tags,
        size_t numShards,
        bool collectionIsEmpty);

    virtual ~InitialSplitPolicy() {}

    /**
     * Generates a list of initial chunks to be created during a shardCollection operation.
     */
    struct ShardCollectionConfig {
        std::vector<ChunkType> chunks;
        Timestamp creationTime;

        const auto& collVersion() const {
            return chunks.back().getVersion();
        }
    };
    virtual ShardCollectionConfig createFirstChunks(OperationContext* opCtx,
                                                    const ShardKeyPattern& shardKeyPattern,
                                                    SplitPolicyParams params) = 0;

    /**
     * Returns whether the chunk generation strategy being used is optimized or not. Since there is
     * only a single unoptimized policy, we return true by default here.
     */
    virtual bool isOptimized() {
        return true;
    }

    /**
     * Returns split points to use for creating chunks in cases where the shard key contains a
     * hashed field. For new collections which use hashed shard keys, we can can pre-split the range
     * of possible hashes into a large number of chunks, and distribute them evenly at creation
     * time. In the case where the shard key is compound hashed, the 'prefix' object specifies the
     * non-hashed prefix to be prepended to each hashed splitpoint. If no such prefix exists, this
     * will be an empty BSONObj. It is an error to pass a 'prefix' object which is not consistent
     * with the given ShardKeyPattern.
     */
    static std::vector<BSONObj> calculateHashedSplitPoints(const ShardKeyPattern& shardKeyPattern,
                                                           BSONObj prefix,
                                                           int numInitialChunks);

    /**
     * Produces the initial chunks that need to be written for an *empty* collection which is being
     * sharded based on a set of 'splitPoints' and 'numContiguousChunksPerShard'.
     *
     * NOTE: The function performs some basic validation of the input parameters, but there is no
     * checking whether the collection contains any data or not.
     *
     * Chunks are assigned to a shard in a round-robin fashion, numContiguousChunksPerShard (k)
     * chunks at a time. For example, the first k chunks are assigned to the first available shard,
     * and the next k chunks are assigned to the second available shard and so on.
     * numContiguousChunksPerShard should only be > 1 when we do not pre-split the range
     * into larger chunks and then split the resulting chunks on the destination shards as in
     * configSvrShardCollection, thus should be equal the number of final split points + 1 divided
     * by the number of initial split points + 1. It serves to preserve the ordering/contigousness
     * of chunks when split by shardSvrShardCollection so that its yields the exact same shard
     * assignments as configSvrShardCollection.
     */
    static ShardCollectionConfig generateShardCollectionInitialChunks(
        SplitPolicyParams params,
        const ShardKeyPattern& shardKeyPattern,
        const Timestamp& validAfter,
        const std::vector<BSONObj>& splitPoints,
        const std::vector<ShardId>& allShardIds,
        int numContiguousChunksPerShard);
};

/**
 * Default optimization strategy where we create a single chunk on the primary shard.
 */
class SingleChunkOnPrimarySplitPolicy : public InitialSplitPolicy {
public:
    ShardCollectionConfig createFirstChunks(OperationContext* opCtx,
                                            const ShardKeyPattern& shardKeyPattern,
                                            SplitPolicyParams params);
};

/**
 * Split point building strategy to be used when no optimizations are available. We send a
 * splitVector command to the primary shard in order to calculate the appropriate split points.
 */
class UnoptimizedSplitPolicy : public InitialSplitPolicy {
public:
    ShardCollectionConfig createFirstChunks(OperationContext* opCtx,
                                            const ShardKeyPattern& shardKeyPattern,
                                            SplitPolicyParams params);
    bool isOptimized() {
        return false;
    }
};

/**
 * Split point building strategy to be used when explicit split points are supplied, or where the
 * appropriate splitpoints can be trivially deduced from the shard key.
 */
class SplitPointsBasedSplitPolicy : public InitialSplitPolicy {
public:
    /**
     * Constructor used when split points are provided.
     */
    SplitPointsBasedSplitPolicy(std::vector<BSONObj> explicitSplitPoints)
        : _splitPoints(std::move(explicitSplitPoints)) {
        _numContiguousChunksPerShard = 1;
    }

    /**
     * Constructor used when generating split points for a hashed-prefix shard key.
     */
    SplitPointsBasedSplitPolicy(const ShardKeyPattern& shardKeyPattern,
                                size_t numShards,
                                size_t numInitialChunks) {
        // If 'numInitialChunks' was not specified, use default value.
        numInitialChunks = numInitialChunks ? numInitialChunks : numShards * 2;
        _splitPoints = calculateHashedSplitPoints(shardKeyPattern, BSONObj(), numInitialChunks);
        _numContiguousChunksPerShard =
            std::max(numInitialChunks / numShards, static_cast<size_t>(1));
    }

    ShardCollectionConfig createFirstChunks(OperationContext* opCtx,
                                            const ShardKeyPattern& shardKeyPattern,
                                            SplitPolicyParams params);

    // Helpers for unit testing.
    const std::vector<BSONObj>& getSplitPoints() const {
        return _splitPoints;
    }
    size_t getNumContiguousChunksPerShard() const {
        return _numContiguousChunksPerShard;
    }

private:
    std::vector<BSONObj> _splitPoints;
    size_t _numContiguousChunksPerShard;
};

/**
 * Abstract base class for all split policies which depend upon zones having already been defined at
 * the moment the collection is sharded.
 */
class AbstractTagsBasedSplitPolicy : public InitialSplitPolicy {
public:
    /**
     * Records the splitpoints and chunk distribution among shards within a particular tag range.
     */
    struct SplitInfo {
        std::vector<BSONObj> splitPoints;
        std::vector<std::pair<ShardId, size_t>> chunkDistribution;
    };

    AbstractTagsBasedSplitPolicy(OperationContext* opCtx, std::vector<TagsType> tags);

    ShardCollectionConfig createFirstChunks(OperationContext* opCtx,
                                            const ShardKeyPattern& shardKeyPattern,
                                            SplitPolicyParams params);

    /**
     * Returns the split points to be used for generating chunks within a given tag.
     */
    virtual SplitInfo buildSplitInfoForTag(TagsType tag,
                                           const ShardKeyPattern& shardKeyPattern) = 0;

    const std::vector<TagsType>& getTags() const {
        return _tags;
    }

    const StringMap<std::vector<ShardId>>& getTagsToShardIds() const {
        return _tagToShardIds;
    }

private:
    const std::vector<TagsType> _tags;
    StringMap<std::vector<ShardId>> _tagToShardIds;
};

/**
 * In this strategy we directly generate a single chunk for each tag range.
 */
class SingleChunkPerTagSplitPolicy : public AbstractTagsBasedSplitPolicy {
public:
    SingleChunkPerTagSplitPolicy(OperationContext* opCtx, std::vector<TagsType> tags)
        : AbstractTagsBasedSplitPolicy(opCtx, tags) {}

    SplitInfo buildSplitInfoForTag(TagsType tag, const ShardKeyPattern& shardKeyPattern);

private:
    StringMap<size_t> _nextShardIndexForZone;
};

/**
 * Split point building strategy to be used when 'presplitHashedZones' flag is set. This policy is
 * only relevant when the zones are set up before sharding and the shard key is hashed. In this
 * case, we generate one chunk per tag range and then further split each of these using the hashed
 * field of the shard key.
 */
class PresplitHashedZonesSplitPolicy : public AbstractTagsBasedSplitPolicy {
public:
    PresplitHashedZonesSplitPolicy(OperationContext* opCtx,
                                   const ShardKeyPattern& shardKeyPattern,
                                   std::vector<TagsType> tags,
                                   size_t numInitialChunks,
                                   bool isCollectionEmpty);

    SplitInfo buildSplitInfoForTag(TagsType tag, const ShardKeyPattern& shardKeyPattern);

private:
    /**
     * Validates that each of tags are set up correctly so that the tags can be split further.
     */
    void _validate(const ShardKeyPattern& shardKeyPattern, bool isCollectionEmpty);
    size_t _numInitialChunks;
    StringMap<size_t> _numTagsPerShard;
};

/**
 * Split point building strategy to be used for resharding when zones are not defined.
 */
class ReshardingSplitPolicy : public InitialSplitPolicy {
public:
    ReshardingSplitPolicy(OperationContext* opCtx,
                          const NamespaceString& nss,
                          const ShardKeyPattern& shardKey,
                          int numInitialChunks,
                          const std::vector<ShardId>& recipientShardIds,
                          const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          int samplingRatio = 10);
    ShardCollectionConfig createFirstChunks(OperationContext* opCtx,
                                            const ShardKeyPattern& shardKeyPattern,
                                            SplitPolicyParams params);
    /**
     * Creates the aggregation pipeline BSON to get documents for sampling from shards.
     */
    static std::vector<BSONObj> createRawPipeline(const ShardKeyPattern& shardKey,
                                                  int samplingRatio,
                                                  int numSplitPoints);

private:
    std::vector<BSONObj> _splitPoints;
    std::vector<ShardId> _recipientShardIds;
    int _numContiguousChunksPerShard;
};
}  // namespace mongo
