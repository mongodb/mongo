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
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/enable_two_phase_index_build_gen.h"
#include "mongo/db/index_build_entry_helpers.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
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
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));

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
 * Logs the index build failure error in a standard format.
 */
void logFailure(Status status,
                const NamespaceString& nss,
                std::shared_ptr<ReplIndexBuildState> replState) {
    log() << "Index build failed: " << replState->buildUUID << ": " << nss << " ( "
          << replState->collectionUUID << " ): " << status;
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

bool IndexBuildsCoordinator::supportsTwoPhaseIndexBuild() const {
    if (!enableTwoPhaseIndexBuild) {
        return false;
    }

    if (!serverGlobalParams.featureCompatibility.isVersionInitialized()) {
        return false;
    }

    if (serverGlobalParams.featureCompatibility.getVersion() !=
        ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) {
        return false;
    }

    return true;
}

StatusWith<std::pair<long long, long long>> IndexBuildsCoordinator::startIndexRebuildForRecovery(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<BSONObj>& specs,
    const UUID& buildUUID) {
    // Index builds in recovery mode have the global write lock.
    invariant(opCtx->lockState()->isW());

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

    ReplIndexBuildState::IndexCatalogStats indexCatalogStats;

    auto& collectionCatalog = CollectionCatalog::get(getGlobalServiceContext());
    Collection* collection = collectionCatalog.lookupCollectionByNamespace(nss);
    auto indexCatalog = collection->getIndexCatalog();
    {
        // These steps are combined into a single WUOW to ensure there are no commits without
        // the indexes.
        // 1) Drop all indexes.
        // 2) Re-create the Collection.
        // 3) Start the index build process.
        WriteUnitOfWork wuow(opCtx);

        // 1
        {
            for (size_t i = 0; i < indexNames.size(); i++) {
                auto descriptor = indexCatalog->findIndexByName(opCtx, indexNames[i], false);
                if (!descriptor) {
                    // If it's unfinished index, drop it directly via removeIndex.
                    Status status =
                        DurableCatalog::get(opCtx)->removeIndex(opCtx, nss, indexNames[i]);
                    continue;
                }
                Status s = indexCatalog->dropIndex(opCtx, descriptor);
                if (!s.isOK()) {
                    return s;
                }
            }
        }

        // We need to initialize the collection to drop and rebuild the indexes.
        collection->init(opCtx);

        // Register the index build. During recovery, collections may not have UUIDs present yet to
        // due upgrading. We don't require collection UUIDs during recovery except to create a
        // ReplIndexBuildState object.
        auto collectionUUID = UUID::gen();
        auto dbName = nss.db().toString();

        // We run the index build using the single phase protocol as we already hold the global
        // write lock.
        auto replIndexBuildState =
            std::make_shared<ReplIndexBuildState>(buildUUID,
                                                  collectionUUID,
                                                  dbName,
                                                  specs,
                                                  IndexBuildProtocol::kSinglePhase,
                                                  /*commitQuorum=*/boost::none);

        Status status = [&]() {
            stdx::unique_lock<stdx::mutex> lk(_mutex);
            return _registerIndexBuild(lk, replIndexBuildState);
        }();
        if (!status.isOK()) {
            return status;
        }

        // Setup the index build.
        indexCatalogStats.numIndexesBefore =
            _getNumIndexesTotal(opCtx, collection) + indexNames.size();

        IndexBuildsManager::SetupOptions options;
        options.forRecovery = true;
        status = _indexBuildsManager.setUpIndexBuild(
            opCtx, collection, specs, buildUUID, MultiIndexBlock::kNoopOnInitFn, options);
        if (!status.isOK()) {
            // An index build failure during recovery is fatal.
            logFailure(status, nss, replIndexBuildState);
            fassertNoTrace(51086, status);
        }

        wuow.commit();
    }

    return _runIndexRebuildForRecovery(opCtx, collection, indexCatalogStats, buildUUID);
}

Future<void> IndexBuildsCoordinator::joinIndexBuilds(const NamespaceString& nss,
                                                     const std::vector<BSONObj>& indexSpecs) {
    // TODO: implement. This code is just to make it compile.
    auto pf = makePromiseFuture<void>();
    auto promise = std::move(pf.promise);
    return std::move(pf.future);
}

void IndexBuildsCoordinator::waitForAllIndexBuildsToStopForShutdown() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

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
    stdx::unique_lock<stdx::mutex> lk(_mutex);

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
    stdx::unique_lock<stdx::mutex> lk(_mutex);

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

Future<void> IndexBuildsCoordinator::abortIndexBuildByBuildUUID(const UUID& buildUUID,
                                                                const std::string& reason) {
    _indexBuildsManager.abortIndexBuild(buildUUID, reason);
    auto pf = makePromiseFuture<void>();
    auto promise = std::move(pf.promise);
    promise.setWith([] {});
    return std::move(pf.future);
}

void IndexBuildsCoordinator::recoverIndexBuilds() {
    // TODO: not yet implemented.
}

int IndexBuildsCoordinator::numInProgForDb(StringData db) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto dbIndexBuildsIt = _databaseIndexBuilds.find(db);
    if (dbIndexBuildsIt == _databaseIndexBuilds.end()) {
        return 0;
    }
    return dbIndexBuildsIt->second->getNumberOfIndexBuilds(lk);
}

