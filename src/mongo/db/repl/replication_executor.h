/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/list.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/concurrency/old_thread_pool.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObjBuilder;
class NamespaceString;
class OperationContext;

namespace executor {
struct ConnectionPoolStats;
class NetworkInterface;
}  // namespace executor

namespace repl {

/**
 * Implementation of the TaskExecutor interface for providing an event loop for driving state
 * machines in replication.
 *
 * Usage: Instantiate an executor, schedule a work item, call run().
 *
 * Implementation details:
 *
 * The executor is composed of several WorkQueues, which are queues of WorkItems.  WorkItems
 * describe units of work -- a callback and state needed to track its lifecycle.  The iterators
 * pointing to WorkItems are spliced between the WorkQueues, rather than copying WorkItems
 * themselves.  Further, those WorkQueue::iterators are never invalidated during the life of an
 * executor.  They may be recycled to represent new work items, but when that happens, a counter
 * on the WorkItem is incremented, to disambiguate.
 *
 * All work executed by the run() method of the executor is popped off the front of the
 * _readyQueue.  Remote commands blocked on the network can be found in the
 * _networkInProgressQueue.  Callbacks waiting for a timer to expire are in the _sleepersQueue.
 * When the network returns or the timer expires, items from these two queues are transferred to
 * the back of the _readyQueue.
 *
 * The _exclusiveLockInProgressQueue, which represents work items to execute while holding the
 * GlobalWrite lock, is exceptional.  WorkItems in that queue execute in unspecified order with
 * respect to work in the _readyQueue or other WorkItems in the _exclusiveLockInProgressQueue,
 * but they are executed in a single serial order with respect to those other WorkItems.  The
 * _terribleExLockSyncMutex is used to provide this serialization, until such time as the global
 * lock may be passed from one thread to another.
 */
class ReplicationExecutor final : public executor::TaskExecutor {
    MONGO_DISALLOW_COPYING(ReplicationExecutor);

public:
    /**
     * Constructs a new executor.
     *
     * Takes ownership of the passed NetworkInterface object.
     */
    ReplicationExecutor(executor::NetworkInterface* netInterface, int64_t pnrgSeed);

    /**
     * Destroys an executor.
     */
    virtual ~ReplicationExecutor();

    std::string getDiagnosticString() override;
    BSONObj getDiagnosticBSON();
    Date_t now() override;
    void startup() override;
    void shutdown() override;
    void join() override;
    void signalEvent(const EventHandle& event) override;
    StatusWith<EventHandle> makeEvent() override;
    StatusWith<CallbackHandle> onEvent(const EventHandle& event, const CallbackFn& work) override;
    void waitForEvent(const EventHandle& event) override;
    StatusWith<CallbackHandle> scheduleWork(const CallbackFn& work) override;
    StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, const CallbackFn& work) override;
    StatusWith<CallbackHandle> scheduleRemoteCommand(const executor::RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb) override;
    void cancel(const CallbackHandle& cbHandle) override;
    void wait(const CallbackHandle& cbHandle) override;

    void appendConnectionStats(executor::ConnectionPoolStats* stats) const override;

    /**
     * Executes the run loop. May be called up to one time.
     *
     * Doesn't need to be public, so do not call directly unless from unit-tests.
     *
     * Returns after the executor has been shutdown and is safe to delete.
     */
    void run();

    /**
     * Schedules DB "work" to be run by the executor..
     *
     * Takes no locks for caller - global, database or collection.
     *
     * The "work" will run exclusively with other DB work items. All DB work items
     * are run the in order they are scheduled.
     *
     * The "work" may run concurrently with other non-DB work items,
     * but there are no ordering guarantees provided with respect to
     * any other work item.
     *
     * Returns a handle for waiting on or canceling the callback, or
     * ErrorCodes::ShutdownInProgress.
     *
     * May be called by client threads or callbacks running in the executor.
     */
    StatusWith<CallbackHandle> scheduleDBWork(const CallbackFn& work);

