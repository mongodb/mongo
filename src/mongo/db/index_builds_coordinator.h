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

#pragma once

#include <map>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/index_builds_manager.h"
#include "mongo/db/collection_index_builds_tracker.h"
#include "mongo/db/database_index_builds_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl_index_build_state.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

namespace mongo {

class OperationContext;
class ServiceContext;

/**
 * This is a coordinator for all things index builds. Index builds can be externally affected,
 * notified, waited upon and aborted through this interface. Index build results are returned to
 * callers via Futures and Promises. The coordinator uses cross replica set index build state
 * to control index build progression.
 *
 * The IndexBuildsCoordinator is instantiated on the ServiceContext as a decoration, and is always
 * accessible via the ServiceContext. It owns an IndexBuildsManager that manages all MultiIndexBlock
 * index builder instances.
 */
class IndexBuildsCoordinator {
public:
    /**
     * Invariants that there are no index builds in-progress.
     */
    virtual ~IndexBuildsCoordinator();

    /**
     * Executes tasks that must be done prior to destruction of the instance.
     */
    virtual void shutdown() = 0;

    /**
     * Stores a coordinator on the specified service context. May only be called once for the
     * lifetime of the service context.
     */
    static void set(ServiceContext* serviceContext, std::unique_ptr<IndexBuildsCoordinator> ibc);

    /**
     * Retrieves the coordinator set on the service context. set() above must be called before any
     * get() calls.
     */
    static IndexBuildsCoordinator* get(ServiceContext* serviceContext);
    static IndexBuildsCoordinator* get(OperationContext* operationContext);

    /**
     * Sets up the in-memory and persisted state of the index build. A Future is returned upon which
     * the user can await the build result.
     *
     * On a successful index build, calling Future::get(), or Future::getNoThrows(), returns index
     * catalog statistics.
     *
     * Returns an error status if there are any errors setting up the index build.
     */
    virtual StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>> startIndexBuild(
        OperationContext* opCtx,
        CollectionUUID collectionUUID,
        const std::vector<BSONObj>& specs,
        const UUID& buildUUID,
        IndexBuildProtocol protocol) = 0;

    /**
     * TODO: not yet implemented.
     */
    Future<void> joinIndexBuilds(const NamespaceString& nss,
                                 const std::vector<BSONObj>& indexSpecs);

    /**
     * Commits the index build identified by 'buildUUID'.
     *
     * TODO: not yet implemented.
     */
    virtual Status commitIndexBuild(OperationContext* opCtx,
                                    const std::vector<BSONObj>& specs,
                                    const UUID& buildUUID) = 0;

    /**
     * Signals all the index builds to stop and then waits for them to finish. Leaves the index
     * builds in a recoverable state.
     *
     * This should only be called when certain the server will not start any new index builds --
     * i.e. when the server is not accepting user requests and no internal operations are
     * concurrently starting new index builds.
     *
     * TODO: not yet fully implemented. IndexBuildsManager::interruptIndexBuild is not yet
     * implemented.
     */
    void interruptAllIndexBuilds(const std::string& reason);

    /**
     * Signals all of the index builds on the specified collection to abort and then waits until the
     * index builds are no longer running. Must identify the collection with a UUID and the caller
     * must continue to operate on the collection by UUID to protect against rename collection. The
     * provided 'reason' will be used in the error message that the index builders return to their
     * callers.
     *
     * First create a ScopedStopNewCollectionIndexBuilds to block further index builds on the
     * collection before calling this and for the duration of the drop collection operation.
     *
     * {
     *     ScopedStopNewCollectionIndexBuilds scopedStop(collectionUUID);
     *     indexBuildsCoord->abortCollectionIndexBuilds(collectionUUID, "...");
     *     AutoGetCollection autoColl(..., collectionUUID, ...);
     *     autoColl->dropCollection(...);
     * }
     *
     * TODO: this is partially implemented. It calls IndexBuildsManager::abortIndexBuild that is not
     * implemented.
     */
    void abortCollectionIndexBuilds(const UUID& collectionUUID, const std::string& reason);

    /**
     * Signals all of the index builds on the specified 'db' to abort and then waits until the index
     * builds are no longer running. The provided 'reason' will be used in the error message that
     * the index builders return to their callers.
     *
     * First create a ScopedStopNewDatabaseIndexBuilds to block further index builds on the
     * specified
     * database before calling this and for the duration of the drop database operation.
     *
     * {
     *     ScopedStopNewDatabaseIndexBuilds scopedStop(dbName);
     *     indexBuildsCoord->abortDatabaseIndexBuilds(dbName, "...");
     *     AutoGetDb autoDb(...);
     *     autoDb->dropDatabase(...);
     * }
     *
     * TODO: this is partially implemented. It calls IndexBuildsManager::abortIndexBuild that is not
     * implemented.
     */
    void abortDatabaseIndexBuilds(StringData db, const std::string& reason);

