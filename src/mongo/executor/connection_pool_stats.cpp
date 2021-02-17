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

#include "mongo/bson/bsonobjbuilder.h"

namespace mongo {
namespace executor {

ConnectionStatsPer::ConnectionStatsPer(size_t nInUse,
                                       size_t nAvailable,
                                       size_t nCreated,
                                       size_t nRefreshing)
    : inUse(nInUse), available(nAvailable), created(nCreated), refreshing(nRefreshing) {}

ConnectionStatsPer::ConnectionStatsPer() = default;

ConnectionStatsPer& ConnectionStatsPer::operator+=(const ConnectionStatsPer& other) {
    inUse += other.inUse;
    available += other.available;
    created += other.created;
    refreshing += other.refreshing;

    return *this;
}

void ConnectionPoolStats::updateStatsForHost(std::string pool,
                                             HostAndPort host,
                                             ConnectionStatsPer newStats) {
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
    totalCreated += newStats.created;
    totalRefreshing += newStats.refreshing;
}

void ConnectionPoolStats::appendToBSON(mongo::BSONObjBuilder& result, bool forFTDC) {
    result.appendNumber("totalInUse", static_cast<long long>(totalInUse));
    result.appendNumber("totalAvailable", static_cast<long long>(totalAvailable));
    result.appendNumber("totalCreated", static_cast<long long>(totalCreated));
    result.appendNumber("totalRefreshing", static_cast<long long>(totalRefreshing));

    if (forFTDC) {
        BSONObjBuilder poolBuilder(result.subobjStart("connectionsInUsePerPool"));
        for (const auto& pool : statsByPool) {
            BSONObjBuilder poolInfo(poolBuilder.subobjStart(pool.first));
            auto& poolStats = pool.second;
            poolInfo.appendNumber("poolInUse", static_cast<long long>(poolStats.inUse));
            for (const auto& host : poolStats.statsByHost) {
                auto hostStats = host.second;
                poolInfo.appendNumber(host.first.toString(),
                                      static_cast<long long>(hostStats.inUse));
            }
        }

        return;
    }

    {
        if (strategy) {
            result.append("replicaSetMatchingStrategy", matchingStrategyToString(*strategy));
        }

        BSONObjBuilder poolBuilder(result.subobjStart("pools"));
        for (const auto& pool : statsByPool) {
            BSONObjBuilder poolInfo(poolBuilder.subobjStart(pool.first));
            auto& poolStats = pool.second;
            poolInfo.appendNumber("poolInUse", static_cast<long long>(poolStats.inUse));
            poolInfo.appendNumber("poolAvailable", static_cast<long long>(poolStats.available));
            poolInfo.appendNumber("poolCreated", static_cast<long long>(poolStats.created));
            poolInfo.appendNumber("poolRefreshing", static_cast<long long>(poolStats.refreshing));

            for (const auto& host : poolStats.statsByHost) {
                BSONObjBuilder hostInfo(poolInfo.subobjStart(host.first.toString()));
                auto& hostStats = host.second;
                hostInfo.appendNumber("inUse", static_cast<long long>(hostStats.inUse));
                hostInfo.appendNumber("available", static_cast<long long>(hostStats.available));
                hostInfo.appendNumber("created", static_cast<long long>(hostStats.created));
                hostInfo.appendNumber("refreshing", static_cast<long long>(hostStats.refreshing));
            }
        }
    }
    {
        BSONObjBuilder hostBuilder(result.subobjStart("hosts"));
        for (auto&& host : statsByHost) {
            BSONObjBuilder hostInfo(hostBuilder.subobjStart(host.first.toString()));
            auto hostStats = host.second;
            hostInfo.appendNumber("inUse", static_cast<long long>(hostStats.inUse));
            hostInfo.appendNumber("available", static_cast<long long>(hostStats.available));
            hostInfo.appendNumber("created", static_cast<long long>(hostStats.created));
            hostInfo.appendNumber("refreshing", static_cast<long long>(hostStats.refreshing));
        }
    }
}

}  // namespace executor
}  // namespace mongo
