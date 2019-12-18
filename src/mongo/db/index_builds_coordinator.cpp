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
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {

using namespace indexbuildentryhelpers;

MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildFirstDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildSecondDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildDumpsInsertsFromBulk);

namespace {

constexpr StringData kCreateIndexesFieldName = "createIndexes"_sd;
constexpr StringData kIndexesFieldName = "indexes"_sd;
constexpr StringData kKeyFieldName = "key"_sd;
constexpr StringData kUniqueFieldName = "unique"_sd;

/**
 * Checks if unique index specification is compatible with sharding configuration.
 */
void checkShardKeyRestrictions(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& newIdxKey) {
    invariant(UncommittedCollections::get(opCtx).hasExclusiveAccessToCollection(opCtx, nss));

    const auto metadata = CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();
    if (!metadata->isSharded())
        return;

    const ShardKeyPattern shardKeyPattern(metadata->getKeyPattern());
    uassert(ErrorCodes::CannotCreateIndex,
            str::stream() << "cannot create unique index over " << newIdxKey
                          << " with shard key pattern " << shardKeyPattern.toBSON(),
            shardKeyPattern.isUniqueIndexCompatible(newIdxKey));
}

/**
 * Returns true if we should wait for a commitIndexBuild or abortIndexBuild oplog entry during oplog
 * application.
 */
bool shouldWaitForCommitOrAbort(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const ReplIndexBuildState& replState) {
    if (IndexBuildProtocol::kTwoPhase != replState.protocol) {
        return false;
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (!replCoord->getSettings().usingReplSets()) {
        return false;
    }

    if (replCoord->canAcceptWritesFor(opCtx, nss)) {
        return false;
    }

    return true;
}

/**
 * Signal downstream secondary nodes to commit index build.
 */
void onCommitIndexBuild(OperationContext* opCtx,
                        const NamespaceString& nss,
                        const ReplIndexBuildState& replState,
                        bool replSetAndNotPrimaryAtStart) {
    if (!serverGlobalParams.featureCompatibility.isVersionInitialized()) {
        return;
    }

    if (serverGlobalParams.featureCompatibility.getVersion() !=
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) {
        return;
    }

    const auto& buildUUID = replState.buildUUID;

    invariant(opCtx->lockState()->isWriteLocked(),
              str::stream() << "onCommitIndexBuild: " << buildUUID);

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    const auto& collUUID = replState.collectionUUID;
    const auto& indexSpecs = replState.indexSpecs;
    auto fromMigrate = false;

    if (IndexBuildProtocol::kTwoPhase != replState.protocol) {
        // Do not expect replication state to change during committing index build when two phase
        // index builds are not in effect because the index build would be aborted (most likely due
        // to a stepdown) before we reach here.
        if (replSetAndNotPrimaryAtStart) {
            // Get a timestamp to complete the index build in the absence of a commitIndexBuild
            // oplog entry.
            repl::UnreplicatedWritesBlock uwb(opCtx);
            if (!IndexTimestampHelper::setGhostCommitTimestampForCatalogWrite(opCtx, nss)) {
                log() << "Did not timestamp index commit write.";
            }
            return;
        }
        opObserver->onCommitIndexBuild(opCtx, nss, collUUID, buildUUID, indexSpecs, fromMigrate);
        return;
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
                       const ReplIndexBuildState& replState,
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
    opObserver->onAbortIndexBuild(
        opCtx, nss, collUUID, replState.buildUUID, replState.indexSpecs, cause, fromMigrate);
}

/**
 * Aborts the index build identified by the provided 'replIndexBuildState'.
 *
 * Sets a signal on the coordinator's repl index build state if the builder does not yet exist in
 * the manager.
 */
void abortIndexBuild(WithLock lk,
                     IndexBuildsManager* indexBuildsManager,
                     std::shared_ptr<ReplIndexBuildState> replIndexBuildState,
                     const std::string& reason) {
    bool res = indexBuildsManager->abortIndexBuild(replIndexBuildState->buildUUID, reason);
    if (res) {
        return;
    }
    // The index builder was not found in the manager, so it only exists in the coordinator. In this
    // case, set the abort signal on the coordinator index build state.
    replIndexBuildState->aborted = true;
    replIndexBuildState->abortReason = reason;
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
    log() << "Index build failed: " << replState->buildUUID << ": " << nss << " ( "
          << replState->collectionUUID << " ): " << status;
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

    log() << logPrefix << "active index builds: " << indexBuilds.size();

    for (auto replState : indexBuilds) {
        std::string indexNamesStr;
        str::joinStringDelim(replState->indexNames, &indexNamesStr, ',');
        log() << logPrefix << replState->buildUUID << ": collection: " << replState->collectionUUID
              << "; indexes: " << replState->indexNames.size() << " [" << indexNamesStr << "]";

        onIndexBuild(replState);
    }
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

IndexBuildsCoordinator* IndexBuildsCoordinator::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

IndexBuildsCoordinator::~IndexBuildsCoordinator() {
    invariant(_databaseIndexBuilds.empty());
    invariant(_disallowedDbs.empty());
    invariant(_disallowedCollections.empty());
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
    const UUID& buildUUID) {

    const auto protocol = IndexBuildProtocol::kSinglePhase;
    auto status = _startIndexBuildForRecovery(opCtx, nss, specs, buildUUID, protocol);
    if (!status.isOK()) {
        return status;
    }

    auto& collectionCatalog = CollectionCatalog::get(getGlobalServiceContext());
    Collection* collection = collectionCatalog.lookupCollectionByNamespace(opCtx, nss);

    // Complete the index build.
    return _runIndexRebuildForRecovery(opCtx, collection, buildUUID);
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
                log() << "The index for build " << buildUUID
                      << " was not found while trying to drop the index during recovery: "
                      << indexNames[i];
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

void IndexBuildsCoordinator::abortCollectionIndexBuilds(const UUID& collectionUUID,
                                                        const std::string& reason) {
    stdx::unique_lock<Latch> lk(_mutex);

    // Ensure the caller correctly stopped any new index builds on the collection.
    auto it = _disallowedCollections.find(collectionUUID);
    invariant(it != _disallowedCollections.end());

    auto collIndexBuildsIt = _collectionIndexBuilds.find(collectionUUID);
    if (collIndexBuildsIt == _collectionIndexBuilds.end()) {
        return;
    }

    collIndexBuildsIt->second->runOperationOnAllBuilds(
        lk, &_indexBuildsManager, abortIndexBuild, reason);
    // Take a shared ptr, rather than accessing the Tracker through the map's iterator, so that the
    // object does not destruct while we are waiting, causing a use-after-free memory error.
    auto collIndexBuildsSharedPtr = collIndexBuildsIt->second;
    collIndexBuildsSharedPtr->waitUntilNoIndexBuildsRemain(lk);
}

void IndexBuildsCoordinator::abortDatabaseIndexBuilds(StringData db, const std::string& reason) {
    stdx::unique_lock<Latch> lk(_mutex);

    // Ensure the caller correctly stopped any new index builds on the database.
    auto it = _disallowedDbs.find(db);
    invariant(it != _disallowedDbs.end());

    auto dbIndexBuilds = _databaseIndexBuilds[db];
    if (!dbIndexBuilds) {
        return;
    }

    dbIndexBuilds->runOperationOnAllBuilds(lk, &_indexBuildsManager, abortIndexBuild, reason);

    // 'dbIndexBuilds' is a shared ptr, so it can be safely waited upon without destructing before
    // waitUntilNoIndexBuildsRemain() returns, which would cause a use-after-free memory error.
    dbIndexBuilds->waitUntilNoIndexBuildsRemain(lk);
}

void IndexBuildsCoordinator::signalCommitAndWait(OperationContext* opCtx, const UUID& buildUUID) {
    auto replState = uassertStatusOK(_getIndexBuild(buildUUID));

    {
        stdx::unique_lock<Latch> lk(replState->mutex);
        replState->isCommitReady = true;
        replState->commitTimestamp = opCtx->recoveryUnit()->getCommitTimestamp();
        invariant(!replState->commitTimestamp.isNull(), buildUUID.toString());
        replState->condVar.notify_all();
    }
    auto fut = replState->sharedPromise.getFuture();
    log() << "Index build joined after commit: " << buildUUID << ": " << fut.waitNoThrow(opCtx);

    // Throws if there was an error building the index.
    fut.get();
}

void IndexBuildsCoordinator::signalAbortAndWait(OperationContext* opCtx,
                                                const UUID& buildUUID,
                                                const std::string& reason) noexcept {
    abortIndexBuildByBuildUUID(opCtx, buildUUID, reason);

    // Because we replicate abort oplog entries for single-phase builds, it is possible to receive
    // an abort for a non-existent index build. Abort should always succeed, so suppress the error.
    auto replStateResult = _getIndexBuild(buildUUID);
    if (!replStateResult.isOK()) {
        log() << "ignoring error while aborting index build " << buildUUID << ": "
              << replStateResult.getStatus();
        return;
    }
    auto replState = replStateResult.getValue();
    auto fut = replState->sharedPromise.getFuture();
    log() << "Index build joined after abort: " << buildUUID << ": " << fut.waitNoThrow(opCtx);
}

void IndexBuildsCoordinator::abortIndexBuildByBuildUUID(OperationContext* opCtx,
                                                        const UUID& buildUUID,
                                                        const std::string& reason) {
    _indexBuildsManager.abortIndexBuild(buildUUID, reason);

    auto replStateResult = _getIndexBuild(buildUUID);
    if (replStateResult.isOK()) {
        auto replState = replStateResult.getValue();

        stdx::unique_lock<Latch> lk(replState->mutex);
        replState->aborted = true;
        replState->abortTimestamp = opCtx->recoveryUnit()->getCommitTimestamp();
        replState->abortReason = reason;
        replState->condVar.notify_all();
    }
}

/**
 * Returns true if index specs include any unique indexes. Due to uniqueness constraints set up at
 * the start of the index build, we are not able to support failing over a two phase index build on
 * a unique index to a new primary on stepdown.
 */
namespace {
// TODO(SERVER-44654): remove when unique indexes support failover
bool containsUniqueIndexes(const std::vector<BSONObj>& specs) {
    for (const auto& spec : specs) {
        if (spec["unique"].trueValue()) {
            return true;
        }
    }
    return false;
}
}  // namespace

void IndexBuildsCoordinator::onStepUp(OperationContext* opCtx) {
    log() << "IndexBuildsCoordinator::onStepUp - this node is stepping up to primary";

    auto indexBuilds = _getIndexBuilds();
    auto onIndexBuild = [this, opCtx](std::shared_ptr<ReplIndexBuildState> replState) {
        // TODO(SERVER-44654): re-enable failover support for unique indexes.
        if (containsUniqueIndexes(replState->indexSpecs)) {
            // We abort unique index builds on step-up on the new primary, as opposed to on
            // step-down on the old primary. This is because the old primary cannot generate any new
            // oplog entries, and consequently does not have a timestamp to delete the index from
            // the durable catalog. This abort will replicate to the old primary, now secondary, to
            // abort the build.
            abortIndexBuildByBuildUUID(
                opCtx, replState->buildUUID, "unique indexes do not support failover");
            return;
        }

        stdx::unique_lock<Latch> lk(replState->mutex);
        if (!replState->aborted) {
            // Leave commit timestamp as null. We will be writing a commitIndexBuild oplog entry now
            // that we are primary and using the timestamp from the oplog entry to update the mdb
            // catalog.
            invariant(replState->commitTimestamp.isNull(), replState->buildUUID.toString());
            invariant(!replState->isCommitReady, replState->buildUUID.toString());
            replState->isCommitReady = true;
            replState->condVar.notify_all();
        }
    };
    forEachIndexBuild(indexBuilds, "IndexBuildsCoordinator::onStepUp - "_sd, onIndexBuild);
}

IndexBuilds IndexBuildsCoordinator::onRollback(OperationContext* opCtx) {
    log() << "IndexBuildsCoordinator::onRollback - this node is entering the rollback state";

    IndexBuilds buildsAborted;

    auto indexBuilds = _getIndexBuilds();
    auto onIndexBuild = [this, &buildsAborted](std::shared_ptr<ReplIndexBuildState> replState) {
        const std::string reason = "rollback";
        _indexBuildsManager.abortIndexBuild(replState->buildUUID, reason);

        stdx::unique_lock<Latch> lk(replState->mutex);
        if (!replState->aborted) {

            IndexBuildDetails aborted{replState->collectionUUID};
            // Record the index builds aborted due to rollback. This allows any rollback algorithm
            // to efficiently restart all unfinished index builds without having to scan all indexes
            // in all collections.
            for (auto spec : replState->indexSpecs) {
                aborted.indexSpecs.emplace_back(spec.getOwned());
            }
            buildsAborted.insert({replState->buildUUID, aborted});

            // Leave abort timestamp as null. This will unblock the index build and allow it to
            // complete using a ghost timestamp. Subsequently, the rollback algorithm can decide how
            // to undo the index build depending on the state of the oplog.
            invariant(replState->abortTimestamp.isNull(), replState->buildUUID.toString());
            invariant(!replState->aborted, replState->buildUUID.toString());
            replState->aborted = true;
            replState->abortReason = reason;
            replState->condVar.notify_all();
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

        log() << "Restarting index build for collection: " << *nss
              << ", collection UUID: " << build.collUUID << ", index build UUID: " << buildUUID;

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
        opObserver->onStartIndexBuild(opCtx, nss, collectionUUID, buildUUID, specs, fromMigrate);
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

    auto nss = collection->ns();
    invariant(
        UncommittedCollections::get(opCtx).hasExclusiveAccessToCollection(opCtx, collection->ns()));

    // Emit startIndexBuild and commitIndexBuild oplog entries if supported by the current FCV.
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    auto buildUUID = serverGlobalParams.featureCompatibility.isVersionInitialized() &&
            serverGlobalParams.featureCompatibility.getVersion() ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44
        ? boost::make_optional(UUID::gen())
        : boost::none;

    if (buildUUID) {
        opObserver->onStartIndexBuild(opCtx, nss, collectionUUID, *buildUUID, specs, fromMigrate);
    }

    // If two phase index builds are enabled, the index build will be coordinated using
    // startIndexBuild and commitIndexBuild oplog entries.
    auto indexCatalog = collection->getIndexCatalog();
    if (supportsTwoPhaseIndexBuild()) {
        invariant(buildUUID, str::stream() << collectionUUID << ": " << nss);

        // All indexes will be added to the mdb catalog using the commitIndexBuild timestamp.
        opObserver->onCommitIndexBuild(opCtx, nss, collectionUUID, *buildUUID, specs, fromMigrate);
        for (const auto& spec : specs) {
            uassertStatusOK(indexCatalog->createIndexOnEmptyCollection(opCtx, spec));
        }
    } else {
        for (const auto& spec : specs) {
            // Each index will be added to the mdb catalog using the preceding createIndexes
            // timestamp.
            opObserver->onCreateIndex(opCtx, nss, collectionUUID, spec, fromMigrate);
            uassertStatusOK(indexCatalog->createIndexOnEmptyCollection(opCtx, spec));
        }
        if (buildUUID) {
            opObserver->onCommitIndexBuild(
                opCtx, nss, collectionUUID, *buildUUID, specs, fromMigrate);
        }
    }
}

void IndexBuildsCoordinator::sleepIndexBuilds_forTestOnly(bool sleep) {
    stdx::unique_lock<Latch> lk(_mutex);
    _sleepForTest = sleep;
}

void IndexBuildsCoordinator::verifyNoIndexBuilds_forTestOnly() {
    invariant(_databaseIndexBuilds.empty());
    invariant(_disallowedDbs.empty());
    invariant(_disallowedCollections.empty());
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

    auto itns = _disallowedCollections.find(replIndexBuildState->collectionUUID);
    auto itdb = _disallowedDbs.find(replIndexBuildState->dbName);
    if (itns != _disallowedCollections.end() || itdb != _disallowedDbs.end()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "Collection ( " << replIndexBuildState->collectionUUID
                                    << " ) is in the process of being dropped. New index builds "
                                       "are not currently allowed.");
    }

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
                    if (existingIndexBuild->isCommitReady) {
                        ss << " (ready to commit with timestamp: "
                           << existingIndexBuild->commitTimestamp.toString() << ")";
                    } else if (existingIndexBuild->aborted) {
                        ss << " (aborted with reason: " << existingIndexBuild->abortReason
                           << " and timestamp: " << existingIndexBuild->abortTimestamp.toString()
                           << ")";
                        aborted = true;
                    } else {
                        ss << " (in-progress)";
                    }
                }
                std::string msg = ss;
                log() << msg;
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
    auto status = CollectionShardingState::get(opCtx, nss)->checkShardVersionNoThrow(opCtx, true);
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

Status IndexBuildsCoordinator::_setUpIndexBuild(OperationContext* opCtx,
                                                StringData dbName,
                                                CollectionUUID collectionUUID,
                                                const UUID& buildUUID,
                                                Timestamp startTimestamp) {
    auto replIndexBuildState = invariant(_getIndexBuild(buildUUID));

    NamespaceStringOrUUID nssOrUuid{dbName.toString(), collectionUUID};
    AutoGetCollection autoColl(opCtx, nssOrUuid, MODE_X);
    auto collection = autoColl.getCollection();
    const auto& nss = collection->ns();
    auto status = CollectionShardingState::get(opCtx, nss)->checkShardVersionNoThrow(opCtx, true);
    if (!status.isOK()) {
        // We need to unregister the index build to allow retries to succeed.
        stdx::unique_lock<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replIndexBuildState);

        return status;
    }

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    const bool replSetAndNotPrimary =
        replCoord->getSettings().usingReplSets() && !replCoord->canAcceptWritesFor(opCtx, nss);

    // We will not have a start timestamp if we are newly a secondary (i.e. we started as
    // primary but there was a stepdown). We will be unable to timestamp the initial catalog write,
    // so we must fail the index build.
    if (replSetAndNotPrimary && startTimestamp.isNull()) {
        stdx::unique_lock<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replIndexBuildState);

        return Status{ErrorCodes::NotMaster,
                      str::stream()
                          << "Replication state changed while setting up the index build: "
                          << replIndexBuildState->buildUUID};
    }

    MultiIndexBlock::OnInitFn onInitFn;
    if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        serverGlobalParams.featureCompatibility.getVersion() ==
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) {

        // Two-phase index builds write a different oplog entry than the default behavior which
        // writes a no-op just to generate an optime.
        onInitFn = [&](std::vector<BSONObj>& specs) {
            opCtx->getServiceContext()->getOpObserver()->onStartIndexBuild(
                opCtx,
                nss,
                replIndexBuildState->collectionUUID,
                replIndexBuildState->buildUUID,
                replIndexBuildState->indexSpecs,
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
    options.protocol = replIndexBuildState->protocol;

    status = [&] {
        if (!replSetAndNotPrimary) {
            // On standalones and primaries, call setUpIndexBuild(), which makes the initial catalog
            // write. On primaries, this replicates the startIndexBuild oplog entry.
            return _indexBuildsManager.setUpIndexBuild(opCtx,
                                                       collection,
                                                       replIndexBuildState->indexSpecs,
                                                       replIndexBuildState->buildUUID,
                                                       onInitFn,
                                                       options);
        }
        // If we are starting the index build as a secondary, we must suppress calls to write
        // our initial oplog entry in setUpIndexBuild().
        repl::UnreplicatedWritesBlock uwb(opCtx);

        // Use the provided timestamp to write the initial catalog entry.
        invariant(!startTimestamp.isNull());
        TimestampBlock tsBlock(opCtx, startTimestamp);
        return _indexBuildsManager.setUpIndexBuild(opCtx,
                                                   collection,
                                                   replIndexBuildState->indexSpecs,
                                                   replIndexBuildState->buildUUID,
                                                   onInitFn,
                                                   options);
    }();

    // The indexes are in the durable catalog in an unfinished state. Return an OK status so
    // that the caller can continue building the indexes by calling _runIndexBuild().
    if (status.isOK()) {
        return Status::OK();
    }

    _indexBuildsManager.tearDownIndexBuild(
        opCtx, collection, replIndexBuildState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);

    // Unregister the index build before setting the promise, so callers do not see the build again.
    {
        stdx::unique_lock<Latch> lk(_mutex);
        _unregisterIndexBuild(lk, replIndexBuildState);
    }

    if (status == ErrorCodes::IndexAlreadyExists ||
        ((status == ErrorCodes::IndexOptionsConflict ||
          status == ErrorCodes::IndexKeySpecsConflict) &&
         options.indexConstraints == IndexBuildsManager::IndexConstraints::kRelax)) {
        LOG(1) << "Ignoring indexing error: " << redact(status);

        // The requested index (specs) are already built or are being built. Return success
        // early (this is v4.0 behavior compatible).
        ReplIndexBuildState::IndexCatalogStats indexCatalogStats;
        int numIndexes = replIndexBuildState->stats.numIndexesBefore;
        indexCatalogStats.numIndexesBefore = numIndexes;
        indexCatalogStats.numIndexesAfter = numIndexes;
        replIndexBuildState->sharedPromise.emplaceValue(indexCatalogStats);
        return Status::OK();
    }
    return status;
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
            opCtx, replState->buildUUID, "shutting down");
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
        // Leave it as-if kill -9 happened. Startup recovery will restart the index build.
        _indexBuildsManager.abortIndexBuildWithoutCleanup(
            opCtx, replState->buildUUID, "shutting down");
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
            invariant(replState->aborted, replState->buildUUID.toString());
            Timestamp abortIndexBuildTimestamp = replState->abortTimestamp;

            // If we were aborted and no abort timestamp is set, then we should leave the index
            // build unfinished. This can happen during rollback because we are not primary and
            // cannot generate an optime to timestamp the index build abort. We rely on the
            // rollback process to correct this state.
            if (abortIndexBuildTimestamp.isNull()) {
                _indexBuildsManager.abortIndexBuildWithoutCleanup(
                    opCtx, replState->buildUUID, "no longer primary");
                _indexBuildsManager.tearDownIndexBuild(
                    opCtx, collection, replState->buildUUID, MultiIndexBlock::kNoopOnCleanUpFn);
                return;
            }

            // Unlock the RSTL to avoid deadlocks with state transitions. See SERVER-42824.
            unlockRSTLForIndexCleanup(opCtx);
            Lock::CollectionLock collLock(opCtx, nss, MODE_X);

            TimestampBlock tsBlock(opCtx, abortIndexBuildTimestamp);
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

        if (indexBuildOptions.replSetAndNotPrimaryAtStart) {
            // This index build can only be interrupted at shutdown. For the duration of the
            // OperationContext::runWithoutInterruptionExceptAtGlobalShutdown() invocation, any kill
            // status set by the killOp command will be ignored. After
            // OperationContext::runWithoutInterruptionExceptAtGlobalShutdown() returns, any call to
            // OperationContext::checkForInterrupt() will see the kill status and respond
            // accordingly (checkForInterrupt() will throw an exception while
            // checkForInterruptNoAssert() returns an error Status).

            // We need to drop the RSTL here, as we do not need synchronization with step up and
            // step down. Dropping the RSTL is important because otherwise if we held the RSTL it
            // would create deadlocks with prepared transactions on step up and step down.  A
            // deadlock could result if the index build was attempting to acquire a Collection S or
            // X lock while a prepared transaction held a Collection IX lock, and a step down was
            // waiting to acquire the RSTL in mode X.
            // TODO(SERVER-44045): Revisit this logic for the non-two phase index build case.
            if (!supportsTwoPhaseIndexBuild()) {
                const bool unlocked = opCtx->lockState()->unlockRSTLforPrepare();
                invariant(unlocked);
            }
            opCtx->runWithoutInterruptionExceptAtGlobalShutdown([&, this] {
                _buildIndex(opCtx, dbAndUUID, replState, indexBuildOptions, &collLock);
            });
        } else {
            _buildIndex(opCtx, dbAndUUID, replState, indexBuildOptions, &collLock);
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

        log() << "Index build completed successfully: " << replState->buildUUID << ": " << nss
              << " ( " << replState->collectionUUID
              << " ). Index specs built: " << replState->indexSpecs.size()
              << ". Indexes in catalog before build: " << replState->stats.numIndexesBefore
              << ". Indexes in catalog after build: " << replState->stats.numIndexesAfter;
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
    const NamespaceStringOrUUID& dbAndUUID,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    boost::optional<Lock::CollectionLock>* exclusiveCollectionLock) {

    if (IndexBuildProtocol::kSinglePhase == replState->protocol) {
        _buildIndexSinglePhase(
            opCtx, dbAndUUID, replState, indexBuildOptions, exclusiveCollectionLock);
        return;
    }

    invariant(IndexBuildProtocol::kTwoPhase == replState->protocol,
              str::stream() << replState->buildUUID);
    _buildIndexTwoPhase(opCtx, dbAndUUID, replState, indexBuildOptions, exclusiveCollectionLock);
}

void IndexBuildsCoordinator::_buildIndexSinglePhase(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& dbAndUUID,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    boost::optional<Lock::CollectionLock>* exclusiveCollectionLock) {
    _scanCollectionAndInsertKeysIntoSorter(opCtx, dbAndUUID, replState, exclusiveCollectionLock);
    _insertKeysFromSideTablesWithoutBlockingWrites(opCtx, dbAndUUID, replState);
    _insertKeysFromSideTablesAndCommit(
        opCtx, dbAndUUID, replState, indexBuildOptions, exclusiveCollectionLock, {});
}

void IndexBuildsCoordinator::_buildIndexTwoPhase(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& dbAndUUID,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    boost::optional<Lock::CollectionLock>* exclusiveCollectionLock) {

    auto nss = *CollectionCatalog::get(opCtx).lookupNSSByUUID(opCtx, replState->collectionUUID);
    auto preAbortStatus = Status::OK();
    try {
        _scanCollectionAndInsertKeysIntoSorter(
            opCtx, dbAndUUID, replState, exclusiveCollectionLock);
        nss = _insertKeysFromSideTablesWithoutBlockingWrites(opCtx, dbAndUUID, replState);
    } catch (DBException& ex) {
        // Locks may no longer be held when we are interrupted. We should return immediately and, in
        // the case of a primary index build, signal downstream nodes to abort via the
        // abortIndexBuild oplog entry. On secondaries, a server shutdown is the only way an index
        // build can be interrupted (InterruptedAtShutdown).
        if (ex.isA<ErrorCategory::Interruption>()) {
            throw;
        }
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        auto replSetAndNotPrimary =
            replCoord->getSettings().usingReplSets() && !replCoord->canAcceptWritesFor(opCtx, nss);
        if (!replSetAndNotPrimary) {
            throw;
        }
        log() << "Index build failed before final phase during oplog application. "
                 "Waiting for abort: "
              << replState->buildUUID << ": " << ex;
        preAbortStatus = ex.toStatus();
    }

    auto commitIndexBuildTimestamp = _waitForCommitOrAbort(opCtx, nss, replState, preAbortStatus);
    _insertKeysFromSideTablesAndCommit(opCtx,
                                       dbAndUUID,
                                       replState,
                                       indexBuildOptions,
                                       exclusiveCollectionLock,
                                       commitIndexBuildTimestamp);
}

void IndexBuildsCoordinator::_scanCollectionAndInsertKeysIntoSorter(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& dbAndUUID,
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
    // storage engines if they're missing. This includes the mobile storage engine which builds
    // its indexes in the foreground.
    invariant(_indexBuildsManager.isBackgroundBuilding(replState->buildUUID) ||
              storageGlobalParams.engine == "mobile");

    // Index builds can safely ignore prepare conflicts and perform writes. On secondaries, prepare
    // operations wait for index builds to complete.
    opCtx->recoveryUnit()->abandonSnapshot();
    opCtx->recoveryUnit()->setPrepareConflictBehavior(
        PrepareConflictBehavior::kIgnoreConflictsAllowWrites);

    // Collection scan and insert into index, followed by a drain of writes received in the
    // background.
    exclusiveCollectionLock->reset();
    {
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_IS);

        // The collection object should always exist while an index build is registered.
        auto collection =
            CollectionCatalog::get(opCtx).lookupCollectionByUUID(opCtx, replState->collectionUUID);
        invariant(collection);

        uassertStatusOK(
            _indexBuildsManager.startBuildingIndex(opCtx, collection, replState->buildUUID));
    }

    if (MONGO_unlikely(hangAfterIndexBuildDumpsInsertsFromBulk.shouldFail())) {
        log() << "Hanging after dumping inserts from bulk builder";
        hangAfterIndexBuildDumpsInsertsFromBulk.pauseWhileSet();
    }
}

/**
 * Second phase is extracting the sorted keys and writing them into the new index table.
 * Looks up collection namespace while holding locks.
 */
NamespaceString IndexBuildsCoordinator::_insertKeysFromSideTablesWithoutBlockingWrites(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& dbAndUUID,
    std::shared_ptr<ReplIndexBuildState> replState) {
    // Perform the first drain while holding an intent lock.
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
        log() << "Hanging after index build first drain";
        hangAfterIndexBuildFirstDrain.pauseWhileSet();
    }

    // Cache collection namespace for shouldWaitForCommitOrAbort().
    NamespaceString nss;

    // Perform the second drain while stopping writes on the collection.
    {
        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_S);

        uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(
            opCtx,
            replState->buildUUID,
            RecoveryUnit::ReadSource::kUnset,
            IndexBuildInterceptor::DrainYieldPolicy::kNoYield));

        nss = *CollectionCatalog::get(opCtx).lookupNSSByUUID(opCtx, replState->collectionUUID);
    }

    if (MONGO_unlikely(hangAfterIndexBuildSecondDrain.shouldFail())) {
        log() << "Hanging after index build second drain";
        hangAfterIndexBuildSecondDrain.pauseWhileSet();
    }

    return nss;
}

/**
 * Waits for commit or abort signal from primary.
 */
Timestamp IndexBuildsCoordinator::_waitForCommitOrAbort(
    OperationContext* opCtx,
    const NamespaceString& nss,
    std::shared_ptr<ReplIndexBuildState> replState,
    const Status& preAbortStatus) {
    Timestamp commitIndexBuildTimestamp;
    if (shouldWaitForCommitOrAbort(opCtx, nss, *replState)) {
        log() << "Index build waiting for commit or abort before completing final phase: "
              << replState->buildUUID;

        // Yield locks and storage engine resources before blocking.
        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::TempRelease release(opCtx->lockState());
        invariant(!opCtx->lockState()->isLocked(),
                  str::stream()
                      << "failed to yield locks for index build while waiting for commit or abort: "
                      << replState->buildUUID);

        stdx::unique_lock<Latch> lk(replState->mutex);
        auto isReadyToCommitOrAbort = [rs = replState] { return rs->isCommitReady || rs->aborted; };
        opCtx->waitForConditionOrInterrupt(replState->condVar, lk, isReadyToCommitOrAbort);

        if (replState->isCommitReady) {
            log() << "Committing index build: " << replState->buildUUID
                  << ", timestamp: " << replState->commitTimestamp
                  << ", collection UUID: " << replState->collectionUUID;
            commitIndexBuildTimestamp = replState->commitTimestamp;
            invariant(!replState->aborted, replState->buildUUID.toString());
            uassertStatusOK(preAbortStatus.withContext(
                str::stream() << "index build failed on this node but we received a "
                                 "commitIndexBuild oplog entry from the primary with timestamp: "
                              << replState->commitTimestamp.toString()));
        } else if (replState->aborted) {
            log() << "Aborting index build: " << replState->buildUUID
                  << ", timestamp: " << replState->abortTimestamp
                  << ", reason: " << replState->abortReason
                  << ", collection UUID: " << replState->collectionUUID
                  << ", local index error (if any): " << preAbortStatus;
            invariant(!replState->isCommitReady, replState->buildUUID.toString());
        }
    }
    return commitIndexBuildTimestamp;
}

/**
 * Third phase is catching up on all the writes that occurred during the first two phases.
 * Accepts a commit timestamp for the index (null if not available).
 */
void IndexBuildsCoordinator::_insertKeysFromSideTablesAndCommit(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& dbAndUUID,
    std::shared_ptr<ReplIndexBuildState> replState,
    const IndexBuildOptions& indexBuildOptions,
    boost::optional<Lock::CollectionLock>* exclusiveCollectionLock,
    const Timestamp& commitIndexBuildTimestamp) {
    // Need to return the collection lock back to exclusive mode, to complete the index build.
    opCtx->recoveryUnit()->abandonSnapshot();
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

    // Index constraint checking phase.
    uassertStatusOK(
        _indexBuildsManager.checkIndexConstraintViolations(opCtx, replState->buildUUID));

    // Generate both createIndexes and commitIndexBuild oplog entries.
    // Secondaries currently interpret commitIndexBuild commands as noops.
    auto onCommitFn = [&] {
        onCommitIndexBuild(
            opCtx, collection->ns(), *replState, indexBuildOptions.replSetAndNotPrimaryAtStart);
    };

    auto onCreateEachFn = [&](const BSONObj& spec) {
        // If two phase index builds is enabled, index build will be coordinated using
        // startIndexBuild and commitIndexBuild oplog entries.
        if (supportsTwoPhaseIndexBuild()) {
            return;
        }

        if (indexBuildOptions.replSetAndNotPrimaryAtStart) {
            LOG(1) << "Skipping createIndexes oplog entry for index build: "
                   << replState->buildUUID;
            // Get a timestamp to complete the index build in the absence of a createIndexBuild
            // oplog entry.
            repl::UnreplicatedWritesBlock uwb(opCtx);
            if (!IndexTimestampHelper::setGhostCommitTimestampForCatalogWrite(opCtx,
                                                                              collection->ns())) {
                log() << "Did not timestamp index commit write.";
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
    OperationContext* opCtx, Collection* collection, const UUID& buildUUID) noexcept {
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
        log() << "Index builds manager starting: " << buildUUID << ": " << nss;

        std::tie(numRecords, dataSize) = uassertStatusOK(
            _indexBuildsManager.startBuildingIndexForRecovery(opCtx, collection->ns(), buildUUID));

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

        log() << "Index builds manager completed successfully: " << buildUUID << ": " << nss
              << ". Index specs requested: " << replState->indexSpecs.size()
              << ". Indexes in catalog before build: " << indexCatalogStats.numIndexesBefore
              << ". Indexes in catalog after build: " << indexCatalogStats.numIndexesAfter;
    } catch (const DBException& ex) {
        status = ex.toStatus();
        invariant(status != ErrorCodes::IndexAlreadyExists);
        log() << "Index builds manager failed: " << buildUUID << ": " << nss << ": " << status;
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

void IndexBuildsCoordinator::_stopIndexBuildsOnDatabase(StringData dbName) {
    stdx::unique_lock<Latch> lk(_mutex);

    auto it = _disallowedDbs.find(dbName);
    if (it != _disallowedDbs.end()) {
        ++(it->second);
        return;
    }
    _disallowedDbs[dbName] = 1;
}

void IndexBuildsCoordinator::_stopIndexBuildsOnCollection(const UUID& collectionUUID) {
    stdx::unique_lock<Latch> lk(_mutex);

    auto it = _disallowedCollections.find(collectionUUID);
    if (it != _disallowedCollections.end()) {
        ++(it->second);
        return;
    }
    _disallowedCollections[collectionUUID] = 1;
}

void IndexBuildsCoordinator::_allowIndexBuildsOnDatabase(StringData dbName) {
    stdx::unique_lock<Latch> lk(_mutex);

    auto it = _disallowedDbs.find(dbName);
    invariant(it != _disallowedDbs.end());
    invariant(it->second);
    if (--(it->second) == 0) {
        _disallowedDbs.erase(it);
    }
}

void IndexBuildsCoordinator::_allowIndexBuildsOnCollection(const UUID& collectionUUID) {
    stdx::unique_lock<Latch> lk(_mutex);

    auto it = _disallowedCollections.find(collectionUUID);
    invariant(it != _disallowedCollections.end());
    invariant(it->second > 0);
    if (--(it->second) == 0) {
        _disallowedCollections.erase(it);
    }
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
    std::vector<std::shared_ptr<ReplIndexBuildState>> indexBuilds;
    {
        stdx::unique_lock<Latch> lk(_mutex);
        for (auto pair : _allIndexBuilds) {
            indexBuilds.push_back(pair.second);
        }
    }
    return indexBuilds;
}

ScopedStopNewDatabaseIndexBuilds::ScopedStopNewDatabaseIndexBuilds(
    IndexBuildsCoordinator* indexBuildsCoordinator, StringData dbName)
    : _indexBuildsCoordinatorPtr(indexBuildsCoordinator), _dbName(dbName.toString()) {
    _indexBuildsCoordinatorPtr->_stopIndexBuildsOnDatabase(_dbName);
}

ScopedStopNewDatabaseIndexBuilds::~ScopedStopNewDatabaseIndexBuilds() {
    _indexBuildsCoordinatorPtr->_allowIndexBuildsOnDatabase(_dbName);
}

ScopedStopNewCollectionIndexBuilds::ScopedStopNewCollectionIndexBuilds(
    IndexBuildsCoordinator* indexBuildsCoordinator, const UUID& collectionUUID)
    : _indexBuildsCoordinatorPtr(indexBuildsCoordinator), _collectionUUID(collectionUUID) {
    _indexBuildsCoordinatorPtr->_stopIndexBuildsOnCollection(_collectionUUID);
}

ScopedStopNewCollectionIndexBuilds::~ScopedStopNewCollectionIndexBuilds() {
    _indexBuildsCoordinatorPtr->_allowIndexBuildsOnCollection(_collectionUUID);
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
    invariant(
        UncommittedCollections::get(opCtx).hasExclusiveAccessToCollection(opCtx, collection->ns()));
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
