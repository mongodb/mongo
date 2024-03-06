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
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/active_index_builds.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/commit_quorum_options.h"
#include "mongo/db/catalog/index_build_oplog_entry.h"
#include "mongo/db/catalog/index_builds.h"
#include "mongo/db/catalog/index_builds_manager.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/replication_state_transition_lock_guard.h"
#include "mongo/db/database_name.h"
#include "mongo/db/index/column_key_generator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/rebuild_indexes.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl_index_build_state.h"
#include "mongo/db/resumable_index_builds_gen.h"
#include "mongo/db/serverless/serverless_types_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/disk_space_monitor.h"
#include "mongo/db/tenant_id.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
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
     * Returns Status::OK if there is enough available disk space to start an index build. Will
     * return OutOfDiskSpace otherwise, with the context string providing the details.
     */
    static Status checkDiskSpaceSufficientToStartIndexBuild(OperationContext* opCtx);

    /**
     * Updates CurOp's 'op' type to 'command', the 'nss' field, and the 'opDescription' field with
     * 'createIndexes' command and index specs. Also ensures the timer is started. If provided,
     * 'curOpDesc' is used as the base description upon which to perform the updates. Otherwise, the
     * current 'opDescription' is used.
     */
    static void updateCurOpOpDescription(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const std::vector<BSONObj>& indexSpecs,
                                         boost::optional<BSONObj> curOpDesc = boost::none);

    /**
     * Returns index names listed from the index specs list "specs".
     */
    static std::vector<std::string> extractIndexNames(const std::vector<BSONObj>& specs);

    /**
     * Returns true if an index creation error is safe to ignore.
     * Consolidates the checking for the multiple scenarios where we may create indexes.
     * - createIndexes command on the primary;
     * - during oplog application (both empty and non-empty collection cases); and
     * - single-phase index creation for internal collections.
     */
    static bool isCreateIndexesErrorSafeToIgnore(
        const Status& status, IndexBuildsManager::IndexConstraints indexConstraints);

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
        const DatabaseName& dbName,
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
        const DatabaseName& dbName,
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
     * Waits for all index builds to stop.
     *
     * This should only be called when certain the server will not start any new index builds --
     * i.e. after a call to setNewIndexBuildsBlocked -- and potentially after aborting all index
     * builds that can be aborted -- i.e. using abortAllIndexBuildsWithReason -- to avoid an
     * excesively long wait.
     */
    void waitForAllIndexBuildsToStop(OperationContext* opCtx);

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
                                  const DatabaseName& dbName,
                                  const std::string& reason);

    void abortUserIndexBuildsForUserWriteBlocking(OperationContext* opCtx);

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
                                const boost::optional<TenantId>& tenantId,
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
     * Signals all index builds to abort because there is not enough disk space. Returns when index
     * builds have been killed.
     *
     * Does not require holding locks.
     *
     * Does not stop new index builds from starting. Caller must make that guarantee.
     */
    void abortAllIndexBuildsDueToDiskSpace(OperationContext* opCtx,
                                           std::int64_t availableBytes,
                                           std::int64_t requiredBytes);

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
     * Signals all of the index builds to abort and then waits until the index builds are no longer
     * running. The provided 'reason' will be used in the error message that the index builders
     * return to their callers.
     *
     * Does not require holding locks.
     *
     * Does not stop new index builds from starting. If required, caller must make that guarantee
     * with a call to setNewIndexBuildsBlocked.
     */
    void abortAllIndexBuildsWithReason(OperationContext* opCtx, const std::string& reason);

    /**
     * Blocks or unblocks new index builds from starting. When blocking is enabled, new index builds
     * will not immediately start and instead wait until a call to unblock is made. Concurrent calls
     * to this function are not supported.
     */
    void setNewIndexBuildsBlocked(bool newValue, boost::optional<std::string> reason = boost::none);

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
     * Handles the 'voteAbortIndexBuild' command request.
     */
    virtual Status voteAbortIndexBuild(OperationContext* opCtx,
                                       const UUID& buildUUID,
                                       const HostAndPort& hostAndPort,
                                       const StringData& reason) = 0;

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
    int numInProgForDb(const DatabaseName& dbName) const;

    /**
     * Returns true if an index build is in progress on the specified collection.
     */
    bool inProgForCollection(const UUID& collectionUUID, IndexBuildProtocol protocol) const;
    bool inProgForCollection(const UUID& collectionUUID) const;

    /**
     * Returns true if an index build is in progress on the specified database.
     */
    bool inProgForDb(const DatabaseName& dbName) const;

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
    void assertNoBgOpInProgForDb(const DatabaseName& dbName) const;

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
    void awaitNoBgOpInProgForDb(OperationContext* opCtx, const DatabaseName& dbName);

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

    /**
     * Returns an Action for the DiskSpaceMonitor that kills all index builds when the disk space
     * drops below a certain threshold.
     */
    std::unique_ptr<DiskSpaceMonitor::Action> makeKillIndexBuildOnLowDiskSpaceAction();

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

        BSONObj generateSection(OperationContext* opCtx,
                                const BSONElement& configElement) const final {
            BSONObjBuilder indexBuilds;

            indexBuilds.append("total", registered.loadRelaxed());
            indexBuilds.append("killedDueToInsufficientDiskSpace",
                               killedDueToInsufficientDiskSpace.loadRelaxed());
            indexBuilds.append("failedDueToDataCorruption",
                               failedDueToDataCorruption.loadRelaxed());

            BSONObjBuilder phases;
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
        AtomicWord<int> killedDueToInsufficientDiskSpace;
        AtomicWord<int> failedDueToDataCorruption;
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
     * This function should only be called when in recovery mode, because the index tables are
     * recreated.
     */
    Status _startIndexBuildForRecovery(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const std::vector<BSONObj>& specs,
                                       const UUID& buildUUID,
                                       IndexBuildProtocol protocol);

    /**
     * Removes the in-memory and durable state of the passed in indexes in preparation of rebuilding
     * them for repair.
     *
     * This function should only be called when in recovery mode.
     */
    Status _dropIndexesForRepair(OperationContext* opCtx,
                                 CollectionWriter& collection,
                                 const std::vector<std::string>& indexNames);

    void _abortTenantIndexBuilds(OperationContext* opCtx,
                                 const std::vector<std::shared_ptr<ReplIndexBuildState>>& builds,
                                 MigrationProtocolEnum protocol,
                                 const std::string& reason);

    void _abortAllIndexBuildsWithReason(OperationContext* opCtx,
                                        IndexBuildAction signalAction,
                                        const std::string& reason);

protected:
    void _waitIfNewIndexBuildsBlocked(OperationContext* opCtx,
                                      const UUID& collectionUUID,
                                      const std::vector<BSONObj>& specs,
                                      const UUID& buildUUID);

    /**
     * Acquire the collection MODE_X lock (and other locks up the hierarchy) as usual, with a
     * timeout. On timeout, all locks are released. If 'retry' is true, keeps retrying until
     * successful acquisition, and the returned StatusWith will always be OK and contain the locks.
     * If false, it returns with the error after a single try. The timeout is intentionally low to
     * avoid stalling replication state transitions for too long.
     *
     * Taking a collection exclusive lock from an operation which is not killed by step down can
     * cause a 3-way deadlock with prepared transactions, which hold MODE_IX locks, and the step
     * down thread trying to acquire the RSTL in MODE_X.
     *
     * 1. Prepared transaction (Holds Coll MODE_IX)
     * 2. Unkillable index builder (Holds RSTL MODE_IX, Blocked Coll MODE_X)
     * 3. Step down thread (Blocked on RSTL MODE_X)
     *
     * If we don't time out all locks in the hierarchy, there is potential for a 4-way deadlock:
     *
     * 1. Prepared transaction (Holds Coll MODE_IX)
     * 2. Unkillable index builder (Holds RSTL MODE_IX, Enqueues Coll MODE_X)
     * 3. Regular op (not killed by stepdown) (Holds RSTL MODE_IX, Enqueues Coll MODE_IS)
     * 4. Step down thread (Blocked on RSTL MODE_X due to stepdown not killing the op at 3 which
     *    holds the global lock in MODE_IS)
     *
     * See SERVER-44722, SERVER-42621, SERVER-71191 and SERVER-78662.
     *
     * TODO(SERVER-75288): revert SERVER-78662.
     */
    StatusWith<AutoGetCollection> _autoGetCollectionExclusiveWithTimeout(
        OperationContext* opCtx, ReplIndexBuildState* replState, bool retry = true);

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
                                 const DatabaseName& dbName,
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
                                               const DatabaseName& dbName,
                                               const UUID& collectionUUID,
                                               const std::vector<BSONObj>& specs,
                                               const UUID& buildUUID);
    /**
     * Reconstructs the in-memory state of the index build so that it can be resumed from the phase
     * it was in when the node cleanly shut down.
     */
    Status _setUpResumeIndexBuild(OperationContext* opCtx,
                                  const DatabaseName& dbName,
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
     * Cleans up the index build after a failure. If a shutdown happens during clean-up, defaults to
     * shutdown abort behaviour.
     */
    void _cleanUpAfterFailure(OperationContext* opCtx,
                              const CollectionPtr& collection,
                              std::shared_ptr<ReplIndexBuildState> replState,
                              const IndexBuildOptions& indexBuildOptions);

    /**
     * Cleans up a single-phase index build after a failure, only if non-shutdown related. This
     * allows handling shutdown errors during the clean-up itself, in _cleanUpAfterFailure.
     */
    void _cleanUpSinglePhaseAfterNonShutdownFailure(OperationContext* opCtx,
                                                    const CollectionPtr& collection,
                                                    std::shared_ptr<ReplIndexBuildState> replState,
                                                    const IndexBuildOptions& indexBuildOptions);

    /**
     * Cleans up a two-phase index build after a failure, only if non-shutdown related. This allows
     * handling shutdown errors during the clean-up itself, in _cleanUpAfterFailure.
     */
    void _cleanUpTwoPhaseAfterNonShutdownFailure(OperationContext* opCtx,
                                                 const CollectionPtr& collection,
                                                 std::shared_ptr<ReplIndexBuildState> replState,
                                                 const IndexBuildOptions& indexBuildOptions);

    /**
     * Performs last steps of aborting an index build.
     */
    void _completeAbort(OperationContext* opCtx,
                        std::shared_ptr<ReplIndexBuildState> replState,
                        const CollectionPtr& indexBuildEntryCollection,
                        IndexBuildAction signalAction);
    void _completeExternalAbort(OperationContext* opCtx,
                                std::shared_ptr<ReplIndexBuildState> replState,
                                const CollectionPtr& indexBuildEntryCollection,
                                IndexBuildAction signalAction);
    void _completeSelfAbort(OperationContext* opCtx,
                            std::shared_ptr<ReplIndexBuildState> replState,
                            const CollectionPtr& indexBuildEntryCollection);
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
     *
     * Returns true when the index build has been signalled, false otherwise.
     */
    virtual bool _signalIfCommitQuorumIsSatisfied(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) = 0;

    /**
     * Attempt to signal the index build to commit and advance the index build to the
     * kApplyCommitOplogEntry state. Returns true if successful and false if the attempt was
     * unnecessful and the caller should retry.
     */
    bool _tryCommit(OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState);
    /**
     * Skips the voting process and directly signal primary to commit index build if
     * commit quorum is not enabled.
     */
    virtual bool _signalIfCommitQuorumNotEnabled(
        OperationContext* opCtx, std::shared_ptr<ReplIndexBuildState> replState) = 0;

    /**
     * Signals the primary to abort the index build by sending "voteAbortIndexBuild" command
     * request to it with write concern 'majority', then waits for that command's response. The
     * command gets retried if failure is due to replication state transition. Finally, it waits for
     * the index build to be externally aborted.
     */
    virtual void _signalPrimaryForAbortAndWaitForExternalAbort(OperationContext* opCtx,
                                                               ReplIndexBuildState* replState) = 0;

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
     * Function to be run in the asynchronous task launched onStepUp to check if there are any index
     * builds that can be aborted because it has skipped records.
     */
    void _onStepUpAsyncTaskFn(OperationContext* opCtx);

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
     * Returns a list of index builds matching the criteria 'indexBuildFilter'.
     * Requires caller to lock '_mutex'.
     */
    using IndexBuildFilterFn = std::function<bool(const ReplIndexBuildState& replState)>;

    // Handles actually building the indexes.
    IndexBuildsManager _indexBuildsManager;

    // Maintains data structures relating to activeIndexBuilds. Thread safe, unless a specific
    // function specifies otherwise.
    ActiveIndexBuilds activeIndexBuilds;

    // The thread spawned during step-up to verify the builds.
    stdx::thread _stepUpThread;

    // Manages _newIndexBuildsBlocked.
    mutable Mutex _newIndexBuildsBlockedMutex =
        MONGO_MAKE_LATCH("IndexBuildsCoordinator::_newIndexBuildsBlocked");
    // Condition signalled to indicate new index builds are unblocked.
    stdx::condition_variable _newIndexBuildsBlockedCV;
    // Protected by _newIndexBuildsBlockedMutex.
    bool _newIndexBuildsBlocked = false;
    // Reason for blocking new index builds.
    boost::optional<std::string> _blockReason;
};

// These fail points are used to control index build progress. Declared here to be shared
// temporarily between createIndexes command and IndexBuildsCoordinator.
extern FailPoint hangAfterIndexBuildFirstDrain;
extern FailPoint hangAfterIndexBuildSecondDrain;
extern FailPoint hangAfterIndexBuildDumpsInsertsFromBulk;

}  // namespace mongo
