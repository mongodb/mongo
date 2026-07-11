// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/cluster_statistics.h"
#include "mongo/util/modules.h"

#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Default implementation for the cluster statistics gathering utility. Uses a blocking method to
 * fetch the statistics and does not perform any caching. If any of the shards fails to report
 * statistics fails the entire refresh.
 */
class ClusterStatisticsImpl final : public ClusterStatistics {
public:
    ~ClusterStatisticsImpl() override;

    StatusWith<std::vector<ShardStatistics>> getStats(OperationContext* opCtx) override;
};

}  // namespace mongo
