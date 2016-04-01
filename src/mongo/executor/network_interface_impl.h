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

#include <queue>
#include <utility>
#include <vector>

#include "mongo/client/remote_command_runner_impl.h"
#include "mongo/executor/network_interface.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/list.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace executor {

struct ConnectionPoolStats;
class NetworkConnectionHook;

/**
 * Implementation of the network interface for use by classes implementing TaskExecutor
 * inside mongod.
 *
 * This implementation manages a dynamically sized group of worker threads for performing
 * network operations.  The minimum and maximum number of threads is set at compile time, and
 * the exact number of threads is adjusted dynamically, using the following two rules.
 *
 * 1.) If the number of worker threads is less than the maximum, there are no idle worker
 * threads, and the client enqueues a new network operation via startCommand(), the network
 * interface spins up a new worker thread.  This decision is made on the assumption that
 * spinning up a new thread is faster than the round-trip time for processing a remote command,
 * and so this will minimize wait time.
 *
 * 2.) If the number of worker threads has exceeded the the peak number of scheduled outstanding
 * network commands continuously for a period of time (kMaxIdleThreadAge), one thread is retired
 * from the pool and the monitoring of idle threads is reset.  This means that at most one
 * thread retires every kMaxIdleThreadAge units of time.  The value of kMaxIdleThreadAge is set
 * to be much larger than the expected frequency of new requests, averaging out short-duration
 * periods of idleness, as occur between heartbeats.
 *
 * The implementation also manages a pool of network connections to recently contacted remote
 * nodes.  The size of this pool is not bounded, but connections are retired unconditionally
 * after they have been connected for a certain maximum period.
 * TODO(spencer): Rename this to ThreadPoolNetworkInterface
 */
class NetworkInterfaceImpl final : public NetworkInterface {
public:
    NetworkInterfaceImpl();
    NetworkInterfaceImpl(std::unique_ptr<NetworkConnectionHook> hook);
    ~NetworkInterfaceImpl();
    std::string getDiagnosticString() override;
    void appendConnectionStats(ConnectionPoolStats* stats) const override;
    void startup() override;
    void shutdown() override;
    bool inShutdown() const override;
    void waitForWork() override;
    void waitForWorkUntil(Date_t when) override;
    void signalWorkAvailable() override;
    Date_t now() override;
    std::string getHostName() override;
    Status startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                        const RemoteCommandRequest& request,
                        const RemoteCommandCompletionFn& onFinish) override;
    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle) override;
    /**
     * Not implemented.
     */
    void cancelAllCommands() override {}
    Status setAlarm(Date_t when, const stdx::function<void()>& action) override;
    bool onNetworkThread() override;

private:
    /**
     * Information describing a scheduled alarm.
     */
    struct AlarmInfo {
        using AlarmAction = stdx::function<void()>;
        AlarmInfo(Date_t inWhen, AlarmAction inAction)
            : when(inWhen), action(std::move(inAction)) {}
        bool operator>(const AlarmInfo& rhs) const {
            return when > rhs.when;
        }

        Date_t when;
        AlarmAction action;
    };

    /**
     * Information describing an in-flight command.
     */
    struct CommandData {
        TaskExecutor::CallbackHandle cbHandle;
        RemoteCommandRequest request;
        RemoteCommandCompletionFn onFinish;
    };
    typedef stdx::list<CommandData> CommandDataList;

    /**
     * Executes one pending network operation, if there is at least one in the pending queue.
     */
    void _runOneCommand();

    /**
     * Worker function that processes alarms set via setAlarm.
     */
    void _processAlarms();

    /**
     * Notifies the network threads that there is work available.
     */
    void _signalWorkAvailable_inlock();

    // Mutex guarding the state of this network interface, except for the remote command
    // executor, which has its own concurrency control.
    stdx::mutex _mutex;

    // Condition signaled to indicate that there is work in the _pending queue.
    stdx::condition_variable _hasPending;

    // Queue of yet-to-be-executed network operations.
    CommandDataList _pending;

    // Worker thread pool.
    ThreadPool _pool;

    // Condition signaled to indicate that the executor, blocked in waitForWorkUntil or
    // waitForWork, should wake up.
    stdx::condition_variable _isExecutorRunnableCondition;

    // Flag indicating whether or not the executor associated with this interface is runnable.
    bool _isExecutorRunnable = false;

    // Flag indicating when this interface is being shut down (because shutdown() has executed).
    std::atomic<bool> _inShutdown;

    // Interface for running remote commands
    RemoteCommandRunnerImpl _commandRunner;

    // Number of active network requests
    size_t _numActiveNetworkRequests = 0;

    // Condition variable to signal in order to wake up the alarm processing thread.
    stdx::condition_variable _newAlarmReady;

    // Heap of alarms, with the next alarm always on top.
    std::priority_queue<AlarmInfo, std::vector<AlarmInfo>, std::greater<AlarmInfo>> _alarms;
};

}  // namespace executor
}  // namespace mongo
