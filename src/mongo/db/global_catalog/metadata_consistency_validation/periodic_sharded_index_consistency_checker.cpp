/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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


#include "mongo/db/global_catalog/metadata_consistency_validation/periodic_sharded_index_consistency_checker.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/router_role/router_role.h"
#include "mongo/db/router_role/routing_cache/routing_information_cache.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/sharding_config_server_parameters_gen.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/s/query/planner/cluster_aggregate.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/none.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
using namespace std::literals::string_view_literals;
namespace {

static const int shardedTimeseriesShardkeyCheckIntervalMS{12 * 60 * 60 * 1000};  // 12 hours

const auto getPeriodicShardedIndexConsistencyChecker =
    ServiceContext::declareDecoration<PeriodicShardedIndexConsistencyChecker>();

}  // namespace

PeriodicShardedIndexConsistencyChecker& PeriodicShardedIndexConsistencyChecker::get(
    OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

PeriodicShardedIndexConsistencyChecker& PeriodicShardedIndexConsistencyChecker::get(
    ServiceContext* serviceContext) {
    return getPeriodicShardedIndexConsistencyChecker(serviceContext);
}

long long PeriodicShardedIndexConsistencyChecker::getNumShardedCollsWithInconsistentIndexes()
    const {
    return _state.load().numShardedCollsWithInconsistentIndexes;
}

void PeriodicShardedIndexConsistencyChecker::_launchShardedIndexConsistencyChecker(
    WithLock, ServiceContext* serviceContext) {
    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob job(
        "PeriodicShardedIndexConsistencyChecker",
        [this](Client* client) {
            if (!enableShardedIndexConsistencyCheck.load()) {
                return;
            }

            auto uniqueOpCtx = client->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            // TODO: SERVER-82965 Remove wait
            try {
                ShardingState::get(opCtx)->awaitClusterRoleRecovery().get(opCtx);
            } catch (DBException&) {
                return;
            }

            LOGV2(22049, "Checking consistency of sharded collection indexes across the cluster");

            const auto aggRequestBSON = fromjson(
                "{pipeline: [{$indexStats: {}},"

                "{$group:"
                "{_id: null, indexDoc: {$push: \"$$ROOT\"}, allShards: {$addToSet: \"$shard\"}}},"

                "{$unwind: \"$indexDoc\"},"

                "{$group: "
                "{\"_id\": \"$indexDoc.name\","
                "\"shards\": {$push: \"$indexDoc.shard\"},"
                "\"specs\": {$push: {$objectToArray: {$ifNull: [\"$indexDoc.spec\", {}]}}},"
                "\"allShards\": {$first: \"$allShards\"}}},"

                "{$project:"
                " {missingFromShards: {$setDifference: [\"$allShards\", \"$shards\"]},"
                " inconsistentProperties: {"
                "  $setDifference: ["
                "   {$reduce: {input: \"$specs\", initialValue: {$arrayElemAt: [\"$specs\", 0]},"
                "in: {$setUnion: [\"$$value\", \"$$this\"]}}},"
                "   {$reduce: {input: \"$specs\", initialValue: {$arrayElemAt: [\"$specs\", 0]},"
                "in: {$setIntersection: [\"$$value\", \"$$this\"]}}}]}}},"

                "{$match:"
                "{$expr: {$or: ["
                " {$gt: [{$size: \"$missingFromShards\"}, 0]},"
                " {$gt: [{$size: \"$inconsistentProperties\"}, 0]}]}}},"

                "{$project:"
                "{_id: 0, indexName: \"$$ROOT._id\", inconsistentProperties: 1, missingFromShards:"
                "1}},"

                "{$limit: 1}], cursor: {}}");

            auto curOp = CurOp::get(opCtx);
            curOp->ensureStarted();
            ON_BLOCK_EXIT([&] { curOp->done(); });

            try {
                long long numShardedCollsWithInconsistentIndexes = 0;
                const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
                auto collections = catalogClient->getShardedCollections(
                    opCtx, DatabaseName::kEmpty, repl::ReadConcernArgs::kLocal, {} /*sort*/);

                for (const auto& coll : collections) {
                    auto nss = coll.getNss();

                    // The only sharded collection in the config database with indexes is
                    // config.system.sessions. Unfortunately, the code path to run aggregation
                    // below would currently invariant if one of the targeted shards was the config
                    // server itself.
                    if (nss.isConfigDB()) {
                        continue;
                    }

                    BSONObjBuilder requestBuilder;
                    requestBuilder.append("aggregate", nss.coll());
                    requestBuilder.append("$db",
                                          DatabaseNameUtil::serialize(
                                              nss.dbName(), SerializationContext::stateDefault()));
                    requestBuilder.appendElements(aggRequestBSON);
                    auto request = aggregation_request_helper::parseFromBSON(
                        requestBuilder.obj(), auth::ValidatedTenancyScope::get(opCtx), boost::none);

                    BSONObjBuilder responseBuilder;
                    auto status = ClusterAggregate::runAggregate(
                        opCtx,
                        ClusterAggregate::Namespaces{nss, nss},
                        request,
                        PrivilegeVector(),
                        boost::none /* verbosity */,
                        &responseBuilder,
                        "pipeline to detect inconsistent sharded indexes"sv);

                    // Stop counting if the agg command failed for one of the
                    // collections to avoid recording a false count.
                    uassertStatusOKWithContext(
                        status, str::stream() << "nss " << nss.toStringForErrorMsg());

                    if (!responseBuilder.obj()["cursor"]["firstBatch"].Array().empty()) {
                        numShardedCollsWithInconsistentIndexes++;
                    }
                }

                if (numShardedCollsWithInconsistentIndexes) {
                    LOGV2_WARNING(22051,
                                  "Found sharded collections with inconsistent indexes",
                                  "numShardedCollectionsWithInconsistentIndexes"_attr =
                                      numShardedCollsWithInconsistentIndexes);
                }

                // Update the count if this node is still primary. This is necessary because a
                // stepdown may complete while this job is running and the count should always be
                // zero on a non-primary node.
                auto expectedState = _state.load();
                State newState;
                do {
                    if (!expectedState.isPrimary) {
                        return;
                    }
                    newState = expectedState;
                    newState.numShardedCollsWithInconsistentIndexes =
                        numShardedCollsWithInconsistentIndexes;
                } while (!_state.compare_exchange_strong(expectedState, newState));
            } catch (DBException& ex) {
                LOGV2(22052,
                      "Error while checking sharded index consistency",
                      "error"_attr = ex.toStatus());
            }
        },
        Milliseconds(shardedIndexConsistencyCheckIntervalMS),
        true);
    _shardedIndexConsistencyChecker = periodicRunner->makeJob(std::move(job));
    _shardedIndexConsistencyChecker.start();
}

void PeriodicShardedIndexConsistencyChecker::_launchShardedTimeseriesShardkeyChecker(
    WithLock, ServiceContext* serviceContext) {
    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

    PeriodicRunner::PeriodicJob job(
        "PeriodicShardedTimeseriesShardkeyChecker",
        [this](Client* client) {
            auto uniqueOpCtx = client->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            // TODO: SERVER-82965 Remove wait
            try {
                ShardingState::get(opCtx)->awaitClusterRoleRecovery().get(opCtx);
            } catch (DBException&) {
                return;
            }

            try {
                uassertStatusOK(
                    ShardingCatalogManager::get(opCtx)->checkTimeseriesShardKeys(opCtx));
            } catch (const DBException& ex) {
                LOGV2(9406000,
                      "Error while checking timeseries sharded index consistency",
                      "error"_attr = ex.toStatus());
            }
        },
        Milliseconds(shardedTimeseriesShardkeyCheckIntervalMS),
        false);
    _shardedTimeseriesShardkeyChecker = periodicRunner->makeJob(std::move(job));
    _shardedTimeseriesShardkeyChecker.start();
}

void PeriodicShardedIndexConsistencyChecker::onStepUp(ServiceContext* serviceContext) {
    std::lock_guard<std::mutex> lk(_mutex);
    auto state = _state.load();
    if (!state.isPrimary) {
        state.isPrimary = true;
        _state.store(state);

        _launchShardedIndexConsistencyChecker(lk, serviceContext);
        _launchShardedTimeseriesShardkeyChecker(lk, serviceContext);
    }
}

void PeriodicShardedIndexConsistencyChecker::onStepDown() {
    std::lock_guard<std::mutex> lk(_mutex);
    auto state = _state.load();
    if (state.isPrimary) {
        state.isPrimary = false;
        // Clear the counter to prevent a secondary from reporting an out-of-date count.
        state.numShardedCollsWithInconsistentIndexes = 0;
        _state.store(state);

        invariant(_shardedIndexConsistencyChecker.isValid());
        _shardedIndexConsistencyChecker.stop();
        if (_shardedTimeseriesShardkeyChecker.isValid()) {
            _shardedTimeseriesShardkeyChecker.stop();
        }
    }
}

void PeriodicShardedIndexConsistencyChecker::onShutDown() {
    if (_shardedIndexConsistencyChecker.isValid()) {
        _shardedIndexConsistencyChecker.stop();
    }
    if (_shardedTimeseriesShardkeyChecker.isValid()) {
        _shardedTimeseriesShardkeyChecker.stop();
    }
}

}  // namespace mongo
