/**
 *    Copyright (C) 2012 10gen Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/client/fetcher.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/optime.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/queue.h"

namespace mongo {

class DBClientBase;
class OperationContext;

namespace repl {

class Member;
class ReplicationCoordinator;

// This interface exists to facilitate easier testing;
// the test infrastructure implements these functions with stubs.
class BackgroundSyncInterface {
public:
    virtual ~BackgroundSyncInterface();

    // Gets the head of the buffer, but does not remove it.
    // Returns true if an element was present at the head;
    // false if the queue was empty.
    virtual bool peek(BSONObj* op) = 0;

    // Deletes objects in the queue;
    // called by sync thread after it has applied an op
    virtual void consume() = 0;

    // wait up to 1 second for more ops to appear
    virtual void waitForMore() = 0;
};


/**
 * Lock order:
 * 1. rslock
 * 2. rwlock
 * 3. BackgroundSync::_mutex
 */
class BackgroundSync : public BackgroundSyncInterface {
public:
    // Allow index prefetching to be turned on/off
    enum IndexPrefetchConfig {
        UNINITIALIZED = 0,
        PREFETCH_NONE = 1,
        PREFETCH_ID_ONLY = 2,
        PREFETCH_ALL = 3
    };

    static BackgroundSync* get();

    // stop syncing (when this node becomes a primary, e.g.)
    void stop();


    void shutdown();

    bool isStopped() const;

    virtual ~BackgroundSync() {}

    // starts the producer thread
    void producerThread();
    // starts the sync target notifying thread
    void notifierThread();

    HostAndPort getSyncTarget();

    // Interface implementation

    virtual bool peek(BSONObj* op);
    virtual void consume();
    virtual void clearSyncTarget();
    virtual void waitForMore();

    // For monitoring
    BSONObj getCounters();

    // Clears any fetched and buffered oplog entries.
    void clearBuffer();

    /**
     * Cancel existing find/getMore commands on the sync source's oplog collection.
     */
    void cancelFetcher();

    bool getInitialSyncRequestedFlag();
    void setInitialSyncRequestedFlag(bool value);

    void setIndexPrefetchConfig(const IndexPrefetchConfig cfg) {
        _indexPrefetchConfig = cfg;
    }

    IndexPrefetchConfig getIndexPrefetchConfig() {
        return _indexPrefetchConfig;
    }


    // Testing related stuff
    void pushTestOpToBuffer(const BSONObj& op);

private:
    static BackgroundSync* s_instance;
    // protects creation of s_instance
    static stdx::mutex s_mutex;

    // Production thread
    BlockingQueue<BSONObj> _buffer;

    // Task executor used to run find/getMore commands on sync source.
    executor::ThreadPoolTaskExecutor _threadPoolTaskExecutor;

    // _mutex protects all of the class variables except _buffer
    mutable stdx::mutex _mutex;

    OpTime _lastOpTimeFetched;

    // lastFetchedHash is used to match ops to determine if we need to rollback, when
    // a secondary.
    long long _lastFetchedHash;

    // if producer thread should not be running
    bool _stopped;

    HostAndPort _syncSourceHost;

    BackgroundSync();
    BackgroundSync(const BackgroundSync& s);
    BackgroundSync operator=(const BackgroundSync& s);

    // Production thread
    void _producerThread();
    void _produce(OperationContext* txn);

    /**
     * Signals to the applier that we have no new data,
     * and are in sync with the applier at this point.
     *
     * NOTE: Used after rollback and during draining to transition to Primary role;
     */
    void _signalNoNewDataForApplier();

    /**
     * Processes query responses from fetcher.
     */
    void _fetcherCallback(const StatusWith<Fetcher::QueryResponse>& result,
                          BSONObjBuilder* bob,
                          const HostAndPort& source,
                          OpTime lastOpTimeFetched,
                          long long lastFetchedHash,
                          Milliseconds fetcherMaxTimeMS,
                          Status* returnStatus);

    /**
     * Executes a rollback.
     * 'getConnection' returns a connection to the sync source.
     */
    void _rollback(OperationContext* txn,
                   const HostAndPort& source,
                   stdx::function<DBClientBase*()> getConnection);

    /**
     * Evaluate if the current sync source is still good.
     * "syncSource" is the name of the current sync source, which will be used to look up the
     * member's heartbeat data.
     * "syncSourceLastOpTime" is the last OpTime the sync source has. This is passed in because the
     * data stored from heartbeats could be too stale and would cause unnecessary sync source
     * changes.
     * "syncSourceHasSyncSource" indicates whether our sync source is currently syncing from another
     * member.
     */
    bool _shouldChangeSyncSource(const HostAndPort& syncSource,
                                 const OpTime& syncSourceLastOpTime,
                                 bool syncSourceHasSyncSource);

    // restart syncing
    void start(OperationContext* txn);

    long long _readLastAppliedHash(OperationContext* txn);

    // A pointer to the replication coordinator running the show.
    ReplicationCoordinator* _replCoord;

    // bool for indicating resync need on this node and the mutex that protects it
    // The resync command sets this flag; the Applier thread observes and clears it.
    bool _initialSyncRequestedFlag;
    stdx::mutex _initialSyncMutex;

    // This setting affects the Applier prefetcher behavior.
    IndexPrefetchConfig _indexPrefetchConfig;
};


}  // namespace repl
}  // namespace mongo
