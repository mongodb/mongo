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

#include "mongo/platform/basic.h"

#include "mongo/db/range_deleter.h"

#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <memory>

#include "mongo/db/global_environment_experiment.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/s/range_arithmetic.h"
#include "mongo/util/concurrency/synchronization.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

using std::auto_ptr;
using std::set;
using std::pair;
using std::string;

using mongoutils::str::stream;

namespace {
    const long int NotEmptyTimeoutMillis = 200;
    const long long int MaxCurorCheckIntervalMillis = 500;
    const unsigned long long LogCursorsThresholdMillis = 60 * 1000;
    const unsigned long long LogCursorsIntervalMillis = 10 * 1000;
    const size_t DeleteJobsHistory = 10; // entries

    /**
     * Removes an element from the container that holds a pointer type, and deletes the
     * pointer as well. Returns true if the element was found.
     */
    template <typename ContainerType, typename ContainerElementType>
    bool deletePtrElement(ContainerType* container, ContainerElementType elem) {
        typename ContainerType::iterator iter = container->find(elem);

        if (iter == container->end()) {
            return false;
        }

        delete *iter;
        container->erase(iter);
        return true;
    }

    void logCursorsWaiting(const std::string& ns,
                           const mongo::BSONObj& min,
                           const mongo::BSONObj& max,
                           unsigned long long int elapsedMS,
                           const std::set<mongo::CursorId>& cursorsToWait) {
        mongo::StringBuilder cursorList;
        for (std::set<mongo::CursorId>::const_iterator it = cursorsToWait.begin();
                it != cursorsToWait.end(); ++it) {
            cursorList << *it << " ";
        }

        mongo::log() << "Waiting for open cursors before deleting ns: " << ns
                     << ", min: " << min
                     << ", max: " << max
                     << ", elapsedMS: " << elapsedMS
                     << ", cursors: [ " << cursorList.str() << "]" << std::endl;
    }
}

namespace mongo {

    namespace duration = boost::posix_time;

    struct RangeDeleter::NSMinMax {
        NSMinMax(std::string ns, const BSONObj min, const BSONObj max):
            ns(ns), min(min), max(max) {
        }

        std::string ns;

        // Inclusive lower range.
        BSONObj min;

        // Exclusive upper range.
        BSONObj max;
    };

    bool RangeDeleter::NSMinMaxCmp::operator()(
            const NSMinMax* lhs, const NSMinMax* rhs) const {
        const int nsComp = lhs->ns.compare(rhs->ns);

        if (nsComp < 0) {
            return true;
        }

        if (nsComp > 0) {
            return false;
        }

        return compareRanges(lhs->min, lhs->max, rhs->min, rhs->max) < 0;
    }

    RangeDeleter::RangeDeleter(RangeDeleterEnv* env):
        _env(env), // ownership xfer
        _stopMutex("stopRangeDeleter"),
        _stopRequested(false),
        _queueMutex("RangeDeleter"),
        _deletesInProgress(0),
        _statsHistoryMutex("RangeDeleterStatsHistory") {
    }

    RangeDeleter::~RangeDeleter() {
        for(TaskList::iterator it = _notReadyQueue.begin();
            it != _notReadyQueue.end();
            ++it) {
            delete (*it);
        }

        for(TaskList::iterator it = _taskQueue.begin();
            it != _taskQueue.end();
            ++it) {
            delete (*it);
        }

        for(NSMinMaxSet::iterator it = _deleteSet.begin();
            it != _deleteSet.end();
            ++it) {
            delete (*it);
        }

        for(NSMinMaxSet::iterator it = _blackList.begin();
            it != _blackList.end();
            ++it) {
            delete (*it);
        }
    }

    void RangeDeleter::startWorkers() {
        if (!_worker) {
            _worker.reset(new boost::thread(stdx::bind(&RangeDeleter::doWork, this)));
        }
    }

    void RangeDeleter::stopWorkers() {
        {
            scoped_lock sl(_stopMutex);
            _stopRequested = true;
        }

        if (_worker) {
            _worker->join();
        }

        scoped_lock sl(_queueMutex);
        while (_deletesInProgress > 0) {
            _nothingInProgressCV.wait(sl.boost());
        }
    }

