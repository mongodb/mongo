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


#include "mongo/db/s/balancer/balancer.h"

#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <ratio>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/balancer/actions_stream_policy.h"
#include "mongo/db/s/balancer/auto_merger_policy.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler_impl.h"
#include "mongo/db/s/balancer/balancer_defragmentation_policy.h"
#include "mongo/db/s/balancer/cluster_statistics_impl.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_config_server_parameters_gen.h"
#include "mongo/db/s/sharding_logging.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/shard_id.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/random.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/s/request_types/balancer_collection_status_gen.h"
#include "mongo/s/request_types/migration_secondary_throttle_options.h"
#include "mongo/s/shard_util.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/pcre.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"
#include "mongo/util/version.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

using std::map;
using std::string;
using std::vector;

using MigrationsAndResponses = std::vector<std::pair<const MigrateInfo&, SemiFuture<void>>>;

namespace {

MONGO_FAIL_POINT_DEFINE(forceBalancerWarningChecks);

const Milliseconds kBalanceRoundDefaultInterval(10 * 1000);

/**
 * Balancer status response
 */
static constexpr StringData kBalancerPolicyStatusDraining = "draining"_sd;
static constexpr StringData kBalancerPolicyStatusZoneViolation = "zoneViolation"_sd;
static constexpr StringData kBalancerPolicyStatusChunksImbalance = "chunksImbalance"_sd;
static constexpr StringData kBalancerPolicyStatusDefragmentingChunks = "defragmentingChunks"_sd;

/**
 * Utility class to generate timing and statistics for a single balancer round.
 */
class BalanceRoundDetails {
public:
    BalanceRoundDetails() : _executionTimer() {}

    void setSucceeded(int numCandidateChunks,
                      int numChunksMoved,
                      int numImbalancedCachedCollections,
                      int numCandidateUnshardedCollections,
                      int numUnshardedCollectionsMoved,
                      Milliseconds selectionTime,
                      Milliseconds throttleTime,
                      Milliseconds migrationTime) {
        tassert(8245236, "Error message is not empty", !_errMsg);
        _numCandidateChunks = numCandidateChunks;
        _numChunksMoved = numChunksMoved;
        _numImbalancedCachedCollections = numImbalancedCachedCollections;
        _numCandidateUnshardedCollections = numCandidateUnshardedCollections;
        _numUnshardedCollectionsMoved = numUnshardedCollectionsMoved;
        _selectionTime = selectionTime;
        _throttleTime = throttleTime;
        _migrationTime = migrationTime;
    }

    void setFailed(const string& errMsg) {
        _errMsg = errMsg;
    }

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        builder.append("executionTimeMillis", _executionTimer.millis());
        builder.append("errorOccurred", _errMsg.has_value());

        if (_errMsg) {
            builder.append("errmsg", *_errMsg);
        } else {
            builder.append("candidateChunks", _numCandidateChunks);
            builder.append("chunksMoved", _numChunksMoved);
            builder.append("candidateUnshardedCollections", _numCandidateUnshardedCollections);
            builder.append("unshardedCollectionsMoved", _numUnshardedCollectionsMoved);
            builder.append("imbalancedCachedCollections", _numImbalancedCachedCollections);
            BSONObjBuilder timeInfo{builder.subobjStart("times"_sd)};
            timeInfo.append("selectionTimeMillis"_sd, _selectionTime.count());
            timeInfo.append("throttleTimeMillis"_sd, _throttleTime.count());
            timeInfo.append("migrationTimeMillis"_sd, _migrationTime.count());
            timeInfo.done();
        }
        return builder.obj();
    }

private:
    const Timer _executionTimer;
    Milliseconds _selectionTime;
    Milliseconds _throttleTime;
    Milliseconds _migrationTime;

    // Set only on success
    int _numCandidateChunks{0};
    int _numChunksMoved{0};
    int _numImbalancedCachedCollections{0};
    int _numCandidateUnshardedCollections{0};
    int _numUnshardedCollectionsMoved{0};

    // Set only on failure
    boost::optional<string> _errMsg;
};

/**
 *  Interface to group a set of migrations of the same type.
 */
class MigrationTask {
public:
    MigrationTask(OperationContext* opCtx,
                  BalancerCommandsScheduler& scheduler,
                  const MigrateInfoVector& migrations)
        : _opCtx(opCtx), _scheduler(scheduler), _numCompleted(0) {
        _migrationsAndResponses.reserve(migrations.size());
        for (const MigrateInfo& x : migrations) {
            _migrationsAndResponses.emplace_back(x, SemiFuture<void>());
        }
    }

    virtual ~MigrationTask() = default;

    virtual void enqueue() = 0;

    int getNumCompleted() {
        return _numCompleted;
    };

    // Resolve all enqueued tasks and record the number completed successfully
    void waitForQueuedAndProcessResponses() {
        for (const auto& [migrateInfo, futureStatus] : _migrationsAndResponses) {
            auto status = futureStatus.getNoThrow(_opCtx);
            if (processResponse(migrateInfo, status)) {
                _numCompleted++;
            }
        }
    }

protected:
    virtual bool processResponse(const MigrateInfo& migrationInfo, const Status& status) = 0;

    OperationContext* _opCtx;
    BalancerCommandsScheduler& _scheduler;
    MigrationsAndResponses _migrationsAndResponses;
    int _numCompleted;
};

Chunk getChunkForMaxBound(const ChunkManager& cm, const BSONObj& max) {
    boost::optional<Chunk> chunkWithMaxBound;
    cm.forEachChunk([&](const auto& chunk) {
        if (chunk.getMax().woCompare(max) == 0) {
            chunkWithMaxBound.emplace(chunk);
            return false;
        }
        return true;
    });
    if (chunkWithMaxBound) {
        return *chunkWithMaxBound;
    }
    return cm.findIntersectingChunkWithSimpleCollation(max);
}

Status processManualMigrationOutcome(OperationContext* opCtx,
                                     const boost::optional<BSONObj>& min,
                                     const boost::optional<BSONObj>& max,
                                     const NamespaceString& nss,
                                     const ShardId& destination,
                                     Status outcome) {
    // Since the commands scheduler uses a separate thread to remotely execute a
    // request, the resulting clusterTime needs to be explicitly retrieved and set on the
    // original context of the requestor to ensure it will be propagated back to the router.
    auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
    replClient.setLastOpToSystemLastOpTime(opCtx);

    if (outcome.isOK()) {
        return outcome;
    }

    auto swCM =
        Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithPlacementRefresh(opCtx,
                                                                                              nss);
    if (!swCM.isOK()) {
        return swCM.getStatus();
    }
    const auto& cm = swCM.getValue().cm;

    const auto currentChunkInfo =
        min ? cm.findIntersectingChunkWithSimpleCollation(*min) : getChunkForMaxBound(cm, *max);

    if (currentChunkInfo.getShardId() == destination &&
        outcome != ErrorCodes::BalancerInterrupted) {
        // Migration calls can be interrupted after the metadata is committed but before the command
        // finishes the waitForDelete stage. Any failovers, therefore, must always cause the
        // moveChunk command to be retried so as to assure that the waitForDelete promise of a
        // successful command has been fulfilled.
        LOGV2(6036622,
              "Migration outcome is not OK, but the transaction was committed. Returning success");
        outcome = Status::OK();
    }
    return outcome;
}

