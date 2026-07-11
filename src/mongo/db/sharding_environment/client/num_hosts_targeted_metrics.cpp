// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/sharding_environment/client/num_hosts_targeted_metrics.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/decorable.h"

#include <string>
#include <utility>

namespace mongo {
namespace {

const auto getNumHostsTargeted = ServiceContext::declareDecoration<NumHostsTargetedMetrics>();

std::string queryTypeToString(NumHostsTargetedMetrics::QueryType queryType) {
    switch (queryType) {
        case NumHostsTargetedMetrics::QueryType::kFindCmd:
            return "find";
        case NumHostsTargetedMetrics::QueryType::kInsertCmd:
            return "insert";
        case NumHostsTargetedMetrics::QueryType::kUpdateCmd:
            return "update";
        case NumHostsTargetedMetrics::QueryType::kDeleteCmd:
            return "delete";
        case NumHostsTargetedMetrics::QueryType::kAggregateCmd:
            return "aggregate";
        default:
            return "";
    }
}

}  // namespace

void NumHostsTargetedMetrics::addNumHostsTargeted(NumHostsTargetedMetrics::QueryType queryType,
                                                  NumHostsTargetedMetrics::TargetType targetType) {
    switch (targetType) {
        case TargetType::kAllShards:
            _numHostsTargeted[queryType]->allShards.addAndFetch(1);
            return;
        case TargetType::kManyShards:
            _numHostsTargeted[queryType]->manyShards.addAndFetch(1);
            return;
        case TargetType::kOneShard:
            _numHostsTargeted[queryType]->oneShard.addAndFetch(1);
            return;
        case TargetType::kUnsharded:
            _numHostsTargeted[queryType]->unsharded.addAndFetch(1);
            return;
    }
}

void NumHostsTargetedMetrics::report(BSONObjBuilder* builder) const {
    BSONObjBuilder numHostsTargetedStatsBuilder(builder->subobjStart("numHostsTargeted"));
    for (auto i = 0; i < kNumQueryType; i++) {
        auto& targetStat = _numHostsTargeted[i];
        auto queryType = static_cast<QueryType>(i);
        BSONObjBuilder queryStatsBuilder(
            numHostsTargetedStatsBuilder.subobjStart(queryTypeToString(queryType)));
        queryStatsBuilder.appendNumber("allShards", targetStat->allShards.load());
        queryStatsBuilder.appendNumber("manyShards", targetStat->manyShards.load());
        queryStatsBuilder.appendNumber("oneShard", targetStat->oneShard.load());
        queryStatsBuilder.appendNumber("unsharded", targetStat->unsharded.load());
    }
}

NumHostsTargetedMetrics::TargetType NumHostsTargetedMetrics::parseTargetType(
    OperationContext* opCtx, int nShardsTargeted, int nShardsOwningChunks, bool isSharded) {
    // If nShardsOwningChunks == 0, this means the routing info did not contain a chunk manager so
    // the collection is unsharded.
    if (!isSharded || nShardsOwningChunks == 0)
        return TargetType::kUnsharded;

    if (nShardsTargeted == 1)
        return TargetType::kOneShard;

    // If an update or delete targets > 1 shard, it will be broadcast to all shards in the cluster
    // *regardless* of whether the shard owns any data for that collection. We should count this as
    // 'allShards'.
    if (nShardsTargeted >= nShardsOwningChunks)
        return TargetType::kAllShards;

    return TargetType::kManyShards;
}

NumHostsTargetedMetrics& NumHostsTargetedMetrics::get(ServiceContext* serviceContext) {
    return getNumHostsTargeted(serviceContext);
}

NumHostsTargetedMetrics& NumHostsTargetedMetrics::get(OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void NumHostsTargetedMetrics::startup() {
    _numHostsTargeted.reserve(kNumQueryType);
    for (auto i = 0; i < kNumQueryType; i++) {
        _numHostsTargeted.push_back(std::make_unique<TargetStats>());
    }
}

}  // namespace mongo