    /**
     * Schedules DB "work" to be run by the executor while holding the collection lock.
     *
     * Takes collection lock in specified mode (and slightly more permissive lock for the
     * database lock) but not the global exclusive lock.
     *
     * The "work" will run exclusively with other DB work items. All DB work items
     * are run the in order they are scheduled.
     *
     * The "work" may run concurrently with other non-DB work items,
     * but there are no ordering guarantees provided with respect to
     * any other work item.
     *
     * Returns a handle for waiting on or canceling the callback, or
     * ErrorCodes::ShutdownInProgress.
     *
     * May be called by client threads or callbacks running in the executor.
     */
    StatusWith<CallbackHandle> scheduleDBWork(const CallbackFn& work,
                                              const NamespaceString& nss,
                                              LockMode mode);

    /**
     * Schedules "work" to be run by the executor while holding the global exclusive lock.
     *
     * Takes collection lock in specified mode (and slightly more permissive lock for the
     * database lock) but not the global exclusive lock.
     *
     * The "work" will run exclusively, as though it were executed by the main
     * run loop, but there are no ordering guarantees provided with respect to
     * any other work item.
     *
     * Returns a handle for waiting on or canceling the callback, or
     * ErrorCodes::ShutdownInProgress.
     *
     * May be called by client threads or callbacks running in the executor.
     */
    StatusWith<CallbackHandle> scheduleWorkWithGlobalExclusiveLock(const CallbackFn& work);

    /**
     * Returns an int64_t generated by the prng with a max value of "limit".
     */
    int64_t nextRandomInt64(int64_t limit);

private:
    class Callback;
    class Event;
    struct WorkItem;
    friend class Callback;
    friend class Event;


    /**
     * A linked list of WorkItem objects.
     *
     * WorkItems get moved among lists by splicing iterators of work lists together,
     * not by copying underlying WorkItem objects.
     */
    typedef stdx::list<WorkItem> WorkQueue;

    /**
     * A linked list of EventHandles.
     */
    typedef stdx::list<EventHandle> EventList;

    /**
     * Returns diagnostic info
     */
    std::string _getDiagnosticString_inlock() const;

    /**
     * Implementation of makeEvent() for use when _mutex is already held.
     */
    StatusWith<EventHandle> makeEvent_inlock();

    /**
     * Implementation of signalEvent() for use when _mutex is already held.
     */
    void signalEvent_inlock(const EventHandle&);

    /**
     * Gets a single piece of work to execute.
     *
     * If the "callback" member of the returned WorkItem is falsey, that is a signal
     * to the run loop to wait for shutdown.
     */
    std::pair<WorkItem, CallbackHandle> getWork();

    /**
     * Marks as runnable any sleepers whose ready date has passed as of "now".
     * Returns the date when the next sleeper will be ready, or Date_t(~0ULL) if there are no
     * remaining sleepers.
     */
    Date_t scheduleReadySleepers_inlock(Date_t now);

    /**
     * Enqueues "callback" into "queue".
     */
    StatusWith<CallbackHandle> enqueueWork_inlock(WorkQueue* queue, const CallbackFn& callback);

    /**
     * Notifies interested parties that shutdown has completed, if it has.
     */
    void maybeNotifyShutdownComplete_inlock();

    /**
     * Completes the shutdown process.  Called by run().
     */
    void finishShutdown();

    void _finishRemoteCommand(const executor::RemoteCommandRequest& request,
                              const StatusWith<executor::RemoteCommandResponse>& response,
                              const CallbackHandle& cbHandle,
                              const uint64_t expectedHandleGeneration,
                              const RemoteCommandCallbackFn& cb);

    /**
     * Executes the callback referenced by "cbHandle", and moves the underlying
     * WorkQueue::iterator from "workQueue" into the _freeQueue.
     *
     * "txn" is a pointer to the OperationContext.
     *
     * "status" is the callback status from the task runner. Only possible values are
     * Status::OK and ErrorCodes::CallbackCanceled (when task runner is canceled).
     *
     * If "terribleExLockSyncMutex" is not null, serializes execution of "cbHandle" with the
     * execution of other callbacks.
     */
    void _doOperation(OperationContext* txn,
                      const Status& taskRunnerStatus,
                      const CallbackHandle& cbHandle,
                      WorkQueue* workQueue,
                      stdx::mutex* terribleExLockSyncMutex);

    /**
     * Wrapper around TaskExecutor::getCallbackFromHandle that return an Event* instead of
     * a generic EventState*.
     */
    Event* _getEventFromHandle(const EventHandle& eventHandle);

