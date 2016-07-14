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

#include <memory>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

class QueryFetcher;

namespace repl {

namespace {
using CallbackArgs = executor::TaskExecutor::CallbackArgs;
using Event = executor::TaskExecutor::EventHandle;
using Handle = executor::TaskExecutor::CallbackHandle;
using Operations = MultiApplier::Operations;
using QueryResponseStatus = StatusWith<Fetcher::QueryResponse>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

}  // namespace


extern const int kInitialSyncMaxRetries;

// TODO: Remove forward declares once we remove rs_initialsync.cpp and other dependents.
// Failpoint which fails initial sync and leaves an oplog entry in the buffer.
MONGO_FP_FORWARD_DECLARE(failInitSyncWithBufferedEntriesLeft);

// Failpoint which causes the initial sync function to hang before copying databases.
MONGO_FP_FORWARD_DECLARE(initialSyncHangBeforeCopyingDatabases);

// Failpoint which causes the initial sync function to hang before calling shouldRetry on a failed
// operation.
MONGO_FP_FORWARD_DECLARE(initialSyncHangBeforeGettingMissingDocument);

// Failpoint which stops the applier.
MONGO_FP_FORWARD_DECLARE(rsSyncApplyStop);

struct InitialSyncState;
struct MemberState;
class RollbackChecker;
class StorageInterface;


/** State for decision tree */
enum class DataReplicatorState {
    Steady,  // Default
    InitialSync,
    Rollback,
    Uninitialized,
};


// Helper to convert enum to a string.
std::string toString(DataReplicatorState s);

// TBD -- ignore for now
enum class DataReplicatorScope { ReplicateAll, ReplicateDB, ReplicateCollection };

struct DataReplicatorOptions {
    /**
     * Function to rollback operations on the current node to a common point with
     * the sync source.
     *
     * In production, this function should invoke syncRollback (rs_rollback.h) using the
     * OperationContext to create a OplogInterfaceLocal; the HostAndPort to create a
     * DBClientConnection for the RollbackSourceImpl. The reference to the ReplicationCoordinator
     * can be provided separately.
     * */
    using RollbackFn = stdx::function<Status(OperationContext*, const OpTime&, const HostAndPort&)>;

    /** Function to return optime of last operation applied on this node */
    using GetMyLastOptimeFn = stdx::function<OpTime()>;

    /** Function to update optime of last operation applied on this node */
    using SetMyLastOptimeFn = stdx::function<void(const OpTime&)>;

    /** Function to sets this node into a specific follower mode. */
    using SetFollowerModeFn = stdx::function<bool(const MemberState&)>;

    /** Function to get this node's slaveDelay. */
    using GetSlaveDelayFn = stdx::function<Seconds()>;

    // Error and retry values
    Milliseconds syncSourceRetryWait{1000};
    Milliseconds initialSyncRetryWait{1000};
    Seconds blacklistSyncSourcePenaltyForNetworkConnectionError{10};
    Minutes blacklistSyncSourcePenaltyForOplogStartMissing{10};

    // Batching settings.
    size_t replBatchLimitBytes = 512 * 1024 * 1024;
    size_t replBatchLimitOperations = 5000;

    // Replication settings
    NamespaceString localOplogNS = NamespaceString("local.oplog.rs");
    NamespaceString remoteOplogNS = NamespaceString("local.oplog.rs");

    // TBD -- ignore below for now
    DataReplicatorScope scope = DataReplicatorScope::ReplicateAll;
    std::string scopeNS;
    BSONObj filterCriteria;

    RollbackFn rollbackFn;
    Reporter::PrepareReplSetUpdatePositionCommandFn prepareReplSetUpdatePositionCommandFn;
    GetMyLastOptimeFn getMyLastOptime;
    SetMyLastOptimeFn setMyLastOptime;
    SetFollowerModeFn setFollowerMode;
    GetSlaveDelayFn getSlaveDelay;

    SyncSourceSelector* syncSourceSelector = nullptr;

    std::string toString() const {
        return str::stream() << "DataReplicatorOptions -- "
                             << " localOplogNs: " << localOplogNS.toString()
                             << " remoteOplogNS: " << remoteOplogNS.toString();
    }
};

/**
 * The data replicator provides services to keep collection in sync by replicating
 * changes via an oplog source to the local system storage.
 *
 * This class will use existing machinery like the Executor to schedule work and
 * network tasks, as well as provide serial access and synchronization of state.
 *
 *
 * Entry Points:
 *      -- doInitialSync: Will drop all data and copy to a consistent state of data (via the oplog).
 *      -- startup: Start data replication from existing data.
 */
class DataReplicator {
public:
    DataReplicator(DataReplicatorOptions opts,
                   std::unique_ptr<DataReplicatorExternalState> dataReplicatorExternalState,
                   StorageInterface* storage);

    virtual ~DataReplicator();

    // Starts steady-state replication. This will *not* do an initial sync implicitly.
    Status start(OperationContext* txn);

    // Shuts down replication if "start" has been called, and blocks until shutdown has completed.
    Status shutdown(OperationContext* txn);

    /**
     * Cancels outstanding work and begins shutting down.
     */
    Status scheduleShutdown(OperationContext* txn);

