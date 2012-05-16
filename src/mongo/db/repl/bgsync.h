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
 */

#pragma once

#include <boost/thread/mutex.hpp>

#include "mongo/util/queue.h"
#include "mongo/db/oplogreader.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/jsobj.h"

namespace mongo {
namespace replset {

    class BackgroundSyncInterface {
    public:
        virtual ~BackgroundSyncInterface();
        virtual BSONObj* peek() = 0;
        virtual void consume() = 0;
        virtual Member* getSyncTarget() = 0;
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

        BSONObj _currentOp;
        OpTime _lastOpTimeFetched;
        long long _lastH;
        // if produce thread should be running
        bool _pause;

        Member* _currentSyncTarget;

        // Notifier thread

        // used to wait until another op has been replicated
        boost::condition_variable _lastOpCond;
        boost::mutex _lastOpMutex;

        Member* _oplogMarkerTarget;
        OplogReader _oplogMarker; // not locked, only used by notifier thread
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

        // Gets the head of the buffer, but does not remove it. Returns a pointer to the list
        // element.
        virtual BSONObj* peek();

        // called by sync thread when it has applied an op
        virtual void consume();

        // return the member we're currently syncing from (or NULL)
        virtual Member* getSyncTarget();
    };


} // namespace replset
} // namespace mongo