uint64_t getMaxChunkSizeBytes(OperationContext* opCtx, const CollectionType& coll) {
    const auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    uassertStatusOK(balancerConfig->refreshAndCheck(opCtx));
    return coll.getMaxChunkSizeBytes().value_or(balancerConfig->getMaxChunkSizeBytes());
}

int64_t getMaxChunkSizeMB(OperationContext* opCtx, const CollectionType& coll) {
    return getMaxChunkSizeBytes(opCtx, coll) / (1024 * 1024);
}

// Returns a boolean flag indicating whether secondary throttling is enabled and the write concern
// to apply for migrations
std::tuple<bool, WriteConcernOptions> getSecondaryThrottleAndWriteConcern(
    const boost::optional<MigrationSecondaryThrottleOptions>& secondaryThrottle) {
    if (secondaryThrottle &&
        secondaryThrottle->getSecondaryThrottle() == MigrationSecondaryThrottleOptions::kOn) {
        if (secondaryThrottle->isWriteConcernSpecified()) {
            return {true, secondaryThrottle->getWriteConcern()};
        }
        return {true, WriteConcernOptions()};
    }
    return {false, WriteConcernOptions()};
}

const auto _balancerDecoration = ServiceContext::declareDecoration<Balancer>();

const ReplicaSetAwareServiceRegistry::Registerer<Balancer> _balancerRegisterer("Balancer");

/**
 * Returns the names of shards that are currently draining. When the balancer is disabled, draining
 * shards are stuck in this state as chunks cannot be migrated.
 */
std::vector<std::string> getDrainingShardNames(OperationContext* opCtx) {
    // Find the shards that are currently draining.
    const auto& configShard = ShardingCatalogManager::get(opCtx)->localConfigShard();
    const auto drainingShardsDocs{
        uassertStatusOK(
            configShard->exhaustiveFindOnConfig(opCtx,
                                                ReadPreferenceSetting{ReadPreference::Nearest},
                                                repl::ReadConcernLevel::kMajorityReadConcern,
                                                NamespaceString::kConfigsvrShardsNamespace,
                                                BSON(ShardType::draining << true),
                                                BSONObj() /* No sorting */,
                                                boost::none /* No limit */))
            .docs};

    // Build the list of the draining shard names.
    std::vector<std::string> drainingShardNames;
    drainingShardNames.reserve(drainingShardsDocs.size());
    std::transform(drainingShardsDocs.begin(),
                   drainingShardsDocs.end(),
                   std::back_inserter(drainingShardNames),
                   [](const auto& shardDoc) {
                       const auto shardEntry{uassertStatusOK(ShardType::fromBSON(shardDoc))};
                       return shardEntry.getName();
                   });
    return drainingShardNames;
}

void enqueueCollectionMigrations(OperationContext* opCtx,
                                 BalancerCommandsScheduler& scheduler,
                                 MigrationsAndResponses& migrationsAndResponses) {
    auto requestMigration = [&](const MigrateInfo& migrateInfo) -> SemiFuture<void> {
        auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
        const auto dbEntry = catalogClient->getDatabase(
            opCtx, migrateInfo.nss.dbName(), repl::ReadConcernLevel::kMajorityReadConcern);

        return scheduler.requestMoveCollection(
            opCtx, migrateInfo.nss, migrateInfo.to, dbEntry.getPrimary(), dbEntry.getVersion());
    };

    for (auto& migrationAndResponse : migrationsAndResponses) {
        migrationAndResponse.second = requestMigration(migrationAndResponse.first);
    }
}

void enqueueChunkMigrations(OperationContext* opCtx,
                            BalancerCommandsScheduler& scheduler,
                            MigrationsAndResponses& migrationsAndResponses) {
    auto requestMigration = [&](const MigrateInfo& migrateInfo) -> SemiFuture<void> {
        auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
        auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();

        auto maxChunkSizeBytes = [&]() {
            if (migrateInfo.optMaxChunkSizeBytes.has_value()) {
                return *migrateInfo.optMaxChunkSizeBytes;
            }

            auto coll = catalogClient->getCollection(
                opCtx, migrateInfo.nss, repl::ReadConcernLevel::kMajorityReadConcern);
            return coll.getMaxChunkSizeBytes().value_or(balancerConfig->getMaxChunkSizeBytes());
        }();

        MoveRangeRequestBase requestBase(migrateInfo.to);
        requestBase.setWaitForDelete(balancerConfig->waitForDelete());
        requestBase.setMin(migrateInfo.minKey);
        requestBase.setMax(migrateInfo.maxKey);

        ShardsvrMoveRange shardSvrRequest(migrateInfo.nss);
        shardSvrRequest.setDbName(DatabaseName::kAdmin);
        shardSvrRequest.setMoveRangeRequestBase(requestBase);
        shardSvrRequest.setMaxChunkSizeBytes(maxChunkSizeBytes);
        shardSvrRequest.setFromShard(migrateInfo.from);
        shardSvrRequest.setCollectionTimestamp(migrateInfo.version.getTimestamp());
        shardSvrRequest.setEpoch(migrateInfo.version.epoch());
        shardSvrRequest.setForceJumbo(migrateInfo.forceJumbo);
        const auto [secondaryThrottle, wc] =
            getSecondaryThrottleAndWriteConcern(balancerConfig->getSecondaryThrottle());
        shardSvrRequest.setSecondaryThrottle(secondaryThrottle);

        return scheduler.requestMoveRange(
            opCtx, shardSvrRequest, wc, false /* issuedByRemoteUser */);
    };

    for (auto& migrationAndResponse : migrationsAndResponses) {
        migrationAndResponse.second = requestMigration(migrationAndResponse.first);
    }
}

bool processActionStreamPolicyResponse(OperationContext* opCtx,
                                       ActionsStreamPolicy& streamPolicy,
                                       const MigrateInfo& migrationInfo,
                                       Status status) {
    streamPolicy.applyActionResult(opCtx, migrationInfo, status);
    return status.isOK();
};


