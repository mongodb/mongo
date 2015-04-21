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

#include <boost/scoped_ptr.hpp>
#include <boost/thread.hpp>
#include <boost/thread/mutex.hpp>
#include <boost/thread/condition_variable.hpp>
#include <vector>

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/fetcher.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_executor.h"
#include "mongo/db/repl/reporter.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace repl {

namespace {
    typedef ReplicationExecutor::CallbackHandle Handle;
    typedef ReplicationExecutor::EventHandle Event;
    typedef ReplicationExecutor::CallbackData CallbackData;
    typedef ReplicationExecutor::RemoteCommandCallbackData CommandCallbackData;

} // namespace
/** State for decision tree */
enum class DataReplicatiorState {
    Steady, // Default
    InitialSync,
    Rollback,
    Uninitialized,
};

// TBD -- ignore for now
enum class DataReplicatiorScope {
    ReplicateAll,
    ReplicateDB,
    ReplicateCollection
};

struct DataReplicatorOptions {
    Timestamp startOptime;
    NamespaceString localOplogNS = NamespaceString("local.oplog.rs");
    NamespaceString remoteOplogNS = NamespaceString("local.oplog.rs");

    // TBD -- ignore below for now
    DataReplicatiorScope scope = DataReplicatiorScope::ReplicateAll;
    std::string scopeNS;
    BSONObj filterCriteria;
    HostAndPort syncSource; // for use without replCoord -- maybe some kind of rsMonitor/interface
};

// TODO: Break out: or at least move body to cpp
class InitialSyncImpl {
public:
    InitialSyncImpl(ReplicationExecutor* exec)
            : _status(StatusWith<Timestamp>(Timestamp())),
              _exec(exec) {
    };

    Status start();

    void wait() {
        // TODO
    };

    bool isActive() { return _finishEvent.isValid(); };

    StatusWith<Timestamp> getStatus() {return _status;}
private:
    StatusWith<Timestamp> _status;
    ReplicationExecutor* _exec;
    Event _finishEvent;
};

class Applier {};

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
                   ReplicationExecutor* exec,
                   ReplicationCoordinator* replCoord);
    /**
     * Used by non-replication coordinator processes, like sharding.
     */
    DataReplicator(DataReplicatorOptions opts,
                   ReplicationExecutor* exec);

    Status start();
    Status shutdown();

    // Resumes apply replication events from the oplog
    Status resume(bool wait=false);

    // Pauses replication and application
    Status pause();

    // Pauses replication and waits to return until all un-applied ops have been applied
    StatusWith<Timestamp> flushAndPause();

    // Called when a slave has progressed to a new oplog position
    void slavesHaveProgressed();

    // just like initialSync but can be called anytime.
    StatusWith<Timestamp> resync();

    // Don't use above methods before these
    StatusWith<Timestamp> initialSync();

    // For testing only
    void _resetState(Timestamp lastAppliedOptime);
private:

    // Run a member function in the executor, waiting for it to finish.
//    Status _run(void*());

    // Only executed via executor
    void _resumeFinish(CallbackData cbData);
    void _onFetchFinish(const StatusWith<Fetcher::BatchData>& fetchResult,
                        Fetcher::NextAction* nextAction);
    void _onApplyBatchFinish(CallbackData cbData);
    void _doNextActionsCB(CallbackData cbData);
    void _doNextActions();
    void _doNextActions_InitialSync_inlock();
    void _doNextActions_Rollback_inlock();
    void _doNextActions_Steady_inlock();

    // Applies up till the specified Timestamp and pauses automatic application
    Timestamp _applyUntilAndPause(Timestamp);
    Timestamp _applyUntil(Timestamp);
    void _pauseApplier();

    // returns true if a rollback is needed
    bool _needToRollback(HostAndPort source, Timestamp lastApplied);

    Status _scheduleApplyBatch();
    Status _scheduleFetch();
    Status _scheduleReport();

    const void _cancelAllHandles();
    const bool _anyActiveHandles();

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
    mutable boost::mutex _mutex;                                                        // (S)
    DataReplicatiorState _state;                                                        // (MX)

    // set during scheduling and onFinish
    bool _fetcherPaused;                                                                // (X)
    boost::scoped_ptr<Fetcher> _fetcher;                                                // (S)

    bool _reporterActive;                                                               // (M)
    Handle  _reporterHandle;                                                            // (M)
    boost::scoped_ptr<Reporter> _reporter;                                              // (M)

    bool _applierActive;                                                                // (M)
    bool _applierPaused;                                                                // (X)
    Handle _applierHandle;                                                              // (M)
    boost::scoped_ptr<Applier> _applier;                                                // (M)

    boost::scoped_ptr<InitialSyncImpl> _initialSync;                                    // (M)

    HostAndPort _syncSource;                                                            // (M)
    Timestamp _lastOptimeFetched;                                                       // (MX)
    Timestamp _lastOptimeApplied;                                                       // (MX)
    std::vector<BSONObj> _oplogBuffer;                                                  // (M)

    // Shutdown
    bool _doShutdown;                                                                   // (M)
    Event _onShutdown;                                                                  // (M)

    // Rollback stuff
    Timestamp _rollbackCommonOptime;                                                    // (MX)



};

} // namespace repl
} // namespace mongo
