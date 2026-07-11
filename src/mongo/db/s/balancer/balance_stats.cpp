// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/balancer/balance_stats.h"

#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/string_map.h"

#include <cstdint>
#include <iterator>
#include <set>
#include <string>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <absl/container/node_hash_map.h>

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
        auto zone = zoneInfo.getZoneForRange(chunk.getRange());
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
