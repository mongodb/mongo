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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/index_builds_coordinator.h"

#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_build_entry_gen.h"
#include "mongo/db/catalog/index_timestamp_helper.h"
#include "mongo/db/catalog/uncommitted_collections.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_build_entry_helpers.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/timestamp_block.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_recovery.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/logv2/log.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

using namespace indexbuildentryhelpers;

MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildFirstDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildSecondDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildDumpsInsertsFromBulk);

namespace {

constexpr StringData kCreateIndexesFieldName = "createIndexes"_sd;
constexpr StringData kCommitIndexBuildFieldName = "commitIndexBuild"_sd;
constexpr StringData kAbortIndexBuildFieldName = "abortIndexBuild"_sd;
constexpr StringData kIndexesFieldName = "indexes"_sd;
constexpr StringData kKeyFieldName = "key"_sd;
constexpr StringData kUniqueFieldName = "unique"_sd;

/**
 * Checks if unique index specification is compatible with sharding configuration.
 */
void checkShardKeyRestrictions(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& newIdxKey) {
    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx, nss);

    const auto collDesc = CollectionShardingState::get(opCtx, nss)->getCollectionDescription();
    if (!collDesc.isSharded())
        return;

    const ShardKeyPattern shardKeyPattern(collDesc.getKeyPattern());
    uassert(ErrorCodes::CannotCreateIndex,
            str::stream() << "cannot create unique index over " << newIdxKey
                          << " with shard key pattern " << shardKeyPattern.toBSON(),
            shardKeyPattern.isUniqueIndexCompatible(newIdxKey));
}

/**
 * Returns true if we should build the indexes an empty collection using the IndexCatalog and
 * bypass the index build registration.
 */
bool shouldBuildIndexesOnEmptyCollectionSinglePhased(OperationContext* opCtx,
                                                     Collection* collection,
                                                     IndexBuildProtocol protocol) {
    const auto& nss = collection->ns();
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X), str::stream() << nss);

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    // Check whether the replica set member's config has {buildIndexes:false} set, which means
    // we are not allowed to build non-_id indexes on this server.
    if (!replCoord->buildsIndexes()) {
        return false;
    }

    // Secondaries should not bypass index build registration (and _runIndexBuild()) for two phase
    // index builds because they need to report index build progress to the primary per commit
    // quorum.
    if (IndexBuildProtocol::kTwoPhase == protocol && replCoord->getSettings().usingReplSets() &&
        !replCoord->canAcceptWritesFor(opCtx, nss)) {
        return false;
    }

    // We use the fast count information, through Collection::numRecords(), to determine if the
    // collection is empty. However, this information is either unavailable or inaccurate when the
    // node is in certain replication states, such as recovery or rollback. In these cases, we
    // have to build the index by scanning the collection.
    auto memberState = replCoord->getMemberState();
    if (memberState.rollback()) {
        return false;
    }
    if (inReplicationRecovery(opCtx->getServiceContext())) {
        return false;
    }

    // Now, it's fine to trust Collection::isEmpty().
    // Fast counts are prone to both false positives and false negatives on unclean shutdowns. False
    // negatives can cause to skip index building. And, false positives can cause mismatch in number
    // of index entries among the nodes in the replica set. So, verify the collection is really
    // empty by opening the WT cursor and reading the first document.
    return collection->isEmpty(opCtx);
}

/*
 * Determines whether to skip the index build state transition check.
 * Index builder not using ReplIndexBuildState::waitForNextAction to signal primary and secondaries
 * to commit or abort signal will violate index build state transition i.e, they can move from
 * prepareAbort to Committed and from Committed to prepareAbort. So, we should skip state transition
 * verification. Otherwise, we would invariant.
 */
bool shouldSkipIndexBuildStateTransitionCheck(OperationContext* opCtx,
                                              IndexBuildProtocol protocol) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().usingReplSets() && protocol == IndexBuildProtocol::kTwoPhase) {
        return false;
    }
    return true;
}

/**
 * Signal downstream secondary nodes to commit index build.
 */
void onCommitIndexBuild(OperationContext* opCtx,
                        const NamespaceString& nss,
                        ReplIndexBuildState& replState,
                        bool replSetAndNotPrimaryAtStart) {
    const auto& buildUUID = replState.buildUUID;

    invariant(IndexBuildProtocol::kTwoPhase == replState.protocol,
              str::stream() << "onCommitIndexBuild: " << buildUUID);
    invariant(opCtx->lockState()->isWriteLocked(),
              str::stream() << "onCommitIndexBuild: " << buildUUID);

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    const auto& collUUID = replState.collectionUUID;
    const auto& indexSpecs = replState.indexSpecs;
    auto fromMigrate = false;
    auto skipCheck = shouldSkipIndexBuildStateTransitionCheck(opCtx, replState.protocol);
    {
        stdx::unique_lock<Latch> lk(replState.mutex);
        replState.indexBuildState.setState(IndexBuildState::kCommitted, skipCheck);
    }

    // Since two phase index builds are allowed to survive replication state transitions, we should
    // check if the node is currently a primary before attempting to write to the oplog.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->getSettings().usingReplSets()) {
        return;
    }

    if (!replCoord->canAcceptWritesFor(opCtx, nss)) {
        invariant(!opCtx->recoveryUnit()->getCommitTimestamp().isNull(),
                  str::stream() << "commitIndexBuild: " << buildUUID);
        return;
    }

    opObserver->onCommitIndexBuild(opCtx, nss, collUUID, buildUUID, indexSpecs, fromMigrate);
}

/**
 * Signal downstream secondary nodes to abort index build.
 */
void onAbortIndexBuild(OperationContext* opCtx,
                       const NamespaceString& nss,
                       ReplIndexBuildState& replState,
                       const Status& cause) {
    if (!serverGlobalParams.featureCompatibility.isVersionInitialized()) {
        return;
    }

    if (serverGlobalParams.featureCompatibility.getVersion() !=
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) {
        return;
    }

    invariant(opCtx->lockState()->isWriteLocked(), replState.buildUUID.toString());

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    auto collUUID = replState.collectionUUID;
    auto fromMigrate = false;
    auto skipCheck = shouldSkipIndexBuildStateTransitionCheck(opCtx, replState.protocol);
    {
        stdx::unique_lock<Latch> lk(replState.mutex);
        replState.indexBuildState.setState(IndexBuildState::kAborted, skipCheck);
    }
    opObserver->onAbortIndexBuild(
        opCtx, nss, collUUID, replState.buildUUID, replState.indexSpecs, cause, fromMigrate);
}

/**
 * Aborts the index build identified by the provided 'replIndexBuildState'.
 * It gets called by drop database/collection/index command.
 */
void abortIndexBuild(WithLock lk,
                     OperationContext* opCtx,
                     IndexBuildsManager* indexBuildsManager,
                     std::shared_ptr<ReplIndexBuildState> replIndexBuildState,
                     const std::string& reason) {
    stdx::unique_lock<Latch> replStateLock(replIndexBuildState->mutex);
    if (replIndexBuildState->waitForNextAction->getFuture().isReady()) {
        const auto nextAction = replIndexBuildState->waitForNextAction->getFuture().get();
        invariant(nextAction == IndexBuildAction::kSinglePhaseCommit ||
                  nextAction == IndexBuildAction::kCommitQuorumSatisfied ||
                  nextAction == IndexBuildAction::kPrimaryAbort);
        // Index build coordinator already received a signal to commit or abort. So, it's ok
        // to return and wait for the index build to complete. The index build coordinator
        // will not perform the signaled action (i.e, will not commit or abort the index build)
        // only when the node steps down. When the node steps down, the caller of this function,
        // drop commands (user operation) will also get interrupted. So, we no longer need to
        // abort the index build on step down.
        return;
    }

    auto skipCheck = shouldSkipIndexBuildStateTransitionCheck(opCtx, replIndexBuildState->protocol);
    // Set the state on replIndexBuildState and indexBuildsManager. And, then signal the value. It's
    // important we do all these 3 things in a critical section by holding mutex lock.
    replIndexBuildState->indexBuildState.setState(
        IndexBuildState::kPrepareAbort, skipCheck, boost::none, reason);
    indexBuildsManager->abortIndexBuild(replIndexBuildState->buildUUID, reason);

    replIndexBuildState->waitForNextAction->emplaceValue(IndexBuildAction::kPrimaryAbort);
}

/**
 * We do not need synchronization with step up and step down. Dropping the RSTL is important because
 * otherwise if we held the RSTL it would create deadlocks with prepared transactions on step up and
 * step down.  A deadlock could result if the index build was attempting to acquire a Collection S
 * or X lock while a prepared transaction held a Collection IX lock, and a step down was waiting to
 * acquire the RSTL in mode X.
 */
void unlockRSTLForIndexCleanup(OperationContext* opCtx) {
    opCtx->lockState()->unlockRSTLforPrepare();
    invariant(!opCtx->lockState()->isRSTLLocked());
}

/**
 * Logs the index build failure error in a standard format.
 */
void logFailure(Status status,
                const NamespaceString& nss,
                std::shared_ptr<ReplIndexBuildState> replState) {
    LOGV2(
        20649,
        "Index build failed: {replState_buildUUID}: {nss} ( {replState_collectionUUID} ): {status}",
        "replState_buildUUID"_attr = replState->buildUUID,
        "nss"_attr = nss,
        "replState_collectionUUID"_attr = replState->collectionUUID,
        "status"_attr = status);
}

/**
 * Iterates over index builds with the provided function.
 */
void forEachIndexBuild(
    const std::vector<std::shared_ptr<ReplIndexBuildState>>& indexBuilds,
    StringData logPrefix,
    std::function<void(std::shared_ptr<ReplIndexBuildState> replState)> onIndexBuild) {
    if (indexBuilds.empty()) {
        return;
    }

    LOGV2(20650,
          "{logPrefix}active index builds: {indexBuilds_size}",
          "logPrefix"_attr = logPrefix,
          "indexBuilds_size"_attr = indexBuilds.size());

    for (auto replState : indexBuilds) {
        std::string indexNamesStr;
        str::joinStringDelim(replState->indexNames, &indexNamesStr, ',');
        LOGV2(20651,
              "{logPrefix}{replState_buildUUID}: collection: {replState_collectionUUID}; indexes: "
              "{replState_indexNames_size} [{indexNamesStr}]; method: "
              "{IndexBuildProtocol_kTwoPhase_replState_protocol_two_phase_single_phase}",
              "logPrefix"_attr = logPrefix,
              "replState_buildUUID"_attr = replState->buildUUID,
              "replState_collectionUUID"_attr = replState->collectionUUID,
              "replState_indexNames_size"_attr = replState->indexNames.size(),
              "indexNamesStr"_attr = indexNamesStr,
              "IndexBuildProtocol_kTwoPhase_replState_protocol_two_phase_single_phase"_attr =
                  (IndexBuildProtocol::kTwoPhase == replState->protocol ? "two phase"
                                                                        : "single phase"));

        onIndexBuild(replState);
    }
}

/**
 * Updates currentOp for commitIndexBuild or abortIndexBuild.
 */
void updateCurOpForCommitOrAbort(OperationContext* opCtx, StringData fieldName, UUID buildUUID) {
    BSONObjBuilder builder;
    buildUUID.appendToBuilder(&builder, fieldName);
    stdx::unique_lock<Client> lk(*opCtx->getClient());
    auto curOp = CurOp::get(opCtx);
    builder.appendElementsUnique(curOp->opDescription());
    auto opDescObj = builder.obj();
    curOp->setLogicalOp_inlock(LogicalOp::opCommand);
    curOp->setOpDescription_inlock(opDescObj);
    curOp->ensureStarted();
}

}  // namespace

const auto getIndexBuildsCoord =
    ServiceContext::declareDecoration<std::unique_ptr<IndexBuildsCoordinator>>();

