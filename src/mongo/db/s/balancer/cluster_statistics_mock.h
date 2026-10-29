// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/s/balancer/cluster_statistics.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Mock implementation for the cluster statistics gathering utility.
 * Empty/missing stats values trigger a failure when getStats()/getCollStats() are invoked.
 */
class ClusterStatisticsMock final : public ClusterStatistics {
public:
    ClusterStatisticsMock() {}

    ~ClusterStatisticsMock() override {}

    void setStats(std::vector<ShardStatistics>&& clusterStats) {
        _clusterStats.swap(clusterStats);
    }

    StatusWith<std::vector<ShardStatistics>> getStats(OperationContext* opCtx) override {

        if (_clusterStats.empty()) {
            return Status(ErrorCodes::DataCorruptionDetected,
                          "Failure on getStats() raised by mock");
        }
        return _clusterStats;
    }

private:
    std::vector<ShardStatistics> _clusterStats;

    bool _forceErrorOnGetStats{false};

    bool _forceErrorOnGetCollStats{false};
};

}  // namespace mongo