void IndexBuildsCoordinator::dump(std::ostream& ss) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

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
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _collectionIndexBuilds.find(collectionUUID) != _collectionIndexBuilds.end();
}

bool IndexBuildsCoordinator::inProgForDb(StringData db) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    return _databaseIndexBuilds.find(db) != _databaseIndexBuilds.end();
}

void IndexBuildsCoordinator::assertNoIndexBuildInProgress() const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
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
    stdx::unique_lock<stdx::mutex> lk(_mutex);

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
    stdx::unique_lock<stdx::mutex> lk(_mutex);

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

void IndexBuildsCoordinator::sleepIndexBuilds_forTestOnly(bool sleep) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _sleepForTest = sleep;
}

void IndexBuildsCoordinator::verifyNoIndexBuilds_forTestOnly() {
    invariant(_databaseIndexBuilds.empty());
    invariant(_disallowedDbs.empty());
    invariant(_disallowedCollections.empty());
    invariant(_collectionIndexBuilds.empty());
}

void IndexBuildsCoordinator::_updateCurOpOpDescription(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<BSONObj>& indexSpecs) const {
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
                auto registeredIndexBuilds =
                    collIndexBuildsIt->second->getIndexBuildState(lk, name);
                return Status(ErrorCodes::IndexBuildAlreadyInProgress,
                              str::stream()
                                  << "There's already an index with name '" << name
                                  << "' being built on the collection: "
                                  << " ( " << replIndexBuildState->collectionUUID
                                  << " ). Index build: " << registeredIndexBuilds->buildUUID);
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

StatusWith<boost::optional<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>>
IndexBuildsCoordinator::_registerAndSetUpIndexBuild(
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

    // TODO (SERVER-40807): disabling the following code for the v4.2 release so it does not have
    // downstream impact.
    /*
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->canAcceptWritesFor(opCtx, nss)) {
        // TODO: Put in a well-defined initialization function within the coordinator.
        ensureIndexBuildEntriesNamespaceExists(opCtx);
    }
    */

    // Lock from when we ascertain what indexes to build through to when the build is registered
    // on the Coordinator and persistedly set up in the catalog. This serializes setting up an
    // index build so that no attempts are made to register the same build twice.
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    std::vector<BSONObj> filteredSpecs;
    try {
        filteredSpecs = _addDefaultsAndFilterExistingIndexes(opCtx, collection, nss, specs);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    if (filteredSpecs.size() == 0) {
        // The requested index (specs) are already built or are being built. Return success
        // early (this is v4.0 behavior compatible).
        ReplIndexBuildState::IndexCatalogStats indexCatalogStats;
        int numIndexes = _getNumIndexesTotal(opCtx, collection);
        indexCatalogStats.numIndexesBefore = numIndexes;
        indexCatalogStats.numIndexesAfter = numIndexes;
        return SharedSemiFuture(indexCatalogStats);
    }

    auto replIndexBuildState = std::make_shared<ReplIndexBuildState>(
        buildUUID, collectionUUID, dbName.toString(), filteredSpecs, protocol, commitQuorum);
    replIndexBuildState->stats.numIndexesBefore = _getNumIndexesTotal(opCtx, collection);

    Status status = _registerIndexBuild(lk, replIndexBuildState);
    if (!status.isOK()) {
        return status;
    }

    MultiIndexBlock::OnInitFn onInitFn;
    // Two-phase index builds write a different oplog entry than the default behavior which
    // writes a no-op just to generate an optime.
    if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
        serverGlobalParams.featureCompatibility.getVersion() ==
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) {
        onInitFn = [&](std::vector<BSONObj>& specs) {
            // TODO (SERVER-40807): disabling the following code for the v4.2 release so it does not
            // have downstream impact.
            /*
            // Only the primary node writes an index build entry to the collection as the
            // secondaries will replicate it.
            if (replCoord->canAcceptWritesFor(opCtx, nss)) {
                invariant(replIndexBuildState->commitQuorum);
                std::vector<std::string> indexNames;
                for (const auto& spec : specs) {
                    indexNames.push_back(spec.getStringField(IndexDescriptor::kIndexNameFieldName));
                }

                IndexBuildEntry entry(replIndexBuildState->buildUUID,
                                      *collection->uuid(),
                                      *replIndexBuildState->commitQuorum,
                                      indexNames);
                Status status = addIndexBuildEntry(opCtx, entry);
                if (!status.isOK()) {
                    return status;
                }
            }
            */

            opCtx->getServiceContext()->getOpObserver()->onStartIndexBuild(
                opCtx,
                nss,
                replIndexBuildState->collectionUUID,
                replIndexBuildState->buildUUID,
                filteredSpecs,
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
    status = _indexBuildsManager.setUpIndexBuild(
        opCtx, collection, filteredSpecs, replIndexBuildState->buildUUID, onInitFn, options);

    // Indexes are present in the catalog in an unfinished state. Return an uninitialized
    // Future so that the caller will continue building the indexes by calling _runIndexBuild().
    // The completion of the index build will be communicated via a Future obtained from
    // 'replIndexBuildState->sharedPromise'.
    if (status.isOK()) {
        return boost::none;
    }

    _indexBuildsManager.tearDownIndexBuild(opCtx, collection, replIndexBuildState->buildUUID);

    // Unregister the index build before setting the promise, so callers do not see the build again.
    _unregisterIndexBuild(lk, replIndexBuildState);

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
        return SharedSemiFuture(indexCatalogStats);
    }

    // Set the promise in case another thread already joined the index build.
    replIndexBuildState->sharedPromise.setError(status);

    return status;
}

void IndexBuildsCoordinator::_runIndexBuild(OperationContext* opCtx,
                                            const UUID& buildUUID,
                                            const IndexBuildOptions& indexBuildOptions) noexcept {
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        while (_sleepForTest) {
            lk.unlock();
            sleepmillis(100);
            lk.lock();
        }
    }

    auto replState = [&] {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        auto it = _allIndexBuilds.find(buildUUID);
        invariant(it != _allIndexBuilds.end());
        return it->second;
    }();

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

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    _unregisterIndexBuild(lk, replState);

    if (status.isOK()) {
        replState->sharedPromise.emplaceValue(replState->stats);
    } else {
        replState->sharedPromise.setError(status);
    }
}

void IndexBuildsCoordinator::_runIndexBuildInner(OperationContext* opCtx,
                                                 std::shared_ptr<ReplIndexBuildState> replState,
                                                 const IndexBuildOptions& indexBuildOptions) {
    const NamespaceStringOrUUID dbAndUUID(replState->dbName, replState->collectionUUID);
    // 'status' should always be set to something else before this function exits.
    Status status{ErrorCodes::InternalError,
                  "Uninitialized status value in IndexBuildsCoordinator"};

    // TODO(SERVER-39484): Since 'replSetAndNotPrimary' is derived from the replication state at the
    // start of the index build, this value is not resilient to member state changes like
    // stepup/stepdown.
    auto replSetAndNotPrimary = indexBuildOptions.replSetAndNotPrimary;

    try {
        // Lock acquisition might throw, and we would still need to clean up the index build state,
        // so do it in the try-catch block
        AutoGetDb autoDb(opCtx, replState->dbName, MODE_IX);

        // Do not use AutoGetCollection since the lock will be reacquired in various modes
        // throughout the index build. Lock by UUID to protect against concurrent collection rename.
        boost::optional<Lock::CollectionLock> collLock;
        collLock.emplace(opCtx, dbAndUUID, MODE_X);

        if (replSetAndNotPrimary) {
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
            const bool unlocked = opCtx->lockState()->unlockRSTLforPrepare();
            invariant(unlocked);
            opCtx->runWithoutInterruptionExceptAtGlobalShutdown(
                [&, this] { _buildIndex(opCtx, dbAndUUID, replState, &collLock); });
        } else {
            _buildIndex(opCtx, dbAndUUID, replState, &collLock);
        }
        // If _buildIndex returned normally, then we should have the collection X lock. It is not
        // required to safely access the collection, though, because an index build is registerd.
        auto collection =
            CollectionCatalog::get(opCtx).lookupCollectionByUUID(replState->collectionUUID);
        invariant(collection);
        replState->stats.numIndexesAfter = _getNumIndexesTotal(opCtx, collection);
        status = Status::OK();
    } catch (const DBException& ex) {
        status = ex.toStatus();
    }

    if (status == ErrorCodes::InterruptedAtShutdown) {
        // Leave it as-if kill -9 happened. This will be handled on restart.
        _indexBuildsManager.interruptIndexBuild(opCtx, replState->buildUUID, "shutting down");

        // On secondaries, a shutdown interruption status is part of normal operation and
        // should be suppressed, unlike other errors which should be raised to the administrator's
        // attention via a server crash. The server will attempt to recover the index build during
        // the next startup.
        // On primary and standalone nodes, the failed index build will not be replicated so it is
        // okay to propagate the shutdown error to the client.
        if (replSetAndNotPrimary) {
            replState->stats.numIndexesAfter = replState->stats.numIndexesBefore;
            status = Status::OK();
        }
    } else if (IndexBuildProtocol::kTwoPhase == replState->protocol) {
        // TODO (SERVER-40807): disabling the following code for the v4.2 release so it does not
        // have downstream impact.
        /*
        // Only the primary node removes the index build entry, as the secondaries will
        // replicate.
        if (!replSetAndNotPrimary) {
            auto removeStatus = removeIndexBuildEntry(opCtx, replState->buildUUID);
            if (!removeStatus.isOK()) {
                logFailure(removeStatus, nss, replState);
                uassertStatusOK(removeStatus);
                MONGO_UNREACHABLE;
            }
        }
        */
    }

    NamespaceString nss;
    {
        // We do not hold a collection lock here, but we are protected against the collection being
        // dropped while the index build is still registered for the collection -- until
        // tearDownIndexBuild is called. The collection can be renamed, but it is OK for the name to
        // be stale just for logging purposes.
        auto collection =
            CollectionCatalog::get(opCtx).lookupCollectionByUUID(replState->collectionUUID);
        invariant(collection,
                  str::stream() << "Collection with UUID " << replState->collectionUUID
                                << " should exist because an index build is in progress: "
                                << replState->buildUUID);
        nss = collection->ns();

        _indexBuildsManager.tearDownIndexBuild(opCtx, collection, replState->buildUUID);
    }

    if (!status.isOK()) {
        logFailure(status, nss, replState);

        // Failed index builds should abort secondary oplog application.
        if (replSetAndNotPrimary) {
            fassert(51101,
                    status.withContext(str::stream() << "Index build: " << replState->buildUUID
                                                     << "; Database: " << replState->dbName));
        }

        // Signal downstream secondary nodes to abort index build.
        if (serverGlobalParams.featureCompatibility.isVersionInitialized() &&
            serverGlobalParams.featureCompatibility.getVersion() ==
                ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) {
            UninterruptibleLockGuard noInterrupt(opCtx->lockState());
            Lock::GlobalLock lock(opCtx, MODE_IX);
            auto collUUID = replState->collectionUUID;
            auto fromMigrate = false;
            writeConflictRetry(
                opCtx, "onAbortIndexBuild", NamespaceString::kRsOplogNamespace.ns(), [&] {
                    opCtx->getServiceContext()->getOpObserver()->onAbortIndexBuild(
                        opCtx,
                        nss,
                        collUUID,
                        replState->buildUUID,
                        replState->indexSpecs,
                        status,
                        fromMigrate);
                });
        }

        uassertStatusOK(status);
        MONGO_UNREACHABLE;
    }

    log() << "Index build completed successfully: " << replState->buildUUID << ": " << nss << " ( "
          << replState->collectionUUID << " ). Index specs built: " << replState->indexSpecs.size()
          << ". Indexes in catalog before build: " << replState->stats.numIndexesBefore
          << ". Indexes in catalog after build: " << replState->stats.numIndexesAfter;
}

void IndexBuildsCoordinator::_buildIndex(
    OperationContext* opCtx,
    const NamespaceStringOrUUID& dbAndUUID,
    std::shared_ptr<ReplIndexBuildState> replState,
    boost::optional<Lock::CollectionLock>* exclusiveCollectionLock) {

    {
        auto nss = CollectionCatalog::get(opCtx).lookupNSSByUUID(replState->collectionUUID);
        invariant(nss);
        invariant(opCtx->lockState()->isDbLockedForMode(replState->dbName, MODE_IX));
        invariant(opCtx->lockState()->isCollectionLockedForMode(*nss, MODE_X));

        // Set up the thread's currentOp information to display createIndexes cmd information.
        _updateCurOpOpDescription(opCtx, *nss, replState->indexSpecs);
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
            CollectionCatalog::get(opCtx).lookupCollectionByUUID(replState->collectionUUID);
        invariant(collection);

        uassertStatusOK(
            _indexBuildsManager.startBuildingIndex(opCtx, collection, replState->buildUUID));
    }

    if (MONGO_unlikely(hangAfterIndexBuildDumpsInsertsFromBulk.shouldFail())) {
        log() << "Hanging after dumping inserts from bulk builder";
        hangAfterIndexBuildDumpsInsertsFromBulk.pauseWhileSet();
    }

    // Perform the first drain while holding an intent lock.
    {
        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_IS);

        uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(
            opCtx, replState->buildUUID, RecoveryUnit::ReadSource::kUnset));
    }

    if (MONGO_unlikely(hangAfterIndexBuildFirstDrain.shouldFail())) {
        log() << "Hanging after index build first drain";
        hangAfterIndexBuildFirstDrain.pauseWhileSet();
    }

    // Perform the second drain while stopping writes on the collection.
    {
        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::CollectionLock collLock(opCtx, dbAndUUID, MODE_S);

        uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(
            opCtx, replState->buildUUID, RecoveryUnit::ReadSource::kUnset));
    }

    if (MONGO_unlikely(hangAfterIndexBuildSecondDrain.shouldFail())) {
        log() << "Hanging after index build second drain";
        hangAfterIndexBuildSecondDrain.pauseWhileSet();
    }

    // Need to return the collection lock back to exclusive mode, to complete the index build.
    opCtx->recoveryUnit()->abandonSnapshot();
    exclusiveCollectionLock->emplace(opCtx, dbAndUUID, MODE_X);

    // The collection object should always exist while an index build is registered.
    auto collection =
        CollectionCatalog::get(opCtx).lookupCollectionByUUID(replState->collectionUUID);
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
        opCtx, replState->buildUUID, RecoveryUnit::ReadSource::kUnset));

    // Index constraint checking phase.
    uassertStatusOK(
        _indexBuildsManager.checkIndexConstraintViolations(opCtx, replState->buildUUID));

    // Generate both createIndexes and commitIndexBuild oplog entries.
    // Secondaries currently interpret commitIndexBuild commands as noops.
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    auto fromMigrate = false;
    auto onCommitFn = [&] {
        if (!serverGlobalParams.featureCompatibility.isVersionInitialized()) {
            return;
        }
        if (serverGlobalParams.featureCompatibility.getVersion() !=
            ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo44) {
            return;
        }
        opObserver->onCommitIndexBuild(opCtx,
                                       collection->ns(),
                                       replState->collectionUUID,
                                       replState->buildUUID,
                                       replState->indexSpecs,
                                       fromMigrate);
    };

    auto onCreateEachFn = [&](const BSONObj& spec) {
        // If two phase index builds is enabled, index build will be coordinated using
        // startIndexBuild and commitIndexBuild oplog entries.
        if (supportsTwoPhaseIndexBuild()) {
            return;
        }
        opObserver->onCreateIndex(
            opCtx, collection->ns(), replState->collectionUUID, spec, fromMigrate);
    };

    // Commit index build.
    uassertStatusOK(_indexBuildsManager.commitIndexBuild(
        opCtx, collection, collection->ns(), replState->buildUUID, onCreateEachFn, onCommitFn));

    return;
}

