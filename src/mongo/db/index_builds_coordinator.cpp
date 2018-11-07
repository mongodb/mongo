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

#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

namespace {

/**
 * Constructs the options for the loader thread pool.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "IndexBuildsCoordinator";
    options.minThreads = 0;
    options.maxThreads = 10;

    // Ensure all threads have a client.
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };

    return options;
}

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

}  // namespace

const auto getIndexBuildsCoord = ServiceContext::declareDecoration<IndexBuildsCoordinator>();

IndexBuildsCoordinator* IndexBuildsCoordinator::get(ServiceContext* serviceContext) {
    return &getIndexBuildsCoord(serviceContext);
}

IndexBuildsCoordinator* IndexBuildsCoordinator::get(OperationContext* operationContext) {
    return get(operationContext->getServiceContext());
}

IndexBuildsCoordinator::IndexBuildsCoordinator() : _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();
}

IndexBuildsCoordinator::~IndexBuildsCoordinator() {
    invariant(_databaseIndexBuilds.empty());
    invariant(_disallowedDbs.empty());
    invariant(_disallowedCollections.empty());
    invariant(_collectionIndexBuilds.empty());
}

void IndexBuildsCoordinator::shutdown() {
    // Stop new scheduling.
    _threadPool.shutdown();

    // Signal active builds to stop and wait for them to stop.
    interruptAllIndexBuilds("Index build interrupted due to shutdown.");

    // Wait for active threads to finish.
    _threadPool.join();
}

StatusWith<Future<void>> IndexBuildsCoordinator::buildIndex(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            const std::vector<BSONObj>& specs,
                                                            const UUID& buildUUID) {
    std::vector<std::string> indexNames;
    for (auto& spec : specs) {
        std::string name = spec.getStringField(IndexDescriptor::kIndexNameFieldName);
        if (name.empty()) {
            return Status(
                ErrorCodes::CannotCreateIndex,
                str::stream() << "Cannot create an index for a spec '" << spec
                              << "' without a non-empty string value for the 'name' field");
        }
        indexNames.push_back(name);
    }

    UUID collectionUUID = [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        return autoColl.getCollection()->uuid().get();
    }();

    auto pf = makePromiseFuture<void>();

    auto replIndexBuildState = std::make_shared<ReplIndexBuildState>(
        buildUUID, collectionUUID, indexNames, specs, std::move(pf.promise));

    Status status = _registerIndexBuild(opCtx, replIndexBuildState);
    if (!status.isOK()) {
        return status;
    }

    status = _threadPool.schedule([ this, buildUUID ]() noexcept {
        auto opCtx = Client::getCurrent()->makeOperationContext();

        // Sets up and runs the index build. Sets result and cleans up index build.
        _runIndexBuild(opCtx.get(), buildUUID);
    });

    // Clean up the index build if we failed to schedule it.
    if (!status.isOK()) {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        // Unregister the index build before setting the promises, so callers do not see the build
        // again.
        _unregisterIndexBuild(lk, opCtx, replIndexBuildState);

        // Set the promises in case another thread already joined the index build.
        for (auto& promise : replIndexBuildState->promises) {
            promise.setError(status);
        }

        return status;
    }

    return std::move(pf.future);
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

Future<void> IndexBuildsCoordinator::abortIndexBuildByUUID(const UUID& buildUUID,
                                                           const std::string& reason) {
    // TODO: not yet implemented. Some code to make it compile.
    auto pf = makePromiseFuture<void>();
    auto promise = std::move(pf.promise);
    return std::move(pf.future);
}

void IndexBuildsCoordinator::signalChangeToPrimaryMode() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _replMode = ReplState::Primary;
}

void IndexBuildsCoordinator::signalChangeToSecondaryMode() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _replMode = ReplState::Secondary;
}

void IndexBuildsCoordinator::signalChangeToInitialSyncMode() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _replMode = ReplState::InitialSync;
}

Status IndexBuildsCoordinator::voteCommitIndexBuild(const UUID& buildUUID,
                                                    const HostAndPort& hostAndPort) {
    // TODO: not yet implemented.
    return Status::OK();
}

Status IndexBuildsCoordinator::setCommitQuorum(const NamespaceString& nss,
                                               const std::vector<std::string>& indexNames,
                                               const BSONObj& newCommitQuorum) {
    // TODO: not yet implemented.
    return Status::OK();
}

void IndexBuildsCoordinator::recoverIndexBuilds() {}

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

    NamespaceString nss =
        UUIDCatalog::get(opCtx).lookupNSSByUUID(replIndexBuildState->collectionUUID);
    if (!nss.isValid()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      "The collection has been dropped since the index build began.");
    }

    auto itns = _disallowedCollections.find(replIndexBuildState->collectionUUID);
    auto itdb = _disallowedDbs.find(nss.db());
    if (itns != _disallowedCollections.end() || itdb != _disallowedDbs.end()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "Collection '" << nss.toString()
                                    << "' is in the process of being dropped. New index builds are "
                                       "not currently allowed.");
    }

    // Check whether any indexes are already being built with the same index name(s). (Duplicate
    // specs will be discovered by the index builder.)
    auto collIndexBuildsIt = _collectionIndexBuilds.find(replIndexBuildState->collectionUUID);
    if (collIndexBuildsIt != _collectionIndexBuilds.end()) {
        for (const auto& name : replIndexBuildState->indexNames) {
            if (collIndexBuildsIt->second->hasIndexBuildState(lk, name)) {
                return Status(ErrorCodes::IndexKeySpecsConflict,
                              str::stream() << "There's already an index with name '" << name
                                            << "' being built on the collection");
            }
        }
    }

    // Register the index build.

    auto dbIndexBuilds = _databaseIndexBuilds[nss.db()];
    if (!dbIndexBuilds) {
        _databaseIndexBuilds[nss.db()] = std::make_shared<DatabaseIndexBuildsTracker>();
        dbIndexBuilds = _databaseIndexBuilds[nss.db()];
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
    NamespaceString nss =
        UUIDCatalog::get(opCtx).lookupNSSByUUID(replIndexBuildState->collectionUUID);
    invariant(!nss.isEmpty());

    auto dbIndexBuilds = _databaseIndexBuilds[nss.db()];
    invariant(dbIndexBuilds);
    dbIndexBuilds->removeIndexBuild(lk, replIndexBuildState->buildUUID);
    if (dbIndexBuilds->getNumberOfIndexBuilds(lk) == 0) {
        _databaseIndexBuilds.erase(nss.db());
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

    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        while (_sleepForTest) {
            lk.unlock();
            sleepmillis(100);
            lk.lock();
        }
    }

    // TODO: create scoped object to create the index builder, then destroy the builder, set the
    // promises and unregister the build.

    // TODO: implement.

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    _unregisterIndexBuild(lk, opCtx, replState);

    for (auto& promise : replState->promises) {
        promise.emplaceValue();
    }

    return;
}

Status IndexBuildsCoordinator::_finishScanningPhase() {
    // TODO: implement.
    return Status::OK();
}

Status IndexBuildsCoordinator::_finishVerificationPhase() {
    // TODO: implement.
    return Status::OK();
}

Status IndexBuildsCoordinator::_finishCommitPhase() {
    // TODO: implement.
    return Status::OK();
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

StatusWith<bool> IndexBuildsCoordinator::_checkCommitQuorum(
    const BSONObj& commitQuorum, const std::vector<HostAndPort>& confirmedMembers) {
    // TODO: not yet implemented.
    return false;
}

void IndexBuildsCoordinator::_refreshReplStateFromPersisted(OperationContext* opCtx,
                                                            const UUID& buildUUID) {}

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

}  // namespace mongo