bool processRebalanceResponse(OperationContext* opCtx,
                              BalancerCommandsScheduler& commandScheduler,
                              const MigrateInfo& migrateInfo,
                              Status status) {
    if (status.isOK()) {
        return true;
    }

    // ChunkTooBig is returned by the source shard during the cloning phase if the migration
    // manager finds that the chunk is larger than some calculated size, the source shard is
    // *not* in draining mode, and the 'forceJumbo' balancer setting is 'kDoNotForce'.
    // ExceededMemoryLimit is returned when the transfer mods queue surpasses 500MB regardless
    // of whether the source shard is in draining mode or the value if the 'froceJumbo' balancer
    // setting.
    if (status == ErrorCodes::ChunkTooBig || status == ErrorCodes::ExceededMemoryLimit) {
        LOGV2(21871,
              "Migration failed, going to try splitting the chunk",
              "migrateInfo"_attr = redact(migrateInfo.toString()),
              "error"_attr = redact(status),
              logAttrs(migrateInfo.nss));

        auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
        const CollectionType collection = catalogClient->getCollection(
            opCtx, migrateInfo.uuid, repl::ReadConcernLevel::kMajorityReadConcern);

        ShardingCatalogManager::get(opCtx)->splitOrMarkJumbo(
            opCtx, collection.getNss(), migrateInfo.minKey, migrateInfo.getMaxChunkSizeBytes());
        return true;
    }

    if (status == ErrorCodes::IndexNotFound &&
        gFeatureFlagShardKeyIndexOptionalHashedSharding.isEnabled(
            serverGlobalParams.featureCompatibility.acquireFCVSnapshot())) {

        const auto [cm, _] =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(
                opCtx, migrateInfo.nss));

        if (cm.getShardKeyPattern().isHashedPattern()) {
            LOGV2(78252,
                  "Turning off balancing for hashed collection because migration failed due to "
                  "missing shardkey index",
                  "migrateInfo"_attr = redact(migrateInfo.toString()),
                  "error"_attr = redact(status),
                  logAttrs(migrateInfo.nss));

            // Schedule writing to config.collections to turn off the balancer.
            commandScheduler.disableBalancerForCollection(opCtx, migrateInfo.nss);
            return false;
        }
    }

    LOGV2(21872,
          "Migration failed",
          "migrateInfo"_attr = redact(migrateInfo.toString()),
          "error"_attr = redact(status),
          logAttrs(migrateInfo.nss));
    return false;
}

class MigrateUnshardedCollectionTask : public MigrationTask {
public:
    MigrateUnshardedCollectionTask(OperationContext* opCtx,
                                   MoveUnshardedPolicy& moveUnshardedPolicy,
                                   BalancerCommandsScheduler& commandScheduler,
                                   const MigrateInfoVector& migrations)
        : MigrationTask(opCtx, commandScheduler, migrations),
          _moveUnshardedPolicy(moveUnshardedPolicy) {}

    void enqueue() override {
        enqueueCollectionMigrations(_opCtx, _scheduler, _migrationsAndResponses);
    }

    bool processResponse(const MigrateInfo& migrateInfo, const Status& status) override {
        return processActionStreamPolicyResponse(_opCtx, _moveUnshardedPolicy, migrateInfo, status);
    }

protected:
    MoveUnshardedPolicy& _moveUnshardedPolicy;
};

class RebalanceChunkTask : public MigrationTask {
public:
    RebalanceChunkTask(OperationContext* opCtx,
                       BalancerCommandsScheduler& commandScheduler,
                       const MigrateInfoVector& migrations)
        : MigrationTask(opCtx, commandScheduler, migrations) {}

    void enqueue() override {
        enqueueChunkMigrations(_opCtx, _scheduler, _migrationsAndResponses);
    }

    bool processResponse(const MigrateInfo& migrateInfo, const Status& status) override {
        return processRebalanceResponse(_opCtx, _scheduler, migrateInfo, status);
    }
};

class DefragmentChunkTask : public MigrationTask {
public:
    DefragmentChunkTask(OperationContext* opCtx,
                        BalancerDefragmentationPolicy& defragmentationPolicy,
                        BalancerCommandsScheduler& scheduler,
                        const MigrateInfoVector& migrations)
        : MigrationTask(opCtx, scheduler, migrations),
          _defragmentationPolicy(defragmentationPolicy) {}

    void enqueue() override {
        enqueueChunkMigrations(_opCtx, _scheduler, _migrationsAndResponses);
    }

    bool processResponse(const MigrateInfo& migrateInfo, const Status& status) override {
        return processActionStreamPolicyResponse(
            _opCtx, _defragmentationPolicy, migrateInfo, status);
    }

protected:
    BalancerDefragmentationPolicy& _defragmentationPolicy;
};

class BalancerWarning {
    // Time interval between checks on draining shards.
    constexpr static Minutes kDrainingShardsCheckInterval{10};

public:
    BalancerWarning() = default;

    void warnIfRequired(OperationContext* opCtx, BalancerSettingsType::BalancerMode balancerMode) {
        if (Date_t::now() - _lastDrainingShardsCheckTime < kDrainingShardsCheckInterval &&
            MONGO_likely(!forceBalancerWarningChecks.shouldFail())) {
            return;
        }
        _lastDrainingShardsCheckTime = Date_t::now();

        LOGV2(7977401, "Performing balancer warning checks");

        const auto drainingShardNames{getDrainingShardNames(opCtx)};
        if (drainingShardNames.empty()) {
            return;
        }

        if (balancerMode == BalancerSettingsType::BalancerMode::kOff) {
            LOGV2_WARNING(
                6434000,
                "Draining of removed shards cannot be completed because the balancer is disabled",
                "shards"_attr = drainingShardNames);
            return;
        }

        _warnIfDrainingShardHasChunksForCollectionWithBalancingDisabled(opCtx, drainingShardNames);
    }

private:
    void _warnIfDrainingShardHasChunksForCollectionWithBalancingDisabled(
        OperationContext* opCtx, const std::vector<std::string>& drainingShardNames) {
        // Balancer is on, emit warning if balancer is disabled for collections which have chunks in
        // shards in draining mode.
        const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
        auto collections =
            catalogClient->getShardedCollections(opCtx,
                                                 DatabaseName::kEmpty,
                                                 repl::ReadConcernLevel::kMajorityReadConcern,
                                                 BSON(CollectionType::kNssFieldName << 1));
        if (collections.empty()) {
            return;
        }

        // Construct BSONArray of draining shard names.
        const auto drainingShardNameArray = [&]() {
            BSONArrayBuilder shardNameArrayBuilder;
            std::for_each(drainingShardNames.begin(),
                          drainingShardNames.end(),
                          [&shardNameArrayBuilder](const auto& shardName) {
                              shardNameArrayBuilder.append(shardName);
                          });
            return shardNameArrayBuilder.arr();
        }();

        // For each collection, check if the collection has balancing disabled. If it is disabled,
        // checks if the collection has any chunks in any of the draining shards. In which case a
        // warning is emitted.
        for (const auto& collType : collections) {
            if (!collType.getAllowBalance() || !collType.getAllowMigrations() ||
                !collType.getPermitMigrations()) {
                auto matchStage = BSON("$match" << BSON(ChunkType::collectionUUID()
                                                        << collType.getUuid() << ChunkType::shard()
                                                        << BSON("$in" << drainingShardNameArray)));
                AggregateCommandRequest aggRequest{ChunkType::ConfigNS, {std::move(matchStage)}};

                auto chunks = catalogClient->runCatalogAggregation(
                    opCtx, aggRequest, {repl::ReadConcernLevel::kMajorityReadConcern});

                if (!chunks.empty()) {
                    stdx::unordered_set<std::string> shardsWithChunks;
                    std::for_each(
                        chunks.begin(), chunks.end(), [&shardsWithChunks](const BSONObj& chunkObj) {
                            shardsWithChunks.emplace(chunkObj.getStringField(ChunkType::shard()));
                        });
                    LOGV2_WARNING(
                        7977400,
                        "Draining of removed shards cannot be completed because the balancer is "
                        "disabled for a collection which has chunks in those shards",
                        "uuid"_attr = collType.getUuid(),
                        "nss"_attr = collType.getNss(),
                        "shardsWithChunks"_attr = shardsWithChunks);
                }
            }
        }
    }

