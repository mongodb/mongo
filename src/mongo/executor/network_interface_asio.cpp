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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include "mongo/executor/network_interface_asio.h"

#include <asio/system_timer.hpp>

#include <utility>

#include "mongo/executor/async_stream_factory.h"
#include "mongo/executor/async_stream_interface.h"
#include "mongo/executor/async_timer_asio.h"
#include "mongo/executor/async_timer_interface.h"
#include "mongo/executor/async_timer_mock.h"
#include "mongo/executor/connection_pool_asio.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/stdx/chrono.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/table_formatter.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace executor {

namespace {
const std::size_t kIOServiceWorkers = 1;
}  // namespace

NetworkInterfaceASIO::Options::Options() = default;

NetworkInterfaceASIO::NetworkInterfaceASIO(Options options)
    : _options(std::move(options)),
      _io_service(),
      _metadataHook(std::move(_options.metadataHook)),
      _hook(std::move(_options.networkConnectionHook)),
      _state(State::kReady),
      _timerFactory(std::move(_options.timerFactory)),
      _streamFactory(std::move(_options.streamFactory)),
      _connectionPool(stdx::make_unique<connection_pool_asio::ASIOImpl>(this),
                      _options.connectionPoolOptions),
      _isExecutorRunnable(false),
      _strand(_io_service) {}

std::string NetworkInterfaceASIO::getDiagnosticString() {
    stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
    return _getDiagnosticString_inlock(nullptr);
}

std::string NetworkInterfaceASIO::_getDiagnosticString_inlock(AsyncOp* currentOp) {
    str::stream output;
    std::vector<TableRow> rows;

    output << "\nNetworkInterfaceASIO Operations' Diagnostic:\n";
    rows.push_back({"Operation:", "Count:"});
    rows.push_back({"Connecting", std::to_string(_inGetConnection.size())});
    rows.push_back({"In Progress", std::to_string(_inProgress.size())});
    rows.push_back({"Succeeded", std::to_string(getNumSucceededOps())});
    rows.push_back({"Canceled", std::to_string(getNumCanceledOps())});
    rows.push_back({"Failed", std::to_string(getNumFailedOps())});
    rows.push_back({"Timed Out", std::to_string(getNumTimedOutOps())});
    output << toTable(rows);

    if (_inProgress.size() > 0) {
        rows.clear();
        rows.push_back(AsyncOp::kFieldLabels);

        // Push AsyncOps
        for (auto&& kv : _inProgress) {
            auto row = kv.first->getStringFields();
            if (currentOp) {
                // If this is the AsyncOp we blew up on, mark with an asterisk
                if (*currentOp == *(kv.first)) {
                    row[0] = "*";
                }
            }

            rows.push_back(row);
        }

        // Format as a table
        output << "\n" << toTable(rows);
    }

    output << "\n";

    return output;
}

uint64_t NetworkInterfaceASIO::getNumCanceledOps() {
    return _numCanceledOps.load();
}

uint64_t NetworkInterfaceASIO::getNumFailedOps() {
    return _numFailedOps.load();
}

uint64_t NetworkInterfaceASIO::getNumSucceededOps() {
    return _numSucceededOps.load();
}

uint64_t NetworkInterfaceASIO::getNumTimedOutOps() {
    return _numTimedOutOps.load();
}

void NetworkInterfaceASIO::appendConnectionStats(ConnectionPoolStats* stats) const {
    _connectionPool.appendConnectionStats(stats);
}

std::string NetworkInterfaceASIO::getHostName() {
    return getHostNameCached();
}

void NetworkInterfaceASIO::startup() {
    _serviceRunners.resize(kIOServiceWorkers);
    for (std::size_t i = 0; i < kIOServiceWorkers; ++i) {
        _serviceRunners[i] = stdx::thread([this, i]() {
            setThreadName(_options.instanceName + "-" + std::to_string(i));
            try {
                LOG(2) << "The NetworkInterfaceASIO worker thread is spinning up";
                asio::io_service::work work(_io_service);
                _io_service.run();
            } catch (...) {
                severe() << "Uncaught exception in NetworkInterfaceASIO IO "
                            "worker thread of type: "
                         << exceptionToStatus();
                fassertFailed(28820);
            }
        });
    };
    _state.store(State::kRunning);
}

