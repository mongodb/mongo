/**
 *    Copyright (C) 2013 10gen Inc.
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

#include <boost/thread/thread.hpp>
#include <deque>
#include <set>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/cc_by_loc.h" // for typedef CursorId
#include "mongo/db/jsobj.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/synchronization.h"

namespace mongo {

    struct RangeDeleterEnv;
    class RangeDeleterStats;

    /**
     * Class for deleting documents for a given namespace and range.  It contains a queue of
     * jobs to be deleted. Deletions can be "immediate", in which case they are going to be put
     * in front of the queue and acted on promptly, or "lazy", in which they would be acted
     * upon when they get to the head of the queue.
     *
     * Threading assumptions:
     *
     *   This class has (currently) one worker thread attacking the queue, one
     *   job at a time. If we want an immediate deletion, that job is going to
     *   be performed on the thread that is requesting it.
     *
     *   All calls regarding deletion are synchronized.
     *
     * Life cycle:
     *   RangeDeleter* deleter = new RangeDeleter(new ...);
     *   deleter->startWorkers();
     *   ...
     *   killCurrentOp.killAll(); // stop all deletes
     *   deleter->stopWorkers();
     *   delete deleter;
     */
    class RangeDeleter {
        MONGO_DISALLOW_COPYING(RangeDeleter);

    public:

        /**
         * Creates a new deleter and uses an environment object to delegate external logic like
         * data deletion. Takes ownership of the environment.
         */
        explicit RangeDeleter(RangeDeleterEnv* env);

        /**
         * Destroys this deleter. Must make sure that no threads are working on this queue. Use
         * stopWorkers to stop the internal workers, it is an error not to do so.
         */
        ~RangeDeleter();

        //
        // Thread management methods
        //

        /**
         * Starts the background thread to work on this queue. Does nothing if the worker
         * thread is already active.
         *
         * This call is _not_ thread safe and must be issued before any other call.
         */
        void startWorkers();

        /**
         * Stops the background thread working on this queue. This will block if there are
         * tasks that are being deleted, but will leave the pending tasks in the queue.
         *
         * Steps:
         * 1. Stop accepting new queued deletes.
         * 2. Stop all idle workers.
         * 3. Waits for all threads to finish any task that is in progress (but see note
         *    below).
         *
         * Note:
         *
         * + restarting this deleter with startWorkers after stopping it is not supported.
         *
         * + the worker thread could be running a call in the environment. The thread is
         *   only going to be returned when the environment decides so. In production,
         *   KillCurrentOp::killAll can be used to get the thread back from the environment.
         */
        void stopWorkers();

        //
        // Queue manipulation methods - can be called by anyone.
        //

        /**
         * Adds a new delete to the queue.
         *
         * If notifyDone is not NULL, it will be signaled after the delete is completed.
         * Note that this will happen only if the delete was actually queued.
         *
         * Returns true if the task is queued and false If the given range is blacklisted,
         * is already queued, or stopWorkers() was called.
         */
        bool queueDelete(const std::string& ns,
                         const BSONObj& min,
                         const BSONObj& max,
                         const BSONObj& shardKeyPattern,
                         bool secondaryThrottle,
                         Notification* notifyDone,
                         std::string* errMsg);

        /**
         * Removes the documents specified by the range. Unlike queueTask, this call
         * blocks and the deletion is performed by the current thread.
         *
         * Returns true if the deletion was performed. False if the range is blacklisted,
         * was already queued, or stopWorkers() was called.
         */
        bool deleteNow(const std::string& ns,
                       const BSONObj& min,
                       const BSONObj& max,
                       const BSONObj& shardKeyPattern,
                       bool secondaryThrottle,
                       std::string* errMsg);

        /**
         * Blacklist the given range for the given namespace. Use the removeFromBlackList
         * method to undo this operation.
         *
         * Note: min is inclusive and max is exclusive.
         *
         * Return false if a task in the queue intersects the given range or
         * if the range intersects another range that is in the black list.
         */
        bool addToBlackList(const StringData& ns,
                            const BSONObj& min,
                            const BSONObj& max,
                            std::string* errMsg);

        /**
         * Removes the exact range from the blacklist.
         *
         * Returns false if range cannot be found from the black list.
         */
        bool removeFromBlackList(const StringData& ns,
                                 const BSONObj& min,
                                 const BSONObj& max);

        //
        // Introspection methods
        //

        const RangeDeleterStats* getStats() const;

        //
        // Methods meant to be only used for testing. Should be treated like private
        // methods.
        //

        /** Returns a BSON representation of the queue contents. For debugging only. */
        BSONObj toBSON() const;

    private:
        struct RangeDeleteEntry;
        struct NSMinMax;

        struct NSMinMaxCmp {
            bool operator()(const NSMinMax* lhs, const NSMinMax* rhs) const;
        };

        typedef std::deque<RangeDeleteEntry*> TaskList;  // owned here

        typedef std::set<NSMinMax*, NSMinMaxCmp> NSMinMaxSet; // owned here

        /** Body of the worker thread */
        void doWork();

        /** Returns true if range is blacklisted. Assumes _queueMutex is held */
        bool isBlacklisted_inlock(const StringData& ns,
                                  const BSONObj& min,
                                  const BSONObj& max,
                                  std::string* errMsg) const;

        /** Returns true if the range doesn't intersect with one other range */
        bool canEnqueue_inlock(const StringData& ns,
                               const BSONObj& min,
                               const BSONObj& max,
                               std::string* errMsg) const;

        /** Returns true if stopWorkers() was called. This call is synchronized. */
        bool stopRequested() const;

        scoped_ptr<RangeDeleterEnv> _env;

        // Initially not active. Must be started explicitly.
        scoped_ptr<boost::thread> _worker;

        // Protects _stopRequested.
        mutable mutex _stopMutex;

        // If set, no other delete taks should be accepted.
        bool _stopRequested;

        // No delete is in progress. Used to make sure that there is no activity
        // in this deleter, and therefore is safe to destroy it. Must be used in
        // conjunction with _stopRequested.
        boost::condition _nothingInProgressCV;

        // Protects all the data structure below this.
        mutable mutex _queueMutex;

        // _taskQueue has a task ready to work on.
        boost::condition _taskQueueNotEmptyCV;

        // Queue for storing the list of ranges that have cursors pending on it.
        //
        // Note: pointer life cycle is not handled here.
        TaskList _notReadyQueue;

        // Queue for storing the list of ranges that are ready to be removed.
        //
        // Note: pointer life cycle is not handled here.
        TaskList _taskQueue;

        // Set of all deletes - deletes waiting for cursors, waiting to be acted upon
        // and in progress. Includes both queued and immediate deletes.
        //
        // queued delete life cycle: new @ queuedDelete, delete @ doWork
        // deleteNow life cycle: deleteNow stack variable
        NSMinMaxSet _deleteSet;

        // Keeps track of ranges that cannot be queued to _notReady.
        // Invariant: should not conflict with any entry in all queues.
        //
        // life cycle: new @ addToBlackList, delete @ removeFromBlackList
        // deleteNow life cycle: deleteNow stack variable
        NSMinMaxSet _blackList;

        // Keeps track of counters regarding each of the queues.
        scoped_ptr<RangeDeleterStats> _stats;
    };

    /**
     * Class for encapsulating logic used by the RangeDeleter class to perform its tasks.
     */
    struct RangeDeleterEnv {
        virtual ~RangeDeleterEnv() {}

        /**
         * Deletes the documents from the given range. This method should be
         * responsible for making sure that the proper contexts are setup
         * to be able to perform deletions.
         *
         * Must be a synchronous call. Docs should be deleted after call ends.
         * Must not throw Exceptions.
         */
        virtual bool deleteRange(const StringData& ns,
                                 const BSONObj& inclusiveLower,
                                 const BSONObj& exclusiveUpper,
                                 const BSONObj& shardKeyPattern,
                                 bool secondaryThrottle,
                                 std::string* errMsg) = 0;

        /**
         * Gets the list of open cursors on a given namespace. The openCursors is an
         * output parameter that will contain all the cursors open after this is called.
         * Assume that openCursors is empty when passed in.
         *
         * Must be a synchronous call. CursorIds should be populated after call.
         * Must not throw exception.
         */
        virtual void getCursorIds(const StringData& ns, set<CursorId>* openCursors) = 0;
    };

} // namespace mongo