void IndexBuildsCoordinator::set(ServiceContext* serviceContext,
                                 std::unique_ptr<IndexBuildsCoordinator> ibc) {
    auto& indexBuildsCoordinator = getIndexBuildsCoord(serviceContext);
    invariant(!indexBuildsCoordinator);

    indexBuildsCoordinator = std::move(ibc);
}

IndexBuildsCoordinator* IndexBuildsCoordinator::get(ServiceContext* serviceContext) {
    auto& indexBuildsCoordinator = getIndexBuildsCoord(serviceContext);
    invariant(indexBuildsCoordinator);

    return indexBuildsCoordinator.get();
}

IndexBuildsCoordinator* IndexBuildsCoordinator::get(OperationContext* OperationContext) {
    return get(OperationContext->getServiceContext());
}

IndexBuildsCoordinator::~IndexBuildsCoordinator() {
    invariant(_databaseIndexBuilds.empty());
    invariant(_collectionIndexBuilds.empty());
}

bool IndexBuildsCoordinator::supportsTwoPhaseIndexBuild() {
    auto storageEngine = getGlobalServiceContext()->getStorageEngine();
    return storageEngine->supportsTwoPhaseIndexBuild();
}

StatusWith<std::pair<long long, long long>> IndexBuildsCoordinator::rebuildIndexesForRecovery(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<BSONObj>& specs,
    const UUID& buildUUID,
    RepairData repair) {

    const auto protocol = IndexBuildProtocol::kSinglePhase;
    auto status = _startIndexBuildForRecovery(opCtx, nss, specs, buildUUID, protocol);
    if (!status.isOK()) {
        return status;
    }

    auto& collectionCatalog = CollectionCatalog::get(getGlobalServiceContext());
    Collection* collection = collectionCatalog.lookupCollectionByNamespace(opCtx, nss);

    // Complete the index build.
    return _runIndexRebuildForRecovery(opCtx, collection, buildUUID, repair);
}

Status IndexBuildsCoordinator::_startIndexBuildForRecovery(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           const std::vector<BSONObj>& specs,
                                                           const UUID& buildUUID,
                                                           IndexBuildProtocol protocol) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));

    std::vector<std::string> indexNames;
    for (auto& spec : specs) {
        std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
        if (name.empty()) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream()
                              << "Cannot create an index for a spec '" << spec
                              << "' without a non-empty string value for the 'name' field");
        }
        indexNames.push_back(name);
    }

    auto& collectionCatalog = CollectionCatalog::get(getGlobalServiceContext());
    Collection* collection = collectionCatalog.lookupCollectionByNamespace(opCtx, nss);
    auto indexCatalog = collection->getIndexCatalog();
    {
        // These steps are combined into a single WUOW to ensure there are no commits without
        // the indexes.
        // 1) Drop all unfinished indexes.
        // 2) Start, but do not complete the index build process.
        WriteUnitOfWork wuow(opCtx);

        for (size_t i = 0; i < indexNames.size(); i++) {
            bool includeUnfinished = false;
            auto descriptor =
                indexCatalog->findIndexByName(opCtx, indexNames[i], includeUnfinished);
            if (descriptor) {
                Status s = indexCatalog->dropIndex(opCtx, descriptor);
                if (!s.isOK()) {
                    return s;
                }
                continue;
            }

            // If the index is not present in the catalog, then we are trying to drop an already
            // aborted index. This may happen when rollback-via-refetch restarts an index build
            // after an abort has been rolled back.
            if (!DurableCatalog::get(opCtx)->isIndexPresent(
                    opCtx, collection->getCatalogId(), indexNames[i])) {
                LOGV2(20652,
                      "The index for build {buildUUID} was not found while trying to drop the "
                      "index during recovery: {indexNames_i}",
                      "buildUUID"_attr = buildUUID,
                      "indexNames_i"_attr = indexNames[i]);
                continue;
            }

            const auto durableBuildUUID = DurableCatalog::get(opCtx)->getIndexBuildUUID(
                opCtx, collection->getCatalogId(), indexNames[i]);

            // A build UUID is present if and only if we are rebuilding a two-phase build.
            invariant((protocol == IndexBuildProtocol::kTwoPhase) ==
                      durableBuildUUID.is_initialized());
            // When a buildUUID is present, it must match the build UUID parameter to this
            // function.
            invariant(!durableBuildUUID || *durableBuildUUID == buildUUID,
                      str::stream() << "durable build UUID: " << durableBuildUUID
                                    << "buildUUID: " << buildUUID);

            // If the unfinished index is in the IndexCatalog, drop it through there, otherwise drop
            // it from the DurableCatalog. Rollback-via-refetch does not clear any in-memory state,
            // so we should do it manually here.
            includeUnfinished = true;
            descriptor = indexCatalog->findIndexByName(opCtx, indexNames[i], includeUnfinished);
            if (descriptor) {
                Status s = indexCatalog->dropUnfinishedIndex(opCtx, descriptor);
                if (!s.isOK()) {
                    return s;
                }
            } else {
                Status status = DurableCatalog::get(opCtx)->removeIndex(
                    opCtx, collection->getCatalogId(), indexNames[i]);
                if (!status.isOK()) {
                    return status;
                }
            }
        }

        // We need to initialize the collection to rebuild the indexes. The collection may already
        // be initialized when rebuilding indexes with rollback-via-refetch.
        if (!collection->isInitialized()) {
            collection->init(opCtx);
        }

        auto dbName = nss.db().toString();
        auto replIndexBuildState =
            std::make_shared<ReplIndexBuildState>(buildUUID,
                                                  collection->uuid(),
                                                  dbName,
                                                  specs,
                                                  protocol,
                                                  /*commitQuorum=*/boost::none);

        Status status = [&]() {
            stdx::unique_lock<Latch> lk(_mutex);
            return _registerIndexBuild(lk, replIndexBuildState);
        }();
        if (!status.isOK()) {
            return status;
        }

        IndexBuildsManager::SetupOptions options;
        status = _indexBuildsManager.setUpIndexBuild(
            opCtx, collection, specs, buildUUID, MultiIndexBlock::kNoopOnInitFn, options);
        if (!status.isOK()) {
            // An index build failure during recovery is fatal.
            logFailure(status, nss, replIndexBuildState);
            fassertNoTrace(51086, status);
        }

        wuow.commit();
    }

    return Status::OK();
}

std::string IndexBuildsCoordinator::_indexBuildActionToString(IndexBuildAction action) {
    if (action == IndexBuildAction::kNoAction) {
        return "No action";
    } else if (action == IndexBuildAction::kOplogCommit) {
        return "Oplog commit";
    } else if (action == IndexBuildAction::kOplogAbort) {
        return "Oplog abort";
    } else if (action == IndexBuildAction::kRollbackAbort) {
        return "Rollback abort";
    } else if (action == IndexBuildAction::kPrimaryAbort) {
        return "Primary abort";
    } else if (action == IndexBuildAction::kSinglePhaseCommit) {
        return "Single-phase commit";
    } else if (action == IndexBuildAction::kCommitQuorumSatisfied) {
        return "Commit quorum Satisfied";
    }
    MONGO_UNREACHABLE;
}

void IndexBuildsCoordinator::waitForAllIndexBuildsToStopForShutdown() {
    stdx::unique_lock<Latch> lk(_mutex);

    // All index builds should have been signaled to stop via the ServiceContext.

    // Wait for all the index builds to stop.
    for (auto& dbIt : _databaseIndexBuilds) {
        // Take a shared ptr, rather than accessing the Tracker through the map's iterator, so that
        // the object does not destruct while we are waiting, causing a use-after-free memory error.
        auto dbIndexBuildsSharedPtr = dbIt.second;
        dbIndexBuildsSharedPtr->waitUntilNoIndexBuildsRemain(lk);
    }
}

std::vector<UUID> IndexBuildsCoordinator::_abortCollectionIndexBuilds(stdx::unique_lock<Latch>& lk,
                                                                      OperationContext* opCtx,
                                                                      const UUID& collectionUUID,
                                                                      const std::string& reason,
                                                                      bool shouldWait) {
    auto collIndexBuildsIt = _collectionIndexBuilds.find(collectionUUID);
    if (collIndexBuildsIt == _collectionIndexBuilds.end()) {
        return {};
    }

    LOGV2(23879,
          "About to abort all index builders on collection with UUID: {collectionUUID}",
          "collectionUUID"_attr = collectionUUID);

    std::vector<UUID> buildUUIDs = collIndexBuildsIt->second->getIndexBuildUUIDs(lk);
    collIndexBuildsIt->second->runOperationOnAllBuilds(
        lk, opCtx, &_indexBuildsManager, abortIndexBuild, reason);

    if (!shouldWait) {
        return buildUUIDs;
    }

    // Take a shared ptr, rather than accessing the Tracker through the map's iterator, so that the
    // object does not destruct while we are waiting, causing a use-after-free memory error.
    auto collIndexBuildsSharedPtr = collIndexBuildsIt->second;
    collIndexBuildsSharedPtr->waitUntilNoIndexBuildsRemain(lk);
    return buildUUIDs;
}

void IndexBuildsCoordinator::abortCollectionIndexBuilds(OperationContext* opCtx,
                                                        const UUID& collectionUUID,
                                                        const std::string& reason) {
    stdx::unique_lock<Latch> lk(_mutex);
    const bool shouldWait = true;
    _abortCollectionIndexBuilds(lk, opCtx, collectionUUID, reason, shouldWait);
}

std::vector<UUID> IndexBuildsCoordinator::abortCollectionIndexBuildsNoWait(
    OperationContext* opCtx, const UUID& collectionUUID, const std::string& reason) {
    stdx::unique_lock<Latch> lk(_mutex);
    const bool shouldWait = false;
    return _abortCollectionIndexBuilds(lk, opCtx, collectionUUID, reason, shouldWait);
}

void IndexBuildsCoordinator::_abortDatabaseIndexBuilds(stdx::unique_lock<Latch>& lk,
                                                       OperationContext* opCtx,
                                                       const StringData& db,
                                                       const std::string& reason,
                                                       bool shouldWait) {
    auto dbIndexBuildsIt = _databaseIndexBuilds.find(db);
    if (dbIndexBuildsIt == _databaseIndexBuilds.end()) {
        return;
    }

    LOGV2(4612302,
          "About to abort all index builders running for collections in the given database",
          "database"_attr = db);

    dbIndexBuildsIt->second->runOperationOnAllBuilds(
        lk, opCtx, &_indexBuildsManager, abortIndexBuild, reason);

    if (!shouldWait) {
        return;
    }

    // Take a shared ptr, rather than accessing the Tracker through the map's iterator, so that the
    // object does not destruct while we are waiting, causing a use-after-free memory error.
    auto dbIndexBuildsSharedPtr = dbIndexBuildsIt->second;
    dbIndexBuildsSharedPtr->waitUntilNoIndexBuildsRemain(lk);
}

void IndexBuildsCoordinator::abortDatabaseIndexBuilds(OperationContext* opCtx,
                                                      StringData db,
                                                      const std::string& reason) {
    stdx::unique_lock<Latch> lk(_mutex);
    const bool shouldWait = true;
    _abortDatabaseIndexBuilds(lk, opCtx, db, reason, shouldWait);
}

void IndexBuildsCoordinator::abortDatabaseIndexBuildsNoWait(OperationContext* opCtx,
                                                            StringData db,
                                                            const std::string& reason) {
    stdx::unique_lock<Latch> lk(_mutex);
    const bool shouldWait = false;
    _abortDatabaseIndexBuilds(lk, opCtx, db, reason, shouldWait);
}

namespace {
NamespaceString getNsFromUUID(OperationContext* opCtx, const UUID& uuid) {
    auto& catalog = CollectionCatalog::get(opCtx);
    auto nss = catalog.lookupNSSByUUID(opCtx, uuid);
    uassert(ErrorCodes::NamespaceNotFound, "No namespace with UUID " + uuid.toString(), nss);
    return *nss;
}
}  // namespace

