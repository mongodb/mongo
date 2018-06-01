/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#pragma once

#include <cstdint>
#include <iosfwd>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/callback_completion_guard.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/rollback_checker.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

// TODO: Remove forward declares once we remove rs_initialsync.cpp and other dependents.
// Failpoint which fails initial sync and leaves an oplog entry in the buffer.
MONGO_FAIL_POINT_DECLARE(failInitSyncWithBufferedEntriesLeft);

// Failpoint which causes the initial sync function to hang before copying databases.
MONGO_FAIL_POINT_DECLARE(initialSyncHangBeforeCopyingDatabases);

// Failpoint which causes the initial sync function to hang before calling shouldRetry on a failed
// operation.
MONGO_FAIL_POINT_DECLARE(initialSyncHangBeforeGettingMissingDocument);

// Failpoint which stops the applier.
MONGO_FAIL_POINT_DECLARE(rsSyncApplyStop);

struct InitialSyncState;
struct MemberState;
class ReplicationProcess;
class StorageInterface;

struct InitialSyncerOptions {
    /** Function to return optime of last operation applied on this node */
    using GetMyLastOptimeFn = stdx::function<OpTime()>;

    /** Function to update optime of last operation applied on this node */
    using SetMyLastOptimeFn =
        stdx::function<void(const OpTime&, ReplicationCoordinator::DataConsistency consistency)>;

    /** Function to reset all optimes on this node (e.g. applied & durable). */
    using ResetOptimesFn = stdx::function<void()>;

    /** Function to sets this node into a specific follower mode. */
    using SetFollowerModeFn = stdx::function<bool(const MemberState&)>;

    // Error and retry values
    Milliseconds syncSourceRetryWait{1000};
    Milliseconds initialSyncRetryWait{1000};
    Seconds blacklistSyncSourcePenaltyForNetworkConnectionError{10};
    Minutes blacklistSyncSourcePenaltyForOplogStartMissing{10};

    // InitialSyncer waits this long before retrying getApplierBatchCallback() if there are
    // currently no operations available to apply or if the 'rsSyncApplyStop' failpoint is active.
    // This default value is based on the duration in BackgroundSync::waitForMore() and
    // SyncTail::tryPopAndWaitForMore().
    Milliseconds getApplierBatchCallbackRetryWait{1000};

    // Replication settings
    NamespaceString localOplogNS = NamespaceString("local.oplog.rs");
    NamespaceString remoteOplogNS = NamespaceString("local.oplog.rs");

    GetMyLastOptimeFn getMyLastOptime;
    SetMyLastOptimeFn setMyLastOptime;
    ResetOptimesFn resetOptimes;

    SyncSourceSelector* syncSourceSelector = nullptr;

    // The oplog fetcher will restart the oplog tailing query this many times on non-cancellation
    // failures.
    std::uint32_t oplogFetcherMaxFetcherRestarts = 0;

    std::string toString() const {
        return str::stream() << "InitialSyncerOptions -- "
                             << " localOplogNs: " << localOplogNS.toString()
                             << " remoteOplogNS: " << remoteOplogNS.toString();
    }
};

/**
 * The initial syncer provides services to keep collection in sync by replicating
 * changes via an oplog source to the local system storage.
 *
 * This class will use existing machinery like the Executor to schedule work and
 * network tasks, as well as provide serial access and synchronization of state.
 *
 *
 * Entry Points:
 *      -- startup: Start initial sync.
 */
class InitialSyncer {
    MONGO_DISALLOW_COPYING(InitialSyncer);

public:
    /**
     * Callback function to report last applied optime (with hash) of initial sync.
     */
    typedef stdx::function<void(const StatusWith<OpTimeWithHash>& lastApplied)> OnCompletionFn;

    /**
     * Callback completion guard for initial syncer.
     */
    using OnCompletionGuard = CallbackCompletionGuard<StatusWith<OpTimeWithHash>>;

