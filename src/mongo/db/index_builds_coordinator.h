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
#include "mongo/db/active_index_builds.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/index_build_oplog_entry.h"
#include "mongo/db/catalog/index_builds.h"
#include "mongo/db/catalog/index_builds_manager.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/rebuild_indexes.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl_index_build_state.h"
#include "mongo/db/resumable_index_builds_gen.h"
#include "mongo/db/serverless/serverless_types_gen.h"
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
     * Represents the set of different application modes used around building indexes that differ
     * from the default behaviour.
     */
    enum class ApplicationMode { kNormal, kStartupRepair, kInitialSync };

    /**
     * Contains additional information required by 'startIndexBuild()'.
     */
    struct IndexBuildOptions {
        boost::optional<CommitQuorumOptions> commitQuorum;
        ApplicationMode applicationMode = ApplicationMode::kNormal;
    };

    virtual ~IndexBuildsCoordinator() = default;

    /**
     * Executes tasks that must be done prior to destruction of the instance.
     */
    virtual void shutdown(OperationContext* opCtx) = 0;

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
     * Returns index names listed from the index specs list "specs".
     */
    static std::vector<std::string> extractIndexNames(const std::vector<BSONObj>& specs);

    /**
     * Sets up the in-memory and durable state of the index build. When successful, returns after
     * the index build has started and the first catalog write has been made, and if called on a
     * primary, when the startIndexBuild oplog entry has been written.
     *
     * A Future is returned that will complete when the index build commits or aborts.
     *
     * On a successful index build, calling Future::get(), or Future::getNoThrow(), returns index
     * catalog statistics.
     *
     * Returns an error status if there are any errors setting up the index build.
     */
    virtual StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>> startIndexBuild(
        OperationContext* opCtx,
        std::string dbName,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& specs,
        const UUID& buildUUID,
        IndexBuildProtocol protocol,
        IndexBuildOptions indexBuildOptions) = 0;

    /**
     * Reconstructs the in-memory state of the index build. When successful, returns after the index
     * build has been resumed from the phase it left off in.
     *
     * A Future is returned that will complete when the index build commits or aborts.
     *
     * On a successful index build, calling Future::get(), or Future::getNoThrow(), returns index
     * catalog statistics.
     *
     * Returns an error status if there are any errors setting up the index build.
     */
    virtual StatusWith<SharedSemiFuture<ReplIndexBuildState::IndexCatalogStats>> resumeIndexBuild(
        OperationContext* opCtx,
        std::string dbName,
        const UUID& collectionUUID,
        const std::vector<BSONObj>& specs,
        const UUID& buildUUID,
        const ResumeIndexInfo& resumeInfo) = 0;

    /**
     * Resumes and restarts index builds for recovery. Anything that fails to resume will be started
     * in a background thread. Each index build will wait for a replicated commit or abort, as in
     * steady-state.
     */
    void restartIndexBuildsForRecovery(OperationContext* opCtx,
                                       const IndexBuilds& buildsToRestart,
                                       const std::vector<ResumeIndexInfo>& buildsToResume);

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
    void applyStartIndexBuild(OperationContext* opCtx,
                              ApplicationMode applicationMode,
                              const IndexBuildOplogEntry& entry);

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
    void waitForAllIndexBuildsToStopForShutdown(OperationContext* opCtx);

    /**
     * Signals all of the index builds on the specified collection to abort and then waits until the
     * index builds are no longer running. Must identify the collection with a UUID and the caller
     * must continue to operate on the collection by UUID to protect against rename collection. The
     * provided 'reason' will be used in the error message that the index builders return to their
     * callers.
     *
     * Does not stop new index builds from starting. Caller must make that guarantee.
     *
     * Does not require holding locks.
     *
     * Returns the UUIDs of the index builds that were aborted or are already in the process of
     * being aborted by another caller.
     */
    std::vector<UUID> abortCollectionIndexBuilds(OperationContext* opCx,
                                                 NamespaceString collectionNss,
                                                 UUID collectionUUID,
                                                 const std::string& reason);

    /**
     * Signals all of the index builds on the specified 'db' to abort and then waits until the index
     * builds are no longer running. The provided 'reason' will be used in the error message that
     * the index builders return to their callers.
     *
     * Does not require holding locks.
     *
     * Does not stop new index builds from starting. Caller must make that guarantee.
     */
    void abortDatabaseIndexBuilds(OperationContext* opCtx,
                                  StringData db,
                                  const std::string& reason);

    /**
     * Signals all of the index builds belonging to the specified tenant to abort and then waits
     * until the index builds are no longer running. The provided 'reason' will be used in the
     * error message that the index builders return to their callers.
     *
     * Does not require holding locks.
     *
     * Does not stop new index builds from starting. Caller must make that guarantee.
     */
    void abortTenantIndexBuilds(OperationContext* opCtx,
                                MigrationProtocolEnum protocol,
                                StringData db,
                                const std::string& reason);
    /**
     * Signals all of the index builds to abort and then waits until the index builds are no longer
     * running. The provided 'reason' will be used in the error message that the index builders
     * return to their callers.
     *
     * Does not require holding locks.
     *
     * Does not stop new index builds from starting. Caller must make that guarantee.
     */
    void abortAllIndexBuildsForInitialSync(OperationContext* opCtx, const std::string& reason);

    /**
     * Signals all index builds on non-internal databases to abort and waits until they are no
     * longer running.
     *
     * Does not require holding locks.
     *
     * Does not stop new index builds from starting. Caller must make that guarantee.
     */
    void abortUserIndexBuildsForUserWriteBlocking(OperationContext* opCtx);

    /**
     * Aborts an index build by index build UUID. Returns when the index build thread exits.
     *
     * Returns true if the index build was aborted or the index build is already aborted.
     * Returns false if the index build does not exist or the index build is already in the process
     * of committing and cannot be aborted.
     */
    bool abortIndexBuildByBuildUUID(OperationContext* opCtx,
                                    const UUID& buildUUID,
                                    IndexBuildAction signalAction,
                                    std::string reason);
    /**
     * Aborts an index build by its index name(s). This will only abort in-progress index builds if
     * all of the indexes are specified that a single builder is building together. When an
     * appropriate builder exists, this returns the build UUID of the index builder that will be
     * aborted.
     */
    boost::optional<UUID> abortIndexBuildByIndexNames(OperationContext* opCtx,
                                                      const UUID& collectionUUID,
                                                      const std::vector<std::string>& indexNames,
                                                      std::string reason);

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
     * Called during rollback to stop all active index builds. The state of these builds is distinct
     * from "aborted" because no abortIndexBuild is replicated and the current node will restart
     * these builds at the completion of rollback. Returns an IndexBuilds of stopped index builds.
     * Single-phase index builds are not stopped.
     */
    IndexBuilds stopIndexBuildsForRollback(OperationContext* opCtx);

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
     * Returns true if there are no index builds in progress.
     */
    bool noIndexBuildInProgress() const;

    /**
     * Returns the number of index builds that are running on the specified database.
     */
    int numInProgForDb(StringData db) const;

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
     * Waits for all index builds on a specified collection to finish.
     */
    void awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                  const UUID& collectionUUID,
                                                  IndexBuildProtocol protocol);
    void awaitNoIndexBuildInProgressForCollection(OperationContext* opCtx,
                                                  const UUID& collectionUUID);

    /**
     * Waits for all index builds on a specified database to finish.
     */
    void awaitNoBgOpInProgForDb(OperationContext* opCtx, StringData db);

    /**
     * Waits until an index build completes. If there are no index builds in progress, returns
     * immediately.
     */
    void waitUntilAnIndexBuildFinishes(OperationContext* opCtx);


    /**
     * Appends the current state information of the index build to the builder.
     * Does nothing if build UUID does not refer to an active index build.
     */
    void appendBuildInfo(const UUID& buildUUID, BSONObjBuilder* builder) const;

    //
    // Helper functions for creating indexes that do not have to be managed by the
    // IndexBuildsCoordinator.
    //

    /**
     * Creates the specified index without yielding locks.
     * Assumes the caller has collection MODE_X lock.
     * Throws exception on error.
     */
    void createIndex(OperationContext* opCtx,
                     UUID collectionUUID,
                     const BSONObj& spec,
                     IndexBuildsManager::IndexConstraints indexConstraints,
                     bool fromMigrate);

    /**
     * Creates the specified indexes on an empty collection without yielding locks.
     * Assumes we are enclosed in a WriteUnitOfWork and the caller has exclusive access to the
     * collection. For two phase index builds, writes both startIndexBuild and commitIndexBuild
     * oplog entries on success. No two phase index build oplog entries, including abortIndexBuild,
     * will be written on failure. Throws exception on error.
     */
    static void createIndexesOnEmptyCollection(OperationContext* opCtx,
                                               CollectionWriter& collection,
                                               const std::vector<BSONObj>& specs,
                                               bool fromMigrate);

    void sleepIndexBuilds_forTestOnly(bool sleep);

    void verifyNoIndexBuilds_forTestOnly() const;

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
                                                         const CollectionPtr& collection,
                                                         const NamespaceString& nss,
                                                         const std::vector<BSONObj>& indexSpecs);

    /**
     * Helper function which normalizes a vector of index specs. This function will populate a
     * complete collation spec in cases where the index spec specifies a collation, and will add
     * the collection-default collation, if present, in cases where collation is omitted. If the
     * index spec omits the collation and the collection does not have a default, the collation
     * field is omitted from the spec. This function also converts the 'wildcardProjection' to
     * canonical form in any cases where it exists.
     *
     * If 'collection' is null, no changes are made to the input specs.
     *
     * This function throws on error.
     */
    static std::vector<BSONObj> normalizeIndexSpecs(OperationContext* opCtx,
                                                    const CollectionPtr& collection,
                                                    const std::vector<BSONObj>& indexSpecs);

    /**
     * Returns total number of indexes in collection, including unfinished/in-progress indexes.
     *
     * Used to set statistics on index build results.
     *
     * Expects a lock to be held by the caller, so that 'collection' is safe to use.
     */
    static int getNumIndexesTotal(OperationContext* opCtx, const CollectionPtr& collection);

    class IndexBuildsSSS : public ServerStatusSection {
    public:
        IndexBuildsSSS();

        bool includeByDefault() const final {
            return true;
        }

        void addRequiredPrivileges(std::vector<Privilege>* out) final {}

        BSONObj generateSection(OperationContext* opCtx,
                                const BSONElement& configElement) const final {
            BSONObjBuilder indexBuilds;
            BSONObjBuilder phases;

            indexBuilds.append("total", registered.loadRelaxed());

            phases.append("scanCollection", scanCollection.loadRelaxed());
            phases.append("drainSideWritesTable", drainSideWritesTable.loadRelaxed());
            phases.append("drainSideWritesTablePreCommit",
                          drainSideWritesTablePreCommit.loadRelaxed());
            phases.append("waitForCommitQuorum", waitForCommitQuorum.loadRelaxed());
            phases.append("drainSideWritesTableOnCommit",
                          drainSideWritesTableOnCommit.loadRelaxed());
            phases.append("processConstraintsViolatonTableOnCommit",
                          processConstraintsViolatonTableOnCommit.loadRelaxed());
            phases.append("commit", commit.loadRelaxed());

            indexBuilds.append("phases", phases.obj());

            return indexBuilds.obj();
        }

        AtomicWord<int> registered;
        AtomicWord<int> scanCollection;
        AtomicWord<int> drainSideWritesTable;
        AtomicWord<int> drainSideWritesTablePreCommit;
        AtomicWord<int> waitForCommitQuorum;
        AtomicWord<int> drainSideWritesTableOnCommit;
        AtomicWord<int> processConstraintsViolatonTableOnCommit;
        AtomicWord<int> commit;
    } indexBuildsSSS;

