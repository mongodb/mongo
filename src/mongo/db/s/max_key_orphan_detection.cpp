/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/max_key_orphan_detection.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/shard_key_index_util.h"
#include "mongo/db/global_catalog/shard_key_pattern.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_chunk_range.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/s/active_migrations_registry.h"
#include "mongo/db/s/max_key_orphan_scan_state_gen.h"
#include "mongo/db/s/range_deletion_task_gen.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/sharding_environment/sharding_runtime_d_params_gen.h"
#include "mongo/db/sharding_environment/sharding_statistics.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log_and_backoff.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <mutex>
#include <string_view>
#include <vector>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

// Pauses the MaxKey orphan inventory scan just before it upserts the state document, so tests
// can deterministically trigger stepdown while the scan is mid-flight.
MONGO_FAIL_POINT_DEFINE(hangBeforePersistingMaxKeyOrphanScanState);

// Pauses the MaxKey orphan inventory scan while it is iterating collections (before it inspects the
// next one), so tests can run operations such as chunk migrations concurrently with the scan's
// detection phase rather than only with the final state-doc upsert.
MONGO_FAIL_POINT_DEFINE(hangDuringMaxKeyOrphanScan);

namespace {

constexpr std::string_view kMaxKeyOrphanScanStateId = "scanState";

/**
 * Errors that abandon the entire sweep so the next step-up restarts it; everything else is a
 * per-collection skip.
 */
bool isScanFatalError(ErrorCodes::Error code) {
    return ErrorCodes::isShutdownError(code) || ErrorCodes::isCancellationError(code) ||
        ErrorCodes::isNotPrimaryError(code) || code == ErrorCodes::InterruptedDueToReplStateChange;
}

/**
 * Returns the largest shard-key value present in the collection if its leading field is MaxKey,
 * else boost::none.
 */
boost::optional<BSONObj> rightmostMaxKeyPrefixedShardKey(OperationContext* opCtx,
                                                         const DatabaseName& dbName,
                                                         const UUID& collUuid,
                                                         const ShardKeyPattern& shardKeyPattern) {
    const auto acquisition = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, NamespaceStringOrUUID{dbName, collUuid}, AcquisitionPrerequisites::kRead),
        MODE_IS);
    if (!acquisition.exists()) {
        return boost::none;
    }

    // Resolve the shard-key-prefixed index. If none exists, skip the collection with a log.
    const auto shardKeyIdx = findShardKeyPrefixedIndex(opCtx,
                                                       acquisition.getCollectionPtr(),
                                                       shardKeyPattern.toBSON(),
                                                       /*requireSingleKey=*/false);
    if (!shardKeyIdx) {
        LOGV2_WARNING(12799001,
                      "MaxKey orphan detection: skipping collection with no shard-key index",
                      "dbName"_attr = dbName,
                      "collectionUUID"_attr = collUuid,
                      "shardKeyPattern"_attr = shardKeyPattern.toBSON());
        return boost::none;
    }

    // Scan the shard-key index backwards over the whole keyspace and take the first entry, i.e. the
    // largest shard-key value present. Inclusive bounds so a shard key that is exactly the global
    // max is returned.
    const KeyPattern kp(shardKeyIdx->keyPattern());
    const BSONObj minKey = Helpers::toKeyFormat(kp.globalMin());
    const BSONObj maxKey = Helpers::toKeyFormat(kp.globalMax());
    auto exec = InternalPlanner::shardKeyIndexScan(opCtx,
                                                   acquisition,
                                                   *shardKeyIdx,
                                                   maxKey,
                                                   minKey,
                                                   BoundInclusion::kIncludeBothStartAndEndKeys,
                                                   PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                   InternalPlanner::BACKWARD);

    BSONObj indexKey;
    if (exec->getNext(&indexKey, nullptr) == PlanExecutor::IS_EOF) {
        return boost::none;
    }

    // Reshuffle fields according to the shard key pattern (the scan yields index keys with stripped
    // field names; drop any trailing fields of a wider index).
    const BSONObj shardKey =
        BSONObjBuilder().appendElementsRenamed(indexKey, shardKeyPattern.toBSON(), false).obj();
    if (!isMaxKeyPrefixedShardKey(shardKey)) {
        return boost::none;
    }
    return shardKey.getOwned();
}

/**
 * Returns true if any outstanding range-deletion task for 'collUuid' already covers 'shardKey', in
 * which case the cleanup machinery will delete the document and detection should skip it.
 */