void IndexBuildsCoordinator::applyStartIndexBuild(OperationContext* opCtx,
                                                  const IndexBuildOplogEntry& oplogEntry) {
    const auto collUUID = oplogEntry.collUUID;
    const auto nss = getNsFromUUID(opCtx, collUUID);

    IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions;
    invariant(oplogEntry.commitQuorum);
    indexBuildOptions.commitQuorum = oplogEntry.commitQuorum.get();
    indexBuildOptions.replSetAndNotPrimaryAtStart = true;

    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    uassertStatusOK(
        indexBuildsCoord
            ->startIndexBuild(opCtx,
                              nss.db().toString(),
                              collUUID,
                              oplogEntry.indexSpecs,
                              oplogEntry.buildUUID,
                              /* This oplog entry is only replicated for two-phase index builds */
                              IndexBuildProtocol::kTwoPhase,
                              indexBuildOptions)
            .getStatus());
}

void IndexBuildsCoordinator::applyCommitIndexBuild(OperationContext* opCtx,
                                                   const IndexBuildOplogEntry& oplogEntry) {
    const auto collUUID = oplogEntry.collUUID;
    const auto nss = getNsFromUUID(opCtx, collUUID);
    const auto& buildUUID = oplogEntry.buildUUID;

    updateCurOpForCommitOrAbort(opCtx, kCommitIndexBuildFieldName, buildUUID);

    uassert(31417,
            str::stream()
                << "No commit timestamp set while applying commitIndexBuild operation. Build UUID: "
                << buildUUID,
            !opCtx->recoveryUnit()->getCommitTimestamp().isNull());

    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto swReplState = indexBuildsCoord->_getIndexBuild(buildUUID);
    if (swReplState == ErrorCodes::NoSuchKey) {
        // If the index build was not found, we must restart the build. For some reason the index
        // build has already been aborted on this node. This is possible in certain infrequent race
        // conditions with stepdown, shutdown, and user interruption.
        // Also, it can be because, when this node was previously in
        // initial sync state and this index build was in progress on sync source. And, initial sync
        // does not start the in progress index builds.
        LOGV2(20653,
              "Could not find an active index build with UUID {buildUUID} while processing a "
              "commitIndexBuild oplog entry. Restarting the index build on "
              "collection {nss} ({collUUID}) at optime {opCtx_recoveryUnit_getCommitTimestamp}",
              "buildUUID"_attr = buildUUID,
              "nss"_attr = nss,
              "collUUID"_attr = collUUID,
              "opCtx_recoveryUnit_getCommitTimestamp"_attr =
                  opCtx->recoveryUnit()->getCommitTimestamp());

        IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions;
        indexBuildOptions.replSetAndNotPrimaryAtStart = true;
        // It's ok to set the commitQuorum value as 0, as we have already received the
        // commitIndexBuild oplog entry. No way in future, this index build will be coordinated by
        // this node.
        indexBuildOptions.commitQuorum = CommitQuorumOptions(0);

        // This spawns a new thread and returns immediately.
        auto fut = uassertStatusOK(indexBuildsCoord->startIndexBuild(
            opCtx,
            nss.db().toString(),
            collUUID,
            oplogEntry.indexSpecs,
            buildUUID,
            /* This oplog entry is only replicated for two-phase index builds */
            IndexBuildProtocol::kTwoPhase,
            indexBuildOptions));

        // In certain optimized cases that return early, the future will already be set, and the
        // index build will already have been torn-down. Any subsequent calls to look up the index
        // build will fail immediately without any error information.
        if (fut.isReady()) {
            // Throws if there were errors building the index.
            fut.get();
            return;
        }
    }

    auto replState = uassertStatusOK(indexBuildsCoord->_getIndexBuild(buildUUID));

    while (true) {
        stdx::unique_lock<Latch> lk(replState->mutex);
        if (replState->waitForNextAction->getFuture().isReady()) {
            // If future wait is made uninterruptible, then the shutdown can stuck behind
            // oplog applier if the indexBuildCoordinator thread died after interruption on
            // shutdown. And, commitIndexBuild oplog entry will stuck waiting for reset of the
            // promise.
            const auto nextAction = replState->waitForNextAction->getFuture().get(opCtx);
            invariant(nextAction == IndexBuildAction::kCommitQuorumSatisfied);
            // Retry until the current promise result is consumed by the index builder thread and
            // a new empty promise got created by the indexBuildscoordinator thread.
            // Don't hammer it.
            sleepmillis(1);
            continue;
        }
        auto skipCheck = shouldSkipIndexBuildStateTransitionCheck(opCtx, replState->protocol);
        replState->indexBuildState.setState(IndexBuildState::kPrepareCommit,
                                            skipCheck,
                                            opCtx->recoveryUnit()->getCommitTimestamp());
        // Promise can be set only once.
        // We can't skip signaling here if a signal is already set because the previous commit or
        // abort signal might have been sent to handle for primary case.
        replState->waitForNextAction->emplaceValue(IndexBuildAction::kOplogCommit);
        break;
    }

    auto fut = replState->sharedPromise.getFuture();
    LOGV2(20654,
          "Index build joined after commit: {buildUUID}: {fut_waitNoThrow_opCtx}",
          "buildUUID"_attr = buildUUID,
          "fut_waitNoThrow_opCtx"_attr = fut.waitNoThrow(opCtx));

    // Throws if there was an error building the index.
    fut.get();
}

void IndexBuildsCoordinator::applyAbortIndexBuild(OperationContext* opCtx,
                                                  const IndexBuildOplogEntry& oplogEntry) {
    const auto collUUID = oplogEntry.collUUID;
    const auto nss = getNsFromUUID(opCtx, collUUID);
    const auto& buildUUID = oplogEntry.buildUUID;

    updateCurOpForCommitOrAbort(opCtx, kCommitIndexBuildFieldName, buildUUID);

    invariant(oplogEntry.cause);
    uassert(31420,
            str::stream()
                << "No commit timestamp set while applying abortIndexBuild operation. Build UUID: "
                << buildUUID,
            !opCtx->recoveryUnit()->getCommitTimestamp().isNull());

    std::string abortReason(str::stream()
                            << "abortIndexBuild oplog entry encountered: " << *oplogEntry.cause);
    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    indexBuildsCoord->abortIndexBuildByBuildUUID(opCtx,
                                                 buildUUID,
                                                 IndexBuildAction::kOplogAbort,
                                                 opCtx->recoveryUnit()->getCommitTimestamp(),
                                                 abortReason);
}

void IndexBuildsCoordinator::abortIndexBuildOnError(OperationContext* opCtx,
                                                    const UUID& buildUUID,
                                                    Status abortStatus) {
    // Use a null abort timestamp because the index build will generate a ghost timestamp
    // for the single-phase build on cleanup.
    std::string abortReason(str::stream() << "Index build interrupted: " << buildUUID << ": "
                                          << abortStatus.toString());
    abortIndexBuildByBuildUUIDNoWait(
        opCtx, buildUUID, IndexBuildAction::kPrimaryAbort, boost::none, abortReason);
}

void IndexBuildsCoordinator::abortIndexBuildByBuildUUID(OperationContext* opCtx,
                                                        const UUID& buildUUID,
                                                        IndexBuildAction signalAction,
                                                        boost::optional<Timestamp> abortTimestamp,
                                                        boost::optional<std::string> reason) {
    if (!abortIndexBuildByBuildUUIDNoWait(opCtx, buildUUID, signalAction, abortTimestamp, reason)) {
        return;
    }

    auto replState =
        invariant(_getIndexBuild(buildUUID),
                  str::stream() << "Abort timestamp: "
                                << abortTimestamp.get_value_or(Timestamp()).toString());

    auto fut = replState->sharedPromise.getFuture();
    LOGV2(20655,
          "Index build joined after abort: {buildUUID}: {fut_waitNoThrow}",
          "buildUUID"_attr = buildUUID,
          "fut_waitNoThrow"_attr = fut.waitNoThrow());
}

boost::optional<UUID> IndexBuildsCoordinator::abortIndexBuildByIndexNamesNoWait(
    OperationContext* opCtx,
    const UUID& collectionUUID,
    const std::vector<std::string>& indexNames,
    boost::optional<std::string> reason) {
    boost::optional<UUID> buildUUID;
    auto indexBuilds = _getIndexBuilds();
    auto onIndexBuild = [&](std::shared_ptr<ReplIndexBuildState> replState) {
        if (replState->collectionUUID != collectionUUID) {
            return;
        }

        bool matchedBuilder = std::is_permutation(indexNames.begin(),
                                                  indexNames.end(),
                                                  replState->indexNames.begin(),
                                                  replState->indexNames.end());
        if (!matchedBuilder) {
            return;
        }

        LOGV2(23880,
              "About to abort index builder: {replState_buildUUID} on collection: "
              "{collectionUUID}. First index: {replState_indexNames_front}",
              "replState_buildUUID"_attr = replState->buildUUID,
              "collectionUUID"_attr = collectionUUID,
              "replState_indexNames_front"_attr = replState->indexNames.front());

        if (this->abortIndexBuildByBuildUUIDNoWait(opCtx,
                                                   replState->buildUUID,
                                                   IndexBuildAction::kPrimaryAbort,
                                                   boost::none,
                                                   reason)) {
            buildUUID = replState->buildUUID;
        }
    };
    forEachIndexBuild(indexBuilds,
                      "IndexBuildsCoordinator::abortIndexBuildByIndexNamesNoWait - "_sd,
                      onIndexBuild);
    return buildUUID;
}

bool IndexBuildsCoordinator::hasIndexBuilder(OperationContext* opCtx,
                                             const UUID& collectionUUID,
                                             const std::vector<std::string>& indexNames) const {
    bool foundIndexBuilder = false;
    boost::optional<UUID> buildUUID;
    auto indexBuilds = _getIndexBuilds();
    auto onIndexBuild = [&](std::shared_ptr<ReplIndexBuildState> replState) {
        if (replState->collectionUUID != collectionUUID) {
            return;
        }

        bool matchedBuilder = std::is_permutation(indexNames.begin(),
                                                  indexNames.end(),
                                                  replState->indexNames.begin(),
                                                  replState->indexNames.end());
        if (!matchedBuilder) {
            return;
        }

        foundIndexBuilder = true;
    };
    forEachIndexBuild(indexBuilds, "IndexBuildsCoordinator::hasIndexBuilder - "_sd, onIndexBuild);
    return foundIndexBuilder;
}

bool IndexBuildsCoordinator::abortIndexBuildByBuildUUIDNoWait(
    OperationContext* opCtx,
    const UUID& buildUUID,
    IndexBuildAction signalAction,
    boost::optional<Timestamp> abortTimestamp,
    boost::optional<std::string> reason) {
    // We need to avoid race between commit and abort index build.
    while (true) {
        // It is possible to receive an abort for a non-existent index build. Abort should always
        // succeed, so suppress the error.
        auto replStateResult = _getIndexBuild(buildUUID);
        if (!replStateResult.isOK()) {
            LOGV2(20656,
                  "ignoring error while aborting index build {buildUUID}: "
                  "{replStateResult_getStatus}",
                  "buildUUID"_attr = buildUUID,
                  "replStateResult_getStatus"_attr = replStateResult.getStatus());
            return false;
        }

        auto replState = replStateResult.getValue();

        stdx::unique_lock<Latch> lk(replState->mutex);
        if (replState->waitForNextAction->getFuture().isReady()) {
            const auto nextAction = replState->waitForNextAction->getFuture().get(opCtx);
            invariant(nextAction == IndexBuildAction::kSinglePhaseCommit ||
                      nextAction == IndexBuildAction::kCommitQuorumSatisfied ||
                      nextAction == IndexBuildAction::kPrimaryAbort);

            // Index build coordinator already received a signal to commit or abort. So, it's ok
            // to return and wait for the index build to complete if we are trying to signal
            // 'kPrimaryAbort'. The index build coordinator will not perform the signaled action
            // (i.e, will not commit or abort the index build) only when the node steps down. When
            // the node steps down, the caller of this function, dropIndexes/createIndexes command
            // (user operation) will also get interrupted. So, we no longer need to abort the index
            // build on step down.
            //
            // Currently dropIndexes command calls this function with the
            // collection lock held in IX mode, So, there are possibilities, we might block the
            // index build from completing, leading to 3 way deadlocks involving step down,
            // dropIndexes command, IndexBuildCoordinator thread.
            if (signalAction == IndexBuildAction::kPrimaryAbort) {
                // Only return true if the index build is being aborted already, not if it is going
                // to commit.
                return nextAction == IndexBuildAction::kPrimaryAbort;
            }

            // Retry until the current promise result is consumed by the index builder thread and
            // a new empty promise got created by the indexBuildscoordinator thread. Or, until the
            // index build got torn down after index build commit.
            // Don't hammer it.
            sleepmillis(1);
            continue;
        }

        auto skipCheck = shouldSkipIndexBuildStateTransitionCheck(opCtx, replState->protocol);
        // Set the state on replState and _indexBuildsManager. And, then signal the value. It's
        // important we do all these 3 things in a critical section by holding mutex lock.
        replState->indexBuildState.setState(
            IndexBuildState::kPrepareAbort, skipCheck, abortTimestamp, reason);
        _indexBuildsManager.abortIndexBuild(buildUUID, reason.get_value_or(""));

        replState->waitForNextAction->emplaceValue(signalAction);
        break;
    }

    return true;
}

