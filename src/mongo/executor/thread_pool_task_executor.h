// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/baton.h"
#include "mongo/db/operation_context.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/transport/baton.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <list>
#include <memory>
#include <mutex>
#include <thread>

#include <boost/optional.hpp>

[[MONGO_MOD_PUBLIC]];

namespace mongo {

class ThreadPoolInterface;

namespace executor {

struct ConnectionPoolStats;

/**
 * Implementation of a TaskExecutor that uses a pool of threads to execute work items.
 */
class ThreadPoolTaskExecutor final : public TaskExecutor {
    /** Protects the constructor from outside use. */
    struct Passkey {
        explicit Passkey() = default;
    };

public:
    /**
     * Creates an instance that runs tasks in `pool` and uses `net` for network
     * operations, exclusively owned by the returned `shared_ptr`.
     */
    static std::shared_ptr<ThreadPoolTaskExecutor> create(std::unique_ptr<ThreadPoolInterface> pool,
                                                          std::shared_ptr<NetworkInterface> net);

    ThreadPoolTaskExecutor(Passkey,
                           std::unique_ptr<ThreadPoolInterface> pool,
                           std::shared_ptr<NetworkInterface> net);

    ~ThreadPoolTaskExecutor() override;

    ThreadPoolTaskExecutor(const ThreadPoolTaskExecutor&) = delete;
    ThreadPoolTaskExecutor& operator=(const ThreadPoolTaskExecutor&) = delete;

    void startup() override;
    void shutdown() override;
    void join() override;
    SharedSemiFuture<void> joinAsync() override;
    bool isShuttingDown() const override;
    void appendDiagnosticBSON(BSONObjBuilder* b) const override;
    Date_t now() override;
    StatusWith<EventHandle> makeEvent() override;
    void signalEvent(const EventHandle& event) override;
    StatusWith<CallbackHandle> onEvent(const EventHandle& event, CallbackFn&& work) override;
    StatusWith<stdx::cv_status> waitForEvent(OperationContext* opCtx,
                                             const EventHandle& event,
                                             Date_t deadline) override;
    void waitForEvent(const EventHandle& event) override;
    StatusWith<CallbackHandle> scheduleWork(CallbackFn&& work) override;
    StatusWith<CallbackHandle> scheduleWorkAt(Date_t when, CallbackFn&& work) override;
    StatusWith<CallbackHandle> scheduleRemoteCommand(const RemoteCommandRequest& request,
                                                     const RemoteCommandCallbackFn& cb,
                                                     const BatonHandle& baton = nullptr) override;
    StatusWith<CallbackHandle> scheduleExhaustRemoteCommand(
        const RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const BatonHandle& baton = nullptr) override;
    void cancel(const CallbackHandle& cbHandle) override;
    void wait(const CallbackHandle& cbHandle,
              Interruptible* interruptible = Interruptible::notInterruptible()) override;

    void appendConnectionStats(ConnectionPoolStats* stats) const override;

    void dropConnections(const HostAndPort& target, const Status& status) override;

    void appendNetworkInterfaceStats(BSONObjBuilder&, bool forServerStatus) const override;

    /**
     * Returns true if there are any tasks currently running or waiting to run.
     */
    bool hasTasks() override;

    /** The network interface used for remote command execution and waiting. */
    const std::shared_ptr<NetworkInterface>& getNetworkInterface() const {
        return _net;
    }

private:
    class CallbackState;
    struct LocalCallbackState;
    struct RemoteCallbackState;
    class EventState;

    using EventList = std::list<std::shared_ptr<EventState>>;

    /**
     * Representation of the stage of life of a thread pool.
     *
     * A pool starts out in the preStart state, and ends life in the shutdownComplete state.  Work
     * may only be scheduled in the preStart and running states. Threads may only be started in the
     * running state. In shutdownComplete, there are no remaining threads or pending tasks to
     * execute.
     *
     * Diagram of legal transitions:
     *
     * preStart -> running -> joinRequired -> joining -> shutdownComplete
     *        \               ^
     *         \_____________/
     *
     * NOTE: The enumeration values below are compared using operator<, etc, with the expectation
     * that a -> b in the diagram above implies that a < b in the enum below.
     */
    enum State { preStart, running, joinRequired, joining, shutdownComplete };

    /**
     * Returns an EventList containing one unsignaled EventState. This is a helper function for
     * performing allocations outside of _mutex, and should only be called by makeSingletonWork and
     * makeEvent().
     */
    static EventList makeSingletonEventList();
    StatusWith<CallbackHandle> _registerCallbackState(std::shared_ptr<CallbackState> cbState);

    void _unregisterCallbackState(const std::shared_ptr<CallbackState>& cbState);

    /**
     * Signals the given event.
     */
    void signalEvent_inlock(const EventHandle& event, std::unique_lock<std::mutex> lk);

    /**
     * Executes the callback specified by "cbState".
     */
    void runCallback(std::shared_ptr<LocalCallbackState> cbState, Status s);

    TaskExecutor::RemoteCommandCallbackArgs makeRemoteCallbackArgs(
        const CallbackHandle& cbHandle,
        const RemoteCallbackState& cbState,
        StatusWith<RemoteCommandResponse> swr);

    bool _inShutdown_inlock() const;
    void _setState_inlock(State newState);
    void _continueExhaustCommand(CallbackHandle cbHandle,
                                 std::shared_ptr<RemoteCallbackState> cbState,
                                 std::shared_ptr<NetworkInterface::ExhaustResponseReader> rdr);

    // The network interface used for remote command execution and waiting.
    std::shared_ptr<NetworkInterface> _net;

    // The thread pool that executes scheduled work items.
    std::shared_ptr<ThreadPoolInterface> _pool;

    // Mutex guarding all remaining fields.
    mutable std::mutex _mutex;

    // List of all events that have yet to be signaled.
    EventList _unsignaledEvents;

    // List of all callbacks that have yet to be fully completed. This includes those that are
    // actively running, those that are waiting to run, and those that are waiting on the network
    // for responses. join() will not return until this list is empty.
    std::list<std::shared_ptr<CallbackState>> _inProgress;

    // Number of tasks that are waiting for a particular point in time to execute.
    Atomic<size_t> _sleepers;

    // Number of networking tasks are in progress.
    Atomic<size_t> _networkInProgress;

    // Lifecycle state of this executor.
    stdx::condition_variable _stateChange;
    State _state = preStart;
};

}  // namespace executor
}  // namespace mongo