    Date_t _lastDrainingShardsCheckTime{Date_t::fromMillisSinceEpoch(0)};
};
}  // namespace

Balancer* Balancer::get(ServiceContext* serviceContext) {
    return &_balancerDecoration(serviceContext);
}

Balancer* Balancer::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

Balancer::Balancer()
    : _balancedLastTime({}),
      _clusterStats(std::make_unique<ClusterStatisticsImpl>()),
      _chunkSelectionPolicy(std::make_unique<BalancerChunkSelectionPolicy>(_clusterStats.get())),
      _commandScheduler(std::make_unique<BalancerCommandsSchedulerImpl>()),
      _defragmentationPolicy(std::make_unique<BalancerDefragmentationPolicy>(
          _clusterStats.get(), [this]() { _onActionsStreamPolicyStateUpdate(); })),
      _autoMergerPolicy(
          std::make_unique<AutoMergerPolicy>([this]() { _onActionsStreamPolicyStateUpdate(); })),
      _moveUnshardedPolicy(std::make_unique<MoveUnshardedPolicy>()) {}

Balancer::~Balancer() {
    onShutdown();
}

void Balancer::onStepUpBegin(OperationContext* opCtx, long long term) {
    // Before starting step-up, ensure the balancer is ready to start. Specifically, that there is
    // not an outstanding termination sequence requested during a previous step down of this node.
    joinTermination();
}

void Balancer::onStepUpComplete(OperationContext* opCtx, long long term) {
    initiate(opCtx);
}

void Balancer::onStepDown() {
    // Asynchronously request to terminate all the worker threads and allow the stepdown sequence to
    // continue.
    requestTermination();
}

void Balancer::onShutdown() {
    // Terminate the balancer thread so it doesn't leak memory.
    requestTermination();
    joinTermination();
}

void Balancer::onBecomeArbiter() {
    // The Balancer is only active on config servers, and arbiters are not permitted in config
    // server replica sets.
    MONGO_UNREACHABLE;
}

void Balancer::initiate(OperationContext* opCtx) {
    stdx::lock_guard<Latch> scopedLock(_mutex);
    _imbalancedCollectionsCache.clear();
    invariant(_threadSetState == ThreadSetState::Terminated);
    _threadSetState = ThreadSetState::Running;

    invariant(!_thread.joinable());
    invariant(!_actionStreamConsumerThread.joinable());
    invariant(!_threadOperationContext);
    _thread = stdx::thread([this] { _mainThread(); });
}

void Balancer::requestTermination() {
    stdx::lock_guard<Latch> scopedLock(_mutex);
    if (_threadSetState != ThreadSetState::Running) {
        return;
    }

    _threadSetState = ThreadSetState::Terminating;

    // Interrupt the balancer thread if it has been started. We are guaranteed that the operation
    // context of that thread is still alive, because we hold the balancer mutex.
    if (_threadOperationContext) {
        stdx::lock_guard<Client> scopedClientLock(*_threadOperationContext->getClient());
        _threadOperationContext->markKilled(ErrorCodes::InterruptedDueToReplStateChange);
    }

    _condVar.notify_all();
    _actionStreamCondVar.notify_all();
}

void Balancer::joinTermination() {
    stdx::unique_lock<Latch> scopedLock(_mutex);
    _joinCond.wait(scopedLock, [this] { return _threadSetState == ThreadSetState::Terminated; });
    if (_thread.joinable()) {
        _thread.join();
    }
}

void Balancer::joinCurrentRound(OperationContext* opCtx) {
    stdx::unique_lock<Latch> scopedLock(_mutex);
    const auto numRoundsAtStart = _numBalancerRounds;
    opCtx->waitForConditionOrInterrupt(_condVar, scopedLock, [&] {
        return !_inBalancerRound || _numBalancerRounds != numRoundsAtStart;
    });
}

Status Balancer::moveRange(OperationContext* opCtx,
                           const NamespaceString& nss,
                           const ConfigsvrMoveRange& request,
                           bool issuedByRemoteUser) {
    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    auto coll =
        catalogClient->getCollection(opCtx, nss, repl::ReadConcernLevel::kMajorityReadConcern);

    if (coll.getUnsplittable())
        return {ErrorCodes::NamespaceNotSharded,
                str::stream() << "Can't execute moveRange on unsharded collection "
                              << nss.toStringForErrorMsg()};

    const auto maxChunkSize = getMaxChunkSizeBytes(opCtx, coll);

    const auto fromShardId = [&]() {
        const auto [cm, _] = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithPlacementRefresh(
                opCtx, nss));
        if (request.getMin()) {
            const auto& chunk = cm.findIntersectingChunkWithSimpleCollation(*request.getMin());
            return chunk.getShardId();
        } else {
            return getChunkForMaxBound(cm, *request.getMax()).getShardId();
        }
    }();

    ShardsvrMoveRange shardSvrRequest(nss);
    shardSvrRequest.setDbName(DatabaseName::kAdmin);
    shardSvrRequest.setMoveRangeRequestBase(request.getMoveRangeRequestBase());
    shardSvrRequest.setMaxChunkSizeBytes(maxChunkSize);
    shardSvrRequest.setFromShard(fromShardId);
    shardSvrRequest.setCollectionTimestamp(coll.getTimestamp());
    shardSvrRequest.setEpoch(coll.getEpoch());
    const auto [secondaryThrottle, wc] =
        getSecondaryThrottleAndWriteConcern(request.getSecondaryThrottle());
    shardSvrRequest.setSecondaryThrottle(secondaryThrottle);
    shardSvrRequest.setForceJumbo(request.getForceJumbo());

    auto response =
        _commandScheduler->requestMoveRange(opCtx, shardSvrRequest, wc, issuedByRemoteUser)
            .getNoThrow(opCtx);
    return processManualMigrationOutcome(opCtx,
                                         request.getMin(),
                                         request.getMax(),
                                         nss,
                                         shardSvrRequest.getToShard(),
                                         std::move(response));
}