    struct InitialSyncAttemptInfo {
        int durationMillis;
        Status status;
        HostAndPort syncSource;

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    struct Stats {
        std::uint32_t failedInitialSyncAttempts{0};
        std::uint32_t maxFailedInitialSyncAttempts{0};
        Date_t initialSyncStart;
        Date_t initialSyncEnd;
        std::vector<InitialSyncer::InitialSyncAttemptInfo> initialSyncAttemptInfos;

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    InitialSyncer(InitialSyncerOptions opts,
                  std::unique_ptr<DataReplicatorExternalState> dataReplicatorExternalState,
                  ThreadPool* writerPool,
                  StorageInterface* storage,
                  ReplicationProcess* replicationProcess,
                  const OnCompletionFn& onCompletion);

    virtual ~InitialSyncer();

    /**
     * Returns true if an initial sync is currently running or in the process of shutting down.
     */
    bool isActive() const;

    /**
     * Starts initial sync process, with the provided number of attempts
     */
    Status startup(OperationContext* opCtx, std::uint32_t maxAttempts) noexcept;

    /**
     * Shuts down replication if "start" has been called, and blocks until shutdown has completed.
     */
    Status shutdown();

    /**
     * Block until inactive.
     */
    void join();

    /**
     * Returns internal state in a loggable format.
     */
    std::string getDiagnosticString() const;

    /**
     * Returns stats about the progress of initial sync. If initial sync is not in progress it
     * returns summary statistics for what occurred during initial sync.
     */
    BSONObj getInitialSyncProgress() const;

    /**
     * Overrides how executor schedules database work.
     *
     * For testing only.
     */
    void setScheduleDbWorkFn_forTest(const CollectionCloner::ScheduleDbWorkFn& scheduleDbWorkFn);

    // State transitions:
    // PreStart --> Running --> ShuttingDown --> Complete
    // It is possible to skip intermediate states. For example, calling shutdown() when the data
    // replicator has not started will transition from PreStart directly to Complete.
    enum class State { kPreStart, kRunning, kShuttingDown, kComplete };

    /**
     * Returns current initial syncer state.
     * For testing only.
     */
    State getState_forTest() const;

private:
    /**
     * Returns true if we are still processing initial sync tasks (_state is either Running or
     * Shutdown).
     */
    bool _isActive_inlock() const;

    /**
     * Cancels all outstanding work.
     * Used by shutdown() and CompletionGuard::setResultAndCancelRemainingWork().
     */
    void _cancelRemainingWork_inlock();

    /**
     * Returns true if the initial syncer has received a shutdown request (_state is ShuttingDown).
     */
    bool _isShuttingDown() const;
    bool _isShuttingDown_inlock() const;

    /**
     * Initial sync flowchart:
     *
     *     start()
     *         |
     *         |
     *         V
     *     _setUp_inlock()
     *         |
     *         |
     *         V
     *    _startInitialSyncAttemptCallback()
     *         |
     *         |
     *         |<-------+
     *         |        |
     *         |        | (bad sync source)
     *         |        |
     *         V        |
     *    _chooseSyncSourceCallback()
     *         |
     *         |
     *         | (good sync source found)
     *         |
     *         |
     *         V
     *    _truncateOplogAndDropReplicatedDatabases()
     *         |
     *         |
     *         V
     *    _rollbackCheckerResetCallback()
     *         |
     *         |
     *         V
     *    _lastOplogEntryFetcherCallbackForBeginTimestamp()
     *         |
     *         |
     *         V
     *    _fcvFetcherCallback()
     *         |
     *         |
     *         +------------------------------+
     *         |                              |
     *         |                              |
     *         V                              V
     *    _oplogFetcherCallback()         _databasesClonerCallback
     *         |                              |
     *         |                              |
     *         |                              V
     *         |                          _lastOplogEntryFetcherCallbackForStopTimestamp()
     *         |                              |       |
     *         |                              |       |
     *         |            (no ops to apply) |       | (have ops to apply)
     *         |                              |       |
     *         |                              |       V
     *         |                              |   _getNextApplierBatchCallback()<-----+
     *         |                              |       |                       ^       |
     *         |                              |       |                       |       |
     *         |                              |       |      (no docs fetched |       |
     *         |                              |       |       and end ts not  |       |
     *         |                              |       |       reached)        |       |
     *         |                              |       |                       |       |
     *         |                              |       V                       |       |
     *         |                              |   _multiApplierCallback()-----+       |
     *         |                              |       |       |                       |
     *         |                              |       |       |                       |
     *         |                              |       |       | (docs fetched)        | (end ts not
     *         |                              |       |       |                       |  reached)
     *         |                              |       |       V                       |
     *         |                              |       |   _lastOplogEntryFetcherCallbackAfter-
     *         |                              |       |       FetchingMissingDocuments()
     *         |                              |       |       |
     *         |                              |       |       |
     *         |                           (reached end timestamp)
     *         |                              |       |       |
     *         |                              V       V       V
     *         |                          _rollbackCheckerCheckForRollbackCallback()
     *         |                              |
     *         |                              |
     *         +------------------------------+
     *         |
     *         |
     *         V
     *    _finishInitialSyncAttempt()
     *         |
     *         |
     *         V
     *    _finishCallback()
     */

    /**
     * Sets up internal state to begin initial sync.
     */
    void _setUp_inlock(OperationContext* opCtx, std::uint32_t initialSyncMaxAttempts);

    /**
     * Tears down internal state before reporting final status to caller.
     */
    void _tearDown_inlock(OperationContext* opCtx, const StatusWith<OpTimeWithHash>& lastApplied);

    /**
     * Callback to start a single initial sync attempt.
     */
    void _startInitialSyncAttemptCallback(const executor::TaskExecutor::CallbackArgs& callbackArgs,
                                          std::uint32_t initialSyncAttempt,
                                          std::uint32_t initialSyncMaxAttempts);

    /**
     * Callback to obtain sync source from sync source selector.
     * For every initial sync attempt, we will try up to 'numInitialSyncConnectAttempts' times (at
     * an interval of '_opts.syncSourceRetryWait' ms) to obtain a valid sync source before giving up
     * and returning ErrorCodes::InitialSyncOplogSourceMissing.
     */
    void _chooseSyncSourceCallback(const executor::TaskExecutor::CallbackArgs& callbackArgs,
                                   std::uint32_t chooseSyncSourceAttempt,
                                   std::uint32_t chooseSyncSourceMaxAttempts,
                                   std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * This function does the following:
     *      1.) Truncate oplog.
     *      2.) Drop user databases (replicated dbs).
     */
    Status _truncateOplogAndDropReplicatedDatabases();

