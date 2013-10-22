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
#include "mongo/db/repl/rs.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace replset {

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

        // Returns the member we're currently syncing from (or NULL)
        virtual const Member* getSyncTarget() = 0;

        // wait up to 1 second for more ops to appear
        virtual void waitForMore() = 0;
    };


    /**
     * notifierThread() uses lastOpTimeWritten to inform the sync target where this member is
     * currently synced to.
     *
     * Lock order:
     * 1. rslock
     * 2. rwlock
     * 3. BackgroundSync::_mutex
     */
    class BackgroundSync : public BackgroundSyncInterface {
        static BackgroundSync *s_instance;
        // protects creation of s_instance
        static boost::mutex s_mutex;

        // _mutex protects all of the class variables
        boost::mutex _mutex;

        // Production thread
        BlockingQueue<BSONObj> _buffer;

        OpTime _lastOpTimeFetched;
        long long _lastH;
        // if produce thread should be running
        bool _pause;
        bool _appliedBuffer;
        bool _assumingPrimary;
        boost::condition _condvar;

        const Member* _currentSyncTarget;

        // Notifier thread

        // used to wait until another op has been replicated
        boost::condition_variable _lastOpCond;
        boost::mutex _lastOpMutex;

        const Member* _oplogMarkerTarget;
        OpTime _consumedOpTime; // not locked, only used by notifier thread

        BackgroundSync();
        BackgroundSync(const BackgroundSync& s);
        BackgroundSync operator=(const BackgroundSync& s);

        // Production thread
        void _producerThread();
        // Adds elements to the list, up to maxSize.
        void produce();
        // Check if rollback is necessary
        bool isRollbackRequired(OplogReader& r);
        void getOplogReader(OplogReader& r);
        // Evaluate if the current sync target is still good
        bool shouldChangeSyncTarget();
        // check lastOpTimeWritten against the remote's earliest op, filling in remoteOldestOp.
        bool isStale(OplogReader& r, BSONObj& remoteOldestOp);
        // stop syncing when this becomes a primary
        void stop();
        // restart syncing
        void start();

        // Tracker thread
        // tells the sync target where this member is synced to
        void markOplog();
        bool hasCursor();

        // Sets _oplogMarkerTarget and calls connect();
        // used for both the notifier command and the older OplogReader style notifier
        bool connectOplogNotifier();

        bool isAssumingPrimary();

    public:
        static BackgroundSync* get();
        static void shutdown();
        static void notify();

        virtual ~BackgroundSync() {}

        // starts the producer thread
        void producerThread();
        // starts the sync target notifying thread
        void notifierThread();

        // Interface implementation

        virtual bool peek(BSONObj* op);
        virtual void consume();
        virtual const Member* getSyncTarget();
        virtual void waitForMore();

        // For monitoring
        BSONObj getCounters();

        // Wait for replication to finish and buffer to be applied so that the member can become
        // primary.
        void stopReplicationAndFlushBuffer();
    };


} // namespace replset
} // namespace mongo
