/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/config/initial_split_policy.h"

#include "mongo/util/log.h"

namespace mongo {

void InitialSplitPolicy::calculateHashedSplitPointsForEmptyCollection(
    const ShardKeyPattern& shardKeyPattern,
    bool isEmpty,
    int numShards,
    int numInitialChunks,
    std::vector<BSONObj>* initialSplitPoints,
    std::vector<BSONObj>* finalSplitPoints) {
    if (!shardKeyPattern.isHashedPattern() || !isEmpty) {
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "numInitialChunks is not supported when the collection is not "
                              << (!shardKeyPattern.isHashedPattern() ? "hashed" : "empty"),
                !numInitialChunks);
        return;
    }

    // no split points are needed
    if (numInitialChunks == 1) {
        return;
    }

    // If initial split points are not specified, only pre-split when using a hashed shard key and
    // the collection is empty
    if (numInitialChunks <= 0) {
        // Default the number of initial chunks it they are not specified
        numInitialChunks = 2 * numShards;
    }

    // Hashes are signed, 64-bit integers. So we divide the range (-MIN long, +MAX long) into
    // intervals of size (2^64/numInitialChunks) and create split points at the boundaries.
    //
    // The logic below ensures that initial chunks are all symmetric around 0.
    const long long intervalSize = (std::numeric_limits<long long>::max() / numInitialChunks) * 2;
    long long current = 0;

    const auto proposedKey(shardKeyPattern.getKeyPattern().toBSON());

    if (numInitialChunks % 2 == 0) {
        finalSplitPoints->push_back(BSON(proposedKey.firstElementFieldName() << current));
        current += intervalSize;
    } else {
        current += intervalSize / 2;
    }

    for (int i = 0; i < (numInitialChunks - 1) / 2; i++) {
        finalSplitPoints->push_back(BSON(proposedKey.firstElementFieldName() << current));
        finalSplitPoints->push_back(BSON(proposedKey.firstElementFieldName() << -current));
        current += intervalSize;
    }

    sort(finalSplitPoints->begin(),
         finalSplitPoints->end(),
         SimpleBSONObjComparator::kInstance.makeLessThan());

    // The initial splits define the "big chunks" that we will subdivide later.
    int lastIndex = -1;
    for (int i = 1; i < numShards; i++) {
        if (lastIndex < (i * numInitialChunks) / numShards - 1) {
            lastIndex = (i * numInitialChunks) / numShards - 1;
            initialSplitPoints->push_back(finalSplitPoints->at(lastIndex));
        }
    }
}

InitialSplitPolicy::ShardCollectionConfig InitialSplitPolicy::generateShardCollectionInitialChunks(
    const NamespaceString& nss,
    const ShardKeyPattern& shardKeyPattern,
    const ShardId& databasePrimaryShardId,
    const Timestamp& validAfter,
    const std::vector<BSONObj>& splitPoints,
    const std::vector<ShardId>& shardIds) {
    invariant(!shardIds.empty());

    ChunkVersion version(1, 0, OID::gen());

    const size_t numChunksToCreate = splitPoints.size() + 1;

    log() << "Going to create " << numChunksToCreate << " chunk(s) for: " << nss
          << " using new epoch " << version.epoch();

    const auto& keyPattern(shardKeyPattern.getKeyPattern());

    std::vector<ChunkType> chunks;

    for (size_t i = 0; i <= splitPoints.size(); i++) {
        const BSONObj min = (i == 0) ? keyPattern.globalMin() : splitPoints[i - 1];
        const BSONObj max = (i < splitPoints.size()) ? splitPoints[i] : keyPattern.globalMax();

        // It's possible there are no split points or fewer split points than total number of
        // shards, and we need to be sure that at least one chunk is placed on the primary shard
        const ShardId shardId = (i == 0) ? databasePrimaryShardId : shardIds[i % shardIds.size()];

        chunks.emplace_back(nss, ChunkRange(min, max), version, shardId);
        auto& chunk = chunks.back();
        chunk.setHistory({ChunkHistory(validAfter, shardId)});

        version.incMinor();
    }

    return {std::move(chunks)};
}

}  // namespace mongo