std::size_t IndexBuildsCoordinator::getActiveIndexBuildCount(OperationContext* opCtx) {
    auto indexBuilds = _getIndexBuilds();
    // We use forEachIndexBuild() to log basic details on the current index builds and don't intend
    // to modify any of the index builds, hence the no-op.
    auto onIndexBuild = [](std::shared_ptr<ReplIndexBuildState> replState) {};
    forEachIndexBuild(indexBuilds, "index build still running: "_sd, onIndexBuild);

    return indexBuilds.size();
}

void IndexBuildsCoordinator::onStepUp(OperationContext* opCtx) {
    LOGV2(20657, "IndexBuildsCoordinator::onStepUp - this node is stepping up to primary");

    // This would create an empty table even for FCV 4.2 to handle case where a primary node started
    // with FCV 4.2, and then upgraded FCV 4.4.
    ensureIndexBuildEntriesNamespaceExists(opCtx);

    auto indexBuilds = _getIndexBuilds();
    auto onIndexBuild = [this, opCtx](std::shared_ptr<ReplIndexBuildState> replState) {
        if (IndexBuildProtocol::kTwoPhase != replState->protocol) {
            return;
        }

        {
            stdx::unique_lock<Latch> lk(replState->mutex);
            // After Sending the abort this might have stepped down and stepped back up.
            if (replState->indexBuildState.isAbortPrepared() &&
                !replState->waitForNextAction->getFuture().isReady()) {
                replState->waitForNextAction->emplaceValue(IndexBuildAction::kPrimaryAbort);
                return;
            }
        }

        if (!_signalIfCommitQuorumNotEnabled(opCtx, replState, true /* onStepUp */)) {
            // This reads from system.indexBuilds collection to see if commit quorum got satisfied.
            _signalIfCommitQuorumIsSatisfied(opCtx, replState);
        }
    };
    forEachIndexBuild(indexBuilds, "IndexBuildsCoordinator::onStepUp - "_sd, onIndexBuild);
}

IndexBuilds IndexBuildsCoordinator::onRollback(OperationContext* opCtx) {
    LOGV2(20658, "IndexBuildsCoordinator::onRollback - this node is entering the rollback state");

    IndexBuilds buildsAborted;

    auto indexBuilds = _getIndexBuilds();
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto onIndexBuild = [&](std::shared_ptr<ReplIndexBuildState> replState) {
        if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
            LOGV2(3856209,
                  "IndexBuildsCoordinator::onRollback - not aborting single phase index build: "
                  "{builduuid} ",
                  "builduuid"_attr = replState->buildUUID);
            return;
        }
        if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
            LOGV2(20659,
                  "IndexBuildsCoordinator::onRollback - not aborting single phase index build: "
                  "{replState_buildUUID}",
                  "replState_buildUUID"_attr = replState->buildUUID);
            return;
        }
        const std::string reason = "rollback";

        IndexBuildDetails aborted{replState->collectionUUID};
        // Record the index builds aborted due to rollback. This allows any rollback algorithm
        // to efficiently restart all unfinished index builds without having to scan all indexes
        // in all collections.
        for (auto spec : replState->indexSpecs) {
            aborted.indexSpecs.emplace_back(spec.getOwned());
        }
        buildsAborted.insert({replState->buildUUID, aborted});

        // Leave abort timestamp as null. This will unblock the index build and allow it to
        // complete without cleaning up. Subsequently, the rollback algorithm can decide how to
        // undo the index build depending on the state of the oplog.
        // Signals the kRollbackAbort and then waits for the thread to join.
        abortIndexBuildByBuildUUID(
            opCtx, replState->buildUUID, IndexBuildAction::kRollbackAbort, boost::none, reason);

        // Now, cancel if there are any active callback handle waiting for the remote
        // "voteCommitIndexBuild" command's response.
        stdx::unique_lock<Latch> lk(replState->mutex);
        if (replState->voteCmdCbkHandle.isValid()) {
            replCoord->cancelCbkHandle(replState->voteCmdCbkHandle);
        }
    };
    forEachIndexBuild(indexBuilds, "IndexBuildsCoordinator::onRollback - "_sd, onIndexBuild);

    return buildsAborted;
}

void IndexBuildsCoordinator::restartIndexBuildsForRecovery(OperationContext* opCtx,
                                                           const IndexBuilds& buildsToRestart) {
    for (auto& [buildUUID, build] : buildsToRestart) {
        boost::optional<NamespaceString> nss =
            CollectionCatalog::get(opCtx).lookupNSSByUUID(opCtx, build.collUUID);
        invariant(nss);

        LOGV2(20660,
              "Restarting index build for collection: {nss}, collection UUID: {build_collUUID}, "
              "index build UUID: {buildUUID}",
              "nss"_attr = *nss,
              "build_collUUID"_attr = build.collUUID,
              "buildUUID"_attr = buildUUID);

        IndexBuildsCoordinator::IndexBuildOptions indexBuildOptions;
        // Start the index build as if in secondary oplog application.
        indexBuildOptions.replSetAndNotPrimaryAtStart = true;
        // Indicate that the intialization should not generate oplog entries or timestamps for the
        // first catalog write, and that the original durable catalog entries should be dropped and
        // replaced.
        indexBuildOptions.twoPhaseRecovery = true;
        // This spawns a new thread and returns immediately. These index builds will start and wait
        // for a commit or abort to be replicated.
        MONGO_COMPILER_VARIABLE_UNUSED auto fut =
            uassertStatusOK(startIndexBuild(opCtx,
                                            nss->db().toString(),
                                            build.collUUID,
                                            build.indexSpecs,
                                            buildUUID,
                                            IndexBuildProtocol::kTwoPhase,
                                            indexBuildOptions));
    }
}

int IndexBuildsCoordinator::numInProgForDb(StringData db) const {
    stdx::unique_lock<Latch> lk(_mutex);

    auto dbIndexBuildsIt = _databaseIndexBuilds.find(db);
    if (dbIndexBuildsIt == _databaseIndexBuilds.end()) {
        return 0;
    }
    return dbIndexBuildsIt->second->getNumberOfIndexBuilds(lk);
}

void IndexBuildsCoordinator::dump(std::ostream& ss) const {
    stdx::unique_lock<Latch> lk(_mutex);

    if (_collectionIndexBuilds.size()) {
        ss << "\n<b>Background Jobs in Progress</b>\n";
        // TODO: We should improve this to print index names per collection, not just collection
        // names.
        for (auto it = _collectionIndexBuilds.begin(); it != _collectionIndexBuilds.end(); ++it) {
            ss << "  " << it->first << '\n';
        }
    }

    for (auto it = _databaseIndexBuilds.begin(); it != _databaseIndexBuilds.end(); ++it) {
        ss << "database " << it->first << ": " << it->second->getNumberOfIndexBuilds(lk) << '\n';
    }
}

bool IndexBuildsCoordinator::inProgForCollection(const UUID& collectionUUID,
                                                 IndexBuildProtocol protocol) const {
    stdx::unique_lock<Latch> lk(_mutex);
    auto indexBuildFilter = [=](const auto& replState) {
        return collectionUUID == replState.collectionUUID && protocol == replState.protocol;
    };
    auto indexBuilds = _filterIndexBuilds_inlock(lk, indexBuildFilter);
    return !indexBuilds.empty();
}

bool IndexBuildsCoordinator::inProgForCollection(const UUID& collectionUUID) const {
    stdx::unique_lock<Latch> lk(_mutex);
    return _collectionIndexBuilds.find(collectionUUID) != _collectionIndexBuilds.end();
}

bool IndexBuildsCoordinator::inProgForDb(StringData db) const {
    stdx::unique_lock<Latch> lk(_mutex);
    return _databaseIndexBuilds.find(db) != _databaseIndexBuilds.end();
}

void IndexBuildsCoordinator::assertNoIndexBuildInProgress() const {
    stdx::unique_lock<Latch> lk(_mutex);
    uassert(ErrorCodes::BackgroundOperationInProgressForDatabase,
            str::stream() << "cannot perform operation: there are currently "
                          << _allIndexBuilds.size() << " index builds running.",
            _allIndexBuilds.size() == 0);
}

void IndexBuildsCoordinator::assertNoIndexBuildInProgForCollection(
    const UUID& collectionUUID) const {
    uassert(ErrorCodes::BackgroundOperationInProgressForNamespace,
            str::stream() << "cannot perform operation: an index build is currently running for "
                             "collection with UUID: "
                          << collectionUUID,
            !inProgForCollection(collectionUUID));
}

void IndexBuildsCoordinator::assertNoBgOpInProgForDb(StringData db) const {
    uassert(ErrorCodes::BackgroundOperationInProgressForDatabase,
            str::stream() << "cannot perform operation: an index build is currently running for "
                             "database "
                          << db,
            !inProgForDb(db));
}

void IndexBuildsCoordinator::awaitIndexBuildFinished(const UUID& collectionUUID,
                                                     const UUID& buildUUID) const {
    stdx::unique_lock<Latch> lk(_mutex);

    auto collIndexBuildsIt = _collectionIndexBuilds.find(collectionUUID);
    if (collIndexBuildsIt == _collectionIndexBuilds.end()) {
        return;
    }

    // Take a shared ptr, rather than accessing the Tracker through the map's iterator, so that the
    // object does not destruct while we are waiting, causing a use-after-free memory error.
    auto collIndexBuildsSharedPtr = collIndexBuildsIt->second;
    collIndexBuildsSharedPtr->waitUntilIndexBuildFinished(lk, buildUUID);
}

void IndexBuildsCoordinator::awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                                      const UUID& collectionUUID,
                                                                      IndexBuildProtocol protocol) {
    stdx::unique_lock<Latch> lk(_mutex);
    auto noIndexBuildsPred = [&, this]() {
        auto indexBuilds = _filterIndexBuilds_inlock(lk, [&](const auto& replState) {
            return collectionUUID == replState.collectionUUID && protocol == replState.protocol;
        });
        return indexBuilds.empty();
    };
    opCtx->waitForConditionOrInterrupt(_indexBuildsCondVar, lk, noIndexBuildsPred);
}

void IndexBuildsCoordinator::awaitNoIndexBuildInProgressForCollection(
    const UUID& collectionUUID) const {
    stdx::unique_lock<Latch> lk(_mutex);

    auto collIndexBuildsIt = _collectionIndexBuilds.find(collectionUUID);
    if (collIndexBuildsIt == _collectionIndexBuilds.end()) {
        return;
    }

    // Take a shared ptr, rather than accessing the Tracker through the map's iterator, so that the
    // object does not destruct while we are waiting, causing a use-after-free memory error.
    auto collIndexBuildsSharedPtr = collIndexBuildsIt->second;
    collIndexBuildsSharedPtr->waitUntilNoIndexBuildsRemain(lk);
    invariant(collIndexBuildsSharedPtr->getNumberOfIndexBuilds(lk) == 0);
}