    bool RangeDeleter::queueDelete(const std::string& ns,
                                   const BSONObj& min,
                                   const BSONObj& max,
                                   const BSONObj& shardKeyPattern,
                                   bool secondaryThrottle,
                                   Notification* notifyDone,
                                   std::string* errMsg) {
        string dummy;
        if (errMsg == NULL) errMsg = &dummy;

        auto_ptr<RangeDeleteEntry> toDelete(new RangeDeleteEntry(ns,
                                                                 min.getOwned(),
                                                                 max.getOwned(),
                                                                 shardKeyPattern.getOwned(),
                                                                 secondaryThrottle));
        toDelete->notifyDone = notifyDone;

        {
            scoped_lock sl(_queueMutex);
            if (_stopRequested) {
                *errMsg = "deleter is already stopped.";
                return false;
            }

            if (!canEnqueue_inlock(ns, min, max, errMsg)) {
                return false;
            }

            _deleteSet.insert(new NSMinMax(ns, min, max));
        }

        {
            boost::scoped_ptr<OperationContext> txn(getGlobalEnvironment()->newOpCtx());
            _env->getCursorIds(txn.get(), ns, &toDelete->cursorsToWait);
        }

        toDelete->stats.queueStartTS = jsTime();

        {
            scoped_lock sl(_queueMutex);

            if (toDelete->cursorsToWait.empty()) {
                toDelete->stats.queueEndTS = jsTime();
                _taskQueue.push_back(toDelete.release());
                _taskQueueNotEmptyCV.notify_one();
            }
            else {
                log() << "rangeDeleter waiting for " << toDelete->cursorsToWait.size()
                      << " cursors in " << ns << " to finish" << endl;

                _notReadyQueue.push_back(toDelete.release());
            }
        }

        return true;
    }

namespace {
    bool _waitForReplication(OperationContext* txn, std::string* errMsg) {
        WriteConcernOptions writeConcern;
        writeConcern.wMode = "majority";
        writeConcern.wTimeout = 60 * 60 * 1000;
        repl::ReplicationCoordinator::StatusAndDuration replStatus =
                repl::getGlobalReplicationCoordinator()->awaitReplicationOfLastOp(txn,
                                                                                  writeConcern);
        repl::ReplicationCoordinator::Milliseconds elapsedTime = replStatus.duration;
        if (replStatus.status.code() == ErrorCodes::ExceededTimeLimit) {
            *errMsg = str::stream() << "rangeDeleter timed out after "
                                    << elapsedTime.seconds() << " seconds while waiting"
                                    << " for deletions to be replicated to majority nodes";
            log() << *errMsg;
        }
        else if (replStatus.status.code() == ErrorCodes::NotMaster) {
            *errMsg = str::stream() << "rangeDeleter no longer PRIMARY after "
                                    << elapsedTime.seconds() << " seconds while waiting"
                                    << " for deletions to be replicated to majority nodes";
        }
        else {
            LOG(elapsedTime.seconds() < 30 ? 1 : 0)
                << "rangeDeleter took " << elapsedTime.seconds() << " seconds "
                << " waiting for deletes to be replicated to majority nodes";

            fassert(18512, replStatus.status);
        }

        return replStatus.status.isOK();
    }
}

