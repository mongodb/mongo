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


#include "mongo/platform/basic.h"

#include "mongo/db/s/balancer/balance_stats.h"

#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/logv2/log.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/stdx/unordered_map.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

int64_t getMaxChunkImbalanceCount(const ChunkManager& routingInfo,
                                  const std::vector<ShardType>& allShards,
                                  const ZoneInfo& zoneInfo) {
    // Map of { zone -> { shardId -> chunkCount }}
    StringMap<stdx::unordered_map<ShardId, int64_t>> chunkDistributionPerZone;
    int64_t zoneMaxChunkImbalance = 0;

    for (const auto& shard : allShards) {
        chunkDistributionPerZone[""][shard.getName()] = 0;

        for (const auto& zone : shard.getTags()) {
            chunkDistributionPerZone[zone][shard.getName()] = 0;
        }
    }

    routingInfo.forEachChunk([&zoneInfo, &chunkDistributionPerZone](auto chunk) {
        auto zone = zoneInfo.getZoneForChunk(chunk.getRange());
        chunkDistributionPerZone[zone][chunk.getShardId()] += 1;
        return true;
    });

    for (auto&& zoneShardPair : chunkDistributionPerZone) {
        std::set<int64_t> chunkCountInShards;

        for (auto&& shardCountPair : zoneShardPair.second) {
            chunkCountInShards.insert(shardCountPair.second);
        }

        int64_t imbalance = 0;
        if (!chunkCountInShards.empty()) {
            imbalance = *chunkCountInShards.rbegin() - *chunkCountInShards.begin();
        }

        if (imbalance > zoneMaxChunkImbalance) {
            zoneMaxChunkImbalance = imbalance;
        }
    }

    return zoneMaxChunkImbalance;
}

}  // namespace mongo