void IndexBuildsCoordinator::awaitNoBgOpInProgForDb(StringData db) const {
    stdx::unique_lock<Latch> lk(_mutex);

    auto dbIndexBuildsIt = _databaseIndexBuilds.find(db);
    if (dbIndexBuildsIt == _databaseIndexBuilds.end()) {
        return;
    }

    // Take a shared ptr, rather than accessing the Tracker through the map's iterator, so that the
    // object does not destruct while we are waiting, causing a use-after-free memory error.
    auto dbIndexBuildsSharedPtr = dbIndexBuildsIt->second;
    dbIndexBuildsSharedPtr->waitUntilNoIndexBuildsRemain(lk);
}

void IndexBuildsCoordinator::onReplicaSetReconfig() {
    // TODO: not yet implemented.
}

void IndexBuildsCoordinator::createIndexes(OperationContext* opCtx,
                                           UUID collectionUUID,
                                           const std::vector<BSONObj>& specs,
                                           IndexBuildsManager::IndexConstraints indexConstraints,
                                           bool fromMigrate) {
    auto collection = CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, collectionUUID);
    invariant(collection,
              str::stream() << "IndexBuildsCoordinator::createIndexes: " << collectionUUID);
    auto nss = collection->ns();
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X),
              str::stream() << "IndexBuildsCoordinator::createIndexes: " << collectionUUID);

    auto buildUUID = UUID::gen();

    // Rest of this function can throw, so ensure the build cleanup occurs.
    ON_BLOCK_EXIT([&] {
        opCtx->recoveryUnit()->abandonSnapshot();
        _indexBuildsManager.tearDownIndexBuild(
            opCtx, collection, buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
    });

    auto onInitFn = MultiIndexBlock::makeTimestampedIndexOnInitFn(opCtx, collection);
    IndexBuildsManager::SetupOptions options;
    options.indexConstraints = indexConstraints;
    uassertStatusOK(_indexBuildsManager.setUpIndexBuild(
        opCtx, collection, specs, buildUUID, onInitFn, options));

    uassertStatusOK(_indexBuildsManager.startBuildingIndex(opCtx, collection, buildUUID));

    uassertStatusOK(_indexBuildsManager.checkIndexConstraintViolations(opCtx, buildUUID));

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    auto onCreateEachFn = [&](const BSONObj& spec) {
        // If two phase index builds is enabled, index build will be coordinated using
        // startIndexBuild and commitIndexBuild oplog entries.
        if (supportsTwoPhaseIndexBuild()) {
            return;
        }
        opObserver->onCreateIndex(opCtx, collection->ns(), collectionUUID, spec, fromMigrate);
    };
    auto onCommitFn = [&] {
        // Index build completion will be timestamped using the createIndexes oplog entry.
        if (!supportsTwoPhaseIndexBuild()) {
            return;
        }
        // Since, we don't use IndexBuildsCoordinatorMongod thread pool to build system indexes,
        // it's ok to set the commit quorum option as 1. Also, this is currently only get
        // called during system index creation on startup. So, onStartIndexBuild() call will be a
        // no-op.
        opObserver->onStartIndexBuild(
            opCtx, nss, collectionUUID, buildUUID, specs, CommitQuorumOptions(1), fromMigrate);
        opObserver->onCommitIndexBuild(opCtx, nss, collectionUUID, buildUUID, specs, fromMigrate);
    };
    uassertStatusOK(_indexBuildsManager.commitIndexBuild(
        opCtx, collection, nss, buildUUID, onCreateEachFn, onCommitFn));
}

void IndexBuildsCoordinator::createIndexesOnEmptyCollection(OperationContext* opCtx,
                                                            UUID collectionUUID,
                                                            const std::vector<BSONObj>& specs,
                                                            bool fromMigrate) {
    auto collection = CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, collectionUUID);

    invariant(collection, str::stream() << collectionUUID);
    invariant(0U == collection->numRecords(opCtx), str::stream() << collectionUUID);
    invariant(!specs.empty(), str::stream() << collectionUUID);

    auto nss = collection->ns();
    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx,
                                                                               collection->ns());

    auto opObserver = opCtx->getServiceContext()->getOpObserver();

    auto indexCatalog = collection->getIndexCatalog();
    // Always run single phase index build for empty collection. And, will be coordinated using
    // createIndexes oplog entry.
    for (const auto& spec : specs) {
        // Each index will be added to the mdb catalog using the preceding createIndexes
        // timestamp.
        opObserver->onCreateIndex(opCtx, nss, collectionUUID, spec, fromMigrate);
        uassertStatusOK(indexCatalog->createIndexOnEmptyCollection(opCtx, spec));
    }
}

void IndexBuildsCoordinator::sleepIndexBuilds_forTestOnly(bool sleep) {
    stdx::unique_lock<Latch> lk(_mutex);
    _sleepForTest = sleep;
}

void IndexBuildsCoordinator::verifyNoIndexBuilds_forTestOnly() {
    invariant(_databaseIndexBuilds.empty());
    invariant(_collectionIndexBuilds.empty());
}

// static
void IndexBuildsCoordinator::updateCurOpOpDescription(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const std::vector<BSONObj>& indexSpecs) {
    BSONObjBuilder builder;

    // If the collection namespace is provided, add a 'createIndexes' field with the collection name
    // to allow tests to identify this op as an index build.
    if (!nss.isEmpty()) {
        builder.append(kCreateIndexesFieldName, nss.coll());
    }

    // If index specs are provided, add them under the 'indexes' field.
    if (!indexSpecs.empty()) {
        BSONArrayBuilder indexesBuilder;
        for (const auto& spec : indexSpecs) {
            indexesBuilder.append(spec);
        }
        builder.append(kIndexesFieldName, indexesBuilder.arr());
    }

    stdx::unique_lock<Client> lk(*opCtx->getClient());
    auto curOp = CurOp::get(opCtx);
    builder.appendElementsUnique(curOp->opDescription());
    auto opDescObj = builder.obj();
    curOp->setLogicalOp_inlock(LogicalOp::opCommand);
    curOp->setOpDescription_inlock(opDescObj);
    curOp->ensureStarted();
}

