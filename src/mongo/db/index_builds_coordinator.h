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
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/database_index_builds_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl_index_build_state.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/string_map.h"
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
     * Contains additional information required by 'startIndexBuild()'.
     */
    struct IndexBuildOptions {
        boost::optional<CommitQuorumOptions> commitQuorum;
    };

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
        IndexBuildProtocol protocol,
        IndexBuildOptions indexBuildOptions) = 0;

    /**
     * Sets up the in-memory and persisted state of the index build.
     *
     * This function should only be called when in recovery mode, because we create new Collection
     * objects and replace old ones after dropping existing indexes.
     *
     * Returns the number of records and the size of the data iterated over, if successful.
     */
    StatusWith<std::pair<long long, long long>> startIndexRebuildForRecovery(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const std::vector<BSONObj>& specs,
        const UUID& buildUUID);

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
     */
    void interruptAllIndexBuildsForShutdown(const std::string& reason);

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
     * Aborts a given index build by index build UUID.
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
     * Sets a new commit quorum on an index build that manages 'indexNames' on collection 'nss'.
     * If the 'newCommitQuorum' is not satisfiable by the current replica set config, then the
     * previous commit quorum is kept and the UnsatisfiableCommitQuorum error code is returned.
     */
    virtual Status setCommitQuorum(OperationContext* opCtx,
                                   const NamespaceString& nss,
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
     * Uasserts if any index builds are in progress on any database.
     */
    void assertNoIndexBuildInProgress() const;

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
     */
    void awaitNoIndexBuildInProgressForCollection(const UUID& collectionUUID) const;

    /**
     * Waits for all index builds on a specified database to finish.
     */
    void awaitNoBgOpInProgForDb(StringData db) const;

    /**
     * Called by the replication coordinator when a replica set reconfig occurs, which could affect
     * any index build to make their commit quorum unachievable.
     *
     * Checks if the commit quorum is still satisfiable for each index build, if it is no longer
     * satisfiable, then those index builds are aborted.
     */
    void onReplicaSetReconfig();

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

    /**
     * Updates CurOp's 'opDescription' field with the current state of this index build.
     */
    void _updateCurOpOpDescription(OperationContext* opCtx,
                                   const NamespaceString& nss,
                                   const std::vector<BSONObj>& indexSpecs) const;

    /**
     * Registers an index build so that the rest of the system can discover it.
     *
     * If stopIndexBuildsOnNsOrDb has been called on the index build's collection or database, then
     * an error will be returned.
     */
    Status _registerIndexBuild(WithLock, std::shared_ptr<ReplIndexBuildState> replIndexBuildState);

protected:
    /**
     * Unregisters the index build.
     */
    void _unregisterIndexBuild(WithLock lk,
                               std::shared_ptr<ReplIndexBuildState> replIndexBuildState);

    /**
     * Sets up the in-memory and persisted state of the index build.
     *
     * Helper function for startIndexBuild. If the returned boost::optional is set, then the task
     * does not require scheduling and can be immediately returned to the caller of startIndexBuild.
     *
     * Returns an error status if there are any errors setting up the index build.
     */
    StatusWith<boost::optional<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>>
    _registerAndSetUpIndexBuild(OperationContext* opCtx,
                                CollectionUUID collectionUUID,
                                const std::vector<BSONObj>& specs,
                                const UUID& buildUUID,
                                IndexBuildProtocol protocol,
                                boost::optional<CommitQuorumOptions> commitQuorum);

    /**
     * Runs the index build on the caller thread. Handles unregistering the index build and setting
     * the index build's Promise with the outcome of the index build.
     */
    void _runIndexBuild(OperationContext* opCtx, const UUID& buildUUID) noexcept;

    /**
     * Acquires locks and runs index build. Throws on error.
     */
    void _runIndexBuildInner(OperationContext* opCtx,
                             std::shared_ptr<ReplIndexBuildState> replState);

    /**
     * Modularizes the _indexBuildsManager calls part of _runIndexBuildInner. Throws on error.
     */
    void _buildIndex(OperationContext* opCtx,
                     Collection* collection,
                     const NamespaceString& nss,
                     std::shared_ptr<ReplIndexBuildState> replState,
                     boost::optional<Lock::CollectionLock>* collLock);
    /**
     * Returns total number of indexes in collection, including unfinished/in-progress indexes.
     *
     * Helper function that is used in sub-classes. Used to set statistics on index build results.
     *
     * Expects a lock to be held by the caller, so that 'collection' is safe to use.
     */
    int _getNumIndexesTotal(OperationContext* opCtx, Collection* collection);

    /**
     * Adds collation defaults to 'indexSpecs', as well as filtering out existing indexes (ready or
     * building) and checking uniqueness constraints are compatible with sharding.
     *
     * Helper function that is used in sub-classes. Produces final specs that the Coordinator will
     * register and use for the build, if the result is non-empty.
     *
     * This function throws on error. Expects a DB X lock to be held by the caller.
     */
    std::vector<BSONObj> _addDefaultsAndFilterExistingIndexes(
        OperationContext* opCtx,
        Collection* collection,
        const NamespaceString& nss,
        const std::vector<BSONObj>& indexSpecs);

    /**
     * Runs the index build.
     * Rebuilding an index in recovery mode verifies each document to ensure that it is a valid
     * BSON object. It will remove any documents with invalid BSON.
     *
     * Returns the number of records and the size of the data iterated over, if successful.
     */
    StatusWith<std::pair<long long, long long>> _runIndexRebuildForRecovery(
        OperationContext* opCtx,
        Collection* collection,
        ReplIndexBuildState::IndexCatalogStats& indexCatalogStats,
        const UUID& buildUUID) noexcept;

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
    bool _shuttingDown = false;
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
    ScopedStopNewDatabaseIndexBuilds(const ScopedStopNewDatabaseIndexBuilds&) = delete;
    ScopedStopNewDatabaseIndexBuilds& operator=(const ScopedStopNewDatabaseIndexBuilds&) = delete;

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
    ScopedStopNewCollectionIndexBuilds(const ScopedStopNewCollectionIndexBuilds&) = delete;
    ScopedStopNewCollectionIndexBuilds& operator=(const ScopedStopNewCollectionIndexBuilds&) =
        delete;

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
