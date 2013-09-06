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

#include "mongo/db/range_deleter.h"

#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <memory>

#include "mongo/s/range_arithmetic.h"
#include "mongo/db/range_deleter_stats.h"
#include "mongo/util/concurrency/synchronization.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"

using std::auto_ptr;
using std::set;
using std::pair;
using std::string;

using mongoutils::str::stream;

namespace {
    const long int NotEmptyTimeoutMillis = 200;
    const long long int MaxCurorCheckIntervalMillis = 500;

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
}

namespace mongo {

    namespace duration = boost::posix_time;

    struct RangeDeleter::RangeDeleteEntry {
        RangeDeleteEntry():
                secondaryThrottle(true),
                notifyDone(NULL) {
        }

        std::string ns;

        // Inclusive lower range.
        BSONObj min;

        // Exclusive upper range.
        BSONObj max;

        // The key pattern of the index the range refers to.
        // This is relevant especially with special indexes types
        // like hash indexes.
        BSONObj shardKeyPattern;

        bool secondaryThrottle;

        // Sets of cursors to wait to close until this can be ready
        // for deletion.
        std::set<CursorId> cursorsToWait;

        // Not owned here.
        // Important invariant: Can only be set and used by one thread.
        Notification* notifyDone;

        // For debugging only
        BSONObj toBSON() const {
            return BSON("ns" << ns
                    << "min" << min
                    << "max" << max
                    << "notifyDoneAddr" << reinterpret_cast<long long>(notifyDone));
        }
    };

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
        _stats(new RangeDeleterStats(&_queueMutex)) {
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
            _worker.reset(new boost::thread(boost::bind(&RangeDeleter::doWork, this)));
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
        while (_stats->hasInProgress_inlock()) {
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

        auto_ptr<RangeDeleteEntry> toDelete(new RangeDeleteEntry);
        toDelete->ns = ns;
        toDelete->min = min.getOwned();
        toDelete->max = max.getOwned();
        toDelete->shardKeyPattern = shardKeyPattern.getOwned();
        toDelete->secondaryThrottle = secondaryThrottle;
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
            _stats->incTotalDeletes_inlock();
            _stats->incPendingDeletes_inlock();
        }

        _env->getCursorIds(ns, &toDelete->cursorsToWait);

        {
            scoped_lock sl(_queueMutex);

            if (toDelete->cursorsToWait.empty()) {
                _taskQueue.push_back(toDelete.release());
                _taskQueueNotEmptyCV.notify_one();
            }
            else {
                _notReadyQueue.push_back(toDelete.release());
            }
        }

        return true;
    }

    bool RangeDeleter::deleteNow(const std::string& ns,
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
            _stats->incTotalDeletes_inlock();

            // Note: count for pending deletes is an integral part of the shutdown story.
            // Therefore, to simplify things, there is no "pending" state for deletes in
            // deleteNow, the state transition is simply inProgress -> done.
            _stats->incInProgressDeletes_inlock();
        }

        set<CursorId> cursorsToWait;
        _env->getCursorIds(ns, &cursorsToWait);

        long long checkIntervalMillis = 5;

        while (!cursorsToWait.empty()) {
            set<CursorId> cursorsNow;
            _env->getCursorIds(ns, &cursorsNow);

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

                _stats->decInProgressDeletes_inlock();
                _stats->decTotalDeletes_inlock();

                if (!_stats->hasInProgress_inlock()) {
                    _nothingInProgressCV.notify_one();
                }

                return false;
            }

            if (checkIntervalMillis < MaxCurorCheckIntervalMillis) {
                checkIntervalMillis *= 2;
            }

            sleepmillis(checkIntervalMillis);
        }

        bool result = _env->deleteRange(ns, min, max, shardKeyPattern,
                                        secondaryThrottle, errMsg);

        {
            scoped_lock sl(_queueMutex);
            _deleteSet.erase(&deleteRange);

            _stats->decInProgressDeletes_inlock();
            _stats->decTotalDeletes_inlock();

            if (!_stats->hasInProgress_inlock()) {
                _nothingInProgressCV.notify_one();
            }
        }

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

    const RangeDeleterStats* RangeDeleter::getStats() const {
        return _stats.get();
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
                            _env->getCursorIds(entry->ns, &cursorsNow);

                            set<CursorId> cursorsLeft;
                            std::set_intersection(entry->cursorsToWait.begin(),
                                                  entry->cursorsToWait.end(),
                                                  cursorsNow.begin(),
                                                  cursorsNow.end(),
                                                  std::inserter(cursorsLeft,
                                                                cursorsLeft.end()));

                            entry->cursorsToWait.swap(cursorsLeft);

                            if (entry->cursorsToWait.empty()) {
                                _taskQueue.push_back(*iter);
                                _taskQueueNotEmptyCV.notify_one();
                                iter = _notReadyQueue.erase(iter);
                            }
                            else {
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

                _stats->decPendingDeletes_inlock();
                _stats->incInProgressDeletes_inlock();
            }

            if (!_env->deleteRange(nextTask->ns,
                                   nextTask->min,
                                   nextTask->max,
                                   nextTask->shardKeyPattern,
                                   nextTask->secondaryThrottle,
                                   &errMsg)) {
                warning() << "Error encountered while trying to delete range: "
                          << errMsg << endl;
            }

            {
                scoped_lock sl(_queueMutex);

                NSMinMax setEntry(nextTask->ns, nextTask->min, nextTask->max);
                deletePtrElement(&_deleteSet, &setEntry);
                _stats->decInProgressDeletes_inlock();
                _stats->decTotalDeletes_inlock();

                if (nextTask->notifyDone) {
                    nextTask->notifyDone->notifyOne();
                }

                delete nextTask;
                nextTask = NULL;
            }
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

}