    bool RangeDeleter::deleteNow(OperationContext* txn,
                                 const std::string& ns,
                                 const BSONObj& min,
                                 const BSONObj& max,
                                 const BSONObj& shardKeyPattern,
                                 bool secondaryThrottle,
                                 string* errMsg) {
        if (stopRequested()) {
            *errMsg = "deleter is already stopped.";
            return false;
        }

        string dummy;
        if (errMsg == NULL) errMsg = &dummy;

        NSMinMax deleteRange(ns, min, max);
        {
            scoped_lock sl(_queueMutex);
            if (!canEnqueue_inlock(ns, min, max, errMsg)) {
                return false;
            }

            _deleteSet.insert(&deleteRange);

            // Note: count for pending deletes is an integral part of the shutdown story.
            // Therefore, to simplify things, there is no "pending" state for deletes in
            // deleteNow, the state transition is simply inProgress -> done.
            _deletesInProgress++;
        }

        set<CursorId> cursorsToWait;
        _env->getCursorIds(txn, ns, &cursorsToWait);

        long long checkIntervalMillis = 5;

        if (!cursorsToWait.empty()) {
            log() << "rangeDeleter waiting for " << cursorsToWait.size()
                  << " cursors in " << ns << " to finish" << endl;
        }

        RangeDeleteEntry taskDetails(ns, min, max, shardKeyPattern, secondaryThrottle);
        taskDetails.stats.queueStartTS = jsTime();

        Date_t timeSinceLastLog;
        for (; !cursorsToWait.empty(); sleepmillis(checkIntervalMillis)) {
            const unsigned long long timeNow = curTimeMillis64();
            const unsigned long long elapsedTimeMillis =
                timeNow - taskDetails.stats.queueStartTS.millis;
            const unsigned long long lastLogMillis = timeNow - timeSinceLastLog.millis;

            if (elapsedTimeMillis > LogCursorsThresholdMillis &&
                    lastLogMillis > LogCursorsIntervalMillis) {
                timeSinceLastLog = jsTime();
                logCursorsWaiting(ns, min, max, elapsedTimeMillis, cursorsToWait);
            }

            set<CursorId> cursorsNow;
            _env->getCursorIds(txn, ns, &cursorsNow);

            set<CursorId> cursorsLeft;
            std::set_intersection(cursorsToWait.begin(),
                                  cursorsToWait.end(),
                                  cursorsNow.begin(),
                                  cursorsNow.end(),
                                  std::inserter(cursorsLeft, cursorsLeft.end()));

            cursorsToWait.swap(cursorsLeft);

            if (stopRequested()) {
                *errMsg = "deleter was stopped.";

                scoped_lock sl(_queueMutex);
                _deleteSet.erase(&deleteRange);

                _deletesInProgress--;

                if (_deletesInProgress == 0) {
                    _nothingInProgressCV.notify_one();
                }

                return false;
            }

            if (checkIntervalMillis < MaxCurorCheckIntervalMillis) {
                checkIntervalMillis *= 2;
            }
        }
        taskDetails.stats.queueEndTS = jsTime();

        taskDetails.stats.deleteStartTS = jsTime();
        bool result = _env->deleteRange(txn,
                                        taskDetails,
                                        &taskDetails.stats.deletedDocCount,
                                        errMsg);

        taskDetails.stats.deleteEndTS = jsTime();

        if (result) {
            taskDetails.stats.waitForReplStartTS = jsTime();
            result = _waitForReplication(txn, errMsg);
            taskDetails.stats.waitForReplEndTS = jsTime();
        }

        {
            scoped_lock sl(_queueMutex);
            _deleteSet.erase(&deleteRange);

            _deletesInProgress--;

            if (_deletesInProgress == 0) {
                _nothingInProgressCV.notify_one();
            }
        }

        recordDelStats(new DeleteJobStats(taskDetails.stats));
        return result;
    }

    bool RangeDeleter::addToBlackList(const StringData& ns,
                                      const BSONObj& min,
                                      const BSONObj& max,
                                      std::string* errMsg) {
        string dummy;
        if (errMsg == NULL) errMsg = &dummy;

        scoped_lock sl(_queueMutex);

        if (isBlacklisted_inlock(ns, min, max, errMsg)) {
            return false;
        }

        for (NSMinMaxSet::const_iterator iter = _deleteSet.begin();
                iter != _deleteSet.end(); ++iter) {
            const NSMinMax* const entry = *iter;
            if (entry->ns == ns && rangeOverlaps(entry->min, entry->max, min, max)) {
                *errMsg = stream() << "Cannot black list ns: " << ns
                        << ", min: " << min
                        << ", max: " << max
                        << " since it is already queued for deletion.";
                return false;
            }
        }

        _blackList.insert(new NSMinMax(ns.toString(), min, max));
        return true;
    }

    bool RangeDeleter::removeFromBlackList(const StringData& ns,
                                           const BSONObj& min,
                                           const BSONObj& max) {
        scoped_lock sl(_queueMutex);
        NSMinMax entry(ns.toString(), min, max);
        return deletePtrElement(&_blackList, &entry);
    }

