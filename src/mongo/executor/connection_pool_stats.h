// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/connection_pool_state.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/duration.h"
#include "mongo/util/histogram.h"
#include "mongo/util/modules.h"
#include "mongo/util/moving_average.h"
#include "mongo/util/net/hostandport.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace [[MONGO_MOD_PUBLIC]] executor {
namespace details {
constexpr inline auto kStartSize = 0;
constexpr inline auto kPartitionStepSize = 50;
constexpr inline auto kMaxPartitionSize = 1000;
}  // namespace details

/**
 * Histogram type to be used for tracking how long it took the connection pool to return a
 * requested connection.
 */
class ConnectionWaitTimeHistogram
    : public Histogram<Milliseconds, std::less<Milliseconds>, int64_t> {
public:
    ConnectionWaitTimeHistogram();
    ConnectionWaitTimeHistogram& operator+=(const ConnectionWaitTimeHistogram& other);
};

/**
 * Holds connection information for a specific pool or remote host. These objects are maintained by
 * a parent ConnectionPoolStats object and should not need to be created directly.
 */
struct ConnectionStatsPer {
    ConnectionStatsPer(size_t nInUse,
                       size_t nAvailable,
                       size_t nLeased,
                       size_t nCreated,
                       size_t nRefreshing,
                       size_t nRefreshed,
                       size_t nWasNeverUsed,
                       size_t nWasUsedOnce,
                       Milliseconds nConnUsageTime,
                       size_t nRejectedConnectionsCount,
                       size_t nPendingRequestsCount,
                       size_t nTotalConnectionAcquisitionRequests,
                       Milliseconds nTotalConnectionAcquisitionWaitTime,
                       ConnectionPoolState nPoolState);

    ConnectionStatsPer();

    ConnectionStatsPer& operator+=(const ConnectionStatsPer& other);

    void appendToBSON(mongo::BSONObjBuilder& result) const;

    size_t inUse = 0u;
    size_t available = 0u;
    size_t leased = 0u;
    size_t created = 0u;
    size_t refreshing = 0u;
    size_t refreshed = 0u;
    size_t wasNeverUsed = 0u;
    size_t wasUsedOnce = 0u;
    Milliseconds connUsageTime{0};
    size_t rejectedRequests = 0u;
    size_t pendingRequests = 0u;
    ConnectionWaitTimeHistogram acquisitionWaitTimes{};
    size_t connectionAcquisitionRequests = 0u;
    Milliseconds connectionAcquisitionWaitTime = Milliseconds::zero();
    ConnectionPoolState poolState{ConnectionPoolState::kHealthy};
};

/**
 * Aggregates connection information for the connPoolStats command. Connection pools should
 * use the updateStatsForHost() method to append their host-specific information to this object.
 * Total connection counts will then be updated accordingly.
 */
struct ConnectionPoolStats {
    void updateStatsForHost(std::string pool, HostAndPort host, const ConnectionStatsPer& newStats);

    // FTDC : Full Time Diagnostic Data Collection
    void appendToBSON(mongo::BSONObjBuilder& result, bool forFTDC = false);

    size_t totalInUse = 0u;
    size_t totalAvailable = 0u;
    size_t totalLeased = 0u;
    size_t totalCreated = 0u;
    size_t totalRefreshing = 0u;
    size_t totalRefreshed = 0u;
    size_t totalWasNeverUsed = 0u;
    size_t totalWasUsedOnce = 0u;
    Milliseconds totalConnUsageTime{0};
    size_t totalRejectedRequests = 0u;
    size_t totalPendingRequests = 0u;

    // The ShardingTaskExecutorPoolController::MatchingStrategy in use by this pool, if any.
    boost::optional<std::string> matchingStrategy;

    ConnectionWaitTimeHistogram acquisitionWaitTimes{};
    size_t totalConnectionAcquisitionRequests = 0u;
    Milliseconds totalConnectionAcquisitionWaitTime = Milliseconds::zero();

    using StatsByHost = std::map<HostAndPort, ConnectionStatsPer>;

    struct PoolStats final : public ConnectionStatsPer {
        StatsByHost statsByHost;
    };
    using StatsByPool = std::map<std::string, PoolStats>;

    StatsByHost statsByHost;
    StatsByPool statsByPool;
};

}  // namespace executor
}  // namespace mongo