bool rangeDeletionCoversKey(OperationContext* opCtx,
                            const UUID& collUuid,
                            const BSONObj& shardKey) {
    const std::string rangeMinField = std::string{RangeDeletionTask::kRangeFieldName} + "." +
        std::string{ChunkRange::kMinFieldName};
    const std::string rangeMaxField = std::string{RangeDeletionTask::kRangeFieldName} + "." +
        std::string{ChunkRange::kMaxFieldName};

    const bool keyIsGlobalMax = [&] {
        for (auto&& elem : shardKey) {
            if (elem.type() != BSONType::maxKey) {
                return false;
            }
        }
        return true;
    }();

    DBDirectClient client(opCtx);
    FindCommandRequest findCmd(NamespaceString::kRangeDeletionNamespace);
    findCmd.setFilter(
        BSON(RangeDeletionTask::kCollectionUuidFieldName
             << collUuid << rangeMinField << BSON("$lte" << shardKey) << rangeMaxField
             << (keyIsGlobalMax ? BSON("$gte" << shardKey) : BSON("$gt" << shardKey))));
    auto cursor = client.find(std::move(findCmd));
    return cursor && cursor->more();
}

/**
 * Classifies a single sharded collection. Returns true iff a document whose leading shard-key field
 * is MaxKey exists locally that this shard does not legitimately own.
 */
bool detectMaxKeyOrphanForCollection(OperationContext* opCtx,
                                     const CollectionType& collType,
                                     const ShardId& myShardId) {
    // getCollection() also returns tracked-but-unsharded collections, skip them.
    if (collType.getUnsplittable()) {
        return false;
    }

    const auto& nss = collType.getNss();
    const auto collUuid = collType.getUuid();

    const ShardKeyPattern shardKeyPattern(collType.getKeyPattern());
    if (shardKeyPattern.isHashedPattern()) {
        return false;
    }

    // Cheap pre-check before taking the migration-blocking guard.
    if (!rightmostMaxKeyPrefixedShardKey(opCtx, nss.dbName(), collUuid, shardKeyPattern)) {
        return false;
    }

    hangDuringMaxKeyOrphanScan.pauseWhileSet(opCtx);

    MigrationBlockingGuard guard(
        opCtx, str::stream() << "MaxKey orphan detection for collection " << collUuid);

    // Re-check authoritatively now that migrations are blocked; the doc may have been migrated or
    // deleted between the pre-check and acquiring the guard.
    auto candidateKey =
        rightmostMaxKeyPrefixedShardKey(opCtx, nss.dbName(), collUuid, shardKeyPattern);
    if (!candidateKey) {
        return false;
    }

    // An outstanding range-deletion task that covers the candidate will delete it through the
    // normal cleanup path, so it is a transient orphan, not a finding.
    if (rangeDeletionCoversKey(opCtx, collUuid, *candidateKey)) {
        return false;
    }

    auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss));
    if (!cm.hasRoutingTable() || !cm.uuidMatches(collUuid)) {
        return false;
    }
    if (cm.keyBelongsToShard(*candidateKey, myShardId)) {
        return false;
    }
    return true;
}

/**
 * Owns the detector thread and the OperationContext it runs on, so a stepdown can interrupt and
 * join it before the next term launches a fresh sweep.
 */
class MaxKeyOrphanDetectionCoordinator {
public:
    static MaxKeyOrphanDetectionCoordinator& get(ServiceContext* serviceContext);

    void launch(ServiceContext* serviceContext, long long term) {
        cancelAndJoin();

        std::lock_guard<std::mutex> lk(_mutex);
        _canceled = false;
        _thread = stdx::thread([this, serviceContext, term] {
            ThreadClient tc("MaxKeyOrphanDetection", serviceContext->getService());
            auto uniqueOpCtx = tc->makeOperationContext();
            auto opCtx = uniqueOpCtx.get();
            opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

            {
                std::lock_guard<std::mutex> lk(_mutex);
                if (_canceled) {
                    return;
                }
                _opCtx = opCtx;
            }
            ON_BLOCK_EXIT([this, opCtx] {
                std::lock_guard<std::mutex> lk(_mutex);
                if (_opCtx == opCtx) {
                    _opCtx = nullptr;
                }
            });

            try {
                runMaxKeyOrphanDetection(opCtx, term);
            } catch (const DBException& ex) {
                LOGV2_DEBUG(12799000,
                            2,
                            "MaxKey orphan detector exited with an error; the next stepup will "
                            "retry",
                            "term"_attr = term,
                            "error"_attr = redact(ex.toStatus()));
            }
        });
    }

