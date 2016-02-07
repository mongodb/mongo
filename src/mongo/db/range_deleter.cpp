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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/range_deleter.h"

#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/concurrency/synchronization.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/timer.h"

using std::unique_ptr;
using std::endl;
using std::set;
using std::pair;
using std::string;

namespace {
const long int kNotEmptyTimeoutMillis = 200;
const long long int kMaxCursorCheckIntervalMillis = 500;
const size_t kDeleteJobsHistory = 10;  // entries

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

static void logCursorsWaiting(RangeDeleteEntry* entry) {
    // We always log the first cursors waiting message (so we have cursor ids in the logs).
    // After 15 minutes (the cursor timeout period), we start logging additional messages at
    // a 1 minute interval.
    static const auto kLogCursorsThreshold = stdx::chrono::minutes{15};
    static const auto kLogCursorsInterval = stdx::chrono::minutes{1};

    Date_t currentTime = jsTime();
    Milliseconds elapsedMillisSinceQueued{0};

    // We always log the first message when lastLoggedTime == 0
    if (entry->lastLoggedTS != Date_t()) {
        if (currentTime > entry->stats.queueStartTS)
            elapsedMillisSinceQueued = currentTime - entry->stats.queueStartTS;

        // Not logging, threshold not passed
        if (elapsedMillisSinceQueued < kLogCursorsThreshold)
            return;

        Milliseconds elapsedMillisSinceLog{0};
        if (currentTime > entry->lastLoggedTS)
            elapsedMillisSinceLog = currentTime - entry->lastLoggedTS;

        // Not logging, logged a short time ago
        if (elapsedMillisSinceLog < kLogCursorsInterval)
            return;
    }

    str::stream cursorList;
    for (std::set<CursorId>::const_iterator it = entry->cursorsToWait.begin();
         it != entry->cursorsToWait.end();
         ++it) {
        if (it != entry->cursorsToWait.begin())
            cursorList << ", ";
        cursorList << *it;
    }

    log() << "waiting for open cursors before removing range "
          << "[" << entry->options.range.minKey << ", " << entry->options.range.maxKey << ") "
          << "in " << entry->options.range.ns
          << (entry->lastLoggedTS == Date_t()
                  ? string("")
                  : string(str::stream() << ", elapsed secs: "
                                         << durationCount<Seconds>(elapsedMillisSinceQueued)))
          << ", cursor ids: [" << string(cursorList) << "]";

    entry->lastLoggedTS = currentTime;
}

struct RangeDeleter::NSMinMax {
    NSMinMax(std::string ns, const BSONObj min, const BSONObj max) : ns(ns), min(min), max(max) {}

    std::string ns;

    // Inclusive lower range.
    BSONObj min;