void Balancer::report(OperationContext* opCtx, BSONObjBuilder* builder) {
    auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();
    balancerConfig->refreshAndCheck(opCtx).ignore();

    const auto mode = balancerConfig->getBalancerMode();

    stdx::lock_guard<Latch> scopedLock(_mutex);
    builder->append("mode", BalancerSettingsType::kBalancerModes[mode]);
    builder->append("inBalancerRound", _inBalancerRound);
    builder->append("numBalancerRounds", _numBalancerRounds);
    builder->append("term", repl::ReplicationCoordinator::get(opCtx)->getTerm());
}

void Balancer::_consumeActionStreamLoop() {
    Client::initThread("BalancerSecondary",
                       getGlobalServiceContext()->getService(ClusterRole::ShardServer));

    auto opCtx = cc().makeOperationContext();
    executor::ScopedTaskExecutor executor(
        Grid::get(opCtx.get())->getExecutorPool()->getFixedExecutor());

    ScopeGuard onExitCleanup([this, &opCtx, &executor] {
        _defragmentationPolicy->interruptAllDefragmentations();
        try {
            _autoMergerPolicy->disable(opCtx.get());
        } catch (const DBException& e) {
            LOGV2_WARNING(8145100,
                          "Failed to log in config.changelog when disabling the auto merger",
                          "error"_attr = redact(e));
        }
        // Explicitly cancel and drain any outstanding streaming action already dispatched to the
        // task executor.
        executor->shutdown();
        executor->join();
        // When shutting down, the task executor may or may not invoke the
        // applyActionResponseTo()callback for canceled streaming actions: to ensure a consistent
        // state of the balancer after a step down, _outstandingStreamingOps needs then to be reset
        // to 0 once all the tasks have been drained.
        _outstandingStreamingOps.store(0);
    });

    // Lambda function to sleep for throttling
    auto applyThrottling =
        [lastActionTime = Date_t::fromMillisSinceEpoch(0)](const Milliseconds throttle) mutable {
            auto timeSinceLastAction = Date_t::now() - lastActionTime;
            if (throttle > timeSinceLastAction) {
                auto sleepingTime = throttle - timeSinceLastAction;
                LOGV2_DEBUG(6443700,
                            2,
                            "Applying throttling on balancer secondary thread",
                            "sleepingTime"_attr = sleepingTime);
                sleepFor(sleepingTime);
            }
            lastActionTime = Date_t::now();
        };

    auto backOff = Backoff(Seconds(1), Milliseconds::max());
    bool errorOccurred = false;

    while (true) {
        {
            stdx::unique_lock<Latch> ul(_mutex);

            // Keep asking for more actions if we meet all these conditions:
            //  - Balancer is in kRunning state
            //  - There are less than kMaxOutstandingStreamingOperations
            //  - There were  actions to schedule on the previous iteration or there is an update on
            //  the streams state
            auto stopWaitingCondition = [&] {
                return _threadSetState != ThreadSetState::Running ||
                    (_outstandingStreamingOps.load() <= kMaxOutstandingStreamingOperations &&
                     _actionStreamsStateUpdated.load());
            };

            if (!errorOccurred) {
                _actionStreamCondVar.wait(ul, stopWaitingCondition);
            } else {
                // Enable retries in case of error by performing a backoff
                _actionStreamCondVar.wait_for(
                    ul, backOff.nextSleep().toSystemDuration(), stopWaitingCondition);
            }

            if (_threadSetState != ThreadSetState::Running) {
                break;
            }
        }

        // Clear flags
        errorOccurred = false;
        _actionStreamsStateUpdated.store(false);

        // Get active streams
        auto activeStreams = [&]() -> std::vector<ActionsStreamPolicy*> {
            auto balancerConfig = Grid::get(opCtx.get())->getBalancerConfiguration();
            std::vector<ActionsStreamPolicy*> streams;
            if (balancerConfig->shouldBalanceForAutoMerge() && _autoMergerPolicy->isEnabled()) {
                streams.push_back(_autoMergerPolicy.get());
            }
            if (balancerConfig->shouldBalance()) {
                streams.push_back(_defragmentationPolicy.get());
            }
            return streams;
        }();

        // Get next action from a random stream together with its stream
        auto [nextAction, sourcedStream] =
            [&]() -> std::tuple<boost::optional<BalancerStreamAction>, ActionsStreamPolicy*> {
            auto client = opCtx->getClient();
            std::shuffle(activeStreams.begin(), activeStreams.end(), client->getPrng().urbg());
            for (auto stream : activeStreams) {
                try {
                    auto action = stream->getNextStreamingAction(opCtx.get());
                    if (action.has_value()) {
                        return std::make_tuple(std::move(action), stream);
                    }

                } catch (const DBException& e) {
                    LOGV2_WARNING(7435001,
                                  "Failed to get next action from action stream",
                                  "error"_attr = redact(e),
                                  "stream"_attr = stream->getName());

                    errorOccurred = true;
                }
            }
            return std::make_tuple(boost::none, nullptr);
        }();

        if (!nextAction.has_value()) {
            continue;
        }

        // Signal there are still actions to be consumed by next iteration
        _actionStreamsStateUpdated.store(true);

        _outstandingStreamingOps.fetchAndAdd(1);
        visit(OverloadedVisitor{
                  [&, stream = sourcedStream](MergeInfo&& mergeAction) {
                      applyThrottling(Milliseconds(chunkDefragmentationThrottlingMS.load()));
                      auto result =
                          _commandScheduler
                              ->requestMergeChunks(opCtx.get(),
                                                   mergeAction.nss,
                                                   mergeAction.shardId,
                                                   mergeAction.chunkRange,
                                                   mergeAction.collectionPlacementVersion)
                              .thenRunOn(*executor)
                              .onCompletion([this, stream, action = std::move(mergeAction)](
                                                const Status& status) {
                                  _applyStreamingActionResponseToPolicy(action, status, stream);
                              });
                  },
                  [&, stream = sourcedStream](DataSizeInfo&& dataSizeAction) {
                      auto result =
                          _commandScheduler
                              ->requestDataSize(opCtx.get(),
                                                dataSizeAction.nss,
                                                dataSizeAction.shardId,
                                                dataSizeAction.chunkRange,
                                                dataSizeAction.version,
                                                dataSizeAction.keyPattern,
                                                dataSizeAction.estimatedValue,
                                                dataSizeAction.maxSize)
                              .thenRunOn(*executor)
                              .onCompletion([this, stream, action = std::move(dataSizeAction)](
                                                const StatusWith<DataSizeResponse>& swDataSize) {
                                  _applyStreamingActionResponseToPolicy(action, swDataSize, stream);
                              });
                  },
                  [&, stream = sourcedStream](MergeAllChunksOnShardInfo&& mergeAllChunksAction) {
                      if (mergeAllChunksAction.applyThrottling) {
                          applyThrottling(Milliseconds(autoMergerThrottlingMS.load()));
                      }

                      auto result =
                          _commandScheduler
                              ->requestMergeAllChunksOnShard(opCtx.get(),
                                                             mergeAllChunksAction.nss,
                                                             mergeAllChunksAction.shardId)
                              .thenRunOn(*executor)
                              .onCompletion(
                                  [this, stream, action = mergeAllChunksAction](
                                      const StatusWith<NumMergedChunks>& swNumMergedChunks) {
                                      _applyStreamingActionResponseToPolicy(
                                          action, swNumMergedChunks, stream);
                                  });
                  },
                  [](MigrateInfo&& _) {
                      uasserted(ErrorCodes::BadValue,
                                "Migrations cannot be processed as Streaming Actions");
                  }},
              std::move(nextAction.value()));
    }
}