    void cancelAndJoin() {
        stdx::thread toJoin;
        {
            std::lock_guard<std::mutex> lk(_mutex);
            _canceled = true;
            if (_opCtx) {
                ClientLock clientLock(_opCtx->getClient());
                _opCtx->getServiceContext()->killOperation(
                    clientLock, _opCtx, ErrorCodes::Interrupted);
            }
            toJoin = std::move(_thread);
        }
        if (toJoin.joinable()) {
            toJoin.join();
        }
    }

private:
    std::mutex _mutex;
    stdx::thread _thread;
    OperationContext* _opCtx = nullptr;
    bool _canceled = false;
};

const auto getMaxKeyOrphanDetectionCoordinator =
    ServiceContext::declareDecoration<MaxKeyOrphanDetectionCoordinator>();

MaxKeyOrphanDetectionCoordinator& MaxKeyOrphanDetectionCoordinator::get(
    ServiceContext* serviceContext) {
    return getMaxKeyOrphanDetectionCoordinator(serviceContext);
}

}  // namespace

bool isMaxKeyPrefixedShardKey(const BSONObj& shardKeyValue) {
    return !shardKeyValue.isEmpty() && shardKeyValue.firstElement().type() == BSONType::maxKey;
}

void runMaxKeyOrphanDetection(OperationContext* opCtx, long long term) {
    LOGV2_DEBUG(12799002, 2, "Starting MaxKey orphan detection", "term"_attr = term);

    // Wait until the node is writable primary.
    if (gFeatureFlagIntentRegistration.isEnabled()) {
        auto* replCoord = repl::ReplicationCoordinator::get(opCtx);
        for (size_t retryAttempts = 0; !replCoord->canAcceptNonLocalWrites(); ++retryAttempts) {
            opCtx->checkForInterrupt();
            if (!replCoord->getMemberState().primary()) {
                LOGV2_DEBUG(12799003,
                            2,
                            "Aborting MaxKey orphan detection: node started a step down before "
                            "becoming a writable primary",
                            "term"_attr = term);
                return;
            }
            logAndBackoff(12799004,
                          MONGO_LOGV2_DEFAULT_COMPONENT,
                          logv2::LogSeverity::Debug(2),
                          retryAttempts,
                          "Waiting until node is writable primary before MaxKey orphan scan");
        }
    }

    auto publishOrphanScanStats = [&](bool foundMaxKey, bool alertEmitted) {
        auto& stats = ShardingStatistics::get(opCtx);
        stats.maxKeyOrphanScanComplete.store(1);
        stats.maxKeyOrphanScanFoundMaxKey.store(foundMaxKey ? 1 : 0);
        stats.maxKeyOrphanScanAlertEmitted.store(alertEmitted ? 1 : 0);
    };

    // Read the prior state doc. A completed sweep short-circuits this one-shot sweep; the prior
    // alertEmitted is preserved so a re-scan never downgrades it.
    boost::optional<MaxKeyOrphanScanState> priorState;
    try {
        DBDirectClient client(opCtx);
        FindCommandRequest findCmd(NamespaceString::kConfigMaxKeyOrphanScanStateNamespace);
        findCmd.setFilter(BSON("_id" << kMaxKeyOrphanScanStateId));
        findCmd.setLimit(1);
        auto cursor = client.find(std::move(findCmd));
        if (cursor && cursor->more()) {
            priorState.emplace(MaxKeyOrphanScanState::parse(
                cursor->next(), IDLParserContext("runMaxKeyOrphanDetection")));
        }
    } catch (const DBException& ex) {
        if (isScanFatalError(ex.code())) {
            throw;
        }
        LOGV2_DEBUG(12799005,
                    2,
                    "MaxKey orphan detection: failed to read prior state doc; treating as missing",
                    "term"_attr = term,
                    "error"_attr = redact(ex.toStatus()));
    }

    if (priorState && priorState->getScanCompletedAt().has_value()) {
        publishOrphanScanStats(priorState->getFoundMaxKey(), priorState->getAlertEmitted());
        LOGV2_DEBUG(12799006,
                    2,
                    "Skipping MaxKey orphan detection: prior sweep already completed",
                    "term"_attr = term);
        return;
    }

    const bool priorAlertEmitted = priorState ? priorState->getAlertEmitted() : false;
    const auto myShardId = ShardingState::get(opCtx)->shardId();
    const auto scanStartedAt = opCtx->fastClockSource().now();

    // Snapshot the local namespaces under the global lock so the per-collection config-server reads
    // below run with it released.
    std::vector<NamespaceString> localNamespaces;
    {
        Lock::GlobalLock globalLock(opCtx, MODE_IS);
        auto catalog = CollectionCatalog::get(opCtx);
        for (const auto& dbName : catalog->getAllDbNames()) {
            if (dbName.isInternalDb()) {
                continue;
            }
            for (const auto& uuid : catalog->getAllCollectionUUIDsFromDb(dbName)) {
                if (auto nss = catalog->lookupNSSByUUID(opCtx, uuid)) {
                    localNamespaces.push_back(std::move(*nss));
                }
            }
        }
    }

    auto* catalogClient = Grid::get(opCtx)->catalogClient();
    bool foundMaxKey = false;
    for (const auto& nss : localNamespaces) {
        opCtx->checkForInterrupt();

        const auto throttle = Milliseconds{maxKeyOrphanScanThrottleMillis.load()};
        if (throttle > Milliseconds{0}) {
            opCtx->sleepFor(throttle);
        }

        try {
            const auto collType = catalogClient->getCollection(opCtx, nss);
            if (detectMaxKeyOrphanForCollection(opCtx, collType, myShardId)) {
                foundMaxKey = true;
                break;
            }
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            continue;
        } catch (const DBException& ex) {
            if (isScanFatalError(ex.code())) {
                throw;
            }
            ShardingStatistics::get(opCtx).maxKeyOrphanScanErrors.fetchAndAdd(1);
            LOGV2_WARNING(
                12799007,
                "MaxKey orphan detection: skipping collection due to per-collection error",
                "namespace"_attr = nss,
                "error"_attr = redact(ex.toStatus()));
        }
    }

    const auto scanCompletedAt = opCtx->fastClockSource().now();

    const bool emitAlert = foundMaxKey && !priorAlertEmitted;
    if (emitAlert) {
        LOGV2_WARNING(12799008,
                      "MaxKey orphan detection found at least one unowned MaxKey shard-key "
                      "document on this shard",
                      "term"_attr = term,
                      "shardId"_attr = myShardId,
                      "scanStartedAt"_attr = scanStartedAt,
                      "scanCompletedAt"_attr = scanCompletedAt);
    }

    BSONObjBuilder setBob;
    setBob.append(MaxKeyOrphanScanState::kScanStartedAtFieldName, scanStartedAt);
    setBob.append(MaxKeyOrphanScanState::kScanCompletedAtFieldName, scanCompletedAt);
    setBob.append(MaxKeyOrphanScanState::kFoundMaxKeyFieldName, foundMaxKey);
    setBob.append(MaxKeyOrphanScanState::kAlertEmittedFieldName, priorAlertEmitted || emitAlert);

    hangBeforePersistingMaxKeyOrphanScanState.pauseWhileSet(opCtx);

    PersistentTaskStore<MaxKeyOrphanScanState> store(
        NamespaceString::kConfigMaxKeyOrphanScanStateNamespace);
    store.upsert(opCtx, BSON("_id" << kMaxKeyOrphanScanStateId), BSON("$set" << setBob.obj()));

    publishOrphanScanStats(foundMaxKey, priorAlertEmitted || emitAlert);

    LOGV2_DEBUG(12799009,
                2,
                "Completed MaxKey orphan detection",
                "term"_attr = term,
                "foundMaxKey"_attr = foundMaxKey);
}

void launchMaxKeyOrphanDetectionOnStepUp(OperationContext* opCtx, long long term) {
    if (!feature_flags::gMaxKeyDetection.isEnabled()) {
        return;
    }
    auto* serviceContext = opCtx->getServiceContext();
    MaxKeyOrphanDetectionCoordinator::get(serviceContext).launch(serviceContext, term);
}

void cancelMaxKeyOrphanDetection(ServiceContext* serviceContext) {
    MaxKeyOrphanDetectionCoordinator::get(serviceContext).cancelAndJoin();
}

}  // namespace mongo