    // Exclusive upper range.
    BSONObj max;
};

bool RangeDeleter::NSMinMaxCmp::operator()(const NSMinMax* lhs, const NSMinMax* rhs) const {
    const int nsComp = lhs->ns.compare(rhs->ns);

    if (nsComp < 0) {
        return true;
    }

    if (nsComp > 0) {
        return false;
    }

    return compareRanges(lhs->min, lhs->max, rhs->min, rhs->max) < 0;
}

RangeDeleter::RangeDeleter(RangeDeleterEnv* env)
    : _env(env),  // ownership xfer
      _stopRequested(false),
      _deletesInProgress(0) {}

RangeDeleter::~RangeDeleter() {
    for (TaskList::iterator it = _notReadyQueue.begin(); it != _notReadyQueue.end(); ++it) {
        delete (*it);
    }

    for (TaskList::iterator it = _taskQueue.begin(); it != _taskQueue.end(); ++it) {
        delete (*it);
    }

    for (NSMinMaxSet::iterator it = _deleteSet.begin(); it != _deleteSet.end(); ++it) {
        delete (*it);
    }

    for (std::deque<DeleteJobStats*>::iterator it = _statsHistory.begin();
         it != _statsHistory.end();
         ++it) {
        delete (*it);
    }
}

void RangeDeleter::startWorkers() {
    if (!_worker) {
        _worker.reset(new stdx::thread(stdx::bind(&RangeDeleter::doWork, this)));
    }
}

void RangeDeleter::stopWorkers() {
    {
        stdx::lock_guard<stdx::mutex> sl(_stopMutex);
        _stopRequested = true;
    }

    if (_worker) {
        _worker->join();
    }

    stdx::unique_lock<stdx::mutex> sl(_queueMutex);
    while (_deletesInProgress > 0) {
        _nothingInProgressCV.wait(sl);
    }
}

bool RangeDeleter::queueDelete(OperationContext* txn,
                               const RangeDeleterOptions& options,
                               Notification* notifyDone,
                               std::string* errMsg) {
    string dummy;
    if (errMsg == NULL)
        errMsg = &dummy;

    const string& ns(options.range.ns);
    const BSONObj& min(options.range.minKey);
    const BSONObj& max(options.range.maxKey);

    unique_ptr<RangeDeleteEntry> toDelete(new RangeDeleteEntry(options));
    toDelete->notifyDone = notifyDone;

    {
        stdx::lock_guard<stdx::mutex> sl(_queueMutex);
        if (_stopRequested) {
            *errMsg = "deleter is already stopped.";
            return false;
        }

        if (!canEnqueue_inlock(ns, min, max, errMsg)) {
            return false;
        }

        _deleteSet.insert(new NSMinMax(ns, min.getOwned(), max.getOwned()));
    }

    if (options.waitForOpenCursors) {
        _env->getCursorIds(txn, ns, &toDelete->cursorsToWait);
    }

    toDelete->stats.queueStartTS = jsTime();

    if (!toDelete->cursorsToWait.empty())
        logCursorsWaiting(toDelete.get());

    {
        stdx::lock_guard<stdx::mutex> sl(_queueMutex);

        if (toDelete->cursorsToWait.empty()) {
            toDelete->stats.queueEndTS = jsTime();
            _taskQueue.push_back(toDelete.release());
            _taskQueueNotEmptyCV.notify_one();
        } else {
            _notReadyQueue.push_back(toDelete.release());
        }
    }

    return true;
}

namespace {
const int kWTimeoutMillis = 60 * 60 * 1000;

bool _waitForMajority(OperationContext* txn, std::string* errMsg) {
    const WriteConcernOptions writeConcern(
        WriteConcernOptions::kMajority, WriteConcernOptions::SyncMode::NONE, kWTimeoutMillis);

    repl::ReplicationCoordinator::StatusAndDuration replStatus =
        repl::getGlobalReplicationCoordinator()->awaitReplicationOfLastOpForClient(txn,
                                                                                   writeConcern);
    Milliseconds elapsedTime = replStatus.duration;
    if (replStatus.status.code() == ErrorCodes::ExceededTimeLimit) {
        *errMsg = str::stream() << "rangeDeleter timed out after "
                                << durationCount<Seconds>(elapsedTime)
                                << " seconds while waiting"
                                   " for deletions to be replicated to majority nodes";
        log() << *errMsg;
    } else if (replStatus.status.code() == ErrorCodes::NotMaster) {
        *errMsg = str::stream() << "rangeDeleter no longer PRIMARY after "
                                << durationCount<Seconds>(elapsedTime)
                                << " seconds while waiting"
                                   " for deletions to be replicated to majority nodes";
    } else if (replStatus.status.code() == ErrorCodes::InterruptedAtShutdown) {
        *errMsg = str::stream() << "rangeDeleter interrupted by shutdown while waiting for "
                                   "deletions to be replicated to a majority of nodes";
    } else {
        LOG(elapsedTime < Seconds(30) ? 1 : 0)
            << "rangeDeleter took " << durationCount<Seconds>(elapsedTime) << " seconds "
            << " waiting for deletes to be replicated to majority nodes";

        fassert(18512, replStatus.status);
    }

    return replStatus.status.isOK();
}
}

bool RangeDeleter::deleteNow(OperationContext* txn,
                             const RangeDeleterOptions& options,
                             string* errMsg) {
    if (stopRequested()) {
        *errMsg = "deleter is already stopped.";
        return false;
    }

    string dummy;
    if (errMsg == NULL)
        errMsg = &dummy;

    const string& ns(options.range.ns);
    const BSONObj& min(options.range.minKey);
    const BSONObj& max(options.range.maxKey);

    NSMinMax deleteRange(ns, min, max);
    {
        stdx::lock_guard<stdx::mutex> sl(_queueMutex);
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
    if (options.waitForOpenCursors) {
        _env->getCursorIds(txn, ns, &cursorsToWait);
    }

    long long checkIntervalMillis = 5;

    RangeDeleteEntry taskDetails(options);
    taskDetails.stats.queueStartTS = jsTime();

    for (; !cursorsToWait.empty(); sleepmillis(checkIntervalMillis)) {
        logCursorsWaiting(&taskDetails);

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

            stdx::lock_guard<stdx::mutex> sl(_queueMutex);
            _deleteSet.erase(&deleteRange);

            _deletesInProgress--;

            if (_deletesInProgress == 0) {
                _nothingInProgressCV.notify_one();
            }

            return false;
        }

        if (checkIntervalMillis < kMaxCursorCheckIntervalMillis) {
            checkIntervalMillis *= 2;
        }
    }
    taskDetails.stats.queueEndTS = jsTime();

    taskDetails.stats.deleteStartTS = jsTime();
    bool result = _env->deleteRange(txn, taskDetails, &taskDetails.stats.deletedDocCount, errMsg);

    taskDetails.stats.deleteEndTS = jsTime();

    if (result) {
        taskDetails.stats.waitForReplStartTS = jsTime();
        result = _waitForMajority(txn, errMsg);
        taskDetails.stats.waitForReplEndTS = jsTime();
    }

    {
        stdx::lock_guard<stdx::mutex> sl(_queueMutex);
        _deleteSet.erase(&deleteRange);

        _deletesInProgress--;

        if (_deletesInProgress == 0) {
            _nothingInProgressCV.notify_one();
        }
    }

    recordDelStats(new DeleteJobStats(taskDetails.stats));
    return result;
}

void RangeDeleter::getStatsHistory(std::vector<DeleteJobStats*>* stats) const {
    stats->clear();
    stats->reserve(kDeleteJobsHistory);

    stdx::lock_guard<stdx::mutex> sl(_statsHistoryMutex);
    for (std::deque<DeleteJobStats*>::const_iterator it = _statsHistory.begin();
         it != _statsHistory.end();
         ++it) {
        stats->push_back(new DeleteJobStats(**it));
    }
}

BSONObj RangeDeleter::toBSON() const {
    stdx::lock_guard<stdx::mutex> sl(_queueMutex);

    BSONObjBuilder builder;

    BSONArrayBuilder notReadyBuilder(builder.subarrayStart("notReady"));
    for (TaskList::const_iterator iter = _notReadyQueue.begin(); iter != _notReadyQueue.end();
         ++iter) {
        notReadyBuilder.append((*iter)->toBSON());
    }
    notReadyBuilder.doneFast();

    BSONArrayBuilder readyBuilder(builder.subarrayStart("ready"));
    for (TaskList::const_iterator iter = _taskQueue.begin(); iter != _taskQueue.end(); ++iter) {
        readyBuilder.append((*iter)->toBSON());
    }
    readyBuilder.doneFast();

    return builder.obj();
}

void RangeDeleter::doWork() {
    Client::initThreadIfNotAlready("RangeDeleter");
    Client* client = &cc();

    while (!inShutdown() && !stopRequested()) {
        string errMsg;

        RangeDeleteEntry* nextTask = NULL;

        {
            stdx::unique_lock<stdx::mutex> sl(_queueMutex);
            while (_taskQueue.empty()) {
                _taskQueueNotEmptyCV.wait_for(sl,
                                              stdx::chrono::milliseconds(kNotEmptyTimeoutMillis));

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
                        if (entry->options.waitForOpenCursors) {
                            auto txn = client->makeOperationContext();
                            _env->getCursorIds(txn.get(), entry->options.range.ns, &cursorsNow);
                        }

                        set<CursorId> cursorsLeft;
                        std::set_intersection(entry->cursorsToWait.begin(),
                                              entry->cursorsToWait.end(),
                                              cursorsNow.begin(),
                                              cursorsNow.end(),
                                              std::inserter(cursorsLeft, cursorsLeft.end()));

                        entry->cursorsToWait.swap(cursorsLeft);

                        if (entry->cursorsToWait.empty()) {
                            (*iter)->stats.queueEndTS = jsTime();
                            _taskQueue.push_back(*iter);
                            _taskQueueNotEmptyCV.notify_one();
                            iter = _notReadyQueue.erase(iter);
                        } else {
                            logCursorsWaiting(entry);
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
            auto txn = client->makeOperationContext();
            nextTask->stats.deleteStartTS = jsTime();
            bool delResult =
                _env->deleteRange(txn.get(), *nextTask, &nextTask->stats.deletedDocCount, &errMsg);
            nextTask->stats.deleteEndTS = jsTime();

            if (delResult) {
                nextTask->stats.waitForReplStartTS = jsTime();

                if (!_waitForMajority(txn.get(), &errMsg)) {
                    warning() << "Error encountered while waiting for replication: " << errMsg;
                }

                nextTask->stats.waitForReplEndTS = jsTime();
            } else {
                warning() << "Error encountered while trying to delete range: " << errMsg << endl;
            }
        }

        {
            stdx::lock_guard<stdx::mutex> sl(_queueMutex);

            NSMinMax setEntry(nextTask->options.range.ns,
                              nextTask->options.range.minKey,
                              nextTask->options.range.maxKey);
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

bool RangeDeleter::canEnqueue_inlock(StringData ns,
                                     const BSONObj& min,
                                     const BSONObj& max,
                                     string* errMsg) const {
    NSMinMax toDelete(ns.toString(), min, max);
    if (_deleteSet.count(&toDelete) > 0) {
        *errMsg = str::stream() << "ns: " << ns << ", min: " << min << ", max: " << max
                                << " is already being processed for deletion.";
        return false;
    }

    return true;
}

bool RangeDeleter::stopRequested() const {
    stdx::lock_guard<stdx::mutex> sl(_stopMutex);
    return _stopRequested;
}

size_t RangeDeleter::getTotalDeletes() const {
    stdx::lock_guard<stdx::mutex> sl(_queueMutex);
    return _deleteSet.size();
}

size_t RangeDeleter::getPendingDeletes() const {
    stdx::lock_guard<stdx::mutex> sl(_queueMutex);
    return _notReadyQueue.size() + _taskQueue.size();
}

size_t RangeDeleter::getDeletesInProgress() const {
    stdx::lock_guard<stdx::mutex> sl(_queueMutex);
    return _deletesInProgress;
}

void RangeDeleter::recordDelStats(DeleteJobStats* newStat) {
    stdx::lock_guard<stdx::mutex> sl(_statsHistoryMutex);
    if (_statsHistory.size() == kDeleteJobsHistory) {
        delete _statsHistory.front();
        _statsHistory.pop_front();
    }

    _statsHistory.push_back(newStat);
}

RangeDeleteEntry::RangeDeleteEntry(const RangeDeleterOptions& options)
    : options(options), notifyDone(NULL) {}

BSONObj RangeDeleteEntry::toBSON() const {
    BSONObjBuilder builder;
    builder.append("ns", options.range.ns);
    builder.append("min", options.range.minKey);
    builder.append("max", options.range.maxKey);
    BSONArrayBuilder cursorBuilder(builder.subarrayStart("cursors"));

    for (std::set<CursorId>::const_iterator it = cursorsToWait.begin(); it != cursorsToWait.end();
         ++it) {
        cursorBuilder.append((long long)*it);
    }
    cursorBuilder.doneFast();

    return builder.done().copy();
}

RangeDeleterOptions::RangeDeleterOptions(const KeyRange& range)
    : range(range), fromMigrate(false), onlyRemoveOrphanedDocs(false), waitForOpenCursors(false) {}
}