    void RangeDeleter::getStatsHistory(std::vector<DeleteJobStats*>* stats) const {
        stats->clear();
        stats->reserve(DeleteJobsHistory);

        scoped_lock sl(_statsHistoryMutex);
        for (deque<DeleteJobStats*>::const_iterator it = _statsHistory.begin();
                it != _statsHistory.end(); ++it) {
            stats->push_back(new DeleteJobStats(**it));
        }
    }

    BSONObj RangeDeleter::toBSON() const {
        scoped_lock sl(_queueMutex);

        BSONObjBuilder builder;

        BSONArrayBuilder notReadyBuilder(builder.subarrayStart("notReady"));
        for (TaskList::const_iterator iter = _notReadyQueue.begin();
                iter != _notReadyQueue.end(); ++iter) {
            notReadyBuilder.append((*iter)->toBSON());
        }
        notReadyBuilder.doneFast();

        BSONArrayBuilder readyBuilder(builder.subarrayStart("ready"));
        for (TaskList::const_iterator iter = _taskQueue.begin();
                iter != _taskQueue.end(); ++iter) {
            readyBuilder.append((*iter)->toBSON());
        }
        readyBuilder.doneFast();

        return builder.obj();
    }

    void RangeDeleter::doWork() {
        _env->initThread();

        while (!inShutdown() && !stopRequested()) {
            string errMsg;

            RangeDeleteEntry* nextTask = NULL;

            {
                scoped_lock sl(_queueMutex);
                while (_taskQueue.empty()) {
                    _taskQueueNotEmptyCV.timed_wait(
                        sl.boost(), duration::milliseconds(NotEmptyTimeoutMillis));

                    if (stopRequested()) {
                        log() << "stopping range deleter worker" << endl;
                        return;
                    }

                    if (_taskQueue.empty()) {
                        // Try to check if some deletes are ready and move them to the
                        // ready queue.

                        TaskList::iterator iter = _notReadyQueue.begin();
                        while (iter != _notReadyQueue.end()) {
                            RangeDeleteEntry* entry = *iter;

                            set<CursorId> cursorsNow;
                            {
                                boost::scoped_ptr<OperationContext> txn(getGlobalEnvironment()->newOpCtx());
                                _env->getCursorIds(txn.get(), entry->ns, &cursorsNow);
                            }

                            set<CursorId> cursorsLeft;
                            std::set_intersection(entry->cursorsToWait.begin(),
                                                  entry->cursorsToWait.end(),
                                                  cursorsNow.begin(),
                                                  cursorsNow.end(),
                                                  std::inserter(cursorsLeft,
                                                                cursorsLeft.end()));

                            entry->cursorsToWait.swap(cursorsLeft);

                            if (entry->cursorsToWait.empty()) {
                               (*iter)->stats.queueEndTS = jsTime();
                                _taskQueue.push_back(*iter);
                                _taskQueueNotEmptyCV.notify_one();
                                iter = _notReadyQueue.erase(iter);
                            }
                            else {
                                const unsigned long long int elapsedMillis =
                                    entry->stats.queueStartTS.millis - curTimeMillis64();
                                if ( elapsedMillis > LogCursorsThresholdMillis &&
                                    entry->timeSinceLastLog.millis > LogCursorsIntervalMillis) {

                                    entry->timeSinceLastLog = jsTime();
                                    logCursorsWaiting(entry->ns,
                                                      entry->min,
                                                      entry->max,
                                                      elapsedMillis,
                                                      entry->cursorsToWait);
                                }

                                ++iter;
                            }
                        }
                    }
                }

                if (stopRequested()) {
                    log() << "stopping range deleter worker" << endl;
                    return;
                }

                nextTask = _taskQueue.front();
                _taskQueue.pop_front();

                _deletesInProgress++;
            }

            {
                boost::scoped_ptr<OperationContext> txn(getGlobalEnvironment()->newOpCtx());

                nextTask->stats.deleteStartTS = jsTime();
                bool delResult = _env->deleteRange(txn.get(),
                                                   *nextTask,
                                                   &nextTask->stats.deletedDocCount,
                                                   &errMsg);
                nextTask->stats.deleteEndTS = jsTime();

                if (delResult) {
                    nextTask->stats.waitForReplStartTS = jsTime();

                    if (!_waitForReplication(txn.get(), &errMsg)) {
                        warning() << "Error encountered while waiting for replication: " << errMsg;
                    }

                    nextTask->stats.waitForReplEndTS = jsTime();
                }
                else {
                    warning() << "Error encountered while trying to delete range: "
                              << errMsg << endl;
                }
            }

            {
                scoped_lock sl(_queueMutex);

                NSMinMax setEntry(nextTask->ns, nextTask->min, nextTask->max);
                deletePtrElement(&_deleteSet, &setEntry);
                _deletesInProgress--;

                if (nextTask->notifyDone) {
                    nextTask->notifyDone->notifyOne();
                }
            }

            recordDelStats(new DeleteJobStats(nextTask->stats));
            delete nextTask;
            nextTask = NULL;
        }
    }

