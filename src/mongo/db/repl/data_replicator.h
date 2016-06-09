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

#include <vector>

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/data_replicator_external_state.h"
#include "mongo/db/repl/database_cloner.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplog_buffer.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/db/repl/sync_source_selector.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/queue.h"

namespace mongo {

class QueryFetcher;

namespace repl {

using Operations = MultiApplier::Operations;
using QueryResponseStatus = StatusWith<Fetcher::QueryResponse>;
using CallbackArgs = ReplicationExecutor::CallbackArgs;
using CBHStatus = StatusWith<ReplicationExecutor::CallbackHandle>;
using CommandCallbackArgs = ReplicationExecutor::RemoteCommandCallbackArgs;
using Event = ReplicationExecutor::EventHandle;
using Handle = ReplicationExecutor::CallbackHandle;
using LockGuard = stdx::lock_guard<stdx::mutex>;
using NextAction = Fetcher::NextAction;
using Request = executor::RemoteCommandRequest;
using Response = executor::RemoteCommandResponse;
using TimestampStatus = StatusWith<Timestamp>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

struct InitialSyncState;
struct MemberState;
class ReplicationProgressManager;
class SyncSourceSelector;

/** State for decision tree */
enum class DataReplicatorState {
    Steady,  // Default
    InitialSync,
    Rollback,
    Uninitialized,
};

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

    /** Function to get current replica set configuration */
    using GetReplSetConfigFn = stdx::function<ReplicaSetConfig()>;

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
    GetReplSetConfigFn getReplSetConfig;

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
 */
class DataReplicator {
public:
    DataReplicator(DataReplicatorOptions opts,
                   std::unique_ptr<DataReplicatorExternalState> dataReplicatorExternalState,
                   ReplicationExecutor* exec);

    virtual ~DataReplicator();

    Status start();
    Status shutdown();

    /**
     * Cancels outstanding work and begins shutting down.
     */
    Status scheduleShutdown();

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
    TimestampStatus flushAndPause();

    // Called when a slave has progressed to a new oplog position
    void slavesHaveProgressed();

    // just like initialSync but can be called anytime.
    TimestampStatus resync(OperationContext* txn);

    // Don't use above methods before these
    TimestampStatus initialSync(OperationContext* txn);

    DataReplicatorState getState() const;

    /**
     * Waits until data replicator state becomes 'state'.
     */
    void waitForState(const DataReplicatorState& state);

    HostAndPort getSyncSource() const;
    Timestamp getLastTimestampFetched() const;
    Timestamp getLastTimestampApplied() const;

    /**
     * Number of operations in the oplog buffer.
     */
    size_t getOplogBufferCount() const;

    std::string getDiagnosticString() const;

    // For testing only

    void _resetState_inlock(Timestamp lastAppliedOpTime);
    void _setInitialSyncStorageInterface(CollectionCloner::StorageInterface* si);

private:
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
    void _onApplyBatchFinish(const CallbackArgs&,
                             const TimestampStatus&,
                             const Operations&,
                             const size_t numApplied);
    void _handleFailedApplyBatch(const TimestampStatus&, const Operations&);
    // Fetches the last doc from the first operation, and reschedules the apply for the ops.
    void _scheduleApplyAfterFetch(const Operations&);
    void _onMissingFetched(const QueryResponseStatus& fetchResult,
                           const Operations& ops,
                           const NamespaceString nss);

    void _onDataClonerFinish(const Status& status);
    // Called after _onDataClonerFinish when the new Timestamp is avail, to use for minvalid
    void _onApplierReadyStart(const QueryResponseStatus& fetchResult);

    Status _scheduleApplyBatch();
    Status _scheduleApplyBatch_inlock();
    Status _scheduleApplyBatch_inlock(const Operations& ops);
    Status _scheduleFetch();
    Status _scheduleFetch_inlock();
    Status _scheduleReport();

    void _cancelAllHandles_inlock();
    void _waitOnAll_inlock();
    bool _anyActiveHandles_inlock() const;

    Status _shutdown();
    void _changeStateIfNeeded();

    // Set during construction
    const DataReplicatorOptions _opts;
    std::unique_ptr<DataReplicatorExternalState> _dataReplicatorExternalState;
    ReplicationExecutor* _exec;

    //
    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access in any way from any context.
    // (PS) Pointer is read-only in concurrent operation, item pointed to is self-synchronizing;
    //      Access in any context.
    // (M)  Reads and writes guarded by _mutex
    // (X)  Reads and writes must be performed in a callback in _exec
    // (MX) Must hold _mutex and be in a callback in _exec to write; must either hold
    //      _mutex or be in a callback in _exec to read.
    // (I)  Independently synchronized, see member variable comment.

    // Protects member data of this DataReplicator.
    mutable stdx::mutex _mutex;  // (S)

    stdx::condition_variable _stateCondition;
    DataReplicatorState _state;  // (MX)

    // initial sync state
    std::unique_ptr<InitialSyncState> _initialSyncState;  // (M)
    CollectionCloner::StorageInterface* _storage;         // (M)

    // set during scheduling and onFinish
    bool _fetcherPaused;                     // (X)
    std::unique_ptr<OplogFetcher> _fetcher;  // (S)
    std::unique_ptr<Fetcher> _tmpFetcher;    // (S)

    bool _reporterPaused;                 // (M)
    Handle _reporterHandle;               // (M)
    std::unique_ptr<Reporter> _reporter;  // (M)

    bool _applierActive;                     // (M)
    bool _applierPaused;                     // (X)
    std::unique_ptr<MultiApplier> _applier;  // (M)

    HostAndPort _syncSource;                    // (M)
    Timestamp _lastTimestampFetched;            // (MX)
    Timestamp _lastTimestampApplied;            // (MX)
    std::unique_ptr<OplogBuffer> _oplogBuffer;  // (M)

    // Shutdown
    Event _onShutdown;  // (M)

    // Rollback stuff
    Timestamp _rollbackCommonOptime;  // (MX)
};

}  // namespace repl
}  // namespace mongo
