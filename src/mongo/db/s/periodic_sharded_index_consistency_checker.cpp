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


#include "mongo/platform/basic.h"

#include "mongo/db/s/periodic_sharded_index_consistency_checker.h"

#include "mongo/db/auth/privilege.h"
#include "mongo/db/curop.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_aggregate.h"
#include "mongo/s/stale_shard_version_helpers.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

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
    stdx::lock_guard<Latch> lk(_mutex);
    return _numShardedCollsWithInconsistentIndexes;
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

            auto uniqueOpCtx = client->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();
            auto curOp = CurOp::get(opCtx);
            curOp->ensureStarted();
            ON_BLOCK_EXIT([&] { curOp->done(); });

            try {
                long long numShardedCollsWithInconsistentIndexes = 0;
                const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
                auto collections = catalogClient->getCollections(
                    opCtx, {}, repl::ReadConcernLevel::kLocalReadConcern);

                for (const auto& coll : collections) {
                    auto nss = coll.getNss();

                    // The only sharded collection in the config database with indexes is
                    // config.system.sessions. Unfortunately, the code path to run aggregation
                    // below would currently invariant if one of the targeted shards was the config
                    // server itself.
                    if (nss.isConfigDB()) {
                        continue;
                    }

                    auto request = aggregation_request_helper::parseFromBSON(
                        opCtx, nss, aggRequestBSON, boost::none, false);

                    auto catalogCache = Grid::get(opCtx)->catalogCache();
                    shardVersionRetry(
                        opCtx,
                        catalogCache,
                        nss,
                        "pipeline to detect inconsistent sharded indexes"_sd,
                        [&] {
                            BSONObjBuilder responseBuilder;
                            auto status = ClusterAggregate::runAggregate(
                                opCtx,
                                ClusterAggregate::Namespaces{nss, nss},
                                request,
                                LiteParsedPipeline{request},
                                PrivilegeVector(),
                                &responseBuilder);

                            // Stop counting if the agg command failed for one of the collections
                            // to avoid recording a false count.
                            uassertStatusOKWithContext(
                                status, str::stream() << "nss " << nss.toStringForErrorMsg());

                            if (!responseBuilder.obj()["cursor"]["firstBatch"].Array().empty()) {
                                numShardedCollsWithInconsistentIndexes++;
                            }
                        });
                }

                if (numShardedCollsWithInconsistentIndexes) {
                    LOGV2_WARNING(22051,
                                  "Found {numShardedCollectionsWithInconsistentIndexes} sharded "
                                  "collection(s) with inconsistent indexes",
                                  "Found sharded collections with inconsistent indexes",
                                  "numShardedCollectionsWithInconsistentIndexes"_attr =
                                      numShardedCollsWithInconsistentIndexes);
                }

                // Update the count if this node is still primary. This is necessary because a
                // stepdown may complete while this job is running and the count should always be
                // zero on a non-primary node.
                stdx::lock_guard<Latch> lk(_mutex);
                if (_isPrimary) {
                    _numShardedCollsWithInconsistentIndexes =
                        numShardedCollsWithInconsistentIndexes;
                }
            } catch (DBException& ex) {
                LOGV2(22052,
                      "Checking sharded index consistency failed with {error}",
                      "Error while checking sharded index consistency",
                      "error"_attr = ex.toStatus());
            }
        },
        Milliseconds(shardedIndexConsistencyCheckIntervalMS),
        // TODO(SERVER-74658): Please revisit if this periodic job could be made killable.
        false /*isKillableByStepdown*/);
    _shardedIndexConsistencyChecker = periodicRunner->makeJob(std::move(job));
    _shardedIndexConsistencyChecker.start();
}

void PeriodicShardedIndexConsistencyChecker::onStepUp(ServiceContext* serviceContext) {
    stdx::lock_guard<Latch> lk(_mutex);
    if (!_isPrimary) {
        _isPrimary = true;
        if (!_shardedIndexConsistencyChecker.isValid()) {
            // If this is the first time we're stepping up, start a thread to periodically check
            // index consistency.
            _launchShardedIndexConsistencyChecker(lk, serviceContext);
        } else {
            // If we're stepping up again after having stepped down, just resume the existing task.
            _shardedIndexConsistencyChecker.resume();
        }
    }
}

void PeriodicShardedIndexConsistencyChecker::onStepDown() {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_isPrimary) {
        _isPrimary = false;
        invariant(_shardedIndexConsistencyChecker.isValid());
        // Note pausing a periodic job does not wait for the job to complete if it is concurrently
        // running, otherwise this would deadlock when the index check tries to lock _mutex when
        // updating the inconsistent index count.
        _shardedIndexConsistencyChecker.pause();
        // Clear the counter to prevent a secondary from reporting an out-of-date count.
        _numShardedCollsWithInconsistentIndexes = 0;
    }
}

void PeriodicShardedIndexConsistencyChecker::onShutDown() {
    if (_shardedIndexConsistencyChecker.isValid()) {
        _shardedIndexConsistencyChecker.stop();
    }
}

}  // namespace mongo