void NetworkInterfaceASIO::shutdown() {
    _state.store(State::kShutdown);
    _io_service.stop();
    for (auto&& worker : _serviceRunners) {
        worker.join();
    }
    LOG(2) << "NetworkInterfaceASIO shutdown successfully";
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
        _isExecutorRunnableCondition.wait_for(lk, waitTime.toSystemDuration());
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

Status NetworkInterfaceASIO::startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                          const RemoteCommandRequest& request,
                                          const RemoteCommandCompletionFn& onFinish) {
    MONGO_ASIO_INVARIANT(onFinish, "Invalid completion function");
    {
        stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
        const auto insertResult = _inGetConnection.emplace(cbHandle);
        // We should never see the same CallbackHandle added twice
        MONGO_ASIO_INVARIANT_INLOCK(insertResult.second, "Same CallbackHandle added twice");
    }

    if (inShutdown()) {
        return {ErrorCodes::ShutdownInProgress, "NetworkInterfaceASIO shutdown in progress"};
    }

    LOG(2) << "startCommand: " << request.toString();

    auto getConnectionStartTime = now();

    auto nextStep = [this, getConnectionStartTime, cbHandle, request, onFinish](
        StatusWith<ConnectionPool::ConnectionHandle> swConn) {

        if (!swConn.isOK()) {
            LOG(2) << "Failed to get connection from pool for request " << request.id << ": "
                   << swConn.getStatus();

            bool wasPreviouslyCanceled = false;
            {
                stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
                wasPreviouslyCanceled = _inGetConnection.erase(cbHandle) == 0;
            }

            onFinish(wasPreviouslyCanceled
                         ? Status(ErrorCodes::CallbackCanceled, "Callback canceled")
                         : swConn.getStatus());
            signalWorkAvailable();
            return;
        }

        auto conn = static_cast<connection_pool_asio::ASIOConnection*>(swConn.getValue().get());

        AsyncOp* op = nullptr;

        stdx::unique_lock<stdx::mutex> lk(_inProgressMutex);

        const auto eraseCount = _inGetConnection.erase(cbHandle);

        // If we didn't find the request, we've been canceled
        if (eraseCount == 0) {
            lk.unlock();

            onFinish({ErrorCodes::CallbackCanceled, "Callback canceled"});

            // Though we were canceled, we know that the stream is fine, so indicate success.
            conn->indicateSuccess();

            signalWorkAvailable();

            return;
        }

        // We can't release the AsyncOp until we know we were not canceled.
        auto ownedOp = conn->releaseAsyncOp();
        op = ownedOp.get();

        // This AsyncOp may be recycled. We expect timeout and canceled to be clean.
        // If this op was most recently used to connect, its state transitions won't have been
        // reset, so we do that here.
        MONGO_ASIO_INVARIANT_INLOCK(!op->canceled(), "AsyncOp has dirty canceled flag", op);
        MONGO_ASIO_INVARIANT_INLOCK(!op->timedOut(), "AsyncOp has dirty timeout flag", op);
        op->clearStateTransitions();

        // Now that we're inProgress, an external cancel can touch our op, but
        // not until we release the inProgressMutex.
        _inProgress.emplace(op, std::move(ownedOp));

        op->_cbHandle = std::move(cbHandle);
        op->_request = std::move(request);
        op->_onFinish = std::move(onFinish);
        op->_connectionPoolHandle = std::move(swConn.getValue());
        op->startProgress(getConnectionStartTime);

        // This ditches the lock and gets us onto the strand (so we're
        // threadsafe)
        op->_strand.post([this, op, getConnectionStartTime] {
            // Set timeout now that we have the correct request object
            if (op->_request.timeout != RemoteCommandRequest::kNoTimeout) {
                // Subtract the time it took to get the connection from the pool from the request
                // timeout.
                auto getConnectionDuration = now() - getConnectionStartTime;
                if (getConnectionDuration >= op->_request.timeout) {
                    // We only assume that the request timer is guaranteed to fire *after* the
                    // timeout duration - but make no stronger assumption. It is thus possible that
                    // we have already exceeded the timeout. In this case we timeout the operation
                    // manually.
                    std::stringstream msg;
                    msg << "Remote command timed out while waiting to get a connection from the "
                        << "pool, took " << getConnectionDuration;
                    return _completeOperation(op, {ErrorCodes::ExceededTimeLimit, msg.str()});
                }

                // The above conditional guarantees that the adjusted timeout will never underflow.
                MONGO_ASIO_INVARIANT(
                    op->_request.timeout > getConnectionDuration, "timeout underflowed", op);
                const auto adjustedTimeout = op->_request.timeout - getConnectionDuration;
                const auto requestId = op->_request.id;

                op->_timeoutAlarm = op->_owner->_timerFactory->make(&op->_strand, adjustedTimeout);

                std::shared_ptr<AsyncOp::AccessControl> access;
                std::size_t generation;
                {
                    stdx::lock_guard<stdx::mutex> lk(op->_access->mutex);
                    access = op->_access;
                    generation = access->id;
                }

                op->_timeoutAlarm->asyncWait(
                    [this, op, access, generation, requestId, adjustedTimeout](std::error_code ec) {
                        // We must pass a check for safe access before using op inside the
                        // callback or we may attempt access on an invalid pointer.
                        stdx::lock_guard<stdx::mutex> lk(access->mutex);
                        if (generation != access->id) {
                            // The operation has been cleaned up, do not access.
                            return;
                        }

                        if (!ec) {
                            LOG(2) << "Request " << requestId << " timed out"
                                   << ", adjusted timeout after getting connection from pool was "
                                   << adjustedTimeout << ", op was " << op->toString();

                            op->timeOut_inlock();
                        } else {
                            LOG(2) << "Failed to time request " << requestId
                                   << "out: " << ec.message() << ", op was " << op->toString();
                        }
                    });
            }

            _beginCommunication(op);
        });
    };

    _connectionPool.get(request.target, request.timeout, nextStep);
    return Status::OK();
}