StatusWith<std::pair<long long, long long>> IndexBuildsCoordinator::_runIndexRebuildForRecovery(
    OperationContext* opCtx,
    Collection* collection,
    ReplIndexBuildState::IndexCatalogStats& indexCatalogStats,
    const UUID& buildUUID) noexcept {
    // Index builds in recovery mode have the global write lock.
    invariant(opCtx->lockState()->isW());

    auto replState = [&] {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        auto it = _allIndexBuilds.find(buildUUID);
        invariant(it != _allIndexBuilds.end());
        return it->second;
    }();

    // We rely on 'collection' for any collection information because no databases are open during
    // recovery.
    NamespaceString nss = collection->ns();
    invariant(!nss.isEmpty());

    auto status = Status::OK();

    long long numRecords = 0;
    long long dataSize = 0;

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

        indexCatalogStats.numIndexesAfter = _getNumIndexesTotal(opCtx, collection);

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
        _indexBuildsManager.tearDownIndexBuild(opCtx, collection, buildUUID);
    } else {
        // An index build failure during recovery is fatal.
        logFailure(status, nss, replState);
        fassertNoTrace(51076, status);
    }

    // 'numIndexesBefore' was before we cleared any unfinished indexes, so it must be the same
    // as 'numIndexesAfter', since we're going to be building any unfinished indexes too.
    invariant(indexCatalogStats.numIndexesBefore == indexCatalogStats.numIndexesAfter);

    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _unregisterIndexBuild(lk, replState);
    }

    if (status.isOK()) {
        return std::make_pair(numRecords, dataSize);
    }
    return status;
}