    /**
     * Aborts a given index build by name on the given collection.
     *
     * TODO: This is not yet implemented.
     */
    Future<void> abortIndexBuildByName(const NamespaceString& nss,
                                       const std::vector<std::string>& indexNames,
                                       const std::string& reason);

    /**
     * Aborts a given index build by index build UUID.
     *
     * TODO: This is not yet implemented.
     */
    Future<void> abortIndexBuildByBuildUUID(const UUID& buildUUID, const std::string& reason);

    /**
     * Signal replica set member state changes that affect cross replica set index building.
     */
    virtual void signalChangeToPrimaryMode() = 0;
    virtual void signalChangeToSecondaryMode() = 0;
    virtual void signalChangeToInitialSyncMode() = 0;

    /**
     * TODO: This is not yet implemented.
     */
    virtual Status voteCommitIndexBuild(const UUID& buildUUID, const HostAndPort& hostAndPort) = 0;

    /**
     * TODO: This is not yet implemented. (This will have to take a collection IS lock to look up
     * the collection UUID.)
     */
    virtual Status setCommitQuorum(const NamespaceString& nss,
                                   const std::vector<StringData>& indexNames,
                                   const CommitQuorumOptions& newCommitQuorum) = 0;

    /**
     * TODO: This is not yet implemented.
     */
    void recoverIndexBuilds();

    /**
     * Returns the number of index builds that are running on the specified database.
     */
    int numInProgForDb(StringData db) const;

    /**
     * Prints out the names of collections on which index builds are running, and the number of
     * index builds per database.
     */
    void dump(std::ostream&) const;

    /**
     * Returns true if an index build is in progress on the specified collection.
     */
    bool inProgForCollection(const UUID& collectionUUID) const;

    /**
     * Returns true if an index build is in progress on the specified database.
     */
    bool inProgForDb(StringData db) const;

    /**
     * Uasserts if any index builds is in progress on the specified collection.
     */
    void assertNoIndexBuildInProgForCollection(const UUID& collectionUUID) const;

    /**
     * Uasserts if any index builds is in progress on the specified database.
     */
    void assertNoBgOpInProgForDb(StringData db) const;

    /**
     * Waits for all index builds on a specified collection to finish.
     *
     * Momentarily takes the collection IS lock for 'ns', to fetch the collection UUID.
     */
    void awaitNoBgOpInProgForNs(OperationContext* opCtx, StringData ns) const;
    void awaitNoBgOpInProgForNs(OperationContext* opCtx, const NamespaceString& ns) const {
        awaitNoBgOpInProgForNs(opCtx, ns.ns());
    }

    /**
     * Waits for all index builds on a specified database to finish.
     */
    void awaitNoBgOpInProgForDb(StringData db) const;

    void sleepIndexBuilds_forTestOnly(bool sleep);

    void verifyNoIndexBuilds_forTestOnly();

private:
    // Friend classes in order to be the only allowed callers of
    //_stopIndexBuildsOnCollection/Database and _allowIndexBuildsOnCollection/Database.
    friend class ScopedStopNewDatabaseIndexBuilds;
    friend class ScopedStopNewCollectionIndexBuilds;

    /**
     * Prevents new index builds being registered on the provided collection or database.
     *
     * It is safe to call this on the same collection/database concurrently in different threads. It
     * will still behave correctly.
     */
    void _stopIndexBuildsOnDatabase(StringData dbName);
    void _stopIndexBuildsOnCollection(const UUID& collectionUUID);

    /**
     * Allows new index builds to again be registered on the provided collection or database. Should
     * only be called after calling stopIndexBuildsOnCollection or stopIndexBuildsOnDatabase on the
     * same collection or database, respectively.
     */
    void _allowIndexBuildsOnDatabase(StringData dbName);
    void _allowIndexBuildsOnCollection(const UUID& collectionUUID);

protected:
    /**
     * Registers an index build so that the rest of the system can discover it.
     *
     * If stopIndexBuildsOnNsOrDb has been called on the index build's collection or database, then
     * an error will be returned.
     */
    Status _registerIndexBuild(OperationContext* opCtx,
                               std::shared_ptr<ReplIndexBuildState> replIndexBuildState);