private:
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
                                 const UUID& collectionUUID,
                                 const std::vector<BSONObj>& specs,
                                 const UUID& buildUUID,
                                 IndexBuildProtocol protocol);

    /**
     * Sets up the durable state of the index build.
     *
     * Returns an error status if there are any errors setting up the index build.
     */
    Status _setUpIndexBuild(OperationContext* opCtx,
                            const UUID& buildUUID,
                            Timestamp startTimestamp,
                            const IndexBuildOptions& indexBuildOptions);

    /**
     * Acquires locks and sets up index build. Throws on error.
     * Returns PostSetupAction which indicates whether to proceed to _runIndexBuild() or complete
     * the index build early (because there is no further work to be done).
     */
    enum class PostSetupAction { kContinueIndexBuild, kCompleteIndexBuildEarly };
    PostSetupAction _setUpIndexBuildInner(OperationContext* opCtx,
                                          std::shared_ptr<ReplIndexBuildState> replState,
                                          Timestamp startTimestamp,
                                          const IndexBuildOptions& indexBuildOptions);

    /**
     * Sets up the in-memory and durable state of the index build for two-phase recovery.
     *
     * Helper function for startIndexBuild during the two-phase index build recovery process.
     */
    Status _setUpIndexBuildForTwoPhaseRecovery(OperationContext* opCtx,
                                               StringData dbName,
                                               const UUID& collectionUUID,
                                               const std::vector<BSONObj>& specs,
                                               const UUID& buildUUID);
    /**
     * Reconstructs the in-memory state of the index build so that it can be resumed from the phase
     * it was in when the node cleanly shut down.
     */
    Status _setUpResumeIndexBuild(OperationContext* opCtx,
                                  std::string dbName,
                                  const UUID& collectionUUID,
                                  const std::vector<BSONObj>& specs,
                                  const UUID& buildUUID,
                                  const ResumeIndexInfo& resumeInfo);

    /**
     * Runs the index build on the caller thread. Handles unregistering the index build and setting
     * the index build's Promise with the outcome of the index build.
     * 'IndexBuildOptios::replSetAndNotPrimary' is determined at the start of the index build.
     */
    void _runIndexBuild(OperationContext* opCtx,
                        const UUID& buildUUID,
                        const IndexBuildOptions& indexBuildOptions,
                        const boost::optional<ResumeIndexInfo>& resumeInfo) noexcept;

    /**
     * Acquires locks and runs index build. Throws on error.
     * 'IndexBuildOptios::replSetAndNotPrimary' is determined at the start of the index build.
     */
    void _runIndexBuildInner(OperationContext* opCtx,
                             std::shared_ptr<ReplIndexBuildState> replState,
                             const IndexBuildOptions& indexBuildOptions,
                             const boost::optional<ResumeIndexInfo>& resumeInfo);

    /**
     * Resumes the index build from the phase that it was in when the node cleanly shut down. By the
     * time this function is called, the in-memory state of the index build should already have been
     * reconstructed.
     */
    void _resumeIndexBuildFromPhase(OperationContext* opCtx,
                                    std::shared_ptr<ReplIndexBuildState> replState,
                                    const IndexBuildOptions& indexBuildOptions,
                                    const ResumeIndexInfo& resumeInfo);

    /**
     * Cleans up a single-phase index build after a failure.
     */
    void _cleanUpSinglePhaseAfterFailure(OperationContext* opCtx,
                                         const CollectionPtr& collection,
                                         std::shared_ptr<ReplIndexBuildState> replState,
                                         const IndexBuildOptions& indexBuildOptions,
                                         const Status& status);

    /**
     * Cleans up a two-phase index build after a failure.
     */
    void _cleanUpTwoPhaseAfterFailure(OperationContext* opCtx,
                                      const CollectionPtr& collection,
                                      std::shared_ptr<ReplIndexBuildState> replState,
                                      const IndexBuildOptions& indexBuildOptions,
                                      const Status& status);

    /**
     * Performs last steps of aborting an index build.
     */
    void _completeAbort(OperationContext* opCtx,
                        std::shared_ptr<ReplIndexBuildState> replState,
                        const CollectionPtr& indexBuildEntryCollection,
                        IndexBuildAction signalAction,
                        Status reason);
    void _completeSelfAbort(OperationContext* opCtx,
                            std::shared_ptr<ReplIndexBuildState> replState,
                            const CollectionPtr& indexBuildEntryCollection,
                            Status reason);
    void _completeAbortForShutdown(OperationContext* opCtx,
                                   std::shared_ptr<ReplIndexBuildState> replState,
                                   const CollectionPtr& collection);

    /**
     * Waits for the last optime before the interceptors were installed on the node to be majority
     * committed and sets that the collection scan for the index build should use a majority read
     * cursor. If no such optime was recorded, it will do nothing.
     */
    void _awaitLastOpTimeBeforeInterceptorsMajorityCommitted(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState);

    /**
     * Modularizes the _indexBuildsManager calls part of _runIndexBuildInner. Throws on error.
     */
    void _buildIndex(OperationContext* opCtx,
                     std::shared_ptr<ReplIndexBuildState> replState,
                     const IndexBuildOptions& indexBuildOptions);

    /**
     * First phase is the collection scan and insertion of the keys into the sorter.
     * Second phase is extracting the sorted keys and writing them into the new index table.
     */
    void _scanCollectionAndInsertSortedKeysIntoIndex(
        OperationContext* opCtx,
        std::shared_ptr<ReplIndexBuildState> replState,
        const boost::optional<RecordId>& resumeAfterRecordId = boost::none);
    /**
     * Performs the second phase of the index build, for use when resuming from the second phase.
     */
    void _insertSortedKeysIntoIndexForResume(OperationContext* opCtx,
                                             std::shared_ptr<ReplIndexBuildState> replState);
    CollectionPtr _setUpForScanCollectionAndInsertSortedKeysIntoIndex(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState);

    /**
     * Third phase is catching up on all the writes that occurred during the first two phases.
     */
    void _insertKeysFromSideTablesWithoutBlockingWrites(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState);
    void _insertKeysFromSideTablesBlockingWrites(OperationContext* opCtx,
                                                 std::shared_ptr<ReplIndexBuildState> replState,
                                                 const IndexBuildOptions& indexBuildOptions);

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
     * Attempt to signal the index build to commit and advance the index build to the kPrepareCommit
     * state.
     * Returns true if successful and false if the attempt was unnecessful and the caller should
     * retry.
     */
    bool _tryCommit(OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState);
    /**
     * Skips the voting process and directly signal primary to commit index build if
     * commit quorum is not enabled.
     */
    virtual bool _signalIfCommitQuorumNotEnabled(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) = 0;

    /**
     * Signals the primary to commit the index build by sending "voteCommitIndexBuild" command
     * request to it with write concern 'majority', then waits for that command's response. And,
     * command gets retried on error. This function gets called after the first draining phase of
     * index build.
     */
    virtual void _signalPrimaryForCommitReadiness(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) = 0;

    /**
     * Drains the side-writes table periodically while waiting for the IndexBuildAction to be ready.
     */
    virtual IndexBuildAction _drainSideWritesUntilNextActionIsAvailable(
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
     * On completion, this function will commit the index build.
     */
    virtual void _waitForNextIndexBuildActionAndCommit(
        OperationContext* opCtx,
        std::shared_ptr<ReplIndexBuildState> replState,
        const IndexBuildOptions& indexBuildOptions) = 0;

    /**
     * Third phase is catching up on all the writes that occurred during the first two phases.
     * Accepts a commit timestamp for the index, which could be null. See
     * _waitForNextIndexBuildAction() comments. This timestamp is used only for committing the
     * index, which sets the ready flag to true, to the catalog; it is not used for the catch-up
     * writes during the final drain phase.
     *
     * This operation released the RSTL temporarily to acquire the collection X lock to prevent
     * deadlocks. It must reacquire the RSTL to commit, but it's possible for the node's state to
     * have changed in that period of time. If the replication state has changed or the lock
     * acquisition times out, a non-success CommitResult will be returned and the caller must retry.
     *
     * Returns a CommitResult that indicates whether or not the commit was successful.
     */
    enum class CommitResult {
        /** The index build was able to commit successfully. */
        kSuccess,
        /** After reacquiring the RSTL to commit, this node was no longer primary. The caller must
           reset and wait for the next IndexBuildAction again.  */
        kNoLongerPrimary,
        /** Reacquiring the RSTL timed out, indicating that conflicting state transition was in
           progress. The caller must try again. */
        kLockTimeout
    };
    CommitResult _insertKeysFromSideTablesAndCommit(OperationContext* opCtx,
                                                    std::shared_ptr<ReplIndexBuildState> replState,
                                                    IndexBuildAction action,
                                                    const IndexBuildOptions& indexBuildOptions,
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
        CollectionWriter& collection,
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

    // Handles actually building the indexes.
    IndexBuildsManager _indexBuildsManager;

    // Maintains data structures relating to activeIndexBuilds. Thread safe, unless a specific
    // function specifies otherwise.
    ActiveIndexBuilds activeIndexBuilds;
};

// These fail points are used to control index build progress. Declared here to be shared
// temporarily between createIndexes command and IndexBuildsCoordinator.
extern FailPoint hangAfterIndexBuildFirstDrain;
extern FailPoint hangAfterIndexBuildSecondDrain;
extern FailPoint hangAfterIndexBuildDumpsInsertsFromBulk;

}  // namespace mongo
