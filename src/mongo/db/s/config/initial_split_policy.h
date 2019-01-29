
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
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_tags.h"
#include "mongo/s/request_types/shard_collection_gen.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {

class InitialSplitPolicy {
public:
    /**
     * For new collections which use hashed shard keys, we can can pre-split the range of possible
     * hashes into a large number of chunks, and distribute them evenly at creation time.
     *
     * Until we design a better initialization scheme, the most performant way to pre-split is to
     * make one big chunk for each shard and migrate them one at a time. Because of this:
     * - 'initialSplitPoints' is populated with the split points to use on the primary shard to
     * produce the initial "big chunks."
     * - 'finalSplitPoints' is populated with the additional split points to use on the "big chunks"
     * after the "big chunks" have been spread evenly across shards through migrations.
     */
    static void calculateHashedSplitPointsForEmptyCollection(
        const ShardKeyPattern& shardKeyPattern,
        bool isEmpty,
        int numShards,
        int numInitialChunks,
        std::vector<BSONObj>* initialSplitPoints,
        std::vector<BSONObj>* finalSplitPoints);

    struct ShardCollectionConfig {
        std::vector<ChunkType> chunks;

        const auto& collVersion() const {
            return chunks.back().getVersion();
        }
    };

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
        const NamespaceString& nss,
        const ShardKeyPattern& shardKeyPattern,
        const ShardId& databasePrimaryShardId,
        const Timestamp& validAfter,
        const std::vector<BSONObj>& splitPoints,
        const std::vector<ShardId>& allShardIds,
        const int numContiguousChunksPerShard = 1);

    /**
     * Produces the initial chunks that need to be written for an *empty* collection which is being
     * sharded based on the given 'tags'.
     *
     * NOTE: The function performs some basic validation of the input parameters, but there is no
     * checking whether the collection contains any data or not.
     *
     * The contents of 'tags' will be used to create chunks, which correspond to these zones and
     * chunks will be assigned to shards from 'tagToShards'. If there are any holes in between the
     * zones (zones are not contiguous), these holes will be assigned to 'shardIdsForGaps' in
     * round-robin fashion.
     */
    static ShardCollectionConfig generateShardCollectionInitialZonedChunks(
        const NamespaceString& nss,
        const ShardKeyPattern& shardKeyPattern,
        const Timestamp& validAfter,
        const std::vector<TagsType>& tags,
        const StringMap<std::vector<ShardId>>& tagToShards,
        const std::vector<ShardId>& shardIdsForGaps);

    /**
     * Generates a list with what are the most optimal first chunks and placement for a newly
     * sharded collection.
     *
     * If the collection 'isEmpty', chunks will be spread across all available (appropriate based on
     * zoning rules) shards. Otherwise, they will all end up on the primary shard after which the
     * balancer will take care of properly distributing them around.
     */
    static ShardCollectionConfig createFirstChunks(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   const ShardKeyPattern& shardKeyPattern,
                                                   const ShardId& primaryShardId,
                                                   const std::vector<BSONObj>& splitPoints,
                                                   const std::vector<TagsType>& tags,
                                                   bool isEmpty,
                                                   int numContiguousChunksPerShard = 1);

    /**
     * Writes to the config server the first chunks for a newly sharded collection.
     */
    static void writeFirstChunksToConfig(
        OperationContext* opCtx, const InitialSplitPolicy::ShardCollectionConfig& initialChunks);

    /**
     * Throws an exception if the collection is already sharded with different options.
     *
     * If the collection is already sharded with the same options, returns the existing collection's
     * full spec, else returns boost::none.
     */
    static boost::optional<CollectionType> checkIfCollectionAlreadyShardedWithSameOptions(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ShardsvrShardCollection& request,
        repl::ReadConcernLevel readConcernLevel);
};
}  // namespace mongo