void NetworkInterfaceASIO::cancelCommand(const TaskExecutor::CallbackHandle& cbHandle) {
    stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);

    // If we found a matching cbHandle in _inGetConnection, then
    // simply removing it has the same effect as cancelling it, so we
    // can just return.
    if (_inGetConnection.erase(cbHandle) != 0) {
        _numCanceledOps.fetchAndAdd(1);
        return;
    }

    // TODO: This linear scan is unfortunate. It is here because our
    // primary data structure is to keep the AsyncOps in an
    // unordered_map by pointer, but here we only have the
    // callback. We could keep two data structures at the risk of
    // having them diverge.
    for (auto&& kv : _inProgress) {
        if (kv.first->cbHandle() == cbHandle) {
            kv.first->cancel();
            _numCanceledOps.fetchAndAdd(1);
            break;
        }
    }
}

void NetworkInterfaceASIO::cancelAllCommands() {
    decltype(_inGetConnection) newInGetConnection;
    {
        stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
        _inGetConnection.swap(newInGetConnection);
        for (auto&& kv : _inProgress) {
            kv.first->cancel();
        }
        _numCanceledOps.fetchAndAdd(_inProgress.size());
    }
}

Status NetworkInterfaceASIO::setAlarm(Date_t when, const stdx::function<void()>& action) {
    if (inShutdown()) {
        return {ErrorCodes::ShutdownInProgress, "NetworkInterfaceASIO shutdown in progress"};
    }

    // "alarm" must stay alive until it expires, hence the shared_ptr.
    auto alarm = std::make_shared<asio::system_timer>(_io_service, when.toSystemTimePoint());
    alarm->async_wait([alarm, this, action](std::error_code ec) {
        if (!ec) {
            return action();
        } else if (ec != asio::error::operation_aborted) {
            // When the network interface is shut down, it will cancel all pending
            // alarms, raising an "operation_aborted" error here, which we ignore.
            warning() << "setAlarm() received an error: " << ec.message();
        }
    });

    return Status::OK();
};

bool NetworkInterfaceASIO::inShutdown() const {
    return (_state.load() == State::kShutdown);
}

bool NetworkInterfaceASIO::onNetworkThread() {
    auto id = stdx::this_thread::get_id();
    return std::any_of(_serviceRunners.begin(),
                       _serviceRunners.end(),
                       [id](const stdx::thread& thread) { return id == thread.get_id(); });
}

void NetworkInterfaceASIO::_failWithInfo(const char* file,
                                         int line,
                                         std::string error,
                                         AsyncOp* op) {
    stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
    _failWithInfo_inlock(file, line, error, op);
}

void NetworkInterfaceASIO::_failWithInfo_inlock(const char* file,
                                                int line,
                                                std::string error,
                                                AsyncOp* op) {
    std::stringstream ss;
    ss << "Invariant failure at " << file << ":" << line << ": " << error;
    ss << _getDiagnosticString_inlock(op);
    Status status{ErrorCodes::InternalError, ss.str()};
    fassertFailedWithStatus(34429, status);
}

}  // namespace executor
}  // namespace mongo
