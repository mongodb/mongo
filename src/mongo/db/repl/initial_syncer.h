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

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/callback_completion_guard.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/initial_sync_shared_data.h"
#include "mongo/db/repl/initial_syncer_interface.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/rollback_checker.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

// TODO: Remove forward declares once we remove rs_initialsync.cpp and other dependents.
// Failpoint which fails initial sync and leaves an oplog entry in the buffer.
extern FailPoint failInitSyncWithBufferedEntriesLeft;

// Failpoint which causes the initial sync function to hang before copying databases.
extern FailPoint initialSyncHangBeforeCopyingDatabases;

// Failpoint which stops the applier.
extern FailPoint rsSyncApplyStop;

struct InitialSyncState;
struct MemberState;
class ReplicationProcess;
class StorageInterface;

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
class InitialSyncer : public InitialSyncerInterface {
    InitialSyncer(const InitialSyncer&) = delete;
    InitialSyncer& operator=(const InitialSyncer&) = delete;

public:
    /**
     * Callback completion guard for initial syncer.
     */
    using OnCompletionGuard = CallbackCompletionGuard<StatusWith<OpTimeAndWallTime>>;

    struct InitialSyncAttemptInfo {
        int durationMillis;
        Status status;
        HostAndPort syncSource;
        int rollBackId;
        int operationsRetried;
        int totalTimeUnreachableMillis;

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    class OplogFetcherRestartDecisionInitialSyncer
        : public OplogFetcher::OplogFetcherRestartDecision {

    public:
        OplogFetcherRestartDecisionInitialSyncer(InitialSyncSharedData* sharedData,
                                                 std::size_t maxFetcherRestarts)
            : _sharedData(sharedData), _defaultDecision(maxFetcherRestarts){};

        bool shouldContinue(OplogFetcher* fetcher, Status status) final;

        void fetchSuccessful(OplogFetcher* fetcher) final;

    private:
        InitialSyncSharedData* _sharedData;

        // We delegate to the default strategy when it's a non-network error.
        OplogFetcher::OplogFetcherRestartDecisionDefault _defaultDecision;

        // The operation, if any, currently being retried because of a network error.
        InitialSyncSharedData::RetryableOperation _retryingOperation;
    };

