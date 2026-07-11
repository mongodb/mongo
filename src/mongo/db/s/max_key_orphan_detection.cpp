// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
        ErrorCodes::isNotPrimaryError(code) || ErrorCodes::isA<ErrorCategory::Interruption>(code);
}

/**
 * Runs a backward shard-key index scan over [minKey, maxKey] (both bounds inclusive) and returns
 * the largest shard-key value present, reshuffled to 'shardKeyPattern' (dropping any trailing
 * fields of a wider index), or boost::none if the scan is empty. Callers resolve the acquisition,
 * the index, and the bounds, and apply their own missing-index policy.
 */
boost::optional<BSONObj> rightmostShardKeyInBounds(OperationContext* opCtx,
                                                   const CollectionAcquisition& acquisition,
                                                   const ShardKeyIndex& shardKeyIdx,
                                                   const ShardKeyPattern& shardKeyPattern,
                                                   const BSONObj& minKey,
                                                   const BSONObj& maxKey) {
    auto exec = InternalPlanner::shardKeyIndexScan(opCtx,
                                                   acquisition,
                                                   shardKeyIdx,
                                                   maxKey,
                                                   minKey,
                                                   BoundInclusion::kIncludeBothStartAndEndKeys,
                                                   PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                   InternalPlanner::BACKWARD);
    BSONObj indexKey;
    if (exec->getNext(&indexKey, nullptr) == PlanExecutor::IS_EOF) {
        return boost::none;
    }
    // The scan yields index keys with stripped field names; rename to the shard-key pattern and
    // drop any trailing fields of a wider index.
    return BSONObjBuilder()
        .appendElementsRenamed(indexKey, shardKeyPattern.toBSON(), false)
        .obj()
        .getOwned();
}

/**
 * Returns the largest shard-key value present in the collection if it is the global maximum (every
 * shard-key field is MaxKey), else boost::none.
 */
boost::optional<BSONObj> rightmostGlobalMaxShardKey(OperationContext* opCtx,
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

    // Scan the whole keyspace: the largest shard-key value present is the global maximum iff the
    // collection holds a document whose shard key is the global max. Inclusive bounds so a key
    // exactly at the global max is returned.
    const KeyPattern kp(shardKeyIdx->keyPattern());
    auto shardKey = rightmostShardKeyInBounds(opCtx,
                                              acquisition,
                                              *shardKeyIdx,
                                              shardKeyPattern,
                                              Helpers::toKeyFormat(kp.globalMin()),
                                              Helpers::toKeyFormat(kp.globalMax()));
    if (!shardKey || !isGlobalMaxShardKey(*shardKey)) {
        return boost::none;
    }
    return shardKey;
}

/**
 * Returns true iff a document whose shard key is the global maximum (every shard-key field is
 * MaxKey) exists locally within 'range'. The global-max key sorts to the very top of the keyspace,
 * so a single backward scan of the range that returns it answers the question. Throws IndexNotFound
 * when no shard-key-prefixed index is available to run the scan.
 */
bool hasGlobalMaxDocInRange(OperationContext* opCtx,
                            const DatabaseName& dbName,
                            const UUID& collUuid,
                            const ShardKeyPattern& shardKeyPattern,
                            const ChunkRange& range) {
    const auto acquisition = acquireCollection(
        opCtx,
        CollectionAcquisitionRequest::fromOpCtx(
            opCtx, NamespaceStringOrUUID{dbName, collUuid}, AcquisitionPrerequisites::kRead),
        MODE_IS);
    if (!acquisition.exists()) {
        return false;
    }

    const auto shardKeyIdx = findShardKeyPrefixedIndex(opCtx,
                                                       acquisition.getCollectionPtr(),
                                                       shardKeyPattern.toBSON(),
                                                       /*requireSingleKey=*/false);
    uassert(ErrorCodes::IndexNotFound,
            str::stream() << "Unable to find shard key index for "
                          << acquisition.nss().toStringForErrorMsg() << " and key pattern `"
                          << shardKeyPattern.toBSON() << "'",
            shardKeyIdx);

    // Extend the task range to the (possibly wider) index key pattern. The caller has already
    // verified the upper bound is the global max, so extend it inclusively and use inclusive bounds
    // so a shard key that is exactly MaxKey is reachable (mirrors deleteRangeInBatches' isMaxGlobal
    // handling).
    const KeyPattern indexKeyPattern(shardKeyIdx->keyPattern());
    auto shardKey = rightmostShardKeyInBounds(
        opCtx,
        acquisition,
        *shardKeyIdx,
        shardKeyPattern,
        Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(range.getMin(), false)),
        Helpers::toKeyFormat(indexKeyPattern.extendRangeBound(range.getMax(), true)));
    return shardKey && isGlobalMaxShardKey(*shardKey);
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
 * Outcome of classifying a single sharded collection for a MaxKey-prefixed document.
 */
