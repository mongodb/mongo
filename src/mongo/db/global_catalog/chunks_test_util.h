/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
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