    /**
     * Wrapper around TaskExecutor::getCallbackFromHandle that return an Event* instead of
     * a generic EventState*.
     */
    Callback* _getCallbackFromHandle(const CallbackHandle& callbackHandle);

    // PRNG; seeded at class construction time.
    PseudoRandom _random;

    std::unique_ptr<executor::NetworkInterface> _networkInterface;

    // Thread which executes the run method. Started by startup and must be jointed after shutdown.
    stdx::thread _executorThread;

    stdx::mutex _mutex;
    stdx::mutex _terribleExLockSyncMutex;
    stdx::condition_variable _noMoreWaitingThreads;
    WorkQueue _freeQueue;
    WorkQueue _readyQueue;
    WorkQueue _dbWorkInProgressQueue;
    WorkQueue _exclusiveLockInProgressQueue;
    WorkQueue _networkInProgressQueue;
    WorkQueue _sleepersQueue;
    EventList _unsignaledEvents;
    int64_t _totalEventWaiters = 0;

    // Counters for metrics, for the whole life of this instance, protected by _mutex.
    int64_t _counterWaitEvents = 0;
    int64_t _counterCreatedEvents = 0;
    int64_t _counterScheduledCommands = 0;
    int64_t _counterScheduledExclusiveWorks = 0;
    int64_t _counterScheduledDBWorks = 0;
    int64_t _counterScheduledWorks = 0;
    int64_t _counterScheduledWorkAts = 0;
    int64_t _counterSchedulingFailures = 0;
    int64_t _counterCancels = 0;
    int64_t _counterWaits = 0;

    bool _inShutdown;
    OldThreadPool _dblockWorkers;
    TaskRunner _dblockTaskRunner;
    TaskRunner _dblockExclusiveLockTaskRunner;
    uint64_t _nextId = 0;
};

class ReplicationExecutor::Callback : public executor::TaskExecutor::CallbackState {
    friend class ReplicationExecutor;

public:
    Callback(ReplicationExecutor* executor,
             const CallbackFn callbackFn,
             const WorkQueue::iterator& iter,
             const EventHandle& finishedEvent);
    virtual ~Callback();

    void cancel() override;
    void waitForCompletion() override;
    bool isCanceled() const override;

private:
    ReplicationExecutor* _executor;

    // All members other than _executor are protected by the executor's _mutex.
    CallbackFn _callbackFn;
    bool _isCanceled;
    bool _isSleeper;
    WorkQueue::iterator _iter;
    EventHandle _finishedEvent;
};

typedef ReplicationExecutor::ResponseStatus ResponseStatus;

/**
 * Description of a scheduled but not-yet-run work item.
 *
 * Once created, WorkItem objects remain in scope until the executor is destroyed.
 * However, over their lifetime, they may represent many different work items.  This
 * divorces the lifetime of CallbackHandles from the lifetime of WorkItem objects, but
 * requires a unique generation identifier in CallbackHandles and WorkItem objects.
 *
 * WorkItem is copyable so that it may be stored in a list.  However, in practice they
 * should only be copied by getWork() and when allocating new entries into a WorkQueue (not
 * when moving entries between work lists).
 */
struct ReplicationExecutor::WorkItem {
    WorkItem();
    uint64_t generation;
    CallbackHandle callback;
    EventHandle finishedEvent;
    Date_t readyDate;
    bool isNetworkOperation;
};

/**
 * Description of an event.
 *
 * Like WorkItem, above, but for events.  On signaling, the executor removes the event from the
 * "unsignaled" EventList and schedules all work items in the _waiters list.
 */
class ReplicationExecutor::Event : public executor::TaskExecutor::EventState {
    friend class ReplicationExecutor;

public:
    Event(ReplicationExecutor* executor, const EventList::iterator& iter);
    virtual ~Event();

    void signal() override;
    void waitUntilSignaled() override;
    bool isSignaled() override;

private:
    // Note that the caller is responsible for removing any references to any EventHandles
    // pointing to this event.
    void _signal_inlock();

    ReplicationExecutor* _executor;

    // All members other than _executor are protected by the executor's _mutex.
    bool _isSignaled;
    stdx::condition_variable _isSignaledCondition;
    EventList::iterator _iter;
    WorkQueue _waiters;
};

}  // namespace repl
}  // namespace mongo
