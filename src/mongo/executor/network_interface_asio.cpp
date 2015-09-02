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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/executor/network_interface_asio.h"

#include <utility>

#include "mongo/executor/async_stream_interface.h"
#include "mongo/executor/async_stream_factory.h"
#include "mongo/executor/connection_pool_asio.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace executor {

NetworkInterfaceASIO::NetworkInterfaceASIO(
    std::unique_ptr<AsyncStreamFactoryInterface> streamFactory, Options options)
    : NetworkInterfaceASIO(std::move(streamFactory), nullptr, std::move(options)) {}

NetworkInterfaceASIO::NetworkInterfaceASIO(
    std::unique_ptr<AsyncStreamFactoryInterface> streamFactory,
    std::unique_ptr<NetworkConnectionHook> networkConnectionHook,
    Options options)
    : _options(std::move(options)),
      _io_service(),
      _hook(std::move(networkConnectionHook)),
      _resolver(_io_service),
      _state(State::kReady),
      _streamFactory(std::move(streamFactory)),
      _connectionPool(stdx::make_unique<connection_pool_asio::ASIOImpl>(this),
                      _options.connectionPoolOptions),
      _isExecutorRunnable(false) {}

std::string NetworkInterfaceASIO::getDiagnosticString() {
    str::stream output;
    output << "NetworkInterfaceASIO";
    output << " inShutdown: " << inShutdown();
    return output;
}

std::string NetworkInterfaceASIO::getHostName() {
    return getHostNameCached();
}

void NetworkInterfaceASIO::startup() {
    _serviceRunner = stdx::thread([this]() {
        asio::io_service::work work(_io_service);
        _io_service.run();
    });
    _state.store(State::kRunning);
}

void NetworkInterfaceASIO::shutdown() {
    _state.store(State::kShutdown);
    _io_service.stop();
    _serviceRunner.join();
}

void NetworkInterfaceASIO::waitForWork() {
    stdx::unique_lock<stdx::mutex> lk(_executorMutex);
    // TODO: This can be restructured with a lambda.
    while (!_isExecutorRunnable) {
        _isExecutorRunnableCondition.wait(lk);
    }
    _isExecutorRunnable = false;
}

void NetworkInterfaceASIO::waitForWorkUntil(Date_t when) {
    stdx::unique_lock<stdx::mutex> lk(_executorMutex);
    // TODO: This can be restructured with a lambda.
    while (!_isExecutorRunnable) {
        const Milliseconds waitTime(when - now());
        if (waitTime <= Milliseconds(0)) {
            break;
        }
        _isExecutorRunnableCondition.wait_for(lk, waitTime);
    }
    _isExecutorRunnable = false;
}

void NetworkInterfaceASIO::signalWorkAvailable() {
    stdx::unique_lock<stdx::mutex> lk(_executorMutex);
    _signalWorkAvailable_inlock();
}

void NetworkInterfaceASIO::_signalWorkAvailable_inlock() {
    if (!_isExecutorRunnable) {
        _isExecutorRunnable = true;
        _isExecutorRunnableCondition.notify_one();
    }
}

Date_t NetworkInterfaceASIO::now() {
    return Date_t::now();
}

void NetworkInterfaceASIO::startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                        const RemoteCommandRequest& request,
                                        const RemoteCommandCompletionFn& onFinish) {
    {
        stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
        _inGetConnection.push_back(cbHandle);
    }

    auto startTime = now();

    auto nextStep = [this, startTime, cbHandle, request, onFinish](
        StatusWith<ConnectionPool::ConnectionHandle> swConn) {

        if (!swConn.isOK()) {
            {
                stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);

                auto& v = _inGetConnection;
                auto iter = std::find(v.begin(), v.end(), cbHandle);
                if (iter != v.end())
                    v.erase(iter);
            }

            onFinish(swConn.getStatus());
            signalWorkAvailable();
            return;
        }

        auto conn = static_cast<connection_pool_asio::ASIOConnection*>(swConn.getValue().get());

        auto ownedOp = conn->releaseAsyncOp();
        AsyncOp* op = ownedOp.get();

        {
            stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);

            auto iter = std::find(_inGetConnection.begin(), _inGetConnection.end(), cbHandle);

            // If we didn't find the request, we've been canceled
            if (iter == _inGetConnection.end())
                return;

            _inGetConnection.erase(iter);
            _inProgress.emplace(op, std::move(ownedOp));
        }

        op->_cbHandle = cbHandle;
        op->_request = request;
        op->_onFinish = onFinish;
        op->_connectionPoolHandle = std::move(swConn.getValue());
        op->_start = startTime;

        _beginCommunication(op);
    };

    // TODO: thread some higher level timeout through, rather than 5 minutes,
    // once we make timeouts pervasive in this api.
    asio::post(
        _io_service,
        [this, request, nextStep] { _connectionPool.get(request.target, Minutes(5), nextStep); });
}

void NetworkInterfaceASIO::cancelCommand(const TaskExecutor::CallbackHandle& cbHandle) {
    stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
    for (auto iter = _inProgress.begin(); iter != _inProgress.end(); ++iter) {
        if (iter->first->cbHandle() == cbHandle) {
            iter->first->cancel();
            break;
        }
    }
}

void NetworkInterfaceASIO::cancelAllCommands() {
    stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
    for (auto iter = _inProgress.begin(); iter != _inProgress.end(); ++iter) {
        iter->first->cancel();
    }
}

void NetworkInterfaceASIO::setAlarm(Date_t when, const stdx::function<void()>& action) {
    // "alarm" must stay alive until it expires, hence the shared_ptr.
    auto alarm = std::make_shared<asio::steady_timer>(_io_service, when - now());
    alarm->async_wait([alarm, this, action](std::error_code ec) {
        if (!ec) {
            return action();
        } else if (ec != asio::error::operation_aborted) {
            // When the network interface is shut down, it will cancel all pending
            // alarms, raising an "operation_aborted" error here, which we ignore.
            warning() << "setAlarm() received an error: " << ec.message();
        }
    });
};

bool NetworkInterfaceASIO::inShutdown() const {
    return (_state.load() == State::kShutdown);
}

}  // namespace executor
}  // namespace mongo
