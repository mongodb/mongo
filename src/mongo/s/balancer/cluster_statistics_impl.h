/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <map>

#include "mongo/s/balancer/cluster_statistics.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class OperationContext;
class Status;

/**
 * Default implementation for the cluster statistics gathering utility. Uses a blocking method to
 * fetch the statistics and does not perform any caching.
 */
class ClusterStatisticsImpl final : public ClusterStatistics {
public:
    ClusterStatisticsImpl();
    ~ClusterStatisticsImpl();

    StatusWith<std::vector<ShardStatistics>> getStats(OperationContext* txn) override;

private:
    typedef std::map<ShardId, ShardStatistics> ShardStatisticsMap;

    /**
     * Refreshes the list of available shards and loops through them in order to collect usage
     * statistics. If any of the shards fails to report statistics, skips it and continues with the
     * next.
     *
     * If the list of shards cannot be retrieved throws an exception.
     */
    void _refreshShardStats(OperationContext* txn);

    // Mutex to protect the mutable state below
    stdx::mutex _mutex;

    // The most up-to-date shard statistics
    ShardStatisticsMap _shardStatsMap;
};

}  // namespace mongo