    struct Stats {
        std::uint32_t failedInitialSyncAttempts{0};
        std::uint32_t maxFailedInitialSyncAttempts{0};
        Date_t initialSyncStart;
        Date_t initialSyncEnd;
        std::vector<InitialSyncer::InitialSyncAttemptInfo> initialSyncAttemptInfos;
        std::weak_ptr<executor::TaskExecutor> exec;

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    InitialSyncer(InitialSyncerInterface::Options opts,
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

    std::string getInitialSyncMethod() const final;

    bool allowLocalDbAccess() const final {
        return true;
    }

    Status startup(OperationContext* opCtx, std::uint32_t maxAttempts) noexcept final;

    Status shutdown() final;

    void join() final;

    /**
     * Returns internal state in a loggable format.
     */
    std::string getDiagnosticString() const;

    BSONObj getInitialSyncProgress() const final;

    void cancelCurrentAttempt() final;

    /**
     *
     * Overrides how the initial syncer creates the client.
     *
     * For testing only
     */
    void setCreateClientFn_forTest(const CreateClientFn& createClientFn);

    /**
     *
     * Overrides how the initial syncer creates the OplogFetcher.
     *
     * For testing only.
     */
    void setCreateOplogFetcherFn_forTest(std::unique_ptr<OplogFetcherFactory> createOplogFetcherFn);

    /**
     *
     * Get a raw pointer to the OplogFetcher. Block up to 10s until the underlying OplogFetcher has
     * started. It is the caller's responsibility to not reuse this pointer beyond the lifetime of
     * the underlying OplogFetcher.
     *
     * For testing only.
     */
    OplogFetcher* getOplogFetcher_forTest() const;

    /**
     *
     * Provides a separate executor for the cloners, so network operations based on
     * TaskExecutor::scheduleRemoteCommand() can use the NetworkInterfaceMock while the cloners
     * are stopped on a failpoint.
     *
     * For testing only
     */
    void setClonerExecutor_forTest(std::shared_ptr<executor::TaskExecutor> clonerExec);

    /**
     *
     * Wait for the cloner thread to finish.
     *
     * For testing only
     */
    void waitForCloner_forTest();

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

    /**
     * Returns the wall clock time component of _lastApplied.
     * For testing only.
     */
    Date_t getWallClockTime_forTest() const;

    /**
     * Sets the allowed outage duration in _sharedData.
     * For testing only.
     */
    void setAllowedOutageDuration_forTest(Milliseconds allowedOutageDuration);

private:
    enum LastOplogEntryFetcherRetryStrategy {
        kFetcherHandlesRetries,
        kInitialSyncerHandlesRetries
    };

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
     *   _lastOplogEntryFetcherCallbackForDefaultBeginFetchingOpTime()
     *         |
     *         |
     *         V
     *   _getBeginFetchingOpTimeCallback()
     *         |
     *         |
     *         V
     *    _lastOplogEntryFetcherCallbackForBeginApplyingTimestamp()
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
     *    _oplogFetcherCallback()         _allDatabaseClonerCallback
     *         |                              |
     *         |                              |
     *         |                              V
     *         |                          _lastOplogEntryFetcherCallbackForStopTimestamp()
     *         |                              |       |
     *         |                              |       |
     *         |            (no ops to apply) |       | (have ops to apply)
     *         |                              |       |
     *         |                              |       V
     *         |                              |   _getNextApplierBatchCallback()
     *         |                              |       |                       ^
     *         |                              |       |                       |
     *         |                              |       |             (end ts not reached)
     *         |                              |       |                       |
     *         |                              |       V                       |
     *         |                              |   _multiApplierCallback()-----+
     *         |                              |       |
     *         |                              |       |
     *         |                        (reached end timestamp)
     *         |                              |       |
     *         |                              V       V
     *         |                _rollbackCheckerCheckForRollbackCallback()
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
    void _tearDown_inlock(OperationContext* opCtx,
                          const StatusWith<OpTimeAndWallTime>& lastApplied);

    /**
     * Callback to start a single initial sync attempt.
     */
    void _startInitialSyncAttemptCallback(const executor::TaskExecutor::CallbackArgs& callbackArgs,
                                          std::uint32_t initialSyncAttempt,
                                          std::uint32_t initialSyncMaxAttempts) noexcept;

    /**
     * Callback to obtain sync source from sync source selector.
     * For every initial sync attempt, we will try up to 'numInitialSyncConnectAttempts' times (at
     * an interval of '_opts.syncSourceRetryWait' ms) to obtain a valid sync source before giving up
     * and returning ErrorCodes::InitialSyncOplogSourceMissing.
     */
    void _chooseSyncSourceCallback(const executor::TaskExecutor::CallbackArgs& callbackArgs,
                                   std::uint32_t chooseSyncSourceAttempt,
                                   std::uint32_t chooseSyncSourceMaxAttempts,
                                   std::shared_ptr<OnCompletionGuard> onCompletionGuard) noexcept;

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
     * determine the default starting point for tailing the oplog using the OplogFetcher if there
     * are no active transactions on the sync source. This will be used as the default for the
     * beginFetchingTimestamp.
     */
    void _lastOplogEntryFetcherCallbackForDefaultBeginFetchingOpTime(
        const StatusWith<Fetcher::QueryResponse>& result,
        std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Schedules a remote command to issue a find command on sync source's transaction table, which
     * will get us the optime of the oldest active transaction on that node. It will be used as the
     * beginFetchingTimestamp.
     */
    Status _scheduleGetBeginFetchingOpTime_inlock(
        std::shared_ptr<OnCompletionGuard> onCompletionGuard,
        const OpTime& defaultBeginFetchingOpTime);

    /**
     * Callback that gets the optime of the oldest active transaction in the sync source's
     * transaction table. It will be used as the beginFetchingTimestamp.
     */
    void _getBeginFetchingOpTimeCallback(const StatusWith<Fetcher::QueryResponse>& result,
                                         std::shared_ptr<OnCompletionGuard> onCompletionGuard,
                                         const OpTime& defaultBeginFetchingOpTime);

    /**
     * Callback for second '_lastOplogEntryFetcher' callback. A successful response lets us
     * determine the starting point for applying oplog entries during the oplog application phase
     * as well as setting a reference point for the state of the sync source's oplog when data
     * cloning completes.
     */
    void _lastOplogEntryFetcherCallbackForBeginApplyingTimestamp(
        const StatusWith<Fetcher::QueryResponse>& result,
        std::shared_ptr<OnCompletionGuard> onCompletionGuard,
        OpTime& beginFetchingOpTime);

    /**
     * Callback for the '_fCVFetcher'. A successful response lets us check if the remote node
     * is in a currently acceptable fCV and if it has a 'targetVersion' set.
     */
    void _fcvFetcherCallback(const StatusWith<Fetcher::QueryResponse>& result,
                             std::shared_ptr<OnCompletionGuard> onCompletionGuard,
                             const OpTime& lastOpTime,
                             OpTime& beginFetchingOpTime);

    /**
     * Callback for oplog fetcher.
     */
    void _oplogFetcherCallback(const Status& status,
                               std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Callback for DatabasesCloner.
     */
    void _allDatabaseClonerCallback(const Status& status,
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
    void _getNextApplierBatchCallback(
        const executor::TaskExecutor::CallbackArgs& callbackArgs,
        std::shared_ptr<OnCompletionGuard> onCompletionGuard) noexcept;

    /**
     * Callback for MultiApplier completion.
     */
    void _multiApplierCallback(const Status& status,
                               OpTimeAndWallTime lastApplied,
                               std::uint32_t numApplied,
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
    void _finishInitialSyncAttempt(const StatusWith<OpTimeAndWallTime>& lastApplied);

    /**
     * Invokes completion callback and transitions state to State::kComplete.
     */
    void _finishCallback(StatusWith<OpTimeAndWallTime> lastApplied);

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
    Status _enqueueDocuments(OplogFetcher::Documents::const_iterator begin,
                             OplogFetcher::Documents::const_iterator end,
                             const OplogFetcher::DocumentsInfo& info);

    void _appendInitialSyncProgressMinimal_inlock(BSONObjBuilder* bob) const;
    BSONObj _getInitialSyncProgress_inlock() const;

    StatusWith<std::vector<OplogEntry>> _getNextApplierBatch_inlock();

    /**
     * Schedules a fetcher to get the last oplog entry from the sync source.
     *
     * If 'retryStrategy' is 'kFetcherHandlesRetries', the fetcher will retry up to the server
     * parameter 'numInitialSyncOplogFindAttempts' times. Otherwise any failures must be handled by
     * the caller.
     */
    Status _scheduleLastOplogEntryFetcher_inlock(Fetcher::CallbackFn callback,
                                                 LastOplogEntryFetcherRetryStrategy retryStrategy);

    /**
     * Checks the current oplog application progress (begin and end timestamps).
     * If necessary, schedules a _getNextApplierBatchCallback() task.
     * If the stop and end timestamps are inconsistent or if there is an issue scheduling the task,
     * we set the error status in 'onCompletionGuard' and shut down the OplogFetcher.
     * Passes 'lock' through to completion guard.
     */
    void _checkApplierProgressAndScheduleGetNextApplierBatch_inlock(
        const stdx::lock_guard<Latch>& lock, std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Schedules a rollback checker to get the rollback ID after data cloning or applying. This
     * helps us check if a rollback occurred on the sync source.
     * If we fail to schedule the rollback checker, we set the error status in 'onCompletionGuard'
     * and shut down the OplogFetcher.
     * Passes 'lock' through to completion guard.
     */
    void _scheduleRollbackCheckerCheckForRollback_inlock(
        const stdx::lock_guard<Latch>& lock, std::shared_ptr<OnCompletionGuard> onCompletionGuard);

    /**
     * Check if a status is one which means there's a retriable error and we should retry the
     * current operation, and records whether an operation is currently being retried.  Note this
     * can only handle one operation at a time (i.e. it should not be used in both parts of the
     * "split" section of Initial Sync)
     */
    bool _shouldRetryError(WithLock lk, Status status);

    /**
     * Indicates we are no longer handling a retriable error.
     */
    void _clearRetriableError(WithLock lk);

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
    Status _scheduleWorkAndSaveHandle_inlock(executor::TaskExecutor::CallbackFn work,
                                             executor::TaskExecutor::CallbackHandle* handle,
                                             const std::string& name);
    Status _scheduleWorkAtAndSaveHandle_inlock(Date_t when,
                                               executor::TaskExecutor::CallbackFn work,
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
    AtomicWord<unsigned> _fetchCount;

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

    mutable Mutex _mutex = MONGO_MAKE_LATCH("InitialSyncer::_mutex");           // (S)
    const InitialSyncerInterface::Options _opts;                                // (R)
    std::unique_ptr<DataReplicatorExternalState> _dataReplicatorExternalState;  // (R)
    std::shared_ptr<executor::TaskExecutor> _exec;                              // (R)
    std::unique_ptr<executor::ScopedTaskExecutor> _attemptExec;                 // (X)
    // The executor that the Cloner thread runs on.  In production code this is the same as _exec,
    // but for unit testing, _exec is single-threaded and our NetworkInterfaceMock runs it in
    // lockstep with the unit test code.  If we pause the cloners using failpoints
    // NetworkInterfaceMock is unaware of this and this causes our unit tests to deadlock.
    std::shared_ptr<executor::TaskExecutor> _clonerExec;               // (R)
    std::unique_ptr<executor::ScopedTaskExecutor> _clonerAttemptExec;  // (X)
    ThreadPool* _writerPool;                                           // (R)
    StorageInterface* _storage;                                        // (R)
    ReplicationProcess* _replicationProcess;                           // (S)

    // This is invoked with the final status of the initial sync. If startup() fails, this callback
    // is never invoked. The caller gets the last applied optime when the initial sync completes
    // successfully or an error status.
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

    // The operation, if any, currently being retried because of a network error.
    InitialSyncSharedData::RetryableOperation _retryingOperation;  // (M)

    std::unique_ptr<InitialSyncState> _initialSyncState;   // (M)
    std::unique_ptr<OplogFetcher> _oplogFetcher;           // (S)
    std::unique_ptr<Fetcher> _beginFetchingOpTimeFetcher;  // (S)
    std::unique_ptr<Fetcher> _lastOplogEntryFetcher;       // (S)
    std::unique_ptr<Fetcher> _fCVFetcher;                  // (S)
    std::unique_ptr<MultiApplier> _applier;                // (M)
    HostAndPort _syncSource;                               // (M)
    std::unique_ptr<DBClientConnection> _client;           // (M)
    OpTime _lastFetched;                                   // (MX)
    OpTimeAndWallTime _lastApplied;                        // (MX)

    std::unique_ptr<OplogBuffer> _oplogBuffer;    // (M)
    std::unique_ptr<OplogApplier> _oplogApplier;  // (M)

    // Used to signal changes in _state.
    mutable stdx::condition_variable _stateCondition;

    // Current initial syncer state. See comments for State enum class for details.
    State _state = State::kPreStart;  // (M)

    // Used to create the DBClientConnection for the cloners
    CreateClientFn _createClientFn;

    // Used to create the OplogFetcher for the InitialSyncer.
    std::unique_ptr<OplogFetcherFactory> _createOplogFetcherFn;

    // Contains stats on the current initial sync request (includes all attempts).
    // To access these stats in a user-readable format, use getInitialSyncProgress().
    Stats _stats;  // (M)

    // Data shared by cloners and fetcher.  Follow InitialSyncSharedData synchronization rules.
    std::unique_ptr<InitialSyncSharedData> _sharedData;  // (S)

    // Amount of time an outage is allowed to continue before the initial sync attempt is marked
    // as failed.
    Milliseconds _allowedOutageDuration;  // (M)

    // The initial sync attempt has been canceled
    bool _attemptCanceled = false;  // (X)
};

}  // namespace repl
}  // namespace mongo
