// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace mongo::chunks_test_util {

static const std::string kSKey{"a"};
static const KeyPattern kShardKeyPattern{BSON(kSKey << 1)};

/*
 * Assert that all the fields contained in the provided ChunkInfo are equal.
 *
 * This is needed since ChunkInfo class does not provide an equal operator.
 */
void assertEqualChunkInfo(const ChunkInfo& x, const ChunkInfo& y);

ShardId getShardId(int shardIdx);

/**
 * Return a vector of randomly generated split points.
 * covering the entire shard key space including the boundaries [minKey, maxKey)
 *
 * e.g. {"a": <int>}
 */
std::vector<BSONObj> genRandomSplitPoints(size_t numChunks);

/*
 * Generate a shuffled list of random chunk versions.
 *
 * The generated versions are all strictly greater than the provided initialVersion.
 */
std::vector<ChunkVersion> genRandomVersions(size_t num, const ChunkVersion& initialVersion);

/*
 * Generate a vector of chunks whose boundaries are defined by the provided split points and random
 * chunk versions.
 */
std::vector<ChunkType> genChunkVector(const UUID& uuid,
                                      const std::vector<BSONObj>& splitPoints,
                                      const ChunkVersion& initialVersion,
                                      size_t numShards);

/*
 * Generate a vector of chunks.
 */
std::vector<ChunkType> genChunkVector(const UUID& uuid,
                                      const std::vector<BSONObj>& splitPoints,
                                      const std::vector<ChunkVersion>& versions,
                                      size_t numShards);

/*
 * Return a randomly generated vector of chunks that are properly sorted based on their min value
 * and cover the full space from [MinKey, MaxKey].
 */
std::vector<ChunkType> genRandomChunkVector(const UUID& uuid,
                                            const OID& epoch,
                                            const Timestamp& timestamp,
                                            size_t maxNumChunks,
                                            size_t minNumChunks = 1);

std::map<ShardId, ChunkVersion> calculateShardVersions(const std::vector<ChunkType>& chunkVector);

std::map<ShardId, Timestamp> calculateShardsMaxValidAfter(
    const std::vector<ChunkType>& chunkVector);

ChunkVersion calculateCollVersion(const std::map<ShardId, ChunkVersion>& shardVersions);

/*
 * Return a shardkey that is in between the given range [leftKey, rightKey]
 */
BSONObj calculateIntermediateShardKey(const BSONObj& leftKey,
                                      const BSONObj& rightKey,
                                      double minKeyProb = 0.0,
                                      double maxKeyProb = 0.0);

/*
 * Perform a series of random operations on the given list of chunks.
 *
 * The operations performed resemble all possible operations that could happen to a routing table in
 * a production cluster (move, merge, split, etc...)
 *
 * @chunks: list of chunks ordered by minKey
 * @numOperations: number of operations to perform
 */
void performRandomChunkOperations(std::vector<ChunkType>* chunks, size_t numOperations);

/**
 * Helper used in chunk map generation.
 */
std::vector<std::shared_ptr<ChunkInfo>> toChunkInfoPtrVector(
    const std::vector<ChunkType>& chunkTypes);

}  // namespace mongo::chunks_test_util
