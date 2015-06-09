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

#include <boost/thread.hpp>
#include <vector>

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/applier.h"
#include "mongo/db/repl/collection_cloner.h"
#include "mongo/db/repl/database_cloner.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/queue.h"

namespace mongo {

class QueryFetcher;

namespace repl {

using Operations = Applier::Operations;
using BatchDataStatus = StatusWith<Fetcher::BatchData>;
using CallbackArgs = ReplicationExecutor::CallbackArgs;
using CBHStatus = StatusWith<ReplicationExecutor::CallbackHandle>;
using CommandCallbackArgs = ReplicationExecutor::RemoteCommandCallbackArgs;
using Event = ReplicationExecutor::EventHandle;
using Handle = ReplicationExecutor::CallbackHandle;
using LockGuard = stdx::lock_guard<stdx::mutex>;
using NextAction = Fetcher::NextAction;
using Request = RemoteCommandRequest;
using Response = RemoteCommandResponse;
using TimestampStatus = StatusWith<Timestamp>;
using UniqueLock = stdx::unique_lock<stdx::mutex>;

class OplogFetcher;
struct InitialSyncState;

/** State for decision tree */
enum class DataReplicatorState {
    Steady, // Default
    InitialSync,
    Rollback,
    Uninitialized,
};

// TBD -- ignore for now
enum class DataReplicatorScope {
    ReplicateAll,
    ReplicateDB,
    ReplicateCollection
};

struct DataReplicatorOptions {
    // Error and retry values
    Milliseconds syncSourceRetryWait{1000};
    Milliseconds initialSyncRetryWait{1000};
    Seconds blacklistSyncSourcePenaltyForNetworkConnectionError{10};
    Minutes blacklistSyncSourcePenaltyForOplogStartMissing{10};

    // Replication settings
    Timestamp startOptime;
    NamespaceString localOplogNS = NamespaceString("local.oplog.rs");
    NamespaceString remoteOplogNS = NamespaceString("local.oplog.rs");

    // TBD -- ignore below for now
    DataReplicatorScope scope = DataReplicatorScope::ReplicateAll;
    std::string scopeNS;
    BSONObj filterCriteria;
    HostAndPort syncSource; // for use without replCoord -- maybe some kind of rsMonitor/interface

    // TODO: replace with real applier function
    Applier::ApplyOperationFn applierFn = [] (OperationContext*, const BSONObj&) -> Status {
        return Status::OK();
    };

    std::string toString() const {
        return str::stream() << "DataReplicatorOptions -- "
                             << " localOplogNs: " << localOplogNS.toString()
                             << " remoteOplogNS: " << remoteOplogNS.toString()
                             << " syncSource: " << syncSource.toString()
                             << " startOptime: " << startOptime.toString();
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
    /** Function to call when a batch is applied. */
    using OnBatchCompleteFn = stdx::function<void (const Timestamp&)>;

    DataReplicator(DataReplicatorOptions opts,
                   ReplicationExecutor* exec,
                   ReplicationCoordinator* replCoord);
    /**
     * Used by non-replication coordinator processes, like sharding.
     */
    DataReplicator(DataReplicatorOptions opts,
                   ReplicationExecutor* exec);

    virtual ~DataReplicator();

    Status start();
    Status shutdown();

    // Resumes apply replication events from the oplog
    Status resume(bool wait=false);

    // Pauses replication and application
    Status pause();

    // Pauses replication and waits to return until all un-applied ops have been applied
    TimestampStatus flushAndPause();

    // Called when a slave has progressed to a new oplog position
    void slavesHaveProgressed();

    // just like initialSync but can be called anytime.
    TimestampStatus resync();

    // Don't use above methods before these
    TimestampStatus initialSync();

    std::string getDiagnosticString() const;

    // For testing only
    void _resetState_inlock(Timestamp lastAppliedOptime);
    void __setSourceForTesting(HostAndPort src) { _syncSource = src; }
    void _setInitialSyncStorageInterface(CollectionCloner::StorageInterface* si);

private:

    // Returns OK when there is a good syncSource at _syncSource.
    Status _ensureGoodSyncSource_inlock();

    // Only executed via executor
    void _resumeFinish(CallbackArgs cbData);
    void _onOplogFetchFinish(const BatchDataStatus& fetchResult,
                             Fetcher::NextAction* nextAction);
    void _doNextActions();
    void _doNextActions_InitialSync_inlock();
    void _doNextActions_Rollback_inlock();
    void _doNextActions_Steady_inlock();

    // Applies up till the specified Timestamp and pauses automatic application
    Timestamp _applyUntilAndPause(Timestamp);
    Timestamp _applyUntil(Timestamp);
    void _pauseApplier();

    Operations _getNextApplierBatch_inlock();
    void _onApplyBatchFinish(const CallbackArgs&,
                             const TimestampStatus&,
                             const Operations&,
                             const size_t numApplied);
    void _handleFailedApplyBatch(const TimestampStatus&, const Operations&);
    // Fetches the last doc from the first operation, and reschedules the apply for the ops.
    void _scheduleApplyAfterFetch(const Operations&);
    void _onMissingFetched(const BatchDataStatus& fetchResult,
                           Fetcher::NextAction* nextAction,
                           const Operations& ops,
                           const NamespaceString nss);

    // returns true if a rollback is needed
    bool _needToRollback(HostAndPort source, Timestamp lastApplied);

    void _onDataClonerFinish(const Status& status);
    // Called after _onDataClonerFinish when the new Timestamp is avail, to use for minvalid
    void _onApplierReadyStart(const BatchDataStatus& fetchResult,
                              Fetcher::NextAction* nextAction);

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
    ReplicationExecutor* _exec;
    ReplicationCoordinator* _replCoord;

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

    // Protects member data of this ReplicationCoordinator.
    mutable stdx::mutex _mutex;                                                         // (S)
    DataReplicatorState _state;                                                        // (MX)

    // initial sync state
    std::unique_ptr<InitialSyncState> _initialSyncState;                                // (M)
    CollectionCloner::StorageInterface* _storage;                                       // (M)

    // set during scheduling and onFinish
    bool _fetcherPaused;                                                                // (X)
    std::unique_ptr<OplogFetcher> _fetcher;                                             // (S)
    std::unique_ptr<QueryFetcher> _tmpFetcher;                                          // (S)

    bool _reporterPaused;                                                               // (M)
    Handle  _reporterHandle;                                                            // (M)
    std::unique_ptr<Reporter> _reporter;                                                // (M)

    bool _applierActive;                                                                // (M)
    bool _applierPaused;                                                                // (X)
    std::unique_ptr<Applier> _applier;                                                  // (M)
    OnBatchCompleteFn _batchCompletedFn;                                                // (M)


    HostAndPort _syncSource;                                                            // (M)
    Timestamp _lastTimestampFetched;                                                    // (MX)
    Timestamp _lastTimestampApplied;                                                    // (MX)
    BlockingQueue<BSONObj> _oplogBuffer;                                                // (M)

    // Shutdown
    bool _doShutdown;                                                                   // (M)
    Event _onShutdown;                                                                  // (M)

    // Rollback stuff
    Timestamp _rollbackCommonOptime;                                                    // (MX)
};

} // namespace repl
} // namespace mongo