    /**
     * Unregisters the index build.
     */
    void _unregisterIndexBuild(WithLock lk,
                               OperationContext* opCtx,
                               std::shared_ptr<ReplIndexBuildState> replIndexBuildState);

    /**
     * Runs the index build on the caller thread.
     */
    virtual void _runIndexBuild(OperationContext* opCtx, const UUID& buildUUID) noexcept;

    // Protects the below state.
    mutable stdx::mutex _mutex;

    // New index builds are not allowed on a collection or database if the collection or database is
    // in either of these maps. These are used when concurrent operations need to abort index builds
    // on a collection or database and must wait for the index builds to drain, without further
    // index builds being allowed to begin.
    StringMap<int> _disallowedDbs;
    stdx::unordered_map<UUID, int, UUID::Hash> _disallowedCollections;

    // Maps database name to database information. Tracks and accesses index builds on a database
    // level. Can be used to abort and wait upon the completion of all index builds for a database.
    //
    // Maps shared_ptrs so that DatabaseIndexBuildsTracker instances can outlive being erased from
    // this map when there are no longer any builds remaining on the database. This is necessary
    // when callers must wait for all index builds to cease.
    StringMap<std::shared_ptr<DatabaseIndexBuildsTracker>> _databaseIndexBuilds;

    // Collection UUID to collection level index build information. Enables index build lookup and
    // abort by collection UUID and index name, as well as collection level interruption.
    //
    // Maps shared_ptrs so that CollectionIndexBuildsTracker instances can outlive being erased from
    // this map when there are no longer any builds remaining on the collection. This is necessary
    // when callers must wait for and index build or all index builds to cease.
    stdx::unordered_map<UUID, std::shared_ptr<CollectionIndexBuildsTracker>, UUID::Hash>
        _collectionIndexBuilds;

    // Build UUID to index build information map.
    stdx::unordered_map<UUID, std::shared_ptr<ReplIndexBuildState>, UUID::Hash> _allIndexBuilds;

    // Handles actually building the indexes.
    IndexBuildsManager _indexBuildsManager;

    bool _sleepForTest = false;
};

/**
 * For this object's lifetime no new index builds will be allowed on the specified database. An
 * error will be returned by the IndexBuildsCoordinator to any caller attempting to register a new
 * index build on the blocked collection or database.
 *
 * This should be used by operations like drop database, where the active index builds must be
 * signaled to abort, but it takes time for them to wrap up, during which time no further index
 * builds should be scheduled.
 */
class ScopedStopNewDatabaseIndexBuilds {
    MONGO_DISALLOW_COPYING(ScopedStopNewDatabaseIndexBuilds);

public:
    /**
     * Takes either the full collection namespace or a database name and will block further index
     * builds on that collection or database.
     */
    ScopedStopNewDatabaseIndexBuilds(IndexBuildsCoordinator* indexBuildsCoordinator,
                                     StringData dbName);

    /**
     * Allows new index builds on the collection or database that were previously disallowed.
     */
    ~ScopedStopNewDatabaseIndexBuilds();

private:
    IndexBuildsCoordinator* _indexBuildsCoordinatorPtr;
    std::string _dbName;
};

/**
 * For this object's lifetime no new index builds will be allowed on the specified collection. An
 * error will be returned by the IndexBuildsCoordinator to any caller attempting to register a new
 * index build on the blocked collection.
 *
 * This should be used by operations like drop collection, where the active index builds must be
 * signaled to abort, but it takes time for them to wrap up, during which time no further index
 * builds should be scheduled.
 */
class ScopedStopNewCollectionIndexBuilds {
    MONGO_DISALLOW_COPYING(ScopedStopNewCollectionIndexBuilds);

public:
    /**
     * Blocks further index builds on the specified collection.
     */
    ScopedStopNewCollectionIndexBuilds(IndexBuildsCoordinator* indexBuildsCoordinator,
                                       const UUID& collectionUUID);

    /**
     * Allows new index builds on the collection that were previously disallowed.
     */
    ~ScopedStopNewCollectionIndexBuilds();

private:
    IndexBuildsCoordinator* _indexBuildsCoordinatorPtr;
    UUID _collectionUUID;
};

// These fail points are used to control index build progress. Declared here to be shared
// temporarily between createIndexes command and IndexBuildsCoordinator.
MONGO_FAIL_POINT_DECLARE(hangAfterIndexBuildFirstDrain);
MONGO_FAIL_POINT_DECLARE(hangAfterIndexBuildSecondDrain);
MONGO_FAIL_POINT_DECLARE(hangAfterIndexBuildDumpsInsertsFromBulk);

}  // namespace mongo