void Balancer::_mainThread() {
    ON_BLOCK_EXIT([this] {
        {
            stdx::lock_guard<Latch> scopedLock(_mutex);
            _threadSetState = ThreadSetState::Terminated;
            LOGV2_DEBUG(21855, 1, "Balancer thread set terminated");
        }
        _joinCond.notify_all();
    });

    ThreadClient threadClient("Balancer",
                              getGlobalServiceContext()->getService(ClusterRole::ShardServer));
    auto opCtx = threadClient->makeOperationContext();

    // TODO(SERVER-74658): Please revisit if this thread could be made killable.
    {
        stdx::lock_guard<Client> lk(*threadClient);
        threadClient->setSystemOperationUnkillableByStepdown(lk);
    }

    auto shardingContext = Grid::get(opCtx.get());

    LOGV2(21856, "CSRS balancer is starting");

    {
        stdx::lock_guard<Latch> scopedLock(_mutex);
        _threadOperationContext = opCtx.get();
    }

    const Seconds kInitBackoffInterval(10);

    auto balancerConfig = shardingContext->getBalancerConfiguration();
    while (!_terminationRequested()) {
        Status refreshStatus = balancerConfig->refreshAndCheck(opCtx.get());
        if (!refreshStatus.isOK()) {
            LOGV2_WARNING(21876,
                          "Got error while refreshing balancer settings, will retry with a backoff",
                          "backoffInterval"_attr = Milliseconds(kInitBackoffInterval),
                          "error"_attr = refreshStatus);

            _sleepFor(opCtx.get(), kInitBackoffInterval);
            continue;
        }

        break;
    }

    LOGV2(6036605, "Starting command scheduler");

    _commandScheduler->start(opCtx.get());

    _actionStreamConsumerThread = stdx::thread([&] { _consumeActionStreamLoop(); });

    LOGV2(6036606, "Balancer worker thread initialised. Entering main loop.");

    // Main balancer loop
    auto lastMigrationTime = Date_t::fromMillisSinceEpoch(0);
    BalancerWarning balancerWarning;
    while (!_terminationRequested()) {
        BalanceRoundDetails roundDetails;

        _beginRound(opCtx.get());

        try {
            shardingContext->shardRegistry()->reload(opCtx.get());

            uassert(13258, "oids broken after resetting!", _checkOIDs(opCtx.get()));

            Status refreshStatus = balancerConfig->refreshAndCheck(opCtx.get());
            if (!refreshStatus.isOK()) {
                LOGV2_WARNING(21877, "Skipping balancing round", "error"_attr = refreshStatus);
                _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
                continue;
            }

            // Warn before we skip the iteration due to balancing being disabled.
            balancerWarning.warnIfRequired(opCtx.get(), balancerConfig->getBalancerMode());

            if (!balancerConfig->shouldBalance() || _terminationRequested()) {
                _autoMergerPolicy->disable(opCtx.get());

                LOGV2_DEBUG(21859, 1, "Skipping balancing round because balancing is disabled");
                _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
                continue;
            }

            if (balancerConfig->shouldBalanceForAutoMerge()) {
                _autoMergerPolicy->enable(opCtx.get());
            }

            LOGV2_DEBUG(21860,
                        1,
                        "Start balancing round",
                        "waitForDelete"_attr = balancerConfig->waitForDelete(),
                        "secondaryThrottle"_attr = balancerConfig->getSecondaryThrottle().toBSON());

            // Collect and apply up-to-date configuration values on the cluster collections.
            _defragmentationPolicy->startCollectionDefragmentations(opCtx.get());

            // Reactivate the Automerger if needed.
            _autoMergerPolicy->checkInternalUpdates(opCtx.get());

            // The current configuration is allowing the balancer to perform operations.
            // Unblock the secondary thread if needed.
            _actionStreamCondVar.notify_all();

            // Split chunk to match zones boundaries
            {
                Status status = _splitChunksIfNeeded(opCtx.get());
                if (!status.isOK()) {
                    LOGV2_WARNING(21878, "Failed to split chunks", "error"_attr = status);
                } else {
                    LOGV2_DEBUG(21861, 1, "Done enforcing zone range boundaries.");
                }
            }

            // Select and migrate chunks
            {
                Timer selectionTimer;

                const std::vector<ClusterStatistics::ShardStatistics> shardStats =
                    uassertStatusOK(_clusterStats->getStats(opCtx.get()));

                stdx::unordered_set<ShardId> availableShards;
                availableShards.reserve(shardStats.size());
                std::transform(
                    shardStats.begin(),
                    shardStats.end(),
                    std::inserter(availableShards, availableShards.end()),
                    [](const ClusterStatistics::ShardStatistics& shardStatistics) -> ShardId {
                        return shardStatistics.shardId;
                    });

                const auto unshardedToMove = _moveUnshardedPolicy->selectCollectionsToMove(
                    opCtx.get(), shardStats, &availableShards);

                const auto chunksToDefragment =
                    _defragmentationPolicy->selectChunksToMove(opCtx.get(), &availableShards);

                const auto chunksToRebalance =
                    uassertStatusOK(_chunkSelectionPolicy->selectChunksToMove(
                        opCtx.get(), shardStats, &availableShards, &_imbalancedCollectionsCache));
                const Milliseconds selectionTimeMillis{selectionTimer.millis()};

                if (chunksToRebalance.empty() && chunksToDefragment.empty() &&
                    unshardedToMove.empty()) {
                    LOGV2_DEBUG(21862, 1, "No need to move any chunk");
                    _balancedLastTime = {};
                    LOGV2_DEBUG(21863, 1, "End balancing round");
                    // Set to 100ms when executed in context of a test
                    _endRound(opCtx.get(),
                              TestingProctor::instance().isEnabled()
                                  ? Milliseconds(100)
                                  : kBalanceRoundDefaultInterval);
                } else {

                    // Sleep according to the migration throttling settings
                    const auto throttleTimeMillis = [&] {
                        const auto& minRoundinterval =
                            Milliseconds(balancerMigrationsThrottlingMs.load());

                        const auto timeSinceLastMigration = Date_t::now() - lastMigrationTime;
                        if (timeSinceLastMigration < minRoundinterval) {
                            return minRoundinterval - timeSinceLastMigration;
                        }
                        return Milliseconds::zero();
                    }();
                    _sleepFor(opCtx.get(), throttleTimeMillis);

                    // Migrate chunks
                    Timer migrationTimer;
                    _balancedLastTime = _doMigrations(
                        opCtx.get(), unshardedToMove, chunksToRebalance, chunksToDefragment);
                    lastMigrationTime = Date_t::now();
                    const Milliseconds migrationTimeMillis{migrationTimer.millis()};

                    // Complete round
                    roundDetails.setSucceeded(
                        static_cast<int>(chunksToRebalance.size() + chunksToDefragment.size()),
                        _balancedLastTime.rebalancedChunks + _balancedLastTime.defragmentedChunks,
                        _imbalancedCollectionsCache.size(),
                        static_cast<int>(unshardedToMove.size()),
                        _balancedLastTime.unshardedCollections,
                        selectionTimeMillis,
                        throttleTimeMillis,
                        migrationTimeMillis);

                    auto catalogManager = ShardingCatalogManager::get(opCtx.get());
                    ShardingLogging::get(opCtx.get())
                        ->logAction(opCtx.get(),
                                    "balancer.round",
                                    NamespaceString::kEmpty,
                                    roundDetails.toBSON(),
                                    catalogManager->localConfigShard(),
                                    catalogManager->localCatalogClient())
                        .ignore();

                    LOGV2_DEBUG(6679500, 1, "End balancing round");
                    // Migration throttling of `balancerMigrationsThrottlingMs` will be applied
                    // before the next call to _doMigrations, so don't sleep here.
                    _endRound(opCtx.get(), Milliseconds(0));
                }
            }
        } catch (const DBException& e) {
            LOGV2(21865, "Error while doing balance", "error"_attr = e);

            // Just to match the opening statement if in log level 1
            LOGV2_DEBUG(21866, 1, "End balancing round");

            // This round failed, tell the world!
            roundDetails.setFailed(e.what());

            auto catalogManager = ShardingCatalogManager::get(opCtx.get());
            ShardingLogging::get(opCtx.get())
                ->logAction(opCtx.get(),
                            "balancer.round",
                            NamespaceString::kEmpty,
                            roundDetails.toBSON(),
                            catalogManager->localConfigShard(),
                            catalogManager->localCatalogClient())
                .ignore();

            // Sleep a fair amount before retrying because of the error
            _endRound(opCtx.get(), kBalanceRoundDefaultInterval);
        }
    }

    {
        stdx::lock_guard<Latch> scopedLock(_mutex);
        invariant(_threadSetState == ThreadSetState::Terminating);
    }

    _commandScheduler->stop();

    _actionStreamConsumerThread.join();


    {
        stdx::lock_guard<Latch> scopedLock(_mutex);
        _threadOperationContext = nullptr;
    }

    LOGV2(21867, "CSRS balancer is now stopped");
}