Status IndexBuildsCoordinator::_registerIndexBuild(
    WithLock lk, std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {
    // Check whether any indexes are already being built with the same index name(s). (Duplicate
    // specs will be discovered by the index builder.)
    auto collIndexBuildsIt = _collectionIndexBuilds.find(replIndexBuildState->collectionUUID);
    if (collIndexBuildsIt != _collectionIndexBuilds.end()) {
        for (const auto& name : replIndexBuildState->indexNames) {
            if (collIndexBuildsIt->second->hasIndexBuildState(lk, name)) {
                auto existingIndexBuild = collIndexBuildsIt->second->getIndexBuildState(lk, name);
                str::stream ss;
                ss << "Index build conflict: " << replIndexBuildState->buildUUID
                   << ": There's already an index with name '" << name
                   << "' being built on the collection "
                   << " ( " << replIndexBuildState->collectionUUID
                   << " ) under an existing index build: " << existingIndexBuild->buildUUID;
                auto aborted = false;
                {
                    // We have to lock the mutex in order to read the committed/aborted state.
                    stdx::unique_lock<Latch> lk(existingIndexBuild->mutex);
                    ss << " index build state: " << existingIndexBuild->indexBuildState.toString();
                    if (auto ts = existingIndexBuild->indexBuildState.getTimestamp()) {
                        ss << ", timestamp: " << ts->toString();
                    }
                    if (existingIndexBuild->indexBuildState.isSet(IndexBuildState::kPrepareAbort |
                                                                  IndexBuildState::kAborted)) {
                        if (auto abortReason =
                                existingIndexBuild->indexBuildState.getAbortReason()) {
                            ss << ", abort reason: " << abortReason.get();
                        }
                        aborted = true;
                    }
                }
                std::string msg = ss;
                LOGV2(20661, "{msg}", "msg"_attr = msg);
                if (aborted) {
                    return {ErrorCodes::IndexBuildAborted, msg};
                }
                return Status(ErrorCodes::IndexBuildAlreadyInProgress, msg);
            }
        }
    }

    // Register the index build.

    auto dbIndexBuilds = _databaseIndexBuilds[replIndexBuildState->dbName];
    if (!dbIndexBuilds) {
        _databaseIndexBuilds[replIndexBuildState->dbName] =
            std::make_shared<DatabaseIndexBuildsTracker>();
        dbIndexBuilds = _databaseIndexBuilds[replIndexBuildState->dbName];
    }
    dbIndexBuilds->addIndexBuild(lk, replIndexBuildState);

    auto collIndexBuildsItAndRes = _collectionIndexBuilds.insert(
        {replIndexBuildState->collectionUUID, std::make_shared<CollectionIndexBuildsTracker>()});
    collIndexBuildsItAndRes.first->second->addIndexBuild(lk, replIndexBuildState);

    invariant(_allIndexBuilds.emplace(replIndexBuildState->buildUUID, replIndexBuildState).second);

    _indexBuildsCondVar.notify_all();

    return Status::OK();
}

void IndexBuildsCoordinator::_unregisterIndexBuild(
    WithLock lk, std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {
    auto dbIndexBuilds = _databaseIndexBuilds[replIndexBuildState->dbName];
    invariant(dbIndexBuilds);
    dbIndexBuilds->removeIndexBuild(lk, replIndexBuildState->buildUUID);
    if (dbIndexBuilds->getNumberOfIndexBuilds(lk) == 0) {
        _databaseIndexBuilds.erase(replIndexBuildState->dbName);
    }

    auto collIndexBuildsIt = _collectionIndexBuilds.find(replIndexBuildState->collectionUUID);
    invariant(collIndexBuildsIt != _collectionIndexBuilds.end());
    collIndexBuildsIt->second->removeIndexBuild(lk, replIndexBuildState);
    if (collIndexBuildsIt->second->getNumberOfIndexBuilds(lk) == 0) {
        _collectionIndexBuilds.erase(collIndexBuildsIt);
    }

    invariant(_allIndexBuilds.erase(replIndexBuildState->buildUUID));

    _indexBuildsCondVar.notify_all();
}

Status IndexBuildsCoordinator::_setUpIndexBuildForTwoPhaseRecovery(
    OperationContext* opCtx,
    StringData dbName,
    CollectionUUID collectionUUID,
    const std::vector<BSONObj>& specs,
    const UUID& buildUUID) {
    NamespaceStringOrUUID nssOrUuid{dbName.toString(), collectionUUID};

    // Don't use the AutoGet helpers because they require an open database, which may not be the
    // case when an index builds is restarted during recovery.
    Lock::DBLock dbLock(opCtx, dbName, MODE_IX);
    Lock::CollectionLock collLock(opCtx, nssOrUuid, MODE_X);
    auto collection = CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, collectionUUID);
    invariant(collection);
    const auto& nss = collection->ns();
    const auto protocol = IndexBuildProtocol::kTwoPhase;
    return _startIndexBuildForRecovery(opCtx, nss, specs, buildUUID, protocol);
}

StatusWith<boost::optional<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>>
IndexBuildsCoordinator::_filterSpecsAndRegisterBuild(
    OperationContext* opCtx,
    StringData dbName,
    CollectionUUID collectionUUID,
    const std::vector<BSONObj>& specs,
    const UUID& buildUUID,
    IndexBuildProtocol protocol,
    boost::optional<CommitQuorumOptions> commitQuorum) {

    // AutoGetCollection throws an exception if it is unable to look up the collection by UUID.
    NamespaceStringOrUUID nssOrUuid{dbName.toString(), collectionUUID};
    AutoGetCollection autoColl(opCtx, nssOrUuid, MODE_X);
    auto collection = autoColl.getCollection();
    const auto& nss = collection->ns();

    // This check is for optimization purposes only as since this lock is released after this,
    // and is acquired again when we build the index in _setUpIndexBuild.
    auto status = CollectionShardingState::get(opCtx, nss)->checkShardVersionNoThrow(opCtx);
    if (!status.isOK()) {
        return status;
    }

    // Lock from when we ascertain what indexes to build through to when the build is registered
    // on the Coordinator and persistedly set up in the catalog. This serializes setting up an
    // index build so that no attempts are made to register the same build twice.
    stdx::unique_lock<Latch> lk(_mutex);

    std::vector<BSONObj> filteredSpecs;
    try {
        filteredSpecs = prepareSpecListForCreate(opCtx, collection, nss, specs);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    if (filteredSpecs.size() == 0) {
        // The requested index (specs) are already built or are being built. Return success
        // early (this is v4.0 behavior compatible).
        ReplIndexBuildState::IndexCatalogStats indexCatalogStats;
        int numIndexes = getNumIndexesTotal(opCtx, collection);
        indexCatalogStats.numIndexesBefore = numIndexes;
        indexCatalogStats.numIndexesAfter = numIndexes;
        return SharedSemiFuture(indexCatalogStats);
    }

    // Bypass the thread pool if we are building indexes on an empty collection.
    if (shouldBuildIndexesOnEmptyCollectionSinglePhased(opCtx, collection, protocol)) {
        ReplIndexBuildState::IndexCatalogStats indexCatalogStats;
        indexCatalogStats.numIndexesBefore = getNumIndexesTotal(opCtx, collection);
        try {
            // Replicate this index build using the old-style createIndexes oplog entry to avoid
            // timestamping issues that would result from this empty collection optimization on a
            // secondary. If we tried to generate two phase index build startIndexBuild and
            // commitIndexBuild oplog entries, this optimization will fail to accurately timestamp
            // the catalog update when it uses the timestamp from the startIndexBuild, rather than
            // the commitIndexBuild, oplog entry.
            writeConflictRetry(
                opCtx, "IndexBuildsCoordinator::_filterSpecsAndRegisterBuild", nss.ns(), [&] {
                    WriteUnitOfWork wuow(opCtx);
                    createIndexesOnEmptyCollection(opCtx, collection->uuid(), filteredSpecs, false);
                    wuow.commit();
                });
        } catch (DBException& ex) {
            ex.addContext(str::stream() << "index build on empty collection failed: " << buildUUID);
            return ex.toStatus();
        }
        indexCatalogStats.numIndexesAfter = getNumIndexesTotal(opCtx, collection);
        return SharedSemiFuture(indexCatalogStats);
    }

    auto replIndexBuildState = std::make_shared<ReplIndexBuildState>(
        buildUUID, collectionUUID, dbName.toString(), filteredSpecs, protocol, commitQuorum);
    replIndexBuildState->stats.numIndexesBefore = getNumIndexesTotal(opCtx, collection);

    status = _registerIndexBuild(lk, replIndexBuildState);
    if (!status.isOK()) {
        return status;
    }

    // The index has been registered on the Coordinator in an unstarted state. Return an
    // uninitialized Future so that the caller can set up the index build by calling
    // _setUpIndexBuild(). The completion of the index build will be communicated via a Future
    // obtained from 'replIndexBuildState->sharedPromise'.
    return boost::none;
}

IndexBuildsCoordinator::PostSetupAction IndexBuildsCoordinator::_setUpIndexBuildInner(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    Timestamp startTimestamp) {
    const NamespaceStringOrUUID nssOrUuid{replState->dbName, replState->collectionUUID};

    AutoGetCollection autoColl(opCtx, nssOrUuid, MODE_X);

    auto collection = autoColl.getCollection();
    const auto& nss = collection->ns();
    CollectionShardingState::get(opCtx, nss)->checkShardVersionOrThrow(opCtx);

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool replSetAndNotPrimary =
        replCoord->getSettings().usingReplSets() && !replCoord->canAcceptWritesFor(opCtx, nss);

    // We will not have a start timestamp if we are newly a secondary (i.e. we started as
    // primary but there was a stepdown). We will be unable to timestamp the initial catalog write,
    // so we must fail the index build.
    if (replSetAndNotPrimary) {
        uassert(ErrorCodes::NotMaster,
                str::stream() << "Replication state changed while setting up the index build: "
                              << replState->buildUUID,
                !startTimestamp.isNull());
    }

    MultiIndexBlock::OnInitFn onInitFn;
    if (IndexBuildProtocol::kTwoPhase == replState->protocol) {
        // Change the startIndexBuild Oplog entry.
        // Two-phase index builds write a different oplog entry than the default behavior which
        // writes a no-op just to generate an optime.
        onInitFn = [&](std::vector<BSONObj>& specs) {
            if (!replCoord->canAcceptWritesFor(opCtx, nss)) {
                // Not primary.
                return Status::OK();
            }

            stdx::unique_lock<Latch> lk(replState->mutex);
            // Need to run this in repl mutex, as we want to the commitQuorum value. And, generate
            // the startIndexBuild oplog entry with mutex lock held. We basically don't want
            // something like this, SetIndexCommitQuorum command changes the commit quorum from 3
            // to 5. And, the startIndexBuild resets the commit quorum value to be 3 on secondaries.
            invariant(replState->commitQuorum, "Commit quorum required for two phase index build");
            opCtx->getServiceContext()->getOpObserver()->onStartIndexBuild(
                opCtx,
                nss,
                replState->collectionUUID,
                replState->buildUUID,
                replState->indexSpecs,
                replState->commitQuorum.get(),
                false /* fromMigrate */);

            return Status::OK();
        };
    } else {
        onInitFn = MultiIndexBlock::makeTimestampedIndexOnInitFn(opCtx, collection);
    }

    IndexBuildsManager::SetupOptions options;
    options.indexConstraints =
        repl::ReplicationCoordinator::get(opCtx)->shouldRelaxIndexConstraints(opCtx, nss)
        ? IndexBuildsManager::IndexConstraints::kRelax
        : IndexBuildsManager::IndexConstraints::kEnforce;
    options.protocol = replState->protocol;

    try {
        if (!replSetAndNotPrimary) {
            // On standalones and primaries, call setUpIndexBuild(), which makes the initial catalog
            // write. On primaries, this replicates the startIndexBuild oplog entry.
            uassertStatusOK(_indexBuildsManager.setUpIndexBuild(
                opCtx, collection, replState->indexSpecs, replState->buildUUID, onInitFn, options));
        } else {
            // If we are starting the index build as a secondary, we must suppress calls to write
            // our initial oplog entry in setUpIndexBuild().
            repl::UnreplicatedWritesBlock uwb(opCtx);

            // Use the provided timestamp to write the initial catalog entry.
            invariant(!startTimestamp.isNull());
            TimestampBlock tsBlock(opCtx, startTimestamp);
            uassertStatusOK(_indexBuildsManager.setUpIndexBuild(
                opCtx, collection, replState->indexSpecs, replState->buildUUID, onInitFn, options));
        }
    } catch (DBException& ex) {
        _indexBuildsManager.tearDownIndexBuild(
            opCtx, collection, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);

        const auto& status = ex.toStatus();
        if (status == ErrorCodes::IndexAlreadyExists ||
            ((status == ErrorCodes::IndexOptionsConflict ||
              status == ErrorCodes::IndexKeySpecsConflict) &&
             options.indexConstraints == IndexBuildsManager::IndexConstraints::kRelax)) {
            LOGV2_DEBUG(
                20662, 1, "Ignoring indexing error: {status}", "status"_attr = redact(status));
            return PostSetupAction::kCompleteIndexBuildEarly;
        }

        throw;
    }

    return PostSetupAction::kContinueIndexBuild;
}

Status IndexBuildsCoordinator::_setUpIndexBuild(OperationContext* opCtx,
                                                const UUID& buildUUID,
                                                Timestamp startTimestamp) {
    auto replState = invariant(_getIndexBuild(buildUUID));

    auto postSetupAction = PostSetupAction::kContinueIndexBuild;
    try {
        postSetupAction = _setUpIndexBuildInner(opCtx, replState, startTimestamp);
    } catch (const DBException& ex) {
        stdx::unique_lock<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replState);

        return ex.toStatus();
    }

    // The indexes are in the durable catalog in an unfinished state. Return an OK status so
    // that the caller can continue building the indexes by calling _runIndexBuild().
    if (PostSetupAction::kContinueIndexBuild == postSetupAction) {
        return Status::OK();
    }

    // Unregister the index build before setting the promise, so callers do not see the build again.
    {
        stdx::unique_lock<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replState);
    }

    // The requested index (specs) are already built or are being built. Return success
    // early (this is v4.0 behavior compatible).
    invariant(PostSetupAction::kCompleteIndexBuildEarly == postSetupAction,
              str::stream() << "failed to set up index build " << buildUUID
                            << " with start timestamp " << startTimestamp.toString());
    ReplIndexBuildState::IndexCatalogStats indexCatalogStats;
    int numIndexes = replState->stats.numIndexesBefore;
    indexCatalogStats.numIndexesBefore = numIndexes;
    indexCatalogStats.numIndexesAfter = numIndexes;
    replState->sharedPromise.emplaceValue(indexCatalogStats);
    return Status::OK();
}

