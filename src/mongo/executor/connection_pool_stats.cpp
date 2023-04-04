/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/executor/connection_pool_stats.h"

#include <algorithm>
#include <fmt/format.h>
#include <fmt/ostream.h>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/util/duration.h"

namespace mongo {
namespace executor {

namespace {
constexpr auto kAcquisitionWaitTimesKey = "acquisitionWaitTimes"_sd;
}  // namespace

ConnectionStatsPer::ConnectionStatsPer(size_t nInUse,
                                       size_t nAvailable,
                                       size_t nLeased,
                                       size_t nCreated,
                                       size_t nRefreshing,
                                       size_t nRefreshed,
                                       size_t nWasNeverUsed,
                                       size_t nWasUsedOnce,
                                       Milliseconds nConnUsageTime)
    : inUse(nInUse),
      available(nAvailable),
      leased(nLeased),
      created(nCreated),
      refreshing(nRefreshing),
      refreshed(nRefreshed),
      wasNeverUsed(nWasNeverUsed),
      wasUsedOnce(nWasUsedOnce),
      connUsageTime(nConnUsageTime) {}

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
    acquisitionWaitTimes += other.acquisitionWaitTimes;

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
    acquisitionWaitTimes += newStats.acquisitionWaitTimes;
}

void ConnectionPoolStats::appendToBSON(mongo::BSONObjBuilder& result, bool forFTDC) {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    const auto isCCHMEnabled = gFeatureFlagConnHealthMetrics.isEnabledAndIgnoreFCVUnsafe();

    result.appendNumber("totalInUse", static_cast<long long>(totalInUse));
    result.appendNumber("totalAvailable", static_cast<long long>(totalAvailable));
    result.appendNumber("totalLeased", static_cast<long long>(totalLeased));
    result.appendNumber("totalCreated", static_cast<long long>(totalCreated));
    result.appendNumber("totalRefreshing", static_cast<long long>(totalRefreshing));
    result.appendNumber("totalRefreshed", static_cast<long long>(totalRefreshed));
    if (isCCHMEnabled) {
        result.appendNumber("totalWasNeverUsed", static_cast<long long>(totalWasNeverUsed));
        if (forFTDC) {
            result.appendNumber("totalWasUsedOnce", static_cast<long long>(totalWasUsedOnce));
            result.appendNumber("totalConnUsageTimeMillis",
                                durationCount<Milliseconds>(totalConnUsageTime));
        }
    }

    if (forFTDC) {
        BSONObjBuilder poolBuilder(result.subobjStart("pools"));
        for (const auto& [pool, stats] : statsByPool) {
            BSONObjBuilder poolInfo(poolBuilder.subobjStart(pool));
            poolInfo.appendNumber("poolInUse", static_cast<long long>(stats.inUse));
            if (isCCHMEnabled) {
                poolInfo.appendNumber("poolWasUsedOnce", static_cast<long long>(stats.wasUsedOnce));
                poolInfo.appendNumber("poolConnUsageTimeMillis",
                                      durationCount<Milliseconds>(stats.connUsageTime));
            }
            for (const auto& [host, stats] : stats.statsByHost) {
                BSONObjBuilder hostInfo(poolInfo.subobjStart(host.toString()));
                poolInfo.appendNumber("inUse", static_cast<long long>(stats.inUse));
            }
        }

        return;
    }

    appendHistogram(result, acquisitionWaitTimes, kAcquisitionWaitTimesKey);

    // Process pools stats.
    {
        if (strategy) {
            result.append("replicaSetMatchingStrategy", matchingStrategyToString(*strategy));
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
            if (isCCHMEnabled) {
                poolInfo.appendNumber("poolWasNeverUsed",
                                      static_cast<long long>(stats.wasNeverUsed));
                appendHistogram(poolInfo, stats.acquisitionWaitTimes, kAcquisitionWaitTimesKey);
            }

            for (const auto& [host, stats] : stats.statsByHost) {
                BSONObjBuilder hostInfo(poolInfo.subobjStart(host.toString()));
                hostInfo.appendNumber("inUse", static_cast<long long>(stats.inUse));
                hostInfo.appendNumber("available", static_cast<long long>(stats.available));
                hostInfo.appendNumber("leased", static_cast<long long>(stats.leased));
                hostInfo.appendNumber("created", static_cast<long long>(stats.created));
                hostInfo.appendNumber("refreshing", static_cast<long long>(stats.refreshing));
                hostInfo.appendNumber("refreshed", static_cast<long long>(stats.refreshed));
                if (isCCHMEnabled) {
                    hostInfo.appendNumber("wasNeverUsed",
                                          static_cast<long long>(stats.wasNeverUsed));
                    appendHistogram(hostInfo, stats.acquisitionWaitTimes, kAcquisitionWaitTimesKey);
                }
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
            if (isCCHMEnabled) {
                hostInfo.appendNumber("wasNeverUsed", static_cast<long long>(stats.wasNeverUsed));
                appendHistogram(hostInfo, stats.acquisitionWaitTimes, kAcquisitionWaitTimesKey);
            }
        }
    }
}

namespace {
std::vector<Milliseconds> makePartitions() {
    std::vector<Milliseconds> result;
    for (int64_t ms = 0; ms <= 1000; ms += 50) {
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
