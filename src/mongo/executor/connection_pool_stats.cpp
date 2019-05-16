
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
#include "mongo/util/map_util.h"

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
    // Update stats for this host.
    statsByPool[pool] += newStats;
    statsByHost[host] += newStats;
    statsByPoolHost[pool][host] += newStats;

    // Update total connection stats.
    totalInUse += newStats.inUse;
    totalAvailable += newStats.available;
    totalCreated += newStats.created;
    totalRefreshing += newStats.refreshing;
}

void ConnectionPoolStats::appendToBSON(mongo::BSONObjBuilder& result, bool forFTDC) {
    result.appendNumber("totalInUse", totalInUse);
    result.appendNumber("totalAvailable", totalAvailable);
    result.appendNumber("totalCreated", totalCreated);
    result.appendNumber("totalRefreshing", totalRefreshing);

    if (forFTDC) {
        BSONObjBuilder poolBuilder(result.subobjStart("connectionsInUsePerPool"));
        for (const auto& pool : statsByPool) {
            BSONObjBuilder poolInfo(poolBuilder.subobjStart(pool.first));
            auto& poolStats = pool.second;
            poolInfo.appendNumber("poolInUse", poolStats.inUse);
            for (const auto& host : statsByPoolHost[pool.first]) {
                auto hostStats = host.second;
                poolInfo.appendNumber(host.first.toString(), hostStats.inUse);
            }
        }

        return;
    }

    {
        BSONObjBuilder poolBuilder(result.subobjStart("pools"));
        for (const auto& pool : statsByPool) {
            BSONObjBuilder poolInfo(poolBuilder.subobjStart(pool.first));
            auto& poolStats = pool.second;
            poolInfo.appendNumber("poolInUse", poolStats.inUse);
            poolInfo.appendNumber("poolAvailable", poolStats.available);
            poolInfo.appendNumber("poolCreated", poolStats.created);
            poolInfo.appendNumber("poolRefreshing", poolStats.refreshing);
            for (const auto& host : statsByPoolHost[pool.first]) {
                BSONObjBuilder hostInfo(poolInfo.subobjStart(host.first.toString()));
                auto& hostStats = host.second;
                hostInfo.appendNumber("inUse", hostStats.inUse);
                hostInfo.appendNumber("available", hostStats.available);
                hostInfo.appendNumber("created", hostStats.created);
                hostInfo.appendNumber("refreshing", hostStats.refreshing);
            }
        }
    }
    {
        BSONObjBuilder hostBuilder(result.subobjStart("hosts"));
        for (auto&& host : statsByHost) {
            BSONObjBuilder hostInfo(hostBuilder.subobjStart(host.first.toString()));
            auto hostStats = host.second;
            hostInfo.appendNumber("inUse", hostStats.inUse);
            hostInfo.appendNumber("available", hostStats.available);
            hostInfo.appendNumber("created", hostStats.created);
            hostInfo.appendNumber("refreshing", hostStats.refreshing);
        }
    }
}

}  // namespace executor
}  // namespace mongo