    bool RangeDeleter::isBlacklisted_inlock(const StringData& ns,
                                            const BSONObj& min,
                                            const BSONObj& max,
                                            std::string* errMsg) const {
        for (NSMinMaxSet::const_iterator iter = _blackList.begin();
                iter != _blackList.end(); ++iter) {
            const NSMinMax* const entry = *iter;
            if (ns != entry->ns) continue;

            if (rangeOverlaps(min, max, entry->min, entry->max)) {
                *errMsg = stream() << "ns: " << ns
                        << ", min: " << min
                        << ", max: " << max
                        << " intersects with black list"
                        << " min: " << entry->min
                        << ", max: " << entry->max;
                return true;
            }
        }

        return false;
    }

    bool RangeDeleter::canEnqueue_inlock(const StringData& ns,
                                         const BSONObj& min,
                                         const BSONObj& max,
                                         string* errMsg) const {
        if (isBlacklisted_inlock(ns, min, max, errMsg)) {
            return false;
        }

        NSMinMax toDelete(ns.toString(), min, max);
        if (_deleteSet.count(&toDelete) > 0) {
            *errMsg = stream() << "ns: " << ns
                    << ", min: " << min
                    << ", max: " << max
                    << " is already being processed for deletion.";
            return false;
        }

        return true;
    }

    bool RangeDeleter::stopRequested() const {
        scoped_lock sl(_stopMutex);
        return _stopRequested;
    }

    size_t RangeDeleter::getTotalDeletes() const {
        scoped_lock sl(_queueMutex);
        return _deleteSet.size();
    }

    size_t RangeDeleter::getPendingDeletes() const {
        scoped_lock sl(_queueMutex);
        return _notReadyQueue.size() + _taskQueue.size();
    }

    size_t RangeDeleter::getDeletesInProgress() const {
        scoped_lock sl(_queueMutex);
        return _deletesInProgress;
    }

    void RangeDeleter::recordDelStats(DeleteJobStats* newStat) {
        scoped_lock sl(_statsHistoryMutex);
        if (_statsHistory.size() == DeleteJobsHistory) {
            delete _statsHistory.front();
            _statsHistory.pop_front();
        }

        _statsHistory.push_back(newStat);
    }

    RangeDeleteEntry::RangeDeleteEntry(const std::string& ns,
                                       const BSONObj& min,
                                       const BSONObj& max,
                                       const BSONObj& shardKey,
                                       bool secondaryThrottle):
                                               ns(ns),
                                               min(min),
                                               max(max),
                                               shardKeyPattern(shardKey),
                                               secondaryThrottle(secondaryThrottle),
                                               notifyDone(NULL) {
    }

    BSONObj RangeDeleteEntry::toBSON() const {
        BSONObjBuilder builder;
        builder.append("ns", ns);
        builder.append("min", min);
        builder.append("max", max);
        BSONArrayBuilder cursorBuilder(builder.subarrayStart("cursors"));

        for (std::set<CursorId>::const_iterator it = cursorsToWait.begin();
                it != cursorsToWait.end(); ++it) {
            cursorBuilder.append((long long)*it);
        }
        cursorBuilder.doneFast();

        return builder.done().copy();
    }

}
