/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/list.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"

namespace mongo {

class ThreadPoolInterface;

namespace executor {

struct ConnectionPoolStats;
class NetworkInterface;

/**
 * Implementation of a TaskExecutor that uses a pool of threads to execute work items.
 */
class ThreadPoolTaskExecutor final : public TaskExecutor {
    MONGO_DISALLOW_COPYING(ThreadPoolTaskExecutor);

public:
    /**
     * Constructs an instance of ThreadPoolTaskExecutor that runs tasks in "pool" and uses "net"
     * for network operations.
     */
    ThreadPoolTaskExecutor(std::unique_ptr<ThreadPoolInterface> pool,
                           std::unique_ptr<NetworkInterface> net);

    /**
     * Destroys a ThreadPoolTaskExecutor.
     */
    ~ThreadPoolTaskExecutor();

    void startup() override;
    void shutdown() override;
    void join() override;
    std::string getDiagnosticString() override;
    Date_t now() override;
    StatusWith<EventHandle> makeEvent() override;
    void signalEvent(const EventHandle& event) override;
    StatusWith<CallbackHandle> onEvent(const EventHandle& event, const CallbackFn& work) override;
    void waitForEvent(const EventHandle& event) override;
    StatusWith<CallbackHandle> scheduleWork(const CallbackFn& work) override;
    StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, const CallbackFn& work) override;
    StatusWith<CallbackHandle> scheduleRemoteCommand(const RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb) override;
    void cancel(const CallbackHandle& cbHandle) override;
    void wait(const CallbackHandle& cbHandle) override;

    void appendConnectionStats(ConnectionPoolStats* stats) const override;

    /**
     * Cancels all commands on the network interface.
     */
    void cancelAllCommands();

private:
    class CallbackState;
    class EventState;
    using WorkQueue = stdx::list<std::shared_ptr<CallbackState>>;
    using EventList = stdx::list<std::shared_ptr<EventState>>;

    /**
     * Returns an EventList containing one unsignaled EventState. This is a helper function for
     * performing allocations outside of _mutex, and should only be called by makeSingletonWork and
     * makeEvent().
     */
    static EventList makeSingletonEventList();

    /**
     * Returns an object suitable for passing to enqueueCallbackState_inlock that represents
     * executing "work" no sooner than "when" (defaults to ASAP). This function may and should be
     * called outside of _mutex.
     */
    static WorkQueue makeSingletonWorkQueue(CallbackFn work, Date_t when = {});

    /**
     * Moves the single callback in "wq" to the end of "queue". It is required that "wq" was
     * produced via a call to makeSingletonWorkQueue().
     */
    StatusWith<CallbackHandle> enqueueCallbackState_inlock(WorkQueue* queue, WorkQueue* wq);

    /**
     * Signals the given event.
     */
    void signalEvent_inlock(const EventHandle& event, stdx::unique_lock<stdx::mutex> lk);

    /**
     * Schedules all items from "fromQueue" into the thread pool and moves them into
     * _poolInProgressQueue.
     */
    void scheduleIntoPool_inlock(WorkQueue* fromQueue, stdx::unique_lock<stdx::mutex> lk);

    /**
     * Schedules the given item from "fromQueue" into the thread pool and moves it into
     * _poolInProgressQueue.
     */
    void scheduleIntoPool_inlock(WorkQueue* fromQueue,
                                 const WorkQueue::iterator& iter,
                                 stdx::unique_lock<stdx::mutex> lk);

    /**
     * Schedules entries from "begin" through "end" in "fromQueue" into the thread pool
     * and moves them into _poolInProgressQueue.
     */
    void scheduleIntoPool_inlock(WorkQueue* fromQueue,
                                 const WorkQueue::iterator& begin,
                                 const WorkQueue::iterator& end,
                                 stdx::unique_lock<stdx::mutex> lk);

    /**
     * Executes the callback specified by "cbState".
     */
    void runCallback(std::shared_ptr<CallbackState> cbState);

    // The network interface used for remote command execution and waiting.
    std::unique_ptr<NetworkInterface> _net;

    // The thread pool that executes scheduled work items.
    std::unique_ptr<ThreadPoolInterface> _pool;

    // Mutex guarding all remaining fields.
    stdx::mutex _mutex;

    // Queue containing all items currently scheduled into the thread pool but not yet completed.
    WorkQueue _poolInProgressQueue;

    // Queue containing all items currently scheduled into the network interface.
    WorkQueue _networkInProgressQueue;

    // Queue containing all items waiting for a particular point in time to execute.
    WorkQueue _sleepersQueue;

    // List of all events that have yet to be signaled.
    EventList _unsignaledEvents;

    // Indicates whether or not the executor is shutting down.
    bool _inShutdown = false;
};

}  // namespace executor
}  // namespace mongo
