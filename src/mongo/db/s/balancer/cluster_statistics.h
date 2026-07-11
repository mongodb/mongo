// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace mongo {

class BSONObj;

class OperationContext;

/**
 * This interface serves as means for obtaining data distribution and shard utilization statistics
 * for the entire sharded cluster. Implementations may choose whatever means necessary to perform
 * the statistics collection. There should be one instance of this object per service context.
 */
class ClusterStatistics {
    ClusterStatistics(const ClusterStatistics&) = delete;
    ClusterStatistics& operator=(const ClusterStatistics&) = delete;

public:
    /**
     * Structure, which describes the statistics of a single shard host.
     */
    struct ShardStatistics {
    public:
        ShardStatistics(ShardId shardId, bool isDraining, std::set<std::string> shardZones);

        // The id of the shard for which this statistic applies
        ShardId shardId;

        // Whether the shard is in draining mode
        bool isDraining{false};

        // Set of zones for the shard
        std::set<std::string> shardZones;
    };
    virtual ~ClusterStatistics();

    /**
     * Retrieves a snapshot of the current state of the shards. The implementation of this
     * method may block if necessary in order to refresh its state or may return a cached value.
     */
    virtual StatusWith<std::vector<ShardStatistics>> getStats(OperationContext* opCtx) = 0;

protected:
    ClusterStatistics();
};

}  // namespace mongo
