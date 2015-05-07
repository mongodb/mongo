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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/network_interface_impl.h"

#include <boost/make_shared.hpp>
#include <memory>

#include "mongo/client/connection_pool.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace repl {

namespace {

    const size_t kMinThreads = 1;
    const size_t kMaxThreads = 51;  // Set to 1 + max repl set size, for heartbeat + wiggle room.
    const Seconds kMaxIdleThreadAge(30);

}  // namespace

    NetworkInterfaceImpl::NetworkInterfaceImpl() :
        _numIdleThreads(0),
        _nextThreadId(0),
        _lastFullUtilizationDate(),
        _isExecutorRunnable(false),
        _inShutdown(false),
        _commandExec(kMessagingPortKeepOpen),
        _numActiveNetworkRequests(0) {

    }

    NetworkInterfaceImpl::~NetworkInterfaceImpl() { }

    std::string NetworkInterfaceImpl::getDiagnosticString() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        str::stream output;
        output << "NetworkImpl";
        output << " threads:" << _threads.size();
        output << " inShutdown:" << _inShutdown;
        output << " active:" << _numActiveNetworkRequests;
        output << " pending:" << _pending.size();
        output << " execRunable:" << _isExecutorRunnable;
        return output;

    }

    void NetworkInterfaceImpl::_startNewNetworkThread_inlock() {
        if (_inShutdown) {
            LOG(1) <<
                "Not starting new replication networking thread while shutting down replication.";
            return;
        }
        if (_threads.size() >= kMaxThreads) {
            LOG(1) << "Not starting new replication networking thread because " << kMaxThreads <<
                " are already running; " << _numIdleThreads << " threads are idle and " <<
                _pending.size() << " network requests are waiting for a thread to serve them.";
            return;
        }
        const std::string threadName(str::stream() << "ReplExecNetThread-" << _nextThreadId++);
        try {
            _threads.push_back(
                    boost::make_shared<boost::thread>(
                            stdx::bind(&NetworkInterfaceImpl::_requestProcessorThreadBody,
                                       this,
                                       threadName)));
            ++_numIdleThreads;
        }
        catch (const std::exception& ex) {
            error() << "Failed to start " << threadName << "; " << _threads.size() <<
                " other network threads still running; caught exception: " << ex.what();
        }
    }

    void NetworkInterfaceImpl::startup() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        invariant(!_inShutdown);
        if (!_threads.empty()) {
            return;
        }
        for (size_t i = 0; i < kMinThreads; ++i) {
            _startNewNetworkThread_inlock();
        }
    }

    void NetworkInterfaceImpl::shutdown() {
        using std::swap;
        boost::unique_lock<boost::mutex> lk(_mutex);
        _inShutdown = true;
        _hasPending.notify_all();
        ThreadList threadsToJoin;
        swap(threadsToJoin, _threads);
        lk.unlock();
        _commandExec.shutdown();
        std::for_each(threadsToJoin.begin(),
                      threadsToJoin.end(),
                      stdx::bind(&boost::thread::join, stdx::placeholders::_1));
    }

    void NetworkInterfaceImpl::signalWorkAvailable() {
        boost::lock_guard<boost::mutex> lk(_mutex);
        _signalWorkAvailable_inlock();
    }

    void NetworkInterfaceImpl::_signalWorkAvailable_inlock() {
        if (!_isExecutorRunnable) {
            _isExecutorRunnable = true;
            _isExecutorRunnableCondition.notify_one();
        }
    }

    void NetworkInterfaceImpl::waitForWork() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        while (!_isExecutorRunnable) {
            _isExecutorRunnableCondition.wait(lk);
        }
        _isExecutorRunnable = false;
    }

    void NetworkInterfaceImpl::waitForWorkUntil(Date_t when) {
        boost::unique_lock<boost::mutex> lk(_mutex);
        while (!_isExecutorRunnable) {
            const Milliseconds waitTime(when - now());
            if (waitTime <= Milliseconds(0)) {
                break;
            }
            _isExecutorRunnableCondition.timed_wait(lk, waitTime);
        }
        _isExecutorRunnable = false;
    }

    void NetworkInterfaceImpl::_requestProcessorThreadBody(
            NetworkInterfaceImpl* net,
            const std::string& threadName) {
        setThreadName(threadName);
        LOG(1) << "thread starting";
        net->_consumeNetworkRequests();

        // At this point, another thread may have destroyed "net", if this thread chose to detach
        // itself and remove itself from net->_threads before releasing net->_mutex.  Do not access
        // member variables of "net" from here, on.
        LOG(1) << "thread shutting down";
    }

    void NetworkInterfaceImpl::_consumeNetworkRequests() {
        boost::unique_lock<boost::mutex> lk(_mutex);
        while (!_inShutdown) {
            if (_pending.empty()) {
                if (_threads.size() > kMinThreads) {
                    const Date_t nowDate = now();
                    const Date_t nextThreadRetirementDate =
                        _lastFullUtilizationDate + kMaxIdleThreadAge.total_milliseconds();
                    if (nowDate > nextThreadRetirementDate) {
                        _lastFullUtilizationDate = nowDate;
                        break;
                    }
                }
                _hasPending.timed_wait(lk, kMaxIdleThreadAge);
                continue;
            }
            CommandData todo = _pending.front();
            _pending.pop_front();
            ++_numActiveNetworkRequests;
            --_numIdleThreads;
            lk.unlock();
            ResponseStatus result = _commandExec.runCommand(todo.request);
            LOG(2) << "Network status of sending " << todo.request.cmdObj.firstElementFieldName() <<
                " to " << todo.request.target << " was " << result.getStatus();
            todo.onFinish(result);
            lk.lock();
            --_numActiveNetworkRequests;
            ++_numIdleThreads;
            _signalWorkAvailable_inlock();
        }
        --_numIdleThreads;
        if (_inShutdown) {
            return;
        }
        // This thread is ending because it was idle for too long.
        // Find self in _threads, remove self from _threads, detach self.
        for (size_t i = 0; i < _threads.size(); ++i) {
            if (_threads[i]->get_id() != boost::this_thread::get_id()) {
                continue;
            }
            _threads[i]->detach();
            _threads[i].swap(_threads.back());
            _threads.pop_back();
            return;
        }
        severe().stream() << "Could not find this thread, with id " <<
            boost::this_thread::get_id() << " in the replication networking thread pool";
        fassertFailedNoTrace(28581);
    }

    void NetworkInterfaceImpl::startCommand(
            const ReplicationExecutor::CallbackHandle& cbHandle,
            const RemoteCommandRequest& request,
            const RemoteCommandCompletionFn& onFinish) {
        LOG(2) << "Scheduling " << request.cmdObj.firstElementFieldName() << " to " <<
            request.target;
        boost::lock_guard<boost::mutex> lk(_mutex);
        _pending.push_back(CommandData());
        CommandData& cd = _pending.back();
        cd.cbHandle = cbHandle;
        cd.request = request;
        cd.onFinish = onFinish;
        if (_numIdleThreads < _pending.size()) {
            _startNewNetworkThread_inlock();
        }
        if (_numIdleThreads <= _pending.size()) {
            _lastFullUtilizationDate = curTimeMillis64();
        }
        _hasPending.notify_one();
    }

    void NetworkInterfaceImpl::cancelCommand(const ReplicationExecutor::CallbackHandle& cbHandle) {
        boost::unique_lock<boost::mutex> lk(_mutex);
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
        LOG(2) << "Canceled sending " << iter->request.cmdObj.firstElementFieldName() << " to " <<
            iter->request.target;
        _pending.erase(iter);
        lk.unlock();
        onFinish(ResponseStatus(ErrorCodes::CallbackCanceled, "Callback canceled"));
        lk.lock();
        _signalWorkAvailable_inlock();
    }

    Date_t NetworkInterfaceImpl::now() {
        return curTimeMillis64();
    }

    OperationContext* NetworkInterfaceImpl::createOperationContext() {
        Client::initThreadIfNotAlready();
        return new OperationContextImpl();
    }

}  // namespace repl
} // namespace mongo
