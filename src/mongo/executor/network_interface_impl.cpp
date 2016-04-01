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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/executor/network_interface_impl.h"

#include <memory>

#include "mongo/client/connection_pool.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace executor {

namespace {

const size_t kMinThreads = 2;
const size_t kMaxThreads = 52;  // Set to 1 + max repl set size, for heartbeat + wiggle room.
const Seconds kMaxIdleThreadAge(30);

ThreadPool::Options makeOptions() {
    ThreadPool::Options options;
    options.poolName = "ReplExecNet";
    options.threadNamePrefix = "ReplExecNetThread-";
    options.minThreads = kMinThreads;
    options.maxThreads = kMaxThreads;
    options.maxIdleThreadAge = kMaxIdleThreadAge;
    return options;
}

}  // namespace

NetworkInterfaceImpl::NetworkInterfaceImpl() : NetworkInterfaceImpl(nullptr){};

NetworkInterfaceImpl::NetworkInterfaceImpl(std::unique_ptr<NetworkConnectionHook> hook)
    : NetworkInterface(),
      _pool(makeOptions()),
      _commandRunner(kMessagingPortKeepOpen, std::move(hook)) {}

NetworkInterfaceImpl::~NetworkInterfaceImpl() {}

std::string NetworkInterfaceImpl::getDiagnosticString() {
    const auto poolStats = _pool.getStats();
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    str::stream output;
    output << "NetworkImpl";
    output << " threads:" << poolStats.numThreads;
    output << " inShutdown:" << _inShutdown;
    output << " active:" << _numActiveNetworkRequests;
    output << " pending:" << _pending.size();
    output << " execRunable:" << _isExecutorRunnable;
    return output;
}

void NetworkInterfaceImpl::appendConnectionStats(ConnectionPoolStats* stats) const {}

void NetworkInterfaceImpl::startup() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(!_inShutdown);
    lk.unlock();

    _commandRunner.startup();
    _pool.startup();
    fassert(27824, _pool.schedule([this]() { _processAlarms(); }));
}

void NetworkInterfaceImpl::shutdown() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _inShutdown = true;
    _hasPending.notify_all();
    _newAlarmReady.notify_all();
    lk.unlock();

    _pool.shutdown();
    _pool.join();
    _commandRunner.shutdown();
}

void NetworkInterfaceImpl::signalWorkAvailable() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _signalWorkAvailable_inlock();
}

void NetworkInterfaceImpl::_signalWorkAvailable_inlock() {
    if (!_isExecutorRunnable) {
        _isExecutorRunnable = true;
        _isExecutorRunnableCondition.notify_one();
    }
}

void NetworkInterfaceImpl::waitForWork() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (!_isExecutorRunnable) {
        _isExecutorRunnableCondition.wait(lk);
    }
    _isExecutorRunnable = false;
}

void NetworkInterfaceImpl::waitForWorkUntil(Date_t when) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (!_isExecutorRunnable) {
        const Milliseconds waitTime(when - now());
        if (waitTime <= Milliseconds(0)) {
            break;
        }
        _isExecutorRunnableCondition.wait_for(lk, waitTime);
    }
    _isExecutorRunnable = false;
}

void NetworkInterfaceImpl::_runOneCommand() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (_pending.empty()) {
        // This may happen if any commands were canceled.
        return;
    }
    CommandData todo = _pending.front();
    _pending.pop_front();
    ++_numActiveNetworkRequests;
    lk.unlock();
    TaskExecutor::ResponseStatus result = _commandRunner.runCommand(todo.request);
    LOG(2) << "Network status of sending " << todo.request.cmdObj.firstElementFieldName() << " to "
           << todo.request.target << " was " << result.getStatus();
    todo.onFinish(result);
    lk.lock();
    --_numActiveNetworkRequests;
    _signalWorkAvailable_inlock();
}

void NetworkInterfaceImpl::startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                        const RemoteCommandRequest& request,
                                        const RemoteCommandCompletionFn& onFinish) {
    LOG(2) << "Scheduling " << request.cmdObj.firstElementFieldName() << " to " << request.target;
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _pending.push_back(CommandData());
    CommandData& cd = _pending.back();
    cd.cbHandle = cbHandle;
    cd.request = request;
    cd.onFinish = onFinish;
    fassert(28730, _pool.schedule([this]() -> void { _runOneCommand(); }));
}

void NetworkInterfaceImpl::cancelCommand(const TaskExecutor::CallbackHandle& cbHandle) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    CommandDataList::iterator iter;
    for (iter = _pending.begin(); iter != _pending.end(); ++iter) {
        if (iter->cbHandle == cbHandle) {
            break;
        }
    }
    if (iter == _pending.end()) {
        return;
    }
    const RemoteCommandCompletionFn onFinish = iter->onFinish;
    LOG(2) << "Canceled sending " << iter->request.cmdObj.firstElementFieldName() << " to "
           << iter->request.target;
    _pending.erase(iter);
    lk.unlock();
    onFinish(TaskExecutor::ResponseStatus(ErrorCodes::CallbackCanceled, "Callback canceled"));
    lk.lock();
    _signalWorkAvailable_inlock();
}

Date_t NetworkInterfaceImpl::now() {
    return Date_t::now();
}

std::string NetworkInterfaceImpl::getHostName() {
    return getHostNameCached();
}

void NetworkInterfaceImpl::setAlarm(Date_t when, const stdx::function<void()>& action) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    const bool notify = _alarms.empty() || _alarms.top().when > when;
    _alarms.emplace(when, action);
    if (notify) {
        _newAlarmReady.notify_all();
    }
}

bool NetworkInterfaceImpl::onNetworkThread() {
    return true;
}

void NetworkInterfaceImpl::_processAlarms() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    while (!_inShutdown) {
        if (_alarms.empty()) {
            _newAlarmReady.wait(lk);
        } else if (now() < _alarms.top().when) {
            _newAlarmReady.wait_until(lk, _alarms.top().when.toSystemTimePoint());
        } else {
            auto action = _alarms.top().action;
            _alarms.pop();
            lk.unlock();
            action();
            lk.lock();
        }
    }
}

}  // namespace executor
}  // namespace mongo
