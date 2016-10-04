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

#include <deque>
#include <set>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/range_arithmetic.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/time_support.h"

namespace mongo {

class OperationContext;
struct DeleteJobStats;
struct RangeDeleteEntry;
struct RangeDeleterEnv;
struct RangeDeleterOptions;

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
 *   getGlobalServiceContext()->killAllOperations(); // stop all deletes
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
    bool queueDelete(OperationContext* txn,
                     const RangeDeleterOptions& options,
                     Notification<void>* doneSignal,
                     std::string* errMsg);

    /**
     * Removes the documents specified by the range. Unlike queueTask, this call
     * blocks and the deletion is performed by the current thread.
     *
     * Returns true if the deletion was performed. False if the range is blacklisted,
     * was already queued, or stopWorkers() was called.
     */
    bool deleteNow(OperationContext* txn, const RangeDeleterOptions& options, std::string* errMsg);

    //
    // Introspection methods
    //

    // Note: original contents of stats will be cleared. Caller owns the returned stats.
    void getStatsHistory(std::vector<DeleteJobStats*>* stats) const;

    size_t getTotalDeletes() const;
    size_t getPendingDeletes() const;
    size_t getDeletesInProgress() const;

    //
    // Methods meant to be only used for testing. Should be treated like private
    // methods.
    //

    /** Returns a BSON representation of the queue contents. For debugging only. */
    BSONObj toBSON() const;

private:
    // Ownership is transferred to here.
    void recordDelStats(DeleteJobStats* newStat);


    struct NSMinMax;

    struct NSMinMaxCmp {
        bool operator()(const NSMinMax* lhs, const NSMinMax* rhs) const;
    };

    typedef std::deque<RangeDeleteEntry*> TaskList;  // owned here

    typedef std::set<NSMinMax*, NSMinMaxCmp> NSMinMaxSet;  // owned here

    /** Body of the worker thread */
    void doWork();

    /** Returns true if the range doesn't intersect with one other range */
    bool canEnqueue_inlock(StringData ns,
                           const BSONObj& min,
                           const BSONObj& max,
                           std::string* errMsg) const;

    /** Returns true if stopWorkers() was called. This call is synchronized. */
    bool stopRequested() const;

    std::unique_ptr<RangeDeleterEnv> _env;

    // Initially not active. Must be started explicitly.
    std::unique_ptr<stdx::thread> _worker;

    // Protects _stopRequested.
    mutable stdx::mutex _stopMutex;

    // If set, no other delete taks should be accepted.
    bool _stopRequested;

    // No delete is in progress. Used to make sure that there is no activity
    // in this deleter, and therefore is safe to destroy it. Must be used in
    // conjunction with _stopRequested.
    stdx::condition_variable _nothingInProgressCV;

    // Protects all the data structure below this.
    mutable stdx::mutex _queueMutex;

    // _taskQueue has a task ready to work on.
    stdx::condition_variable _taskQueueNotEmptyCV;

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

    // Keeps track of number of tasks that are in progress, including the inline deletes.
    size_t _deletesInProgress;

    // Protects _statsHistory
    mutable stdx::mutex _statsHistoryMutex;
    std::deque<DeleteJobStats*> _statsHistory;
};


/**
 * Simple class for storing statistics for the RangeDeleter.
 */
struct DeleteJobStats {
    Date_t queueStartTS;
    Date_t queueEndTS;
    Date_t deleteStartTS;
    Date_t deleteEndTS;
    Date_t waitForReplStartTS;
    Date_t waitForReplEndTS;

    long long int deletedDocCount;

    DeleteJobStats() : deletedDocCount(0) {}
};

struct RangeDeleterOptions {
    RangeDeleterOptions(const KeyRange& range);

    const KeyRange range;

    WriteConcernOptions writeConcern;
    std::string removeSaverReason;
    bool fromMigrate;
    bool onlyRemoveOrphanedDocs;
    bool waitForOpenCursors;
};

/**
 * For internal use only.
 */
struct RangeDeleteEntry {
    RangeDeleteEntry(const RangeDeleterOptions& options);

    const RangeDeleterOptions options;

    // Sets of cursors to wait to close until this can be ready
    // for deletion.
    std::set<CursorId> cursorsToWait;

    // Not owned here.
    // Important invariant: Can only be set and used by one thread.
    Notification<void>* doneSignal;

    // Time since the last time we reported this object.
    Date_t lastLoggedTS;

    DeleteJobStats stats;

    // For debugging only
    BSONObj toBSON() const;
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
    virtual bool deleteRange(OperationContext* txn,
                             const RangeDeleteEntry& taskDetails,
                             long long int* deletedDocs,
                             std::string* errMsg) = 0;

    /**
     * Gets the list of open cursors on a given namespace. The openCursors is an
     * output parameter that will contain all the cursors open after this is called.
     * Assume that openCursors is empty when passed in.
     *
     * Must be a synchronous call. CursorIds should be populated after call.
     * Must not throw exception.
     */
    virtual void getCursorIds(OperationContext* txn,
                              StringData ns,
                              std::set<CursorId>* openCursors) = 0;
};

}  // namespace mongo
