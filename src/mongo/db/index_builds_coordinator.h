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

#include <algorithm>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/index_build_oplog_entry.h"
#include "mongo/db/catalog/index_builds.h"
#include "mongo/db/catalog/index_builds_manager.h"
#include "mongo/db/collection_index_builds_tracker.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/database_index_builds_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/rebuild_indexes.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl_index_build_state.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/fail_point.h"
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
        bool replSetAndNotPrimaryAtStart = false;
        bool twoPhaseRecovery = false;
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
     * Updates CurOp's 'opDescription' field with the current state of this index build.
     */
    static void updateCurOpOpDescription(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const std::vector<BSONObj>& indexSpecs);

    /**
     * Returns true if two phase index builds are supported.
     * This is determined by the current FCV and the server parameter 'enableTwoPhaseIndexBuild'.
     */
    static bool supportsTwoPhaseIndexBuild();

    /**
     * Sets up the in-memory and durable state of the index build. When successful, returns after
     * the index build has started and the first catalog write has been made, and if called on a
     * primary, when the startIndexBuild oplog entry has been written.
     *
     * A Future is returned that will complete when the index build commits or aborts.
     *
     * On a successful index build, calling Future::get(), or Future::getNoThrows(), returns index
     * catalog statistics.
     *
     * Returns an error status if there are any errors setting up the index build.
     */
    virtual StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>> startIndexBuild(
        OperationContext* opCtx,
        std::string dbName,
        CollectionUUID collectionUUID,
        const std::vector<BSONObj>& specs,
        const UUID& buildUUID,
        IndexBuildProtocol protocol,
        IndexBuildOptions indexBuildOptions) = 0;

    /**
     * Given a set of two-phase index builds, start, but do not complete each one in a background
     * thread. Each index build will wait for a replicated commit or abort, as in steady-state
     * replication.
     */
    void restartIndexBuildsForRecovery(OperationContext* opCtx, const IndexBuilds& buildsToRestart);

    /**
     * Runs the full index rebuild for recovery. This will only rebuild single-phase index builds.
     * Rebuilding an index in recovery mode verifies the BSON format of each document. Upon
     * discovery of corruption, if 'repair' is kYes, this function will remove any documents with
     * invalid BSON; otherwise, it will abort the server process.
     *
     * Returns the number of records and the size of the data iterated over, if successful.
     */
    StatusWith<std::pair<long long, long long>> rebuildIndexesForRecovery(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const std::vector<BSONObj>& specs,
        const UUID& buildUUID,
        RepairData repair);

    /**
     * Apply a 'startIndexBuild' oplog entry. Returns when the index build thread has started and
     * performed the initial ready:false write. Throws if there were any errors building the index.
     */
    void applyStartIndexBuild(OperationContext* opCtx, const IndexBuildOplogEntry& entry);

    /**
     * Apply a 'commitIndexBuild' oplog entry. If no index build is found, starts an index build
     * with the provided information. In all cases, waits until the index build commits and the
     * thread exits. Throws if there were any errors building the index.
     */
    void applyCommitIndexBuild(OperationContext* opCtx, const IndexBuildOplogEntry& entry);

    /**
     * Apply an 'abortIndexBuild' oplog entry. Waits until the index build aborts and the
     * thread exits. Throws if there were any errors aborting the index.
     */
    void applyAbortIndexBuild(OperationContext* opCtx, const IndexBuildOplogEntry& entry);

    /**
     * Waits for all index builds to stop after they have been interrupted during shutdown.
     * Leaves the index builds in a recoverable state.
     *
     * This should only be called when certain the server will not start any new index builds --
     * i.e. when the server is not accepting user requests and no internal operations are
     * concurrently starting new index builds.
     */
    void waitForAllIndexBuildsToStopForShutdown();

    /**
     * Signals all of the index builds on the specified collection to abort and then waits until the
     * index builds are no longer running. Must identify the collection with a UUID and the caller
     * must continue to operate on the collection by UUID to protect against rename collection. The
     * provided 'reason' will be used in the error message that the index builders return to their
     * callers.
     */
    void abortCollectionIndexBuilds(OperationContext* opCtx,
                                    const UUID& collectionUUID,
                                    const std::string& reason);

    /**
     * Signals all of the index builds on the specified collection to abort and returns the build
     * UUIDs of the index builds that will be aborted. Must identify the collection with a UUID. The
     * provided 'reason' will be used in the error message that the index builders return to their
     * callers.
     */
    std::vector<UUID> abortCollectionIndexBuildsNoWait(OperationContext* opCtx,
                                                       const UUID& collectionUUID,
                                                       const std::string& reason);

    /**
     * Signals all of the index builds on the specified 'db' to abort and then waits until the index
     * builds are no longer running. The provided 'reason' will be used in the error message that
     * the index builders return to their callers.
     */
    void abortDatabaseIndexBuilds(OperationContext* opCtx,
                                  StringData db,
                                  const std::string& reason);

    /**
     * Signals all of the index builds on the specified database to abort. The provided 'reason'
     * will be used in the error message that the index builders return to their callers.
     */
    void abortDatabaseIndexBuildsNoWait(OperationContext* opCtx,
                                        StringData db,
                                        const std::string& reason);

    /**
     * Aborts an index build by index build UUID. This gets called when the index build on primary
     * failed due to interruption or replica set state change.
     * It's a wrapper function to abortIndexBuildByBuildUUIDNoWait().
     */
    void abortIndexBuildOnError(OperationContext* opCtx, const UUID& buildUUID, Status abortStatus);

    /**
     * Aborts an index build by index build UUID. Returns when the index build thread exits.
     */
    void abortIndexBuildByBuildUUID(OperationContext* opCtx,
                                    const UUID& buildUUID,
                                    IndexBuildAction signalAction,
                                    boost::optional<Timestamp> abortTimestamp = boost::none,
                                    boost::optional<std::string> reason = boost::none);

    /**
     * Aborts an index build by index build UUID. Does not wait for the index build thread to
     * exit. Returns true if an index build was aborted.
     */
    bool abortIndexBuildByBuildUUIDNoWait(OperationContext* opCtx,
                                          const UUID& buildUUID,
                                          IndexBuildAction signalAction,
                                          boost::optional<Timestamp> abortTimestamp = boost::none,
                                          boost::optional<std::string> reason = boost::none);
    /**
     * Aborts an index build by its index name(s). This will only abort in-progress index builds if
     * all of the indexes are specified that a single builder is building together. When an
     * appropriate builder exists, this returns the build UUID of the index builder that will be
     * aborted.
     */
    boost::optional<UUID> abortIndexBuildByIndexNamesNoWait(
        OperationContext* opCtx,
        const UUID& collectionUUID,
        const std::vector<std::string>& indexNames,
        boost::optional<std::string> reason = boost::none);

    /**
     * Returns true if there is an index builder building the given index names on a collection.
     */
    bool hasIndexBuilder(OperationContext* opCtx,
                         const UUID& collectionUUID,
                         const std::vector<std::string>& indexNames) const;

    /**
     * Returns number of index builds in process.
     *
     * Invoked when the node is processing a shutdown command, an admin command that is
     * used to shut down the server gracefully.
     */
    std::size_t getActiveIndexBuildCount(OperationContext* opCtx);

    /**
     * Invoked when the node enters the primary state.
     * Unblocks index builds that have been waiting to commit/abort during the secondary state.
     */
    void onStepUp(OperationContext* opCtx);

    /**
     * Invoked when the node enters the rollback state.
     * Unblocks index builds that have been waiting to commit/abort during the secondary state.
     * Returns an IndexBuilds of aborted index builds.
     */
    IndexBuilds onRollback(OperationContext* opCtx);

    /**
     * Handles the 'VoteCommitIndexBuild' command request.
     * Writes the host and port information of the replica set member that has voted to commit an
     * index build into config.system.indexBuilds collection.
     */
    virtual Status voteCommitIndexBuild(OperationContext* opCtx,
                                        const UUID& buildUUID,
                                        const HostAndPort& hostAndPort) = 0;

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
    bool inProgForCollection(const UUID& collectionUUID, IndexBuildProtocol protocol) const;
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
     * Waits for the index build with 'buildUUID' to finish on the specified collection before
     * returning. Returns immediately if no such index build with 'buildUUID' is found.
     */
    void awaitIndexBuildFinished(const UUID& collectionUUID, const UUID& buildUUID) const;

    /**
     * Waits for all index builds on a specified collection to finish.
     */
    void awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                  const UUID& collectionUUID,
                                                  IndexBuildProtocol protocol);
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

    //
    // Helper functions for creating indexes that do not have to be managed by the
    // IndexBuildsCoordinator.
    //

    /**
     * Creates indexes in collection.
     * Assumes callers has necessary locks.
     * For two phase index builds, writes both startIndexBuild and commitIndexBuild oplog entries
     * on success. No two phase index build oplog entries, including abortIndexBuild, will be
     * written on failure.
     * Throws exception on error.
     */
    void createIndexes(OperationContext* opCtx,
                       UUID collectionUUID,
                       const std::vector<BSONObj>& specs,
                       IndexBuildsManager::IndexConstraints indexConstraints,
                       bool fromMigrate);

    /**
     * Creates indexes on an empty collection.
     * Assumes we are enclosed in a WriteUnitOfWork and caller has necessary locks.
     * For two phase index builds, writes both startIndexBuild and commitIndexBuild oplog entries
     * on success. No two phase index build oplog entries, including abortIndexBuild, will be
     * written on failure.
     * Throws exception on error.
     */
    static void createIndexesOnEmptyCollection(OperationContext* opCtx,
                                               UUID collectionUUID,
                                               const std::vector<BSONObj>& specs,
                                               bool fromMigrate);

    void sleepIndexBuilds_forTestOnly(bool sleep);

    void verifyNoIndexBuilds_forTestOnly();

    /**
     * Helper function that adds collation defaults to 'indexSpecs', as well as filtering out
     * existing indexes (ready or building) and checking uniqueness constraints are compatible with
     * sharding.
     *
     * Produces final specs to use for an index build, if the result is non-empty.
     *
     * This function throws on error. Expects caller to have exclusive access to `collection`.
     */
    static std::vector<BSONObj> prepareSpecListForCreate(OperationContext* opCtx,
                                                         Collection* collection,
                                                         const NamespaceString& nss,
                                                         const std::vector<BSONObj>& indexSpecs);

    /**
     * Returns total number of indexes in collection, including unfinished/in-progress indexes.
     *
     * Used to set statistics on index build results.
     *
     * Expects a lock to be held by the caller, so that 'collection' is safe to use.
     */
    static int getNumIndexesTotal(OperationContext* opCtx, Collection* collection);