    /**
     * Callback for rollback checker's first replSetGetRBID command before starting data cloning.
     */
    void _rollbackCheckerResetCallback(const RollbackChecker::Result& result,
                                       std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Callback for first '_lastOplogEntryFetcher' callback. A successful response lets us
     * determine the starting point for tailing the oplog using the OplogFetcher as well as
     * setting a reference point for the state of the sync source's oplog when data cloning
     * completes.
     */
    void _lastOplogEntryFetcherCallbackForBeginTimestamp(
        const StatusWith<Fetcher::QueryResponse>& result,
        std::shared_ptr<OnCompletionGuard> onCompletionGuard);


    /**
     * Callback for the '_fCVFetcher'. A successful response lets us check if the remote node
     * is in a currently acceptable fCV and if it has a 'targetVersion' set.
     */
    void _fcvFetcherCallback(const StatusWith<Fetcher::QueryResponse>& result,
                             std::shared_ptr<OnCompletionGuard> onCompletionGuard,
                             const OpTimeWithHash& lastOpTimeWithHash);

    /**
     * Callback for oplog fetcher.
     */
    void _oplogFetcherCallback(const Status& status,
                               std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Callback for DatabasesCloner.
     */
    void _databasesClonerCallback(const Status& status,
                                  std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Callback for second '_lastOplogEntryFetcher' callback. This is scheduled to obtain the stop
     * timestamp after DatabasesCloner has completed and enables us to determine if the oplog on
     * the sync source has advanced since we started cloning the databases.
     */
    void _lastOplogEntryFetcherCallbackForStopTimestamp(
        const StatusWith<Fetcher::QueryResponse>& result,
        std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Callback to obtain next batch of operations to apply.
     */
    void _getNextApplierBatchCallback(const executor::TaskExecutor::CallbackArgs& callbackArgs,
                                      std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Callback for MultiApplier completion.
     */
    void _multiApplierCallback(const Status& status,
                               OpTimeWithHash lastApplied,
                               std::uint32_t numApplied,
                               std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Callback for third '_lastOplogEntryFetcher' callback. This is scheduled after MultiApplier
     * completed successfully and missing documents were fetched from the sync source while
     * DataReplicatorExternalState::_multiApply() was processing operations.
     * This callback will update InitialSyncState::stopTimestamp on success.
     */
    void _lastOplogEntryFetcherCallbackAfterFetchingMissingDocuments(
        const StatusWith<Fetcher::QueryResponse>& result,
        std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Callback for rollback checker's last replSetGetRBID command after cloning data and applying
     * operations.
     */
    void _rollbackCheckerCheckForRollbackCallback(
        const RollbackChecker::Result& result,
        std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Reports result of current initial sync attempt. May schedule another initial sync attempt
     * depending on shutdown state and whether we've exhausted all initial sync retries.
     */
    void _finishInitialSyncAttempt(const StatusWith<OpTimeWithHash>& lastApplied);

    /**
     * Invokes completion callback and transitions state to State::kComplete.
     */
    void _finishCallback(StatusWith<OpTimeWithHash> lastApplied);

    // Obtains a valid sync source from the sync source selector.
    // Returns error if a sync source cannot be found.
    StatusWith<HostAndPort> _chooseSyncSource_inlock();

    /**
     * Pushes documents from oplog fetcher to blocking queue for
     * applier to consume.
     *
     * Returns a status even though it always returns OK, to conform the interface OplogFetcher
     * expects for the EnqueueDocumentsFn.
     */
    Status _enqueueDocuments(Fetcher::Documents::const_iterator begin,
                             Fetcher::Documents::const_iterator end,
                             const OplogFetcher::DocumentsInfo& info);

    void _appendInitialSyncProgressMinimal_inlock(BSONObjBuilder* bob) const;
    BSONObj _getInitialSyncProgress_inlock() const;

    StatusWith<MultiApplier::Operations> _getNextApplierBatch_inlock();

    /**
     * Schedules a fetcher to get the last oplog entry from the sync source.
     */
    Status _scheduleLastOplogEntryFetcher_inlock(Fetcher::CallbackFn callback);

    /**
     * Checks the current oplog application progress (begin and end timestamps).
     * If necessary, schedules a _getNextApplierBatchCallback() task.
     * If the stop and end timestamps are inconsistent or if there is an issue scheduling the task,
     * we set the error status in 'onCompletionGuard' and shut down the OplogFetcher.
     * Passes 'lock' through to completion guard.
     */
    void _checkApplierProgressAndScheduleGetNextApplierBatch_inlock(
        const stdx::lock_guard<stdx::mutex>& lock,
        std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Schedules a rollback checker to get the rollback ID after data cloning or applying. This
     * helps us check if a rollback occurred on the sync source.
     * If we fail to schedule the rollback checker, we set the error status in 'onCompletionGuard'
     * and shut down the OplogFetcher.
     * Passes 'lock' through to completion guard.
     */
    void _scheduleRollbackCheckerCheckForRollback_inlock(
        const stdx::lock_guard<stdx::mutex>& lock,
        std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Checks the given status (or embedded status inside the callback args) and current data
     * replicator shutdown state. If the given status is not OK or if we are shutting down, returns
     * a new error status that should be passed to _finishCallback. The reason in the new error
     * status will include 'message'.
     * Otherwise, returns Status::OK().
     */
    Status _checkForShutdownAndConvertStatus_inlock(
        const executor::TaskExecutor::CallbackArgs& callbackArgs, const std::string& message);
    Status _checkForShutdownAndConvertStatus_inlock(const Status& status,
                                                    const std::string& message);

    /**
     * Schedules work to be run by the task executor.
     * Saves handle if work was successfully scheduled.
     * Returns scheduleWork status (without the handle).
     */
    Status _scheduleWorkAndSaveHandle_inlock(const executor::TaskExecutor::CallbackFn& work,
                                             executor::TaskExecutor::CallbackHandle* handle,
                                             const std::string& name);
    Status _scheduleWorkAtAndSaveHandle_inlock(Date_t when,
                                               const executor::TaskExecutor::CallbackFn& work,
                                               executor::TaskExecutor::CallbackHandle* handle,
                                               const std::string& name);

    /**
     * Cancels task executor callback handle if not null.
     */
    void _cancelHandle_inlock(executor::TaskExecutor::CallbackHandle handle);

    /**
     * Starts up component and checks initial syncer's shutdown state at the same time.
     * If component's startup() fails, resets 'component' (which is assumed to be a unique_ptr
     * to the component type).
     */
    template <typename Component>
    Status _startupComponent_inlock(Component& component);

    /**
     * Shuts down component if not null.
     */
    template <typename Component>
    void _shutdownComponent_inlock(Component& component);

    // Counts how many documents have been refetched from the source in the current batch.
    AtomicUInt32 _fetchCount;

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access in any way from any context.
    // (M)  Reads and writes guarded by _mutex
    // (X)  Reads and writes must be performed in a callback in _exec
    // (MX) Must hold _mutex and be in a callback in _exec to write; must either hold
    //      _mutex or be in a callback in _exec to read.

    mutable stdx::mutex _mutex;                                                 // (S)
    const InitialSyncerOptions _opts;                                           // (R)
    std::unique_ptr<DataReplicatorExternalState> _dataReplicatorExternalState;  // (R)
    executor::TaskExecutor* _exec;                                              // (R)
    ThreadPool* _writerPool;                                                    // (R)
    StorageInterface* _storage;                                                 // (R)
    ReplicationProcess* _replicationProcess;                                    // (S)

    // This is invoked with the final status of the initial sync. If startup() fails, this callback
    // is never invoked. The caller gets the last applied optime with hash when the initial sync
    // completes successfully or an error status.
    // '_onCompletion' is cleared on completion (in _finishCallback()) in order to release any
    // resources that might be held by the callback function object.
    OnCompletionFn _onCompletion;  // (M)

    // Handle to currently scheduled _startInitialSyncAttemptCallback() task.
    executor::TaskExecutor::CallbackHandle _startInitialSyncAttemptHandle;  // (M)

    // Handle to currently scheduled _chooseSyncSourceCallback() task.
    executor::TaskExecutor::CallbackHandle _chooseSyncSourceHandle;  // (M)

    // RollbackChecker to get rollback ID before and after each initial sync attempt.
    std::unique_ptr<RollbackChecker> _rollbackChecker;  // (M)

    // Handle returned from RollbackChecker::reset().
    RollbackChecker::CallbackHandle _getBaseRollbackIdHandle;  // (M)

    // Handle returned from RollbackChecker::checkForRollback().
    RollbackChecker::CallbackHandle _getLastRollbackIdHandle;  // (M)

    // Handle to currently scheduled _getNextApplierBatchCallback() task.
    executor::TaskExecutor::CallbackHandle _getNextApplierBatchHandle;  // (M)

    std::unique_ptr<InitialSyncState> _initialSyncState;  // (M)
    std::unique_ptr<OplogFetcher> _oplogFetcher;          // (S)
    std::unique_ptr<Fetcher> _lastOplogEntryFetcher;      // (S)
    std::unique_ptr<Fetcher> _fCVFetcher;                 // (S)
    std::unique_ptr<MultiApplier> _applier;               // (M)
    HostAndPort _syncSource;                              // (M)
    OpTimeWithHash _lastFetched;                          // (MX)
    OpTimeWithHash _lastApplied;                          // (MX)
    std::unique_ptr<OplogBuffer> _oplogBuffer;            // (M)
    std::unique_ptr<OplogApplier::Observer> _observer;    // (S)
    std::unique_ptr<OplogApplier> _oplogApplier;          // (M)

    // Used to signal changes in _state.
    mutable stdx::condition_variable _stateCondition;

    // Current initial syncer state. See comments for State enum class for details.
    State _state = State::kPreStart;  // (M)

    // Passed to CollectionCloner via DatabasesCloner.
    CollectionCloner::ScheduleDbWorkFn _scheduleDbWorkFn;  // (M)

    // Contains stats on the current initial sync request (includes all attempts).
    // To access these stats in a user-readable format, use getInitialSyncProgress().
    Stats _stats;  // (M)
};

}  // namespace repl
}  // namespace mongo