void Balancer::_applyStreamingActionResponseToPolicy(const BalancerStreamAction& action,
                                                     const BalancerStreamActionResponse& response,
                                                     ActionsStreamPolicy* policy) {
    tassert(8245242, "No action in progress", _outstandingStreamingOps.addAndFetch(-1) >= 0);
    ThreadClient tc("BalancerSecondaryThread::applyActionResponse",
                    getGlobalServiceContext()->getService(ClusterRole::ShardServer));

    auto opCtx = tc->makeOperationContext();
    policy->applyActionResult(opCtx.get(), action, response);
};

bool Balancer::_terminationRequested() {
    stdx::lock_guard<Latch> scopedLock(_mutex);
    return (_threadSetState != ThreadSetState::Running);
}

void Balancer::_beginRound(OperationContext* opCtx) {
    stdx::unique_lock<Latch> lock(_mutex);
    _inBalancerRound = true;
    _condVar.notify_all();
}

void Balancer::_endRound(OperationContext* opCtx, Milliseconds waitTimeout) {
    {
        stdx::lock_guard<Latch> lock(_mutex);
        _inBalancerRound = false;
        _numBalancerRounds++;
        _condVar.notify_all();
    }

    MONGO_IDLE_THREAD_BLOCK;
    _sleepFor(opCtx, waitTimeout);
}

void Balancer::_sleepFor(OperationContext* opCtx, Milliseconds waitTimeout) {
    stdx::unique_lock<Latch> lock(_mutex);
    _condVar.wait_for(lock, waitTimeout.toSystemDuration(), [&] {
        return _threadSetState != ThreadSetState::Running;
    });
}

bool Balancer::_checkOIDs(OperationContext* opCtx) {
    auto shardingContext = Grid::get(opCtx);

    const auto all = shardingContext->shardRegistry()->getAllShardIds(opCtx);

    // map of OID machine ID => shardId
    map<int, ShardId> oids;

    for (const ShardId& shardId : all) {
        if (_terminationRequested()) {
            return false;
        }

        auto shardStatus = shardingContext->shardRegistry()->getShard(opCtx, shardId);
        if (!shardStatus.isOK()) {
            continue;
        }
        const auto s = std::move(shardStatus.getValue());

        auto result = uassertStatusOK(
            s->runCommandWithFixedRetryAttempts(opCtx,
                                                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                DatabaseName::kAdmin,
                                                BSON("features" << 1),
                                                Seconds(30),
                                                Shard::RetryPolicy::kIdempotent));
        uassertStatusOK(result.commandStatus);
        BSONObj f = std::move(result.response);

        if (f["oidMachine"].isNumber()) {
            int x = f["oidMachine"].numberInt();
            if (oids.count(x) == 0) {
                oids[x] = shardId;
            } else {
                LOGV2(21868,
                      "Two machines have the same oidMachine value",
                      "oidMachine"_attr = x,
                      "firstShardId"_attr = shardId,
                      "secondShardId"_attr = oids[x]);

                result = uassertStatusOK(s->runCommandWithFixedRetryAttempts(
                    opCtx,
                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                    DatabaseName::kAdmin,
                    BSON("features" << 1 << "oidReset" << 1),
                    Seconds(30),
                    Shard::RetryPolicy::kIdempotent));
                uassertStatusOK(result.commandStatus);

                auto otherShardStatus = shardingContext->shardRegistry()->getShard(opCtx, oids[x]);
                if (otherShardStatus.isOK()) {
                    result = uassertStatusOK(
                        otherShardStatus.getValue()->runCommandWithFixedRetryAttempts(
                            opCtx,
                            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                            DatabaseName::kAdmin,
                            BSON("features" << 1 << "oidReset" << 1),
                            Seconds(30),
                            Shard::RetryPolicy::kIdempotent));
                    uassertStatusOK(result.commandStatus);
                }

                return false;
            }
        } else {
            LOGV2(21869, "warning: oidMachine not set on shard", "shard"_attr = s->toString());
        }
    }

    return true;
}

