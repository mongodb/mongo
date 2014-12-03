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

#include <boost/thread/mutex.hpp>

#include "mongo/util/queue.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/jsobj.h"

namespace mongo {
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
            PREFETCH_NONE=0, PREFETCH_ID_ONLY=1, PREFETCH_ALL=2
        };

        static BackgroundSync* get();

        // stop syncing (when this node becomes a primary, e.g.)
        void stop();


        void shutdown();
        void notify(OperationContext* txn);

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

        long long getLastAppliedHash() const;
        void setLastAppliedHash(long long oldH);
        void loadLastAppliedHash(OperationContext* txn);

        bool getInitialSyncRequestedFlag();
        void setInitialSyncRequestedFlag(bool value);

        void setIndexPrefetchConfig(const IndexPrefetchConfig cfg) {
            _indexPrefetchConfig = cfg;
        }

        IndexPrefetchConfig getIndexPrefetchConfig() {
            return _indexPrefetchConfig;
        }

    private:
        static BackgroundSync *s_instance;
        // protects creation of s_instance
        static boost::mutex s_mutex;

        // Production thread
        BlockingQueue<BSONObj> _buffer;
        OplogReader _syncSourceReader;

        // _mutex protects all of the class variables except _syncSourceReader and _buffer
        mutable boost::mutex _mutex;

        OpTime _lastOpTimeFetched;

        // lastAppliedHash is used to generate a new hash for the following op, when primary.
        long long _lastAppliedHash;
        // lastFetchedHash is used to match ops to determine if we need to rollback, when
        // a secondary.
        long long _lastFetchedHash;

        // if produce thread should be running
        bool _pause;
        bool _appliedBuffer;
        boost::condition _condvar;

        HostAndPort _syncSourceHost;

        BackgroundSync();
        BackgroundSync(const BackgroundSync& s);
        BackgroundSync operator=(const BackgroundSync& s);

        // Production thread
        void _producerThread();
        // Adds elements to the list, up to maxSize.
        void produce(OperationContext* txn);
        // Checks the criteria for rolling back and executes a rollback if warranted.
        bool _rollbackIfNeeded(OperationContext* txn, OplogReader& r);

        // Evaluate if the current sync target is still good
        bool shouldChangeSyncSource();

        // restart syncing
        void start(OperationContext* txn);

        long long _readLastAppliedHash(OperationContext* txn);

        // A pointer to the replication coordinator running the show.
        ReplicationCoordinator* _replCoord;

        // bool for indicating resync need on this node and the mutex that protects it
        // The resync command sets this flag; the Applier thread observes and clears it.
        bool _initialSyncRequestedFlag;
        boost::mutex _initialSyncMutex;

        // This setting affects the Applier prefetcher behavior.
        IndexPrefetchConfig _indexPrefetchConfig;

    };


} // namespace repl
} // namespace mongo
