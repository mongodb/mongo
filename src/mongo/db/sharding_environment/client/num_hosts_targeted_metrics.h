// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>


namespace mongo {

class BSONObjBuilder;
class OperationContext;
class ServiceContext;

class [[MONGO_MOD_PUBLIC]] NumHostsTargetedMetrics {
public:
    enum QueryType {
        kFindCmd,
        kInsertCmd,
        kUpdateCmd,
        kDeleteCmd,
        kAggregateCmd,
        kNumQueryType,
    };

    enum TargetType { kAllShards, kManyShards, kOneShard, kUnsharded };

    void addNumHostsTargeted(QueryType queryType, TargetType targetType);

    void report(BSONObjBuilder* builder) const;

    TargetType parseTargetType(OperationContext* opCtx,
                               int nShardsTargeted,
                               int nShardsOwningChunks,
                               bool isSharded);

    static NumHostsTargetedMetrics& get(ServiceContext* serviceContext);
    static NumHostsTargetedMetrics& get(OperationContext* opCtx);

    void startup();

private:
    struct TargetStats {
        // the op targeted all shards that own chunks for the collection as long as the total number
        // of shards in the cluster is > 1
        Atomic<long long> allShards;

        // the op targeted > 1 shard that own chunks for the collection as long as the total number
        // of shards in the cluster is > 1
        Atomic<long long> manyShards;

        // the op targeted 1 shard (if there exists only one shard, the metric will count as
        // 'oneShard')
        Atomic<long long> oneShard;

        // the collection is unsharded
        Atomic<long long> unsharded;
    };

    std::vector<std::unique_ptr<TargetStats>> _numHostsTargeted;
};

}  // namespace mongo
