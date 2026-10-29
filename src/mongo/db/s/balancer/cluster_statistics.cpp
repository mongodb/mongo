// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/balancer/cluster_statistics.h"

#include <utility>

namespace mongo {

ClusterStatistics::ClusterStatistics() = default;

ClusterStatistics::~ClusterStatistics() = default;

ClusterStatistics::ShardStatistics::ShardStatistics(ShardId inShardId,
                                                    bool inIsDraining,
                                                    std::set<std::string> inShardZones)
    : shardId(std::move(inShardId)),
      isDraining(inIsDraining),
      shardZones(std::move(inShardZones)) {}
}  // namespace mongo
