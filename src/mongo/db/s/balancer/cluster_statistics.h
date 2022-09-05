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

#pragma once

#include <set>
#include <string>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_id.h"

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
        // hack so we do not accidentally miss code using MB
        struct use_bytes_t {
            explicit use_bytes_t() = default;
        };
        ShardStatistics(ShardId shardId,
                        uint64_t currSizeBytes,
                        bool isDraining,
                        std::set<std::string> shardZones,
                        std::string mongoVersion,
                        use_bytes_t t);

        ShardStatistics(ShardId shardId,
                        uint64_t currSizeMB,
                        bool isDraining,
                        std::set<std::string> shardZones,
                        std::string mongoVersion);

        // The id of the shard for which this statistic applies
        ShardId shardId;

        // The current storage size of the shard.
        uint64_t currSizeBytes{0};

        // Whether the shard is in draining mode
        bool isDraining{false};

        // Set of zones for the shard
        std::set<std::string> shardZones;

        // Version of mongod, which runs on this shard's primary
        std::string mongoVersion;
    };

    virtual ~ClusterStatistics();

    /**
     * Retrieves a snapshot of the current shard utilization state. The implementation of this
     * method may block if necessary in order to refresh its state or may return a cached value.
     */
    virtual StatusWith<std::vector<ShardStatistics>> getStats(OperationContext* opCtx) = 0;

    virtual StatusWith<std::vector<ShardStatistics>> getCollStats(OperationContext* opCtx,
                                                                  NamespaceString const& ns) = 0;

protected:
    ClusterStatistics();
};

}  // namespace mongo