void IndexBuildsCoordinator::_runIndexBuild(OperationContext* opCtx,
                                            const UUID& buildUUID,
                                            const IndexBuildOptions& indexBuildOptions) noexcept {
    {
        stdx::unique_lock<Latch> lk(_mutex);
        while (_sleepForTest) {
            lk.unlock();
            sleepmillis(100);
            lk.lock();
        }
    }

    // If the index build does not exist, do not continue building the index. This may happen if an
    // ignorable indexing error occurred during setup. The promise will have been fulfilled, but the
    // build has already been unregistered.
    auto swReplState = _getIndexBuild(buildUUID);
    if (swReplState.getStatus() == ErrorCodes::NoSuchKey) {
        return;
    }
    auto replState = invariant(swReplState);

    // Add build UUID to lock manager diagnostic output.
    auto locker = opCtx->lockState();
    auto oldLockerDebugInfo = locker->getDebugInfo();
    {
        str::stream ss;
        ss << "index build: " << replState->buildUUID;
        if (!oldLockerDebugInfo.empty()) {
            ss << "; " << oldLockerDebugInfo;
        }
        locker->setDebugInfo(ss);
    }

    auto status = [&]() {
        try {
            _runIndexBuildInner(opCtx, replState, indexBuildOptions);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
        return Status::OK();
    }();

    locker->setDebugInfo(oldLockerDebugInfo);

    // Ensure the index build is unregistered from the Coordinator and the Promise is set with
    // the build's result so that callers are notified of the outcome.

    stdx::unique_lock<Latch> lk(_mutex);

    _unregisterIndexBuild(lk, replState);

    if (status.isOK()) {
        replState->sharedPromise.emplaceValue(replState->stats);
    } else {
        replState->sharedPromise.setError(status);
    }
}

void IndexBuildsCoordinator::_cleanUpSinglePhaseAfterFailure(
    OperationContext* opCtx,
    Collection* collection,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    const Status& status) {
    if (status == ErrorCodes::InterruptedAtShutdown) {
        // Leave it as-if kill -9 happened. Startup recovery will rebuild the index.
        _indexBuildsManager.abortIndexBuildWithoutCleanup(
            opCtx, collection, replState->buildUUID, "shutting down");
        _indexBuildsManager.tearDownIndexBuild(
            opCtx, collection, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
        return;
    }

    // If the index build was not completed successfully, we'll need to acquire some locks to
    // clean it up.
    UninterruptibleLockGuard noInterrupt(opCtx->lockState());

    NamespaceString nss = collection->ns();
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_IX);

    if (indexBuildOptions.replSetAndNotPrimaryAtStart) {
        // This build started and failed as a secondary. Single-phase index builds started on
        // secondaries may not fail. Do not clean up the index build. It must remain unfinished
        // until it is successfully rebuilt on startup.
        fassert(31354,
                status.withContext(str::stream() << "Index build: " << replState->buildUUID
                                                 << "; Database: " << replState->dbName));
    }

    // Unlock the RSTL to avoid deadlocks with state transitions.
    unlockRSTLForIndexCleanup(opCtx);
    Lock::CollectionLock collLock(opCtx, nss, MODE_X);

    // If we started the build as a primary and are now unable to accept writes, this build was
    // aborted due to a stepdown.
    _indexBuildsManager.tearDownIndexBuild(
        opCtx, collection, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
}

void IndexBuildsCoordinator::_cleanUpTwoPhaseAfterFailure(
    OperationContext* opCtx,
    Collection* collection,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    const Status& status) {

    if (status == ErrorCodes::InterruptedAtShutdown) {
        // Promise should be set at least once before it's getting destroyed. Else it would
        // invariant.
        {
            stdx::unique_lock<Latch> lk(replState->mutex);
            if (!replState->waitForNextAction->getFuture().isReady()) {
                replState->waitForNextAction->emplaceValue(IndexBuildAction::kNoAction);
            }
        }
        // Leave it as-if kill -9 happened. Startup recovery will restart the index build.
        _indexBuildsManager.abortIndexBuildWithoutCleanup(
            opCtx, collection, replState->buildUUID, "shutting down");
        _indexBuildsManager.tearDownIndexBuild(
            opCtx, collection, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
        return;
    }

    // If the index build was not completed successfully, we'll need to acquire some locks to
    // clean it up.
    UninterruptibleLockGuard noInterrupt(opCtx->lockState());

    NamespaceString nss = collection->ns();
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_IX);

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().usingReplSets() && !replCoord->canAcceptWritesFor(opCtx, nss)) {
        // We failed this index build as a secondary node.

        // Failed index builds should fatally assert on the secondary, except when the index build
        // was stopped due to an explicit abort oplog entry or rollback.
        if (status == ErrorCodes::IndexBuildAborted) {
            // On a secondary, we should be able to obtain the timestamp for cleaning up the index
            // build from the oplog entry unless the index build did not fail due to processing an
            // abortIndexBuild oplog entry. This is the case if we were aborted due to rollback.
            stdx::unique_lock<Latch> lk(replState->mutex);
            invariant(replState->indexBuildState.isAbortPrepared(),
                      replState->buildUUID.toString());
            auto abortIndexBuildTimestamp = replState->indexBuildState.getTimestamp();

            // If we were aborted and no abort timestamp is set, then we should leave the index
            // build unfinished. This can happen during rollback because we are not primary and
            // cannot generate an optime to timestamp the index build abort. We rely on the
            // rollback process to correct this state.
            if (!abortIndexBuildTimestamp) {
                _indexBuildsManager.abortIndexBuildWithoutCleanup(
                    opCtx, collection, replState->buildUUID, "no longer primary");
                _indexBuildsManager.tearDownIndexBuild(
                    opCtx, collection, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
                return;
            }

            // Unlock the RSTL to avoid deadlocks with state transitions. See SERVER-42824.
            unlockRSTLForIndexCleanup(opCtx);
            Lock::CollectionLock collLock(opCtx, nss, MODE_X);

            TimestampBlock tsBlock(opCtx, abortIndexBuildTimestamp.get());
            _indexBuildsManager.tearDownIndexBuild(
                opCtx, collection, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
            return;
        }

        fassert(51101,
                status.withContext(str::stream() << "Index build: " << replState->buildUUID
                                                 << "; Database: " << replState->dbName));
    }

    // We are currently a primary node. Notify downstream nodes to abort their index builds with the
    // same build UUID.
    Lock::CollectionLock collLock(opCtx, nss, MODE_X);
    auto onCleanUpFn = [&] { onAbortIndexBuild(opCtx, nss, *replState, status); };
    _indexBuildsManager.tearDownIndexBuild(opCtx, collection, replState->buildUUID, onCleanUpFn);
    return;
}

void IndexBuildsCoordinator::_runIndexBuildInner(OperationContext* opCtx,
                                                 std::shared_ptr<ReplIndexBuildState> replState,
                                                 const IndexBuildOptions& indexBuildOptions) {
    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);

    // This Status stays unchanged unless we catch an exception in the following try-catch block.
    auto status = Status::OK();
    try {
        // Lock acquisition might throw, and we would still need to clean up the index build state,
        // so do it in the try-catch block
        AutoGetDb autoDb(opCtx, replState->dbName, MODE_IX);

        // Do not use AutoGetCollection since the lock will be reacquired in various modes
        // throughout the index build. Lock by UUID to protect against concurrent collection rename.
        boost::optional<Lock::CollectionLock> collLock;
        collLock.emplace(opCtx, dbAndUUID, MODE_X);

        // Two phase index builds and single-phase builds on secondaries can only be interrupted at
        // shutdown. For the duration of the runWithoutInterruptionExceptAtGlobalShutdown()
        // invocation, any kill status set by the killOp command will be ignored. After
        // runWithoutInterruptionExceptAtGlobalShutdown() returns, any call to checkForInterrupt()
        // will see the kill status and respond accordingly (checkForInterrupt() will throw an
        // exception while checkForInterruptNoAssert() returns an error Status).
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        if (!replCoord->getSettings().usingReplSets()) {
            _buildIndex(opCtx, replState, indexBuildOptions, &collLock);
        } else if (IndexBuildProtocol::kTwoPhase == replState->protocol) {
            opCtx->runWithoutInterruptionExceptAtGlobalShutdown(
                [&, this] { _buildIndex(opCtx, replState, indexBuildOptions, &collLock); });
        } else {
            if (indexBuildOptions.replSetAndNotPrimaryAtStart) {
                // We need to drop the RSTL here, as we do not need synchronization with step up and
                // step down. Dropping the RSTL is important because otherwise if we held the RSTL
                // it would create deadlocks with prepared transactions on step up and step down.  A
                // deadlock could result if the index build was attempting to acquire a Collection S
                // or X lock while a prepared transaction held a Collection IX lock, and a step down
                // was waiting to acquire the RSTL in mode X.
                // TODO(SERVER-44045): Revisit this logic for the non-two phase index build case.
                const bool unlocked = opCtx->lockState()->unlockRSTLforPrepare();
                invariant(unlocked);
                opCtx->runWithoutInterruptionExceptAtGlobalShutdown(
                    [&, this] { _buildIndex(opCtx, replState, indexBuildOptions, &collLock); });
            } else {
                _buildIndex(opCtx, replState, indexBuildOptions, &collLock);
            }
        }
        // If _buildIndex returned normally, then we should have the collection X lock. It is not
        // required to safely access the collection, though, because an index build is registerd.
        auto collection =
            CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, replState->collectionUUID);
        invariant(collection);
        replState->stats.numIndexesAfter = getNumIndexesTotal(opCtx, collection);
    } catch (const DBException& ex) {
        status = ex.toStatus();
    }

    // We do not hold a collection lock here, but we are protected against the collection being
    // dropped while the index build is still registered for the collection -- until
    // tearDownIndexBuild is called. The collection can be renamed, but it is OK for the name to
    // be stale just for logging purposes.
    auto collection =
        CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, replState->collectionUUID);
    invariant(collection,
              str::stream() << "Collection with UUID " << replState->collectionUUID
                            << " should exist because an index build is in progress: "
                            << replState->buildUUID);
    NamespaceString nss = collection->ns();

    if (status.isOK()) {
        _indexBuildsManager.tearDownIndexBuild(
            opCtx, collection, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);

        LOGV2(20663,
              "Index build completed successfully: {replState_buildUUID}: {nss} ( "
              "{replState_collectionUUID} ). Index specs built: {replState_indexSpecs_size}. "
              "Indexes in catalog before build: {replState_stats_numIndexesBefore}. Indexes in "
              "catalog after build: {replState_stats_numIndexesAfter}",
              "replState_buildUUID"_attr = replState->buildUUID,
              "nss"_attr = nss,
              "replState_collectionUUID"_attr = replState->collectionUUID,
              "replState_indexSpecs_size"_attr = replState->indexSpecs.size(),
              "replState_stats_numIndexesBefore"_attr = replState->stats.numIndexesBefore,
              "replState_stats_numIndexesAfter"_attr = replState->stats.numIndexesAfter);
        return;
    }

    logFailure(status, nss, replState);

    if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
        _cleanUpSinglePhaseAfterFailure(opCtx, collection, replState, indexBuildOptions, status);
    } else {
        invariant(IndexBuildProtocol::kTwoPhase == replState->protocol,
                  str::stream() << replState->buildUUID);
        _cleanUpTwoPhaseAfterFailure(opCtx, collection, replState, indexBuildOptions, status);
    }

    // Any error that escapes at this point is not fatal and can be handled by the caller.
    uassertStatusOK(status);
}

void IndexBuildsCoordinator::_buildIndex(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    boost::optional<Lock::CollectionLock>* exclusiveCollectionLock) {

    if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
        _buildIndexSinglePhase(opCtx, replState, indexBuildOptions, exclusiveCollectionLock);
        return;
    }

    invariant(IndexBuildProtocol::kTwoPhase == replState->protocol,
              str::stream() << replState->buildUUID);
    _buildIndexTwoPhase(opCtx, replState, indexBuildOptions, exclusiveCollectionLock);
}

void IndexBuildsCoordinator::_buildIndexSinglePhase(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    boost::optional<Lock::CollectionLock>* exclusiveCollectionLock) {
    _scanCollectionAndInsertKeysIntoSorter(opCtx, replState, exclusiveCollectionLock);
    _insertKeysFromSideTablesWithoutBlockingWrites(opCtx, replState);
    _signalPrimaryForCommitReadiness(opCtx, replState);
    _waitForNextIndexBuildAction(opCtx, replState);
    _insertKeysFromSideTablesAndCommit(
        opCtx, replState, indexBuildOptions, exclusiveCollectionLock, {});
}

void IndexBuildsCoordinator::_buildIndexTwoPhase(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    boost::optional<Lock::CollectionLock>* exclusiveCollectionLock) {

    _scanCollectionAndInsertKeysIntoSorter(opCtx, replState, exclusiveCollectionLock);
    _insertKeysFromSideTablesWithoutBlockingWrites(opCtx, replState);

    _signalPrimaryForCommitReadiness(opCtx, replState);
    auto commitIndexBuildTimestamp = _waitForNextIndexBuildAction(opCtx, replState);

    _insertKeysFromSideTablesAndCommit(
        opCtx, replState, indexBuildOptions, exclusiveCollectionLock, commitIndexBuildTimestamp);
}

void IndexBuildsCoordinator::_scanCollectionAndInsertKeysIntoSorter(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    boost::optional<Lock::CollectionLock>* exclusiveCollectionLock) {

    {
        auto nss = CollectionCatalog::get(opCtx).lookupNSSByUUID(opCtx, replState->collectionUUID);
        invariant(nss);
        invariant(opCtx->lockState()->isDbLockedForMode(replState->dbName, MODE_IX));
        invariant(opCtx->lockState()->isCollectionLockedForMode(*nss, MODE_X));

        // Set up the thread's currentOp information to display createIndexes cmd information.
        updateCurOpOpDescription(opCtx, *nss, replState->indexSpecs);
    }

    // Rebuilding system indexes during startup using the IndexBuildsCoordinator is done by all
    // storage engines if they're missing.
    invariant(_indexBuildsManager.isBackgroundBuilding(replState->buildUUID));

    // Index builds can safely ignore prepare conflicts and perform writes. On secondaries, prepare
    // operations wait for index builds to complete.
    opCtx->recoveryUnit()->abandonSnapshot();
    opCtx->recoveryUnit()->setPrepareConflictBehavior(
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

    // Collection scan and insert into index, followed by a drain of writes received in the
    // background.
    exclusiveCollectionLock->reset();
    {
        const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_IS);

        // The collection object should always exist while an index build is registered.
        auto collection =
            CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, replState->collectionUUID);
        invariant(collection);

        uassertStatusOK(
            _indexBuildsManager.startBuildingIndex(opCtx, collection, replState->buildUUID));
    }

    if (MONGO_unlikely(hangAfterIndexBuildDumpsInsertsFromBulk.shouldFail())) {
        LOGV2(20665, "Hanging after dumping inserts from bulk builder");
        hangAfterIndexBuildDumpsInsertsFromBulk.pauseWhileSet();
    }
}