    /**
     * Waits for data replicator to finish shutting down.
     * Data replicator will go into uninitialized state.
     */
    void waitForShutdown();

    // Resumes apply replication events from the oplog
    Status resume(bool wait = false);

    // Pauses replication and application
    Status pause();

    // Pauses replication and waits to return until all un-applied ops have been applied
    StatusWith<Timestamp> flushAndPause();

    // Called when a slave has progressed to a new oplog position
    void slavesHaveProgressed();

    // Just like initialSync but can be called any time.
    StatusWith<Timestamp> resync(OperationContext* txn);

    /**
     *  Does an initial sync, with up to 'kInitialSyncMaxRetries' retries.
     *
     *  This should be the first method called after construction (see class comment).
     */
    StatusWith<OpTimeWithHash> doInitialSync(OperationContext* txn);

    DataReplicatorState getState() const;

    /**
     * Waits until data replicator state becomes 'state'.
     */
    void waitForState(const DataReplicatorState& state);

    HostAndPort getSyncSource() const;
    OpTimeWithHash getLastFetched() const;
    OpTimeWithHash getLastApplied() const;

    /**
     * Number of operations in the oplog buffer.
     */
    size_t getOplogBufferCount() const;

    std::string getDiagnosticString() const;

    // For testing only

    void _resetState_inlock(OperationContext* txn, OpTimeWithHash lastAppliedOpTime);

private:
    // Runs a single initial sync attempt.
    Status _runInitialSyncAttempt_inlock(OperationContext* txn,
                                         UniqueLock& lk,
                                         const HostAndPort& syncSource,
                                         RollbackChecker& rollbackChecker);

    void _setState(const DataReplicatorState& newState);
    void _setState_inlock(const DataReplicatorState& newState);

    // Returns OK when there is a good syncSource at _syncSource.
    Status _ensureGoodSyncSource_inlock();

    // Only executed via executor
    void _resumeFinish(CallbackArgs cbData);

    /**
     * Pushes documents from oplog fetcher to blocking queue for
     * applier to consume.
     */
    void _enqueueDocuments(Fetcher::Documents::const_iterator begin,
                           Fetcher::Documents::const_iterator end,
                           const OplogFetcher::DocumentsInfo& info,
                           Milliseconds elapsed);
    void _onOplogFetchFinish(const Status& status, const OpTimeWithHash& lastFetched);
    void _rollbackOperations(const CallbackArgs& cbData);
    void _doNextActions();
    void _doNextActions_InitialSync_inlock();
    void _doNextActions_Rollback_inlock();
    void _doNextActions_Steady_inlock();

    // Applies up till the specified Timestamp and pauses automatic application
    Timestamp _applyUntilAndPause(Timestamp);
    Timestamp _applyUntil(Timestamp);
    void _pauseApplier();

    StatusWith<Operations> _getNextApplierBatch_inlock();
    void _onApplyBatchFinish(const StatusWith<Timestamp>&,
                             const Operations&,
                             const size_t numApplied);

    // Called when the DatabasesCloner finishes.
    void _onDataClonerFinish(const Status& status);
    // Called after _onDataClonerFinish when the new Timestamp is avail, to use for minvalid.
    void _onApplierReadyStart(const QueryResponseStatus& fetchResult);

    Status _scheduleDoNextActions();
    Status _scheduleApplyBatch();
    Status _scheduleApplyBatch_inlock();
    Status _scheduleApplyBatch_inlock(const Operations& ops);
    Status _scheduleFetch();
    Status _scheduleFetch_inlock();
    Status _scheduleReport();

    void _cancelAllHandles_inlock();
    void _waitOnAndResetAll(UniqueLock& lk);
    bool _anyActiveHandles_inlock() const;

    Status _shutdown(OperationContext* txn);
    void _changeStateIfNeeded();

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
    const DataReplicatorOptions _opts;                                          // (R)
    std::unique_ptr<DataReplicatorExternalState> _dataReplicatorExternalState;  // (R)
    executor::TaskExecutor* _exec;                                              // (R)
    stdx::condition_variable _stateCondition;                                   // (R)
    DataReplicatorState _state;                                                 // (MX)
    std::unique_ptr<InitialSyncState> _initialSyncState;                        // (M)
    StorageInterface* _storage;                                                 // (M)
    bool _fetcherPaused = false;                                                // (X)
    std::unique_ptr<OplogFetcher> _oplogFetcher;                                // (S)
    std::unique_ptr<Fetcher> _lastOplogEntryFetcher;                            // (S)
    bool _reporterPaused = false;                                               // (M)
    Handle _reporterHandle;                                                     // (M)
    std::unique_ptr<Reporter> _reporter;                                        // (M)
    bool _applierActive = false;                                                // (M)
    bool _applierPaused = false;                                                // (X)
    std::unique_ptr<MultiApplier> _applier;                                     // (M)
    HostAndPort _syncSource;                                                    // (M)
    OpTimeWithHash _lastFetched;                                                // (MX)
    OpTimeWithHash _lastApplied;                                                // (MX)
    std::unique_ptr<OplogBuffer> _oplogBuffer;                                  // (M)
    Event _onShutdown;                                                          // (M)
    Timestamp _rollbackCommonOptime;                                            // (MX)
};

}  // namespace repl
}  // namespace mongo
