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

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/s/client/shard.h"

namespace mongo {

class BSONObj;
class OperationContext;
template <typename T>
class StatusWith;

/**
 * This interface serves as means for obtaining data distribution and shard utilization statistics
 * for the entire sharded cluster. Implementations may choose whatever means necessary to perform
 * the statistics collection. There should be one instance of this object per service context.
 */
class ClusterStatistics {
    MONGO_DISALLOW_COPYING(ClusterStatistics);

public:
    /**
     * Structure, which describes the statistics of a single shard host.
     */
    struct ShardStatistics {
    public:
        ShardStatistics();
        ShardStatistics(ShardId shardId,
                        uint64_t maxSizeMB,
                        uint64_t currSizeMB,
                        bool isDraining,
                        const std::set<std::string> shardTags,
                        const std::string mongoVersion);

        /**
         * Returns if a shard cannot receive any new chunks because it has reached the per-shard
         * data size limit.
         */
        bool isSizeMaxed() const;

        /**
         * Returns BSON representation of this shard's statistics, for reporting purposes.
         */
        BSONObj toBSON() const;

        // The id of the shard for which this statistic applies
        ShardId shardId;

        // The maximum size allowed for the shard
        uint64_t maxSizeMB{0};

        // The current size of the shard
        uint64_t currSizeMB{0};

        // Whether the shard is in draining mode
        bool isDraining{false};

        // Set of tags for the shard
        std::set<std::string> shardTags;

        // Version of mongod, which runs on this shard's primary
        std::string mongoVersion;
    };

    virtual ~ClusterStatistics();

    /**
     * Retrieves a snapshot of the current shard utilization state. The implementation of this
     * method may block if necessary in order to refresh its state or may return a cached value.
     */
    virtual StatusWith<std::vector<ShardStatistics>> getStats(OperationContext* txn) = 0;

protected:
    ClusterStatistics();
};

}  // namespace mongo