enum class MaxKeyFinding {
    // No MaxKey-prefixed document, or a transient orphan that the cleanup path will remove.
    kNone,
    // A MaxKey-prefixed document that this shard legitimately owns. Its version could be stale if
    // it was re-owned after some series of application operations.
    kOwned,
    // A MaxKey-prefixed document that this shard does not legitimately own (a true orphan).
    kUnowned,
};

/**
 * Classifies a single sharded collection for a document whose leading shard-key field is MaxKey.
 * See MaxKeyFinding for the meaning of each outcome.
 */
MaxKeyFinding detectMaxKeyForCollection(OperationContext* opCtx,
                                        const CollectionType& collType,
                                        const ShardId& myShardId) {
    // getCollection() also returns tracked-but-unsharded collections, skip them.
    if (collType.getUnsplittable()) {
        return MaxKeyFinding::kNone;
    }

    const auto& nss = collType.getNss();
    const auto collUuid = collType.getUuid();

    const ShardKeyPattern shardKeyPattern(collType.getKeyPattern());
    if (shardKeyPattern.isHashedPattern()) {
        return MaxKeyFinding::kNone;
    }

    // Cheap pre-check before taking the migration-blocking guard.
    if (!rightmostGlobalMaxShardKey(opCtx, nss.dbName(), collUuid, shardKeyPattern)) {
        return MaxKeyFinding::kNone;
    }

    hangDuringMaxKeyOrphanScan.pauseWhileSet(opCtx);

    MigrationBlockingGuard guard(
        opCtx, str::stream() << "MaxKey orphan detection for collection " << collUuid);

    // Re-check authoritatively now that migrations are blocked; the doc may have been migrated or
    // deleted between the pre-check and acquiring the guard.
    auto candidateKey = rightmostGlobalMaxShardKey(opCtx, nss.dbName(), collUuid, shardKeyPattern);
    if (!candidateKey) {
        return MaxKeyFinding::kNone;
    }

    // An outstanding range-deletion task that covers the candidate will delete it through the
    // normal cleanup path, so it is a transient orphan, not a finding.
    if (rangeDeletionCoversKey(opCtx, collUuid, *candidateKey)) {
        return MaxKeyFinding::kNone;
    }

    auto cm = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionPlacementInfoWithRefresh(opCtx, nss));
    if (!cm.hasRoutingTable() || !cm.uuidMatches(collUuid)) {
        return MaxKeyFinding::kNone;
    }
    return cm.keyBelongsToShard(*candidateKey, myShardId) ? MaxKeyFinding::kOwned
                                                          : MaxKeyFinding::kUnowned;
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

