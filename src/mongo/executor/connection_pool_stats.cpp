// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/connection_pool_stats.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/util/duration.h"

#include <algorithm>
#include <iosfwd>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>
#include <fmt/ostream.h>

namespace mongo {
namespace executor {

namespace {
using namespace std::literals::string_view_literals;
constexpr auto kAcquisitionWaitTimesKey = "acquisitionWaitTimes"sv;
}  // namespace

ConnectionStatsPer::ConnectionStatsPer(size_t nInUse,
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
                                       ConnectionPoolState nPoolState)
    : inUse(nInUse),
      available(nAvailable),
      leased(nLeased),
      created(nCreated),
      refreshing(nRefreshing),
      refreshed(nRefreshed),
      wasNeverUsed(nWasNeverUsed),
      wasUsedOnce(nWasUsedOnce),
      connUsageTime(nConnUsageTime),
      rejectedRequests(nRejectedConnectionsCount),
      pendingRequests(nPendingRequestsCount),
      connectionAcquisitionRequests(nTotalConnectionAcquisitionRequests),
      connectionAcquisitionWaitTime(nTotalConnectionAcquisitionWaitTime),
      poolState(nPoolState) {}

ConnectionStatsPer::ConnectionStatsPer() = default;

ConnectionStatsPer& ConnectionStatsPer::operator+=(const ConnectionStatsPer& other) {
    inUse += other.inUse;
    available += other.available;
    leased += other.leased;
    created += other.created;
    refreshing += other.refreshing;
    refreshed += other.refreshed;
    wasNeverUsed += other.wasNeverUsed;
    wasUsedOnce += other.wasUsedOnce;
    connUsageTime += other.connUsageTime;
    rejectedRequests += other.rejectedRequests;
    acquisitionWaitTimes += other.acquisitionWaitTimes;
    connectionAcquisitionRequests += other.connectionAcquisitionRequests;
    connectionAcquisitionWaitTime += other.connectionAcquisitionWaitTime;
    pendingRequests += other.pendingRequests;
    poolState = other.poolState;

    return *this;
}

void ConnectionPoolStats::updateStatsForHost(std::string pool,
                                             HostAndPort host,
                                             const ConnectionStatsPer& newStats) {
    if (newStats.created == 0) {
        // A pool that has never been successfully used does not get listed
        return;
    }

    // Update stats for this pool
    auto& byPool = statsByPool[pool];
    byPool += newStats;

    // Update stats for this host.
    statsByHost[host] += newStats;
    byPool.statsByHost[host] += newStats;

    // Update total connection stats.
    totalInUse += newStats.inUse;
    totalAvailable += newStats.available;
    totalLeased += newStats.leased;
    totalCreated += newStats.created;
    totalRefreshing += newStats.refreshing;
    totalRefreshed += newStats.refreshed;
    totalWasNeverUsed += newStats.wasNeverUsed;
    totalWasUsedOnce += newStats.wasUsedOnce;
    totalConnUsageTime += newStats.connUsageTime;
    totalRejectedRequests += newStats.rejectedRequests;
    acquisitionWaitTimes += newStats.acquisitionWaitTimes;
    totalConnectionAcquisitionRequests += newStats.connectionAcquisitionRequests;
    totalConnectionAcquisitionWaitTime += newStats.connectionAcquisitionWaitTime;
    totalPendingRequests += newStats.pendingRequests;
}

void ConnectionPoolStats::appendToBSON(mongo::BSONObjBuilder& result, bool forFTDC) {
    result.appendNumber("totalInUse", static_cast<long long>(totalInUse));
    result.appendNumber("totalAvailable", static_cast<long long>(totalAvailable));
    result.appendNumber("totalLeased", static_cast<long long>(totalLeased));
    result.appendNumber("totalCreated", static_cast<long long>(totalCreated));
    result.appendNumber("totalRefreshing", static_cast<long long>(totalRefreshing));
    result.appendNumber("totalRefreshed", static_cast<long long>(totalRefreshed));
    result.appendNumber("totalWasNeverUsed", static_cast<long long>(totalWasNeverUsed));
    result.appendNumber("totalWasUsedOnce", static_cast<long long>(totalWasUsedOnce));
    result.appendNumber("totalConnUsageTimeMillis",
                        durationCount<Milliseconds>(totalConnUsageTime));
    result.appendNumber("totalRejectedRequests", static_cast<long long>(totalRejectedRequests));
    result.appendNumber("totalPendingRequests", static_cast<long long>(totalPendingRequests));
    result.append("totalConnectionAcquisitionRequests",
                  static_cast<long long>(totalConnectionAcquisitionRequests));
    result.append("totalConnectionAcquisitionWaitTimeMillis",
                  durationCount<Milliseconds>(totalConnectionAcquisitionWaitTime));

    appendHistogram(result, acquisitionWaitTimes, kAcquisitionWaitTimesKey);

    if (forFTDC) {
        BSONObjBuilder poolBuilder(result.subobjStart("pools"));
        for (const auto& [pool, stats] : statsByPool) {
            BSONObjBuilder poolInfo(poolBuilder.subobjStart(pool));
            poolInfo.appendNumber("poolInUse", static_cast<long long>(stats.inUse));
            poolInfo.appendNumber("poolWasUsedOnce", static_cast<long long>(stats.wasUsedOnce));
            poolInfo.appendNumber("poolConnUsageTimeMillis",
                                  durationCount<Milliseconds>(stats.connUsageTime));
            for (const auto& [host, stats] : stats.statsByHost) {
                BSONObjBuilder hostInfo(poolInfo.subobjStart(host.toString()));
                poolInfo.appendNumber("inUse", static_cast<long long>(stats.inUse));
            }
        }

        return;
    }

    // Process pools stats.
    {
        if (matchingStrategy) {
            result.append("replicaSetMatchingStrategy", *matchingStrategy);
        }

        BSONObjBuilder poolBuilder(result.subobjStart("pools"));
        for (const auto& [pool, stats] : statsByPool) {
            BSONObjBuilder poolInfo(poolBuilder.subobjStart(pool));
            poolInfo.appendNumber("poolInUse", static_cast<long long>(stats.inUse));
            poolInfo.appendNumber("poolAvailable", static_cast<long long>(stats.available));
            poolInfo.appendNumber("poolLeased", static_cast<long long>(stats.leased));
            poolInfo.appendNumber("poolCreated", static_cast<long long>(stats.created));
            poolInfo.appendNumber("poolRefreshing", static_cast<long long>(stats.refreshing));
            poolInfo.appendNumber("poolRefreshed", static_cast<long long>(stats.refreshed));
            poolInfo.appendNumber("poolWasNeverUsed", static_cast<long long>(stats.wasNeverUsed));
            poolInfo.appendNumber("poolRejectedRequests",
                                  static_cast<long long>(stats.rejectedRequests));
            poolInfo.appendNumber("poolPendingRequests",
                                  static_cast<long long>(stats.pendingRequests));
            poolInfo.appendNumber("poolConnectionAcquisitionRequests",
                                  static_cast<long long>(stats.connectionAcquisitionRequests));
            poolInfo.appendNumber("poolConnectionAcquisitionWaitTimeMillis",
                                  durationCount<Milliseconds>(stats.connectionAcquisitionWaitTime));
            appendHistogram(poolInfo, stats.acquisitionWaitTimes, kAcquisitionWaitTimesKey);

            for (const auto& [host, stats] : stats.statsByHost) {
                BSONObjBuilder hostInfo(poolInfo.subobjStart(host.toString()));
                hostInfo.appendNumber("inUse", static_cast<long long>(stats.inUse));
                hostInfo.appendNumber("available", static_cast<long long>(stats.available));
                hostInfo.appendNumber("leased", static_cast<long long>(stats.leased));
                hostInfo.appendNumber("created", static_cast<long long>(stats.created));
                hostInfo.appendNumber("refreshing", static_cast<long long>(stats.refreshing));
                hostInfo.appendNumber("refreshed", static_cast<long long>(stats.refreshed));
                hostInfo.appendNumber("wasNeverUsed", static_cast<long long>(stats.wasNeverUsed));
                hostInfo.appendNumber("rejectedRequests",
                                      static_cast<long long>(stats.rejectedRequests));
                hostInfo.appendNumber("pendingRequests",
                                      static_cast<long long>(stats.pendingRequests));
                hostInfo.appendNumber("connectionAcquisitionRequests",
                                      static_cast<long long>(stats.connectionAcquisitionRequests));
                hostInfo.appendNumber(
                    "connectionAcquisitionWaitTimeMillis",
                    durationCount<Milliseconds>(stats.connectionAcquisitionWaitTime));
                appendHistogram(hostInfo, stats.acquisitionWaitTimes, kAcquisitionWaitTimesKey);
                hostInfo.appendNumber("poolState", static_cast<int>(stats.poolState));
            }
        }
    }

    // Processes hosts stats.
    {
        BSONObjBuilder hostBuilder(result.subobjStart("hosts"));
        for (const auto& [host, stats] : statsByHost) {
            BSONObjBuilder hostInfo(hostBuilder.subobjStart(host.toString()));
            hostInfo.appendNumber("inUse", static_cast<long long>(stats.inUse));
            hostInfo.appendNumber("available", static_cast<long long>(stats.available));
            hostInfo.appendNumber("leased", static_cast<long long>(stats.leased));
            hostInfo.appendNumber("created", static_cast<long long>(stats.created));
            hostInfo.appendNumber("refreshing", static_cast<long long>(stats.refreshing));
            hostInfo.appendNumber("refreshed", static_cast<long long>(stats.refreshed));
            hostInfo.appendNumber("wasNeverUsed", static_cast<long long>(stats.wasNeverUsed));
            hostInfo.appendNumber("rejectedRequests",
                                  static_cast<long long>(stats.rejectedRequests));
            hostInfo.appendNumber("pendingRequests", static_cast<long long>(stats.pendingRequests));
            hostInfo.appendNumber("connectionAcquisitionRequests",
                                  static_cast<long long>(stats.connectionAcquisitionRequests));
            hostInfo.appendNumber("connectionAcquisitionWaitTimeMillis",
                                  durationCount<Milliseconds>(stats.connectionAcquisitionWaitTime));
            appendHistogram(hostInfo, stats.acquisitionWaitTimes, kAcquisitionWaitTimesKey);
            hostInfo.appendNumber("poolState", static_cast<int>(stats.poolState));
        }
    }
}

namespace {
std::vector<Milliseconds> makePartitions() {
    std::vector<Milliseconds> result;
    for (int64_t ms = details::kStartSize; ms <= details::kMaxPartitionSize;
         ms += details::kPartitionStepSize) {
        result.push_back(Milliseconds(ms));
    }
    return result;
}
}  // namespace

ConnectionWaitTimeHistogram::ConnectionWaitTimeHistogram() : Histogram(makePartitions()) {}

ConnectionWaitTimeHistogram& ConnectionWaitTimeHistogram::operator+=(
    const ConnectionWaitTimeHistogram& other) {
    std::transform(
        _counts.begin(), _counts.end(), other._counts.begin(), _counts.begin(), std::plus{});
    return *this;
}

}  // namespace executor
}  // namespace mongo
