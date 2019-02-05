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

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/locker.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildFirstDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildSecondDrain);
MONGO_FAIL_POINT_DEFINE(hangAfterIndexBuildDumpsInsertsFromBulk);

namespace {

constexpr auto kUniqueFieldName = "unique"_sd;
constexpr auto kKeyFieldName = "key"_sd;

/**
 * Returns the collection UUID for the given 'nss', or a NamespaceNotFound error.
 *
 * Momentarily takes the collection IS lock for 'nss' to access the collection UUID.
 */
StatusWith<UUID> getCollectionUUID(OperationContext* opCtx, const NamespaceString& nss) {
    try {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        return autoColl.getCollection()->uuid().get();
    } catch (const DBException& ex) {
        invariant(ex.toStatus().code() == ErrorCodes::NamespaceNotFound);
        return ex.toStatus();
    }
}

/**
 * Checks if unique index specification is compatible with sharding configuration.
 */
void checkShardKeyRestrictions(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& newIdxKey) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss.ns(), MODE_X));

    const auto metadata = CollectionShardingState::get(opCtx, nss)->getCurrentMetadata();
    if (!metadata->isSharded())
        return;

    const ShardKeyPattern shardKeyPattern(metadata->getKeyPattern());
    uassert(ErrorCodes::CannotCreateIndex,
            str::stream() << "cannot create unique index over " << newIdxKey
                          << " with shard key pattern "
                          << shardKeyPattern.toBSON(),
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

Future<void> IndexBuildsCoordinator::joinIndexBuilds(const NamespaceString& nss,
                                                     const std::vector<BSONObj>& indexSpecs) {
    // TODO: implement. This code is just to make it compile.
    auto pf = makePromiseFuture<void>();
    auto promise = std::move(pf.promise);
    return std::move(pf.future);
}

void IndexBuildsCoordinator::interruptAllIndexBuilds(const std::string& reason) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // Signal all the index builds to stop.
    for (auto& buildStateIt : _allIndexBuilds) {
        _indexBuildsManager.interruptIndexBuild(buildStateIt.second->buildUUID, reason);
    }

    // Wait for all the index builds to stop.
    for (auto& dbIt : _databaseIndexBuilds) {
        dbIt.second->waitUntilNoIndexBuildsRemain(lk);
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
    collIndexBuildsIt->second->waitUntilNoIndexBuildsRemain(lk);
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
    dbIndexBuilds->waitUntilNoIndexBuildsRemain(lk);
}

Future<void> IndexBuildsCoordinator::abortIndexBuildByName(
    const NamespaceString& nss,
    const std::vector<std::string>& indexNames,
    const std::string& reason) {
    // TODO: not yet implemented. Some code to make it compile.
    auto pf = makePromiseFuture<void>();
    auto promise = std::move(pf.promise);
    return std::move(pf.future);
}

Future<void> IndexBuildsCoordinator::abortIndexBuildByBuildUUID(const UUID& buildUUID,
                                                                const std::string& reason) {
    // TODO: not yet implemented. Some code to make it compile.
    auto pf = makePromiseFuture<void>();
    auto promise = std::move(pf.promise);
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

void IndexBuildsCoordinator::assertNoIndexBuildInProgForCollection(
    const UUID& collectionUUID) const {
    uassert(ErrorCodes::BackgroundOperationInProgressForNamespace,
            mongoutils::str::stream()
                << "cannot perform operation: an index build is currently running",
            !inProgForCollection(collectionUUID));
}

void IndexBuildsCoordinator::assertNoBgOpInProgForDb(StringData db) const {
    uassert(ErrorCodes::BackgroundOperationInProgressForDatabase,
            mongoutils::str::stream()
                << "cannot perform operation: an index build is currently running for "
                   "database "
                << db,
            !inProgForDb(db));
}

void IndexBuildsCoordinator::awaitNoBgOpInProgForNs(OperationContext* opCtx, StringData ns) const {
    auto statusWithCollectionUUID = getCollectionUUID(opCtx, NamespaceString(ns));
    if (!statusWithCollectionUUID.isOK()) {
        // The collection does not exist, so there are no index builds on it.
        invariant(statusWithCollectionUUID.getStatus().code() == ErrorCodes::NamespaceNotFound);
        return;
    }

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto collIndexBuildsIt = _collectionIndexBuilds.find(statusWithCollectionUUID.getValue());
    if (collIndexBuildsIt == _collectionIndexBuilds.end()) {
        return;
    }

    collIndexBuildsIt->second->waitUntilNoIndexBuildsRemain(lk);
}

void IndexBuildsCoordinator::awaitNoBgOpInProgForDb(StringData db) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto dbIndexBuildsIt = _databaseIndexBuilds.find(db);
    if (dbIndexBuildsIt != _databaseIndexBuilds.end()) {
        return;
    }

    dbIndexBuildsIt->second->waitUntilNoIndexBuildsRemain(lk);
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

Status IndexBuildsCoordinator::_registerIndexBuild(
    OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

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
                return Status(ErrorCodes::IndexKeySpecsConflict,
                              str::stream() << "There's already an index with name '" << name
                                            << "' being built on the collection: "
                                            << " ( "
                                            << replIndexBuildState->collectionUUID
                                            << " )");
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
    WithLock lk,
    OperationContext* opCtx,
    std::shared_ptr<ReplIndexBuildState> replIndexBuildState) {
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

void IndexBuildsCoordinator::_runIndexBuild(OperationContext* opCtx,
                                            const UUID& buildUUID) noexcept {
    auto replState = [&] {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        auto it = _allIndexBuilds.find(buildUUID);
        invariant(it != _allIndexBuilds.end());
        return it->second;
    }();

    // 'status' should always be set to something else before this function exits.
    Status status{ErrorCodes::InternalError,
                  "Uninitialized status value in IndexBuildsCoordinator"};

    ON_BLOCK_EXIT([&] {
        // Ensure the index build is unregistered from the Coordinator and the Promise is set with
        // the build's result so that callers are notified of the outcome.

        invariant(status.code() != ErrorCodes::InternalError, status.toString());

        stdx::unique_lock<stdx::mutex> lk(_mutex);

        _unregisterIndexBuild(lk, opCtx, replState);

        if (status.isOK()) {
            replState->sharedPromise.emplaceValue(replState->stats);
        } else {
            replState->sharedPromise.setError(status);
        }
    });

    NamespaceString nss = UUIDCatalog::get(opCtx).lookupNSSByUUID(replState->collectionUUID);

    if (nss.isEmpty()) {
        status = Status(ErrorCodes::NamespaceNotFound,
                        str::stream() << "Collection with UUID '" << replState->collectionUUID
                                      << "' was dropped since the index build request began.");
        log() << "Index build failed (" << replState->buildUUID << "): " << status;
        return;
    }

    // Do not use AutoGetOrCreateDb because we may relock the database in mode IX.
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);

    // Allow the strong lock acquisition above to be interrupted, but from this point forward do
    // not allow locks or re-locks to be interrupted.
    UninterruptibleLockGuard noInterrupt(opCtx->lockState());

    auto collection = UUIDCatalog::get(opCtx).lookupCollectionByUUID(replState->collectionUUID);
    if (!collection) {
        status = Status(ErrorCodes::NamespaceNotFound,
                        str::stream() << "Collection '" << replState->collectionUUID
                                      << "' was dropped.");
        logFailure(status, nss, replState);
        return;
    }

    replState->stats.numIndexesBefore = _getNumIndexesTotal(opCtx, collection);

    std::vector<BSONObj> filteredSpecs;
    try {
        filteredSpecs =
            _addDefaultsAndFilterExistingIndexes(opCtx, collection, nss, replState->indexSpecs);
    } catch (const DBException& ex) {
        status = ex.toStatus();
        logFailure(status, nss, replState);
        return;
    }

    if (filteredSpecs.size() == 0) {
        // Finish setting the index catalog stats and return success.
        replState->stats.numIndexesAfter = replState->stats.numIndexesBefore;
        status = Status::OK();
        return;
    }

    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        while (_sleepForTest) {
            lk.unlock();
            sleepmillis(100);
            lk.lock();
        }
    }

    try {
        _buildIndex(opCtx, collection, nss, replState, filteredSpecs, &dbLock);
        replState->stats.numIndexesAfter = _getNumIndexesTotal(opCtx, collection);
    } catch (const DBException& ex) {
        status = ex.toStatus();
        logFailure(status, nss, replState);
        return;
    }

    invariant(opCtx->lockState()->isDbLockedForMode(replState->dbName, MODE_X));
    _indexBuildsManager.tearDownIndexBuild(replState->buildUUID);

    log() << "Index build completed successfully: " << replState->buildUUID << ": " << nss << " ( "
          << replState->collectionUUID
          << " ). Index specs requested: " << replState->indexSpecs.size()
          << ". Indexes built: " << filteredSpecs.size()
          << ". Indexes in catalog before build: " << replState->stats.numIndexesBefore
          << ". Indexes in catalog after build: " << replState->stats.numIndexesAfter;

    status = Status::OK();

    return;
}

void IndexBuildsCoordinator::_buildIndex(OperationContext* opCtx,
                                         Collection* collection,
                                         const NamespaceString& nss,
                                         std::shared_ptr<ReplIndexBuildState> replState,
                                         const std::vector<BSONObj>& filteredSpecs,
                                         Lock::DBLock* dbLock) {
    MultiIndexBlock::OnInitFn onInitFn;
    // Two-phase index builds write a different oplog entry than the default behavior which
    // writes a no-op just to generate an optime.
    if (IndexBuildProtocol::kTwoPhase == replState->protocol) {
        onInitFn = [&] {
            opCtx->getServiceContext()->getOpObserver()->onStartIndexBuild(
                opCtx,
                nss,
                replState->collectionUUID,
                replState->buildUUID,
                filteredSpecs,
                false /* fromMigrate */);
        };
    } else {
        onInitFn = MultiIndexBlock::makeTimestampedIndexOnInitFn(opCtx, collection);
    }
    uassertStatusOK(_indexBuildsManager.setUpIndexBuild(
        opCtx, collection, filteredSpecs, replState->buildUUID, onInitFn));

    // If we're a background index, replace exclusive db lock with an intent lock, so that
    // other readers and writers can proceed during this phase.
    if (_indexBuildsManager.isBackgroundBuilding(replState->buildUUID)) {
        opCtx->recoveryUnit()->abandonSnapshot();
        dbLock->relockWithMode(MODE_IX);
    }

    auto relockOnErrorGuard = makeGuard([&] {
        // Must have exclusive DB lock before we clean up the index build via the
        // destructor of 'indexer'.
        if (_indexBuildsManager.isBackgroundBuilding(replState->buildUUID)) {
            opCtx->recoveryUnit()->abandonSnapshot();
            dbLock->relockWithMode(MODE_X);
        }
    });

    // Collection scan and insert into index, followed by a drain of writes received in the
    // background.
    {
        Lock::CollectionLock colLock(opCtx->lockState(), nss.ns(), MODE_IX);
        uassertStatusOK(_indexBuildsManager.startBuildingIndex(replState->buildUUID));
    }

    if (MONGO_FAIL_POINT(hangAfterIndexBuildDumpsInsertsFromBulk)) {
        log() << "Hanging after dumping inserts from bulk builder";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterIndexBuildDumpsInsertsFromBulk);
    }

    // Perform the first drain while holding an intent lock.
    {
        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::CollectionLock colLock(opCtx->lockState(), nss.ns(), MODE_IS);
    }

    if (MONGO_FAIL_POINT(hangAfterIndexBuildFirstDrain)) {
        log() << "Hanging after index build first drain";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterIndexBuildFirstDrain);
    }

    // Perform the second drain while stopping writes on the collection.
    {
        opCtx->recoveryUnit()->abandonSnapshot();
        Lock::CollectionLock colLock(opCtx->lockState(), nss.ns(), MODE_S);

        uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(replState->buildUUID));
    }

    if (MONGO_FAIL_POINT(hangAfterIndexBuildSecondDrain)) {
        log() << "Hanging after index build second drain";
        MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangAfterIndexBuildSecondDrain);
    }

    relockOnErrorGuard.dismiss();

    // Need to return db lock back to exclusive, to complete the index build.
    if (_indexBuildsManager.isBackgroundBuilding(replState->buildUUID)) {
        opCtx->recoveryUnit()->abandonSnapshot();
        dbLock->relockWithMode(MODE_X);

        auto db = DatabaseHolder::get(opCtx)->getDb(opCtx, nss.db());
        if (db) {
            auto& dss = DatabaseShardingState::get(db);
            auto dssLock = DatabaseShardingState::DSSLock::lock(opCtx, &dss);
            dss.checkDbVersion(opCtx, dssLock);
        }

        invariant(db,
                  str::stream() << "Database not found after relocking. Index build: "
                                << replState->buildUUID
                                << ": "
                                << nss.ns()
                                << " ("
                                << replState->collectionUUID
                                << ")");
        invariant(db->getCollection(opCtx, nss),
                  str::stream() << "Collection not found after relocking. Index build: "
                                << replState->buildUUID
                                << ": "
                                << nss.ns()
                                << " ("
                                << replState->collectionUUID
                                << ")");
    }

    // Perform the third and final drain after releasing a shared lock and reacquiring an
    // exclusive lock on the database.
    uassertStatusOK(_indexBuildsManager.drainBackgroundWrites(replState->buildUUID));

    // Index constraint checking phase.
    uassertStatusOK(_indexBuildsManager.checkIndexConstraintViolations(replState->buildUUID));

    auto collectionUUID = replState->collectionUUID;
    auto onCommitFn = MultiIndexBlock::kNoopOnCommitFn;
    auto onCreateEachFn = MultiIndexBlock::kNoopOnCreateEachFn;
    if (IndexBuildProtocol::kTwoPhase == replState->protocol) {
        // Two-phase index builds write one oplog entry for all indexes that are completed.
        onCommitFn = [&] {
            opCtx->getServiceContext()->getOpObserver()->onCommitIndexBuild(
                opCtx,
                nss,
                collectionUUID,
                replState->buildUUID,
                filteredSpecs,
                false /* fromMigrate */);
        };
    } else {
        // Single-phase index builds write an oplog entry per index being built.
        onCreateEachFn = [opCtx, &nss, &collectionUUID](const BSONObj& spec) {
            opCtx->getServiceContext()->getOpObserver()->onCreateIndex(
                opCtx, nss, collectionUUID, spec, false);
        };
    }

    // Commit index build.
    uassertStatusOK(_indexBuildsManager.commitIndexBuild(
        opCtx, nss, replState->buildUUID, onCreateEachFn, onCommitFn));

    return;
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
    invariant(indexCatalog, str::stream() << "Collection is missing index catalog: " << nss.ns());

    return indexCatalog->numIndexesTotal(opCtx);
}

std::vector<BSONObj> IndexBuildsCoordinator::_addDefaultsAndFilterExistingIndexes(
    OperationContext* opCtx,
    Collection* collection,
    const NamespaceString& nss,
    const std::vector<BSONObj>& indexSpecs) {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss.ns(), MODE_X));
    invariant(collection);

    auto specsWithCollationDefaults =
        uassertStatusOK(collection->addCollationDefaultsToIndexSpecsForCreate(opCtx, indexSpecs));

    auto indexCatalog = collection->getIndexCatalog();
    auto filteredSpecs = indexCatalog->removeExistingIndexes(
        opCtx, specsWithCollationDefaults, /*throwOnError=*/true);

    for (const BSONObj& spec : filteredSpecs) {
        if (spec[kUniqueFieldName].trueValue()) {
            checkShardKeyRestrictions(opCtx, nss, spec[kKeyFieldName].Obj());
        }
    }

    return filteredSpecs;
}

}  // namespace mongo
