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

#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/condition_variable.hpp>
#include <boost/thread/mutex.hpp>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/list.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

    /**
     * Event loop for driving state machines in replication.
     *
     * The event loop has notions of events and callbacks.
     *
     * Callbacks are function objects representing work to be performed in some sequential order by
     * the executor.  They may be scheduled by client threads or by other callbacks.  Methods that
     * schedule callbacks return a CallbackHandle if they are able to enqueue the callback in the
     * appropriate work queue.  Every CallbackHandle represents an invocation of a function that
     * will happen before the executor returns from run().  Calling cancel(CallbackHandle) schedules
     * the specified callback to run with a flag indicating that it is "canceled," but it will run.
     * Client threads may block waiting for a callback to execute by calling wait(CallbackHandle).
     *
     * Events are level-triggered and may only be signaled one time.  Client threads and callbacks
     * may schedule callbacks to be run by the executor after the event is signaled, and client
     * threads may ask the executor to block them until after the event is signaled.
     *
     * If an event is unsignaled when shutdown is called, the executor will ensure that any threads
     * blocked in waitForEvent() eventually return.
     *
     * Logically, Callbacks and Events exist for the life of the executor.  That means that while
     * the executor is in scope, no CallbackHandle or EventHandle is stale.
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
     * on the WorkItem is incremented, to disambiguate.  Handles referencing WorkQueue::iterators,
     * called CallbackHandles, are thus valid for the life of the executor, simplifying lifecycle
     * management.
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
     *
     * Events work similiarly to WorkItems, and EventList is akin to WorkQueue.
     */
    class ReplicationExecutor {
        MONGO_DISALLOW_COPYING(ReplicationExecutor);
    public:
        typedef boost::posix_time::milliseconds Milliseconds;
        struct CallbackData;
        class CallbackHandle;
        class EventHandle;
        class NetworkInterface;
        struct RemoteCommandCallbackData;
        struct RemoteCommandRequest;

        static const Milliseconds kNoTimeout;
        static const Date_t kNoExpirationDate;

        /**
         * Type of a regular callback function.
         *
         * The status argument passed at invocation will have code ErrorCodes::CallbackCanceled if
         * the callback was canceled for any reason (including shutdown).  Otherwise, it should have
         * Status::OK().
         */
        typedef stdx::function<void (const CallbackData&)> CallbackFn;

        /**
         * Type of a callback from a request to run a command on a remote MongoDB node.
         *
         * The StatusWith<const BSONObj> will have ErrorCodes::CallbackCanceled if the callback was
         * canceled.  Otherwise, its status will represent any failure to execute the command.
         * If the command executed and a response came back, then the status object will contain
         * the BSONObj returned by the command, with the "ok" field indicating the success of the
         * command in the usual way.
         */
        typedef stdx::function<void (const RemoteCommandCallbackData&)> RemoteCommandCallbackFn;

        /**
         * Constructs a new executor.
         *
         * Takes ownership of the passed NetworkInterface object.
         */
        explicit ReplicationExecutor(NetworkInterface* netInterface);

        /**
         * Destroys an executor.
         */
        ~ReplicationExecutor();

        /**
         * Executes the run loop.  May be called up to one time.
         *
         * Returns after the executor has been shutdown and is safe to delete.
         */
        void run();

        /**
         * Signals to the executor that it should shut down.  The only reliable indication
         * that shutdown has completed is that the run() method returns.
         *
         * May be called by client threads or callbacks running in the executor.
         */
        void shutdown();

        /**
         * Creates a new event.  Returns a handle to the event, or ErrorCodes::ShutdownInProgress if
         * makeEvent() fails because the executor is shutting down.
         *
         * May be called by client threads or callbacks running in the executor.
         */
        StatusWith<EventHandle> makeEvent();

        /**
         * Signals the event, making waiting client threads and callbacks runnable.
         *
         * May be called up to one time per event.
         *
         * May be called by client threads or callbacks running in the executor.
         */
        void signalEvent(const EventHandle&);

        /**
         * Schedules a callback, "work", to run after "event" is signaled.  If "event"
         * has already been signaled, marks "work" as immediately runnable.
         *
         * If "event" has yet to be signaled when "shutdown()" is called, "work" will
         * be scheduled with a status of ErrorCodes::CallbackCanceled.
         *
         * May be called by client threads or callbacks running in the executor.
         */
        StatusWith<CallbackHandle> onEvent(const EventHandle& event, const CallbackFn& work);

        /**
         * Blocks the calling thread until after "event" is signaled.  Also returns
         * if the event is never signaled but shutdown() is called on the executor.
         *
         * NOTE: Do not call from a callback running in the executor.
         *
         * TODO(schwerin): Change return type so that the caller can know which of the two reasons
         * led to this method returning.
         */
        void waitForEvent(const EventHandle& event);

        /**
         * Schedules "work" to be run by the executor ASAP.
         *
         * Returns a handle for waiting on or canceling the callback, or
         * ErrorCodes::ShutdownInProgress.
         *
         * May be called by client threads or callbacks running in the executor.
         */
        StatusWith<CallbackHandle> scheduleWork(const CallbackFn& work);

        /**
         * Schedules "work" to be run by the executor no sooner than "when".
         *
         * Returns a handle for waiting on or canceling the callback, or
         * ErrorCodes::ShutdownInProgress.
         *
         * May be called by client threads or callbacks running in the executor.
         */
        StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, const CallbackFn& work);

        /**
         * Schedules "work" to be run by the executor while holding the global exclusive lock.
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
        StatusWith<CallbackHandle> scheduleWorkWithGlobalExclusiveLock(
                const CallbackFn& work);

        /**
         * Schedules "cb" to be run by the executor with the result of executing the remote command
         * described by "request".
         *
         * Returns a handle for waiting on or canceling the callback, or
         * ErrorCodes::ShutdownInProgress.
         *
         * May be called by client threads or callbacks running in the executor.
         */
        StatusWith<CallbackHandle> scheduleRemoteCommand(
                const RemoteCommandRequest& request,
                const RemoteCommandCallbackFn& cb);

        /**
         * If the callback referenced by "cbHandle" hasn't already executed, marks it as
         * canceled and runnable.
         *
         * May be called by client threads or callbacks running in the executor.
         */
        void cancel(const CallbackHandle& cbHandle);

        /**
         * Blocks until the executor finishes running the callback referenced by "cbHandle".
         *
         * Becaue callbacks all run during shutdown if they weren't run beforehand, there is no need
         * to indicate the reason for returning from wait(CallbackHandle).  It is always that the
         * callback ran.
         *
         * NOTE: Do not call from a callback running in the executor.
         */
        void wait(const CallbackHandle& cbHandle);

    private:
        struct Event;
        struct WorkItem;

        /**
         * A linked list of WorkItem objects.
         *
         * WorkItems get moved among lists by splicing iterators of work lists together,
         * not by copying underlying WorkItem objects.
         */
        typedef stdx::list<WorkItem> WorkQueue;

        /**
         * A linked list of Event objects, like WorkQueue, above.
         */
        typedef stdx::list<Event> EventList;

        /**
         * Implementation of makeEvent() for use when _mutex is already held.
         */
        StatusWith<EventHandle> makeEvent_inlock();

        /**
         * Gets a single piece of work to execute.
         *
         * If the "callback" member of the returned WorkItem is falsey, that is a signal
         * to the run loop to wait for shutdown.
         */
        std::pair<WorkItem, CallbackHandle> getWork();

        /**
         * Marks as runnable any sleepers whose ready date has passed.
         * Returns the amount of time before the next sleeper will be ready,
         * or -1ms if there are no remaining sleepers. 
         */
        Milliseconds scheduleReadySleepers_inlock();

        /**
         * Enqueues "callback" into "queue".
         *
         * Assumes that "queue" is sorted by readyDate, and performs insertion sort, starting
         * at the back of the "queue" working toward the front.
         *
         * Use Date_t(0) for readyDate to mean "ready now".
         */
        StatusWith<CallbackHandle> enqueueWork_inlock(WorkQueue* queue, const CallbackFn& callback);

        /**
         * Implementation of signalEvent() that assumes the caller owns _mutex.
         */
        void signalEvent_inlock(const EventHandle& event);

        /**
         * Notifies interested parties that shutdown has completed, if it has.
         */
        void maybeNotifyShutdownComplete_inlock();

        /**
         * Completes the shutdown process.  Called by run().
         */
        void finishShutdown();

        /**
         * Callback to do perform the described remote command.  These
         * get scheduled in the _networkWorkers pool.
         */
        void doRemoteCommand(const CallbackHandle& cbHandle,
                             const RemoteCommandRequest& request,
                             const RemoteCommandCallbackFn& cb);

        /**
         * Executes the callback referenced by "cbHandle", and moves the underlying
         * WorkQueue::iterator into the _freeQueue.
         *
         * Serializes execution of "cbHandle" with the execution of other callbacks.
         */
        void doOperationWithGlobalExclusiveLock(const CallbackHandle& cbHandle);

        boost::scoped_ptr<NetworkInterface> _networkInterface;
        boost::mutex _mutex;
        boost::mutex _terribleExLockSyncMutex;
        boost::condition_variable _workAvailable;
        boost::condition_variable _noMoreWaitingThreads;
        WorkQueue _freeQueue;
        WorkQueue _readyQueue;
        WorkQueue _exclusiveLockInProgressQueue;
        WorkQueue _networkInProgressQueue;
        WorkQueue _sleepersQueue;
        EventList _unsignaledEvents;
        EventList _signaledEvents;
        int64_t _totalEventWaiters;
        bool _inShutdown;
        threadpool::ThreadPool _networkWorkers;
        uint64_t _nextId;
    };

    /**
     * Reference to an event object in the executor.
     */
    class ReplicationExecutor::EventHandle {
        friend class ReplicationExecutor;
    public:
        EventHandle() : _generation(0), _id(0) {}

        /**
         * Returns true if the handle is valid, meaning that it identifies
         */
        bool isValid() const { return _id != 0; }

        bool operator==(const EventHandle &other) const {
            return (_id == other._id);
        }

        bool operator!=(const EventHandle &other) const {
            return !(*this == other);
        }

    private:
        EventHandle(const EventList::iterator& iter, const uint64_t id);

        EventList::iterator _iter;
        uint64_t _generation;
        uint64_t _id;
    };

    /**
     * Reference to a scheduled callback.
     */
    class ReplicationExecutor::CallbackHandle {
        friend class ReplicationExecutor;
    public:
        CallbackHandle() : _generation(0) {}

        bool isValid() const { return _finishedEvent.isValid(); }

        bool operator==(const CallbackHandle &other) const {
            return (_finishedEvent == other._finishedEvent);
        }

        bool operator!=(const CallbackHandle &other) const {
            return !(*this == other);
        }

    private:
        explicit CallbackHandle(const WorkQueue::iterator& iter);

        WorkQueue::iterator _iter;
        uint64_t _generation;
        EventHandle _finishedEvent;
    };

    struct ReplicationExecutor::CallbackData {
        CallbackData(ReplicationExecutor* theExecutor,
                     const CallbackHandle& theHandle,
                     const Status& theStatus);

        ReplicationExecutor* executor;
        CallbackHandle myHandle;
        Status status;
    };

    /**
     * Type of object describing a command to execute against a remote MongoDB node.
     */
    struct ReplicationExecutor::RemoteCommandRequest {
        RemoteCommandRequest();
        RemoteCommandRequest(const HostAndPort& theTarget,
                             const std::string& theDbName,
                             const BSONObj& theCmdObj,
                             const Milliseconds timeoutMillis = kNoTimeout);

        HostAndPort target;
        std::string dbname;
        BSONObj cmdObj;
        Date_t expirationDate;
    };

    struct ReplicationExecutor::RemoteCommandCallbackData {
        RemoteCommandCallbackData(ReplicationExecutor* theExecutor,
                                  const CallbackHandle& theHandle,
                                  const RemoteCommandRequest& theRequest,
                                  const StatusWith<BSONObj>& theResponse);

        ReplicationExecutor* executor;
        CallbackHandle myHandle;
        RemoteCommandRequest request;
        StatusWith<BSONObj> response;
    };

    /**
     * Interface to networking and lock manager.
     */
    class ReplicationExecutor::NetworkInterface {
        MONGO_DISALLOW_COPYING(NetworkInterface);
    public:
        virtual ~NetworkInterface();

        /**
         * Returns the current time.
         */
        virtual Date_t now() = 0;

        /**
         * Runs the command described by "request" synchronously.
         */
        virtual StatusWith<BSONObj> runCommand(
                const RemoteCommandRequest& request) = 0;

        /**
         * Runs the given callback while holding the global exclusive lock.
         */
        virtual void runCallbackWithGlobalExclusiveLock(
                const stdx::function<void ()>& callback) = 0;

    protected:
        NetworkInterface();
    };

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
        CallbackFn callback;
        EventHandle finishedEvent;
        Date_t readyDate;
        bool isCanceled;
    };

    /**
     * Description of an unsignaled event.
     *
     * Like WorkItem, above, but for events.  On signaling, the executor bumps the
     * generation, marks all waiters as runnable, and moves the event from the "unsignaled"
     * EventList to the "signaled" EventList, the latter being a free list of events.
     */
    struct ReplicationExecutor::Event {
        Event();
        uint64_t generation;
        bool isSignaled;
        WorkQueue waiters;
        boost::shared_ptr<boost::condition_variable> isSignaledCondition;
    };

}  // namespace repl
}  // namespace mongo