Status Balancer::_splitChunksIfNeeded(OperationContext* opCtx) {
    auto chunksToSplitStatus = _chunkSelectionPolicy->selectChunksToSplit(opCtx);
    if (!chunksToSplitStatus.isOK()) {
        return chunksToSplitStatus.getStatus();
    }

    for (const auto& splitInfo : chunksToSplitStatus.getValue()) {
        auto routingInfoStatus =
            Grid::get(opCtx)->catalogCache()->getShardedCollectionRoutingInfoWithPlacementRefresh(
                opCtx, splitInfo.nss);
        if (!routingInfoStatus.isOK()) {
            return routingInfoStatus.getStatus();
        }

        const auto& [cm, _] = routingInfoStatus.getValue();

        auto splitStatus = shardutil::splitChunkAtMultiplePoints(
            opCtx,
            splitInfo.shardId,
            splitInfo.nss,
            cm.getShardKeyPattern(),
            splitInfo.collectionPlacementVersion.epoch(),
            splitInfo.collectionPlacementVersion.getTimestamp(),
            ChunkRange(splitInfo.minKey, splitInfo.maxKey),
            splitInfo.splitKeys);
        if (!splitStatus.isOK()) {
            LOGV2_WARNING(21879,
                          "Failed to split chunk",
                          "splitInfo"_attr = redact(splitInfo.toString()),
                          "error"_attr = redact(splitStatus));
        }
    }

    return Status::OK();
}

Balancer::MigrationStats Balancer::_doMigrations(OperationContext* opCtx,
                                                 const MigrateInfoVector& unshardedToMove,
                                                 const MigrateInfoVector& chunksToRebalance,
                                                 const MigrateInfoVector& chunksToDefragment) {
    auto balancerConfig = Grid::get(opCtx)->getBalancerConfiguration();

    // If the balancer was disabled since we started this round, don't start new chunk moves
    if (const bool terminating = _terminationRequested(), enabled = balancerConfig->shouldBalance();
        terminating || !enabled) {
        LOGV2_DEBUG(21870,
                    1,
                    "Skipping balancing round",
                    "terminating"_attr = terminating,
                    "balancerEnabled"_attr = enabled);
        return {};
    }

    std::array<std::unique_ptr<MigrationTask>, 3> allMigrationTasks = {
        make_unique<MigrateUnshardedCollectionTask>(
            opCtx, *_moveUnshardedPolicy, *_commandScheduler, unshardedToMove),
        make_unique<RebalanceChunkTask>(opCtx, *_commandScheduler, chunksToRebalance),
        make_unique<DefragmentChunkTask>(
            opCtx, *_defragmentationPolicy, *_commandScheduler, chunksToDefragment)};

    for (const auto& migrationTask : allMigrationTasks) {
        migrationTask->enqueue();
    }

    for (const auto& migrationTask : allMigrationTasks) {
        migrationTask->waitForQueuedAndProcessResponses();
    }

    return MigrationStats{allMigrationTasks[0]->getNumCompleted(),
                          allMigrationTasks[1]->getNumCompleted(),
                          allMigrationTasks[2]->getNumCompleted()};
}

void Balancer::_onActionsStreamPolicyStateUpdate() {
    // On any internal update of the defragmentation/cluster chunks resize policy status,
    // wake up the thread consuming the stream of actions
    _actionStreamsStateUpdated.store(true);
    _actionStreamCondVar.notify_all();
}

void Balancer::notifyPersistedBalancerSettingsChanged(OperationContext* opCtx) {
    if (!Grid::get(opCtx)->getBalancerConfiguration()->shouldBalanceForAutoMerge()) {
        _autoMergerPolicy->disable(opCtx);
    }

    // Try to awake the main balancer thread
    _condVar.notify_all();
}

void Balancer::abortCollectionDefragmentation(OperationContext* opCtx, const NamespaceString& nss) {
    _defragmentationPolicy->abortCollectionDefragmentation(opCtx, nss);
}

BalancerCollectionStatusResponse Balancer::getBalancerStatusForNs(OperationContext* opCtx,
                                                                  const NamespaceString& ns) {
    const auto catalogClient = ShardingCatalogManager::get(opCtx)->localCatalogClient();
    CollectionType coll;
    try {
        coll = catalogClient->getCollection(opCtx, ns, {});
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        uasserted(ErrorCodes::NamespaceNotSharded, "Collection unsharded or undefined");
    }


    const auto maxChunkSizeBytes = getMaxChunkSizeBytes(opCtx, coll);
    double maxChunkSizeMB = (double)maxChunkSizeBytes / (1024 * 1024);
    // Keep only 2 decimal digits to return a readable value
    maxChunkSizeMB = std::ceil(maxChunkSizeMB * 100.0) / 100.0;

    BalancerCollectionStatusResponse response(maxChunkSizeMB, true /*balancerCompliant*/);
    auto setViolationOnResponse =
        [&response](StringData reason, const boost::optional<BSONObj>& details = boost::none) {
            response.setBalancerCompliant(false);
            response.setFirstComplianceViolation(reason);
            response.setDetails(details);
        };

    bool isDefragmenting = coll.getDefragmentCollection();
    if (isDefragmenting) {
        setViolationOnResponse(kBalancerPolicyStatusDefragmentingChunks,
                               _defragmentationPolicy->reportProgressOn(coll.getUuid()));
        return response;
    }

    auto splitChunks = uassertStatusOK(_chunkSelectionPolicy->selectChunksToSplit(opCtx, ns));
    if (!splitChunks.empty()) {
        setViolationOnResponse(kBalancerPolicyStatusZoneViolation);
        return response;
    }

    auto [_, reason] = uassertStatusOK(_chunkSelectionPolicy->selectChunksToMove(opCtx, ns));

    switch (reason) {
        case MigrationReason::none:
            break;
        case MigrationReason::drain:
            setViolationOnResponse(kBalancerPolicyStatusDraining);
            break;
        case MigrationReason::zoneViolation:
            setViolationOnResponse(kBalancerPolicyStatusZoneViolation);
            break;
        case MigrationReason::chunksImbalance:
            setViolationOnResponse(kBalancerPolicyStatusChunksImbalance);
            break;
    }

    return response;
}

}  // namespace mongo