private:
    /**
     * Registers an index build so that the rest of the system can discover it.
     *
     * If stopIndexBuildsOnNsOrDb has been called on the index build's collection or database, then
     * an error will be returned.
     */
    Status _registerIndexBuild(WithLock, std::shared_ptr<ReplIndexBuildState> replIndexBuildState);

    /**
     * Sets up the in-memory and durable state of the index build.
     *
     * This function should only be called when in recovery mode, because we drop and replace
     * existing indexes in a single WriteUnitOfWork.
     */
    Status _startIndexBuildForRecovery(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const std::vector<BSONObj>& specs,
                                       const UUID& buildUUID,
                                       IndexBuildProtocol protocol);

protected:
    /**
     * Unregisters the index build.
     */
    void _unregisterIndexBuild(WithLock lk,
                               std::shared_ptr<ReplIndexBuildState> replIndexBuildState);

    /**
     * Sets up the in-memory state of the index build. Validates index specs and filters out
     * existing indexes from the list of specs.
     *
     * Helper function for startIndexBuild. If the returned boost::optional is set, then the task
     * does not require scheduling and can be immediately returned to the caller of startIndexBuild.
     *
     * Returns an error status if there are any errors registering the index build.
     */
    StatusWith<boost::optional<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>>>
    _filterSpecsAndRegisterBuild(OperationContext* opCtx,
                                 StringData dbName,
                                 CollectionUUID collectionUUID,
                                 const std::vector<BSONObj>& specs,
                                 const UUID& buildUUID,
                                 IndexBuildProtocol protocol,
                                 boost::optional<CommitQuorumOptions> commitQuorum);

    /**
     * Sets up the durable state of the index build.
     *
     * Returns an error status if there are any errors setting up the index build.
     */
    Status _setUpIndexBuild(OperationContext* opCtx,
                            const UUID& buildUUID,
                            Timestamp startTimestamp);

    /**
     * Acquires locks and sets up index build. Throws on error.
     * Returns PostSetupAction which indicates whether to proceed to _runIndexBuild() or complete
     * the index build early (because there is no further work to be done).
     */
    enum class PostSetupAction { kContinueIndexBuild, kCompleteIndexBuildEarly };
    PostSetupAction _setUpIndexBuildInner(OperationContext* opCtx,
                                          std::shared_ptr<ReplIndexBuildState> replState,
                                          Timestamp startTimestamp);

    /**
     * Sets up the in-memory and durable state of the index build for two-phase recovery.
     *
     * Helper function for startIndexBuild during the two-phase index build recovery process.
     */
    Status _setUpIndexBuildForTwoPhaseRecovery(OperationContext* opCtx,
                                               StringData dbName,
                                               CollectionUUID collectionUUID,
                                               const std::vector<BSONObj>& specs,
                                               const UUID& buildUUID);
    /**
     * Runs the index build on the caller thread. Handles unregistering the index build and setting
     * the index build's Promise with the outcome of the index build.
     * 'IndexBuildOptios::replSetAndNotPrimary' is determined at the start of the index build.
     */
    void _runIndexBuild(OperationContext* opCtx,
                        const UUID& buildUUID,
                        const IndexBuildOptions& indexBuildOptions) noexcept;

    /**
     * Acquires locks and runs index build. Throws on error.
     * 'IndexBuildOptios::replSetAndNotPrimary' is determined at the start of the index build.
     */
    void _runIndexBuildInner(OperationContext* opCtx,
                             std::shared_ptr<ReplIndexBuildState> replState,
                             const IndexBuildOptions& indexBuildOptions);

    /**
     * Cleans up a single-phase index build after a failure.
     */
    void _cleanUpSinglePhaseAfterFailure(OperationContext* opCtx,
                                         Collection* collection,
                                         std::shared_ptr<ReplIndexBuildState> replState,
                                         const IndexBuildOptions& indexBuildOptions,
                                         const Status& status);

    /**
     * Cleans up a two-phase index build after a failure.
     */
    void _cleanUpTwoPhaseAfterFailure(OperationContext* opCtx,
                                      Collection* collection,
                                      std::shared_ptr<ReplIndexBuildState> replState,
                                      const IndexBuildOptions& indexBuildOptions,
                                      const Status& status);

    /**
     * Modularizes the _indexBuildsManager calls part of _runIndexBuildInner. Throws on error.
     */
    void _buildIndex(OperationContext* opCtx,
                     std::shared_ptr<ReplIndexBuildState> replState,
                     const IndexBuildOptions& indexBuildOptions,
                     boost::optional<Lock::CollectionLock>* collLock);

    /**
     * Builds the indexes single-phased.
     * This method matches pre-4.4 behavior for a background index build driven by a single
     * createIndexes oplog entry.
     */
    void _buildIndexSinglePhase(OperationContext* opCtx,
                                std::shared_ptr<ReplIndexBuildState> replState,
                                const IndexBuildOptions& indexBuildOptions,
                                boost::optional<Lock::CollectionLock>* collLock);

    /**
     * Builds the indexes two-phased.
     * The beginning and completion of a index build is driven by the startIndexBuild and
     * commitIndexBuild oplog entries, respectively.
     */
    void _buildIndexTwoPhase(OperationContext* opCtx,
                             std::shared_ptr<ReplIndexBuildState> replState,
                             const IndexBuildOptions& indexBuildOptions,
                             boost::optional<Lock::CollectionLock>* collLock);

    /**
     * First phase is the collection scan and insertion of the keys into the sorter.
     */
    void _scanCollectionAndInsertKeysIntoSorter(
        OperationContext* opCtx,
        std::shared_ptr<ReplIndexBuildState> replState,
        boost::optional<Lock::CollectionLock>* exclusiveCollectionLock);

    /**
     * Second phase is extracting the sorted keys and writing them into the new index table.
     */
    void _insertKeysFromSideTablesWithoutBlockingWrites(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState);

    /**
     * Reads the commit ready members list for index build UUID in 'replState' from
     * "config.system.indexBuilds" collection. And, signals the index builder thread on primary to
     * commit the index build if the number of voters have satisfied the commit quorum for that
     * index build. Sets the ReplIndexBuildState::waitForNextAction promise value to be
     * IndexBuildAction::kCommitQuorumSatisfied.
     */
    virtual void _signalIfCommitQuorumIsSatisfied(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) = 0;

    /**
     * Skips the voting process and directly signal primary to commit index build if
     * commit quorum is not enabled.
     */
    virtual bool _signalIfCommitQuorumNotEnabled(OperationContext* opCtx,
                                                 std::shared_ptr<ReplIndexBuildState> replState,
                                                 bool onStepUp) = 0;

    /**
     * Signals the primary to commit the index build by sending "voteCommitIndexBuild" command
     * request to it with write concern 'majority', then waits for that command's response. And,
     * command gets retried on error. This function gets called after the second draining phase of
     * index build.
     */
    virtual void _signalPrimaryForCommitReadiness(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) = 0;

    /**
     * Both primary and secondaries will wait on 'ReplIndexBuildState::waitForNextAction' future for
     * commit or abort index build signal.
     * On primary:
     *   - Commit signal can be sent either by voteCommitIndexBuild command or stepup.
     *   - Abort signal can be sent either by createIndexes command thread on user interruption or
     *     drop indexes/databases/collection commands.
     * On secondaries:
     *   - Commit signal can be sent only by oplog applier.
     *   - Abort signal on secondaries can be sent by oplog applier, bgSync on rollback.
     *
     * On completion, this function returns a timestamp, which may be null, that may be used to
     * update the mdb catalog as we commit the index build. The commit index build timestamp is
     * obtained from a commitIndexBuild oplog entry during secondary oplog application.
     * This function returns a null timestamp on receiving a abortIndexBuild oplog entry; or if we
     * are currently a primary, in which case we do not need to wait any external signal to commit
     * the index build.
     */
    virtual Timestamp _waitForNextIndexBuildAction(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) = 0;

    std::string _indexBuildActionToString(IndexBuildAction action);

    /**
     * Third phase is catching up on all the writes that occurred during the first two phases.
     * Accepts a commit timestamp for the index, which could be null. See
     * _waitForNextIndexBuildAction() comments. This timestamp is used only for committing the
     * index, which sets the ready flag to true, to the catalog; it is not used for the catch-up
     * writes during the final drain phase.
     */
    void _insertKeysFromSideTablesAndCommit(
        OperationContext* opCtx,
        std::shared_ptr<ReplIndexBuildState> replState,
        const IndexBuildOptions& indexBuildOptions,
        boost::optional<Lock::CollectionLock>* exclusiveCollectionLock,
        const Timestamp& commitIndexBuildTimestamp);

    /**
     * Runs the index build.
     * Rebuilding an index in recovery mode verifies each document to ensure that it is a valid
     * BSON object. If repair is kYes, it will remove any documents with invalid BSON.
     *
     * Returns the number of records and the size of the data iterated over, if successful.
     */
    StatusWith<std::pair<long long, long long>> _runIndexRebuildForRecovery(
        OperationContext* opCtx,
        Collection* collection,
        const UUID& buildUUID,
        RepairData repair) noexcept;

    /**
     * Looks up active index build by UUID. Returns NoSuchKey if the build does not exist.
     */
    StatusWith<std::shared_ptr<ReplIndexBuildState>> _getIndexBuild(const UUID& buildUUID) const;

    /**
     * Returns a snapshot of active index builds. Since each index build state is reference counted,
     * it is fine to examine the returned index builds without re-locking 'mutex'.
     */
    std::vector<std::shared_ptr<ReplIndexBuildState>> _getIndexBuilds() const;

    /**
     * Returns a list of index builds matching the criteria 'indexBuildFilter'.
     * Requires caller to lock '_mutex'.
     */
    using IndexBuildFilterFn = std::function<bool(const ReplIndexBuildState& replState)>;
    std::vector<std::shared_ptr<ReplIndexBuildState>> _filterIndexBuilds_inlock(
        WithLock lk, IndexBuildFilterFn indexBuildFilter) const;

    /**
     * Helper for 'abortCollectionIndexBuilds' and 'abortCollectionIndexBuildsNoWait'. Returns the
     * UUIDs of the aborted index builders
     */
    std::vector<UUID> _abortCollectionIndexBuilds(stdx::unique_lock<Latch>& lk,
                                                  OperationContext* opCtx,
                                                  const UUID& collectionUUID,
                                                  const std::string& reason,
                                                  bool shouldWait);

    /**
     * Helper for 'abortDatabaseIndexBuilds' and 'abortDatabaseIndexBuildsNoWait'.
     */
    void _abortDatabaseIndexBuilds(stdx::unique_lock<Latch>& lk,
                                   OperationContext* opCtx,
                                   const StringData& db,
                                   const std::string& reason,
                                   bool shouldWait);

    // Protects the below state.
    mutable Mutex _mutex = MONGO_MAKE_LATCH("IndexBuildsCoordinator::_mutex");

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

    // Waiters are notified whenever one of the three maps above has something added or removed.
    stdx::condition_variable _indexBuildsCondVar;

    // Handles actually building the indexes.
    IndexBuildsManager _indexBuildsManager;

    bool _sleepForTest = false;
};

// These fail points are used to control index build progress. Declared here to be shared
// temporarily between createIndexes command and IndexBuildsCoordinator.
extern FailPoint hangAfterIndexBuildFirstDrain;
extern FailPoint hangAfterIndexBuildSecondDrain;
extern FailPoint hangAfterIndexBuildDumpsInsertsFromBulk;

}  // namespace mongo