/**
 * Second phase is extracting the sorted keys and writing them into the new index table.
 */
void IndexBuildsCoordinator::_insertKeysFromSideTablesWithoutBlockingWrites(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) {
    // Perform the first drain while holding an intent lock.
    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
    {
        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_IS);

        uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(
            opCtx,
            replState->buildUUID,
            RecoveryUnit::ReadSource::kUnset,
            IndexBuildInterceptor::DrainYieldPolicy::kYield));
    }

    if (MONGO_unlikely(hangAfterIndexBuildFirstDrain.shouldFail())) {
        LOGV2(20666, "Hanging after index build first drain");
        hangAfterIndexBuildFirstDrain.pauseWhileSet();
    }

    // Perform the second drain while stopping writes on the collection.
    {
        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_S);

        uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(
            opCtx,
            replState->buildUUID,
            RecoveryUnit::ReadSource::kUnset,
            IndexBuildInterceptor::DrainYieldPolicy::kNoYield));
    }

    if (MONGO_unlikely(hangAfterIndexBuildSecondDrain.shouldFail())) {
        LOGV2(20667, "Hanging after index build second drain");
        hangAfterIndexBuildSecondDrain.pauseWhileSet();
    }
}

/**
 * Third phase is catching up on all the writes that occurred during the first two phases.
 * Accepts a commit timestamp for the index (null if not available).
 */
void IndexBuildsCoordinator::_insertKeysFromSideTablesAndCommit(
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    boost::optional<Lock::CollectionLock>* exclusiveCollectionLock,
    const Timestamp& commitIndexBuildTimestamp) {
    // Need to return the collection lock back to exclusive mode, to complete the index build.
    opCtx->recoveryUnit()->abandonSnapshot();
    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
    exclusiveCollectionLock->emplace(opCtx, dbAndUUID, MODE_X);

    // The collection object should always exist while an index build is registered.
    auto collection =
        CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, replState->collectionUUID);
    invariant(collection,
              str::stream() << "Collection not found after relocking. Index build: "
                            << replState->buildUUID
                            << ", collection UUID: " << replState->collectionUUID);

    {
        auto dss = DatabaseShardingState::get(opCtx, replState->dbName);
        auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, dss);
        dss->checkDbVersion(opCtx, dssLock);
    }

    // Perform the third and final drain after releasing a shared lock and reacquiring an
    // exclusive lock on the database.
    uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(
        opCtx,
        replState->buildUUID,
        RecoveryUnit::ReadSource::kUnset,
        IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

    // Retry indexing records that may have been skipped while relaxing constraints (i.e. as
    // secondary), but only if we are primary and committing the index build and during two-phase
    // builds. Single-phase index builds are not resilient to state transitions.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (IndexBuildProtocol::kTwoPhase == replState->protocol &&
        replCoord->canAcceptWritesFor(opCtx, collection->ns())) {
        uassertStatusOK(
            _indexBuildsManager.retrySkippedRecords(opCtx, replState->buildUUID, collection));
    }

    // Index constraint checking phase.
    uassertStatusOK(
        _indexBuildsManager.checkIndexConstraintViolations(opCtx, replState->buildUUID));

    // If two phase index builds is enabled, index build will be coordinated using
    // startIndexBuild and commitIndexBuild oplog entries.
    auto onCommitFn = [&] {
        if (IndexBuildProtocol::kTwoPhase != replState->protocol) {
            return;
        }

        onCommitIndexBuild(
            opCtx, collection->ns(), *replState, indexBuildOptions.replSetAndNotPrimaryAtStart);
    };

    auto onCreateEachFn = [&](const BSONObj& spec) {
        if (IndexBuildProtocol::kTwoPhase == replState->protocol) {
            return;
        }

        if (indexBuildOptions.replSetAndNotPrimaryAtStart) {
            LOGV2_DEBUG(20671,
                        1,
                        "Skipping createIndexes oplog entry for index build: {replState_buildUUID}",
                        "replState_buildUUID"_attr = replState->buildUUID);
            // Get a timestamp to complete the index build in the absence of a createIndexBuild
            // oplog entry.
            repl::UnreplicatedWritesBlock uwb(opCtx);
            if (!IndexTimestampHelper::setGhostCommitTimestampForCatalogWrite(opCtx,
                                                                              collection->ns())) {
                LOGV2(20672, "Did not timestamp index commit write.");
            }
            return;
        }

        auto opObserver = opCtx->getServiceContext()->getOpObserver();
        auto fromMigrate = false;
        opObserver->onCreateIndex(
            opCtx, collection->ns(), replState->collectionUUID, spec, fromMigrate);
    };

    // Commit index build.
    TimestampBlock tsBlock(opCtx, commitIndexBuildTimestamp);
    uassertStatusOK(_indexBuildsManager.commitIndexBuild(
        opCtx, collection, collection->ns(), replState->buildUUID, onCreateEachFn, onCommitFn));

    return;
}

StatusWith<std::pair<long long, long long>> IndexBuildsCoordinator::_runIndexRebuildForRecovery(
    OperationContext* opCtx,
    Collection* collection,
    const UUID& buildUUID,
    RepairData repair) noexcept {
    invariant(opCtx->lockState()->isCollectionLockedForMode(collection->ns(), MODE_X));

    auto replState = invariant(_getIndexBuild(buildUUID));

    // We rely on 'collection' for any collection information because no databases are open during
    // recovery.
    NamespaceString nss = collection->ns();
    invariant(!nss.isEmpty());

    auto status = Status::OK();

    long long numRecords = 0;
    long long dataSize = 0;

    ReplIndexBuildState::IndexCatalogStats indexCatalogStats;
    indexCatalogStats.numIndexesBefore = getNumIndexesTotal(opCtx, collection);

    try {
        LOGV2(20673,
              "Index builds manager starting: {buildUUID}: {nss}",
              "buildUUID"_attr = buildUUID,
              "nss"_attr = nss);

        std::tie(numRecords, dataSize) =
            uassertStatusOK(_indexBuildsManager.startBuildingIndexForRecovery(
                opCtx, collection->ns(), buildUUID, repair));

        uassertStatusOK(
            _indexBuildsManager.checkIndexConstraintViolations(opCtx, replState->buildUUID));

        // Commit the index build.
        uassertStatusOK(_indexBuildsManager.commitIndexBuild(opCtx,
                                                             collection,
                                                             nss,
                                                             buildUUID,
                                                             MultiIndexBlock::kNoopOnCreateEachFn,
                                                             MultiIndexBlock::kNoopOnCommitFn));

        indexCatalogStats.numIndexesAfter = getNumIndexesTotal(opCtx, collection);

        LOGV2(20674,
              "Index builds manager completed successfully: {buildUUID}: {nss}. Index specs "
              "requested: {replState_indexSpecs_size}. Indexes in catalog before build: "
              "{indexCatalogStats_numIndexesBefore}. Indexes in catalog after build: "
              "{indexCatalogStats_numIndexesAfter}",
              "buildUUID"_attr = buildUUID,
              "nss"_attr = nss,
              "replState_indexSpecs_size"_attr = replState->indexSpecs.size(),
              "indexCatalogStats_numIndexesBefore"_attr = indexCatalogStats.numIndexesBefore,
              "indexCatalogStats_numIndexesAfter"_attr = indexCatalogStats.numIndexesAfter);
    } catch (const DBException& ex) {
        status = ex.toStatus();
        invariant(status != ErrorCodes::IndexAlreadyExists);
        LOGV2(20675,
              "Index builds manager failed: {buildUUID}: {nss}: {status}",
              "buildUUID"_attr = buildUUID,
              "nss"_attr = nss,
              "status"_attr = status);
    }

    // Index build is registered in manager regardless of IndexBuildsManager::setUpIndexBuild()
    // result.
    if (status.isOK()) {
        // A successful index build means that all the requested indexes are now part of the
        // catalog.
        _indexBuildsManager.tearDownIndexBuild(
            opCtx, collection, buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
    } else {
        // An index build failure during recovery is fatal.
        logFailure(status, nss, replState);
        fassertNoTrace(51076, status);
    }

    // 'numIndexesBefore' was before we cleared any unfinished indexes, so it must be the same
    // as 'numIndexesAfter', since we're going to be building any unfinished indexes too.
    invariant(indexCatalogStats.numIndexesBefore == indexCatalogStats.numIndexesAfter);

    {
        stdx::unique_lock<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replState);
    }

    if (status.isOK()) {
        return std::make_pair(numRecords, dataSize);
    }
    return status;
}

StatusWith<std::shared_ptr<ReplIndexBuildState>> IndexBuildsCoordinator::_getIndexBuild(
    const UUID& buildUUID) const {
    stdx::unique_lock<Latch> lk(_mutex);
    auto it = _allIndexBuilds.find(buildUUID);
    if (it == _allIndexBuilds.end()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "No index build with UUID: " << buildUUID};
    }
    return it->second;
}

std::vector<std::shared_ptr<ReplIndexBuildState>> IndexBuildsCoordinator::_getIndexBuilds() const {
    stdx::unique_lock<Latch> lk(_mutex);
    auto filter = [](const auto& replState) { return true; };
    return _filterIndexBuilds_inlock(lk, filter);
}

std::vector<std::shared_ptr<ReplIndexBuildState>> IndexBuildsCoordinator::_filterIndexBuilds_inlock(
    WithLock lk, IndexBuildFilterFn indexBuildFilter) const {
    std::vector<std::shared_ptr<ReplIndexBuildState>> indexBuilds;
    for (auto pair : _allIndexBuilds) {
        auto replState = pair.second;
        if (!indexBuildFilter(*replState)) {
            continue;
        }
        indexBuilds.push_back(replState);
    }
    return indexBuilds;
}

int IndexBuildsCoordinator::getNumIndexesTotal(OperationContext* opCtx, Collection* collection) {
    invariant(collection);
    const auto& nss = collection->ns();
    invariant(opCtx->lockState()->isLocked(),
              str::stream() << "Unable to get index count because collection was not locked"
                            << nss);

    auto indexCatalog = collection->getIndexCatalog();
    invariant(indexCatalog, str::stream() << "Collection is missing index catalog: " << nss);

    return indexCatalog->numIndexesTotal(opCtx);
}

std::vector<BSONObj> IndexBuildsCoordinator::prepareSpecListForCreate(
    OperationContext* opCtx,
    Collection* collection,
    const NamespaceString& nss,
    const std::vector<BSONObj>& indexSpecs) {
    UncommittedCollections::get(opCtx).invariantHasExclusiveAccessToCollection(opCtx,
                                                                               collection->ns());
    invariant(collection);

    // During secondary oplog application, the index specs have already been normalized in the
    // oplog entries read from the primary. We should not be modifying the specs any further.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getSettings().usingReplSets() && !replCoord->canAcceptWritesFor(opCtx, nss)) {
        return indexSpecs;
    }

    auto specsWithCollationDefaults =
        uassertStatusOK(collection->addCollationDefaultsToIndexSpecsForCreate(opCtx, indexSpecs));

    auto indexCatalog = collection->getIndexCatalog();
    std::vector<BSONObj> resultSpecs;

    resultSpecs = indexCatalog->removeExistingIndexes(
        opCtx, specsWithCollationDefaults, true /*removeIndexBuildsToo*/);

    for (const BSONObj& spec : resultSpecs) {
        if (spec[kUniqueFieldName].trueValue()) {
            checkShardKeyRestrictions(opCtx, nss, spec[kKeyFieldName].Obj());
        }
    }

    return resultSpecs;
}

}  // namespace mongo