void IndexBuildsCoordinator::_stopIndexBuildsOnDatabase(StringData dbName) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto it = _disallowedDbs.find(dbName);
    if (it != _disallowedDbs.end()) {
        ++(it->second);
        return;
    }
    _disallowedDbs[dbName] = 1;
}

void IndexBuildsCoordinator::_stopIndexBuildsOnCollection(const UUID& collectionUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto it = _disallowedCollections.find(collectionUUID);
    if (it != _disallowedCollections.end()) {
        ++(it->second);
        return;
    }
    _disallowedCollections[collectionUUID] = 1;
}

void IndexBuildsCoordinator::_allowIndexBuildsOnDatabase(StringData dbName) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto it = _disallowedDbs.find(dbName);
    invariant(it != _disallowedDbs.end());
    invariant(it->second);
    if (--(it->second) == 0) {
        _disallowedDbs.erase(it);
    }
}

void IndexBuildsCoordinator::_allowIndexBuildsOnCollection(const UUID& collectionUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto it = _disallowedCollections.find(collectionUUID);
    invariant(it != _disallowedCollections.end());
    invariant(it->second > 0);
    if (--(it->second) == 0) {
        _disallowedCollections.erase(it);
    }
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

int IndexBuildsCoordinator::_getNumIndexesTotal(OperationContext* opCtx, Collection* collection) {
    invariant(collection);
    const auto& nss = collection->ns();
    invariant(opCtx->lockState()->isLocked(),
              str::stream() << "Unable to get index count because collection was not locked"
                            << nss);

    auto indexCatalog = collection->getIndexCatalog();
    invariant(indexCatalog, str::stream() << "Collection is missing index catalog: " << nss);

    return indexCatalog->numIndexesTotal(opCtx);
}

std::vector<BSONObj> IndexBuildsCoordinator::_addDefaultsAndFilterExistingIndexes(
    OperationContext* opCtx,
    Collection* collection,
    const NamespaceString& nss,
    const std::vector<BSONObj>& indexSpecs) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));
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
    auto filteredSpecs = indexCatalog->removeExistingIndexes(
        opCtx, specsWithCollationDefaults, true /*removeIndexBuildsToo*/);

    for (const BSONObj& spec : filteredSpecs) {
        if (spec[kUniqueFieldName].trueValue()) {
            checkShardKeyRestrictions(opCtx, nss, spec[kKeyFieldName].Obj());
        }
    }

    return filteredSpecs;
}

}  // namespace mongo
