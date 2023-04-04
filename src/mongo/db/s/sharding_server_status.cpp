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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/metrics/sharding_data_transform_cumulative_metrics.h"
#include "mongo/db/s/range_deleter_service.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/vector_clock.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/is_mongos.h"
#include "mongo/s/resharding/resharding_feature_flag_gen.h"
#include "mongo/s/sharding_feature_flags_gen.h"

namespace mongo {
namespace {

class ShardingServerStatus final : public ServerStatusSection {
public:
    ShardingServerStatus() : ServerStatusSection("sharding") {}

    bool includeByDefault() const override {
        return isClusterNode();
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!isClusterNode())
            return {};

        auto const shardingState = ShardingState::get(opCtx);
        if (!shardingState->enabled())
            return {};

        auto const grid = Grid::get(opCtx);
        auto const shardRegistry = grid->shardRegistry();

        BSONObjBuilder result;

        result.append("configsvrConnectionString",
                      shardRegistry->getConfigServerConnectionString().toString());

        const auto vcTime = VectorClock::get(opCtx)->getTime();

        const auto configOpTime = [&]() {
            const auto vcConfigTimeTs = vcTime.configTime().asTimestamp();
            return mongo::repl::OpTime(vcConfigTimeTs, mongo::repl::OpTime::kUninitializedTerm);
        }();
        configOpTime.append(&result, "lastSeenConfigServerOpTime");

        const auto topologyOpTime = [&]() {
            const auto vcTopologyTimeTs = vcTime.topologyTime().asTimestamp();
            return mongo::repl::OpTime(vcTopologyTimeTs, mongo::repl::OpTime::kUninitializedTerm);
        }();
        topologyOpTime.append(&result, "lastSeenTopologyOpTime");

        const long long maxChunkSizeInBytes =
            grid->getBalancerConfiguration()->getMaxChunkSizeBytes();
        result.append("maxChunkSizeInBytes", maxChunkSizeInBytes);

        // Get a migration status report if a migration is active. The call to
        // getActiveMigrationStatusReport will take an IS lock on the namespace of the active
        // migration if there is one that is active.
        BSONObj migrationStatus =
            ActiveMigrationsRegistry::get(opCtx).getActiveMigrationStatusReport(opCtx);
        if (!migrationStatus.isEmpty()) {
            result.append("migrations", migrationStatus);
        }

        return result.obj();
    }

} shardingServerStatus;

class ShardingStatisticsServerStatus final : public ServerStatusSection {
public:
    ShardingStatisticsServerStatus() : ServerStatusSection("shardingStatistics") {}

    bool includeByDefault() const override {
        return isClusterNode();
    }

    BSONObj generateSection(OperationContext* opCtx,
                            const BSONElement& configElement) const override {
        if (!isClusterNode())
            return {};

        BSONObjBuilder result;
        if (auto const shardingState = ShardingState::get(opCtx); shardingState->enabled()) {
            auto const grid = Grid::get(opCtx);
            auto const catalogCache = grid->catalogCache();

            ShardingStatistics::get(opCtx).report(&result);
            catalogCache->report(&result);
            // (Ignore FCV check): This feature doesn't have any upgrade/downgrade concerns. The
            // feature flag is used to turn on new range deleter on startup.
            if (mongo::feature_flags::gRangeDeleterService.isEnabledAndIgnoreFCVUnsafe()) {
                auto nRangeDeletions = [&]() {
                    try {
                        return RangeDeleterService::get(opCtx)->totalNumOfRegisteredTasks();
                    } catch (const ExceptionFor<ErrorCodes::NotYetInitialized>&) {
                        return 0LL;
                    }
                }();
                result.appendNumber("rangeDeleterTasks", nRangeDeletions);
            }

            CollectionShardingState::appendInfoForServerStatus(opCtx, &result);
        }

        // To calculate the number of sharded collection we simply get the number of records from
        // `config.collections` collection. This count must only be appended when serverStatus is
        // invoked on the config server.
        if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
            AutoGetCollectionForReadLockFree autoColl(opCtx, CollectionType::ConfigNS);
            const auto& collection = autoColl.getCollection();
            const auto numShardedCollections = collection ? collection->numRecords(opCtx) : 0;
            result.append("numShardedCollections", numShardedCollections);
        }

        reportDataTransformMetrics(opCtx, &result);

        return result.obj();
    }

    void reportDataTransformMetrics(OperationContext* opCtx, BSONObjBuilder* bob) const {
        auto sCtx = opCtx->getServiceContext();
        using Metrics = ShardingDataTransformCumulativeMetrics;

        // The serverStatus command is run before the FCV is initialized so we ignore it when
        // checking whether the resharding and global index features are enabled here.
        if (resharding::gFeatureFlagResharding.isEnabledAndIgnoreFCVUnsafeAtStartup()) {
            Metrics::getForResharding(sCtx)->reportForServerStatus(bob);
        }
        if (gFeatureFlagGlobalIndexes.isEnabledAndIgnoreFCVUnsafeAtStartup()) {
            Metrics::getForGlobalIndexes(sCtx)->reportForServerStatus(bob);
        }
    }

} shardingStatisticsServerStatus;

}  // namespace
}  // namespace mongo