bool isGlobalMaxShardKey(const BSONObj& shardKeyValue) {
    if (shardKeyValue.isEmpty()) {
        return false;
    }
    for (auto&& elem : shardKeyValue) {
        if (elem.type() != BSONType::maxKey) {
            return false;
        }
    }
    return true;
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

    auto publishOrphanScanStats = [&](bool foundUnownedMaxKey,
                                      bool unownedAlertEmitted,
                                      bool foundOwnedMaxKey,
                                      bool ownedAlertEmitted) {
        auto& stats = ShardingStatistics::get(opCtx);
        stats.maxKeyOrphanScanComplete.store(1);
        stats.maxKeyOrphanScanFoundUnownedMaxKey.store(foundUnownedMaxKey ? 1 : 0);
        stats.maxKeyOrphanScanUnownedAlertEmitted.store(unownedAlertEmitted ? 1 : 0);
        stats.maxKeyOrphanScanFoundOwnedMaxKey.store(foundOwnedMaxKey ? 1 : 0);
        stats.maxKeyOrphanScanOwnedAlertEmitted.store(ownedAlertEmitted ? 1 : 0);
    };

    // Read the prior state doc. A completed sweep short-circuits this one-shot sweep; the prior
    // unownedAlertEmitted is preserved so a re-scan never downgrades it.
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
        publishOrphanScanStats(priorState->getFoundUnownedMaxKey().value_or(false),
                               priorState->getUnownedAlertEmitted().value_or(false),
                               priorState->getFoundOwnedMaxKey().value_or(false),
                               priorState->getOwnedAlertEmitted().value_or(false));
        LOGV2_DEBUG(12799006,
                    2,
                    "Skipping MaxKey orphan detection: prior sweep already completed",
                    "term"_attr = term);
        return;
    }

    const bool priorUnownedAlertEmitted =
        priorState ? priorState->getUnownedAlertEmitted().value_or(false) : false;
    const bool priorOwnedAlertEmitted =
        priorState ? priorState->getOwnedAlertEmitted().value_or(false) : false;
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
    bool foundUnownedMaxKey = false;
    bool foundOwnedMaxKey = false;
    for (const auto& nss : localNamespaces) {
        opCtx->checkForInterrupt();

        const auto throttle = Milliseconds{maxKeyOrphanScanThrottleMillis.load()};
        if (throttle > Milliseconds{0}) {
            opCtx->sleepFor(throttle);
        }

        try {
            const auto collType = catalogClient->getCollection(opCtx, nss);
            switch (detectMaxKeyForCollection(opCtx, collType, myShardId)) {
                case MaxKeyFinding::kUnowned:
                    foundUnownedMaxKey = true;
                    break;
                case MaxKeyFinding::kOwned:
                    foundOwnedMaxKey = true;
                    break;
                case MaxKeyFinding::kNone:
                    break;
            }
            // Both signals are one-shot, so stop early once each has been observed at least once.
            if (foundUnownedMaxKey && foundOwnedMaxKey) {
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

    const bool emitUnownedAlert = foundUnownedMaxKey && !priorUnownedAlertEmitted;
    if (emitUnownedAlert) {
        LOGV2_WARNING(12799008,
                      "MaxKey orphan detection found at least one unowned MaxKey shard-key "
                      "document on this shard",
                      "term"_attr = term,
                      "shardId"_attr = myShardId,
                      "scanStartedAt"_attr = scanStartedAt,
                      "scanCompletedAt"_attr = scanCompletedAt);
    }

    const bool emitOwnedAlert = foundOwnedMaxKey && !priorOwnedAlertEmitted;
    if (emitOwnedAlert) {
        LOGV2_WARNING(13048900,
                      "MaxKey orphan detection found at least one accessible (owned) MaxKey "
                      "shard-key document on this shard",
                      "term"_attr = term,
                      "shardId"_attr = myShardId,
                      "scanStartedAt"_attr = scanStartedAt,
                      "scanCompletedAt"_attr = scanCompletedAt);
    }

    BSONObjBuilder setBob;
    setBob.append(MaxKeyOrphanScanState::kScanStartedAtFieldName, scanStartedAt);
    setBob.append(MaxKeyOrphanScanState::kScanCompletedAtFieldName, scanCompletedAt);
    setBob.append(MaxKeyOrphanScanState::kFoundUnownedMaxKeyFieldName, foundUnownedMaxKey);
    setBob.append(MaxKeyOrphanScanState::kUnownedAlertEmittedFieldName,
                  priorUnownedAlertEmitted || emitUnownedAlert);
    setBob.append(MaxKeyOrphanScanState::kFoundOwnedMaxKeyFieldName, foundOwnedMaxKey);
    setBob.append(MaxKeyOrphanScanState::kOwnedAlertEmittedFieldName,
                  priorOwnedAlertEmitted || emitOwnedAlert);

    hangBeforePersistingMaxKeyOrphanScanState.pauseWhileSet(opCtx);

    PersistentTaskStore<MaxKeyOrphanScanState> store(
        NamespaceString::kConfigMaxKeyOrphanScanStateNamespace);
    store.upsert(opCtx, BSON("_id" << kMaxKeyOrphanScanStateId), BSON("$set" << setBob.obj()));

    publishOrphanScanStats(foundUnownedMaxKey,
                           priorUnownedAlertEmitted || emitUnownedAlert,
                           foundOwnedMaxKey,
                           priorOwnedAlertEmitted || emitOwnedAlert);

    LOGV2_DEBUG(12799009,
                2,
                "Completed MaxKey orphan detection",
                "term"_attr = term,
                "foundUnownedMaxKey"_attr = foundUnownedMaxKey,
                "foundOwnedMaxKey"_attr = foundOwnedMaxKey);
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

bool shouldSkipRangeDeletionForMaxKeyOrphans(OperationContext* opCtx,
                                             const DatabaseName& dbName,
                                             const UUID& collUuid,
                                             const BSONObj& shardKeyPattern,
                                             const ChunkRange& range) {
    const ShardKeyPattern skPattern(shardKeyPattern);
    // Hashed shard keys never produce MaxKey-prefixed values.
    if (skPattern.isHashedPattern()) {
        return false;
    }
    // Cheap short-circuit: only a global-max upper bound can cover MaxKey-prefixed documents.
    if (!skPattern.getKeyPattern().isGlobalMax(range.getMax())) {
        return false;
    }
    return hasGlobalMaxDocInRange(opCtx, dbName, collUuid, skPattern, range);
}

namespace {

/**
 * Reads the singleton config.maxKeyOrphanScanState document, or boost::none if it does not exist. A
 * read failure is propagated so the caller retries.
 */
boost::optional<MaxKeyOrphanScanState> readScanStateDoc(OperationContext* opCtx) {
    DBDirectClient client(opCtx);
    FindCommandRequest findCmd(NamespaceString::kConfigMaxKeyOrphanScanStateNamespace);
    findCmd.setFilter(BSON("_id" << kMaxKeyOrphanScanStateId));
    findCmd.setLimit(1);
    auto cursor = client.find(std::move(findCmd));
    if (cursor && cursor->more()) {
        return MaxKeyOrphanScanState::parse(cursor->next(), IDLParserContext("readScanStateDoc"));
    }
    return boost::none;
}

/**
 * Persists the classified blocked-task id set to config.maxKeyOrphanScanState. 'blockedTasks' is
 * written unconditionally (even when empty) so its presence marks the classification complete for
 * this epoch.
 */
void persistBlockedTasks(OperationContext* opCtx, const std::vector<UUID>& blocked) {
    BSONObjBuilder update;
    {
        BSONObjBuilder setBob(update.subobjStart("$set"));
        BSONArrayBuilder arr(setBob.subarrayStart(MaxKeyOrphanScanState::kBlockedTasksFieldName));
        for (const auto& id : blocked) {
            id.appendToArrayBuilder(&arr);
        }
    }

    PersistentTaskStore<MaxKeyOrphanScanState> store(
        NamespaceString::kConfigMaxKeyOrphanScanStateNamespace);
    store.upsert(opCtx, BSON("_id" << kMaxKeyOrphanScanStateId), update.obj());
}

bool rangeMaxIsAllMaxKey(const ChunkRange& range) {
    const auto& max = range.getMax();
    if (max.isEmpty()) {
        return false;
    }
    for (auto&& elem : max) {
        if (elem.type() != BSONType::maxKey) {
            return false;
        }
    }
    return true;
}

}  // namespace

std::vector<UUID> loadOrComputeBlockedMaxKeyRangeDeletionTasks(OperationContext* opCtx) {
    auto existing = readScanStateDoc(opCtx);

    // A present 'blockedTasks' field (even empty) means classification already ran this epoch.
    if (existing && existing->getBlockedTasks().has_value()) {
        const auto& blocked = *existing->getBlockedTasks();
        LOGV2_DEBUG(13018002,
                    2,
                    "MaxKey orphan guard: rehydrated blocked range-deletion tasks from state doc",
                    "blockedTaskCount"_attr = blocked.size());
        return blocked;
    }

    std::vector<UUID> blocked;
    PersistentTaskStore<RangeDeletionTask> store(NamespaceString::kRangeDeletionNamespace);
    store.forEach(opCtx, BSONObj{}, [&](const RangeDeletionTask& task) {
        opCtx->checkForInterrupt();

        // Only a global-max upper bound can cover MaxKey-prefixed docs, so a task whose range max
        // is not all-MaxKey is deletable and needs neither its key pattern resolved nor an index
        // probe.
        if (!rangeMaxIsAllMaxKey(task.getRange())) {
            return true;
        }

        const auto& dbName = task.getNss().dbName();
        const auto& collUuid = task.getCollectionUuid();
        try {
            const auto shardKeyPattern = task.getKeyPattern()
                ? task.getKeyPattern()->toBSON()
                : Grid::get(opCtx)
                      ->catalogClient()
                      ->getCollection(opCtx, task.getNss())
                      .getKeyPattern()
                      .toBSON();
            if (shouldSkipRangeDeletionForMaxKeyOrphans(
                    opCtx, dbName, collUuid, shardKeyPattern, task.getRange())) {
                blocked.push_back(task.getId());
            }
        } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
            // Collection dropped since the task was written: nothing to preserve, leave it
            // deletable.
        } catch (const DBException& ex) {
            if (isScanFatalError(ex.code())) {
                throw;
            }
            LOGV2_WARNING(13018003,
                          "MaxKey orphan guard: conservatively blocking unclassifiable task",
                          "taskId"_attr = task.getId(),
                          "collectionUUID"_attr = collUuid,
                          "error"_attr = redact(ex.toStatus()));
            blocked.push_back(task.getId());
        }
        return true;
    });

    persistBlockedTasks(opCtx, blocked);

    LOGV2_INFO(13018004,
               "MaxKey orphan guard: completed range-deletion task classification",
               "blockedTaskCount"_attr = blocked.size());
    return blocked;
}

}  // namespace mongo
