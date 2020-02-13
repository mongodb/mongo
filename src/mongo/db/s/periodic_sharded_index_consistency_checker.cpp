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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/periodic_sharded_index_consistency_checker.h"

#include "mongo/db/auth/privilege.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_aggregate.h"
#include "mongo/util/log.h"

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
    return _numShardedCollsWithInconsistentIndexes.load();
}

void PeriodicShardedIndexConsistencyChecker::_launchShardedIndexConsistencyChecker(
    ServiceContext* serviceContext) {
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
                "{$group: {_id: null, indexDoc: {$push: \"$$ROOT\"}, allShards: {$addToSet: "
                "\"$shard\"}}}, "
                "{$unwind: \"$indexDoc\"}, "
                "{$group: {\"_id\": \"$indexDoc.name\", \"shards\": {$push: "
                "\"$indexDoc.shard\"}, "
                "\"specs\": {$addToSet: {$arrayToObject: {$setUnion: {$objectToArray: "
                "\"$indexDoc.spec\"}}}}, "
                "\"allShards\": {$first: \"$allShards\"}}},"
                "{$addFields: {\"missingFromShards\": {$setDifference: [\"$allShards\", "
                "\"$shards\"]}}},"
                "{$match: {$expr: {$or: [{$gt: [{$size: \"$missingFromShards\"}, 0]}, {$gt: "
                "[{$size: \"$specs\"}, 1]}]}}},"
                "{$project: {_id: 0, indexName: \"$$ROOT._id\", specs: 1, missingFromShards: "
                "1}}, {$limit: 1}], cursor: {}}");

            auto uniqueOpCtx = client->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();

            try {
                long long numShardedCollsWithInconsistentIndexes = 0;
                auto collections =
                    uassertStatusOK(Grid::get(opCtx)->catalogClient()->getCollections(
                        opCtx, nullptr, nullptr, repl::ReadConcernLevel::kLocalReadConcern));

                for (const auto& coll : collections) {
                    auto nss = coll.getNs();

                    // The only sharded collection in the config database with indexes is
                    // config.system.sessions. Unfortunately, the code path to run aggregation
                    // below would currently invariant if one of the targeted shards was the config
                    // server itself.
                    if (nss.isConfigDB()) {
                        continue;
                    }

                    auto request =
                        uassertStatusOK(AggregationRequest::parseFromBSON(nss, aggRequestBSON));

                    for (int tries = 0;; ++tries) {
                        const bool canRetry = tries < kMaxNumStaleVersionRetries - 1;

                        try {
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
                            uassertStatusOKWithContext(status, str::stream() << "nss " << nss);

                            if (!responseBuilder.obj()["cursor"]["firstBatch"].Array().empty()) {
                                numShardedCollsWithInconsistentIndexes++;
                            }
                            break;
                        } catch (const ExceptionForCat<ErrorCategory::StaleShardVersionError>& ex) {
                            LOGV2(22050,
                                  "Attempt {tries} to check index consistency for {nss} received "
                                  "StaleShardVersion error{causedBy_ex}",
                                  "tries"_attr = tries,
                                  "nss"_attr = nss,
                                  "causedBy_ex"_attr = causedBy(ex));
                            if (canRetry) {
                                continue;
                            }
                            throw;
                        }
                    }
                }

                LOGV2(22051,
                      "Found {numShardedCollsWithInconsistentIndexes} collections with "
                      "inconsistent indexes",
                      "numShardedCollsWithInconsistentIndexes"_attr =
                          numShardedCollsWithInconsistentIndexes);

                // Update the count.
                _numShardedCollsWithInconsistentIndexes.store(
                    numShardedCollsWithInconsistentIndexes);
            } catch (DBException& ex) {
                LOGV2(22052,
                      "Failed to check index consistency {causedBy_ex_toStatus}",
                      "causedBy_ex_toStatus"_attr = causedBy(ex.toStatus()));
            }
        },
        Milliseconds(shardedIndexConsistencyCheckIntervalMS));
    _shardedIndexConsistencyChecker = periodicRunner->makeJob(std::move(job));
    _shardedIndexConsistencyChecker.start();
}

void PeriodicShardedIndexConsistencyChecker::onStepUp(ServiceContext* serviceContext) {
    if (!_isPrimary) {
        _isPrimary = true;
        if (!_shardedIndexConsistencyChecker.isValid()) {
            // If this is the first time we're stepping up, start a thread to periodically check
            // index consistency.
            _launchShardedIndexConsistencyChecker(serviceContext);
        } else {
            // If we're stepping up again after having stepped down, just resume the existing task.
            _shardedIndexConsistencyChecker.resume();
        }
    }
}

void PeriodicShardedIndexConsistencyChecker::onStepDown() {
    if (_isPrimary) {
        _isPrimary = false;
        invariant(_shardedIndexConsistencyChecker.isValid());
        // We don't need to be checking index consistency unless we're primary.
        _shardedIndexConsistencyChecker.pause();
        // Clear the counter to prevent a secondary from reporting an out-of-date count.
        _numShardedCollsWithInconsistentIndexes.store(0);
    }
}

void PeriodicShardedIndexConsistencyChecker::onShutDown() {
    if (_shardedIndexConsistencyChecker.isValid()) {
        _shardedIndexConsistencyChecker.stop();
    }
}

}  // namespace mongo
