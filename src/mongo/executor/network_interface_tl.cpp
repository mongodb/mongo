/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include "mongo/executor/network_interface_tl.h"

#include "mongo/db/commands/test_commands_enabled.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/connection_pool_tl.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/strong_weak_finish_line.h"

namespace mongo {
namespace executor {

NetworkInterfaceTL::NetworkInterfaceTL(std::string instanceName,
                                       ConnectionPool::Options connPoolOpts,
                                       ServiceContext* svcCtx,
                                       std::unique_ptr<NetworkConnectionHook> onConnectHook,
                                       std::unique_ptr<rpc::EgressMetadataHook> metadataHook)
    : _instanceName(std::move(instanceName)),
      _svcCtx(svcCtx),
      _tl(nullptr),
      _ownedTransportLayer(nullptr),
      _reactor(nullptr),
      _connPoolOpts(std::move(connPoolOpts)),
      _onConnectHook(std::move(onConnectHook)),
      _metadataHook(std::move(metadataHook)),
      _state(kDefault) {}

std::string NetworkInterfaceTL::getDiagnosticString() {
    return "DEPRECATED: getDiagnosticString is deprecated in NetworkInterfaceTL";
}

void NetworkInterfaceTL::appendConnectionStats(ConnectionPoolStats* stats) const {
    auto pool = [&] {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _pool.get();
    }();
    if (pool)
        pool->appendConnectionStats(stats);
}

NetworkInterface::Counters NetworkInterfaceTL::getCounters() const {
    invariant(getTestCommandsEnabled());
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _counters;
}

std::string NetworkInterfaceTL::getHostName() {
    return getHostNameCached();
}

void NetworkInterfaceTL::startup() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_svcCtx) {
        _tl = _svcCtx->getTransportLayer();
    }

    if (!_tl) {
        warning() << "No TransportLayer configured during NetworkInterface startup";
        _ownedTransportLayer =
            transport::TransportLayerManager::makeAndStartDefaultEgressTransportLayer();
        _tl = _ownedTransportLayer.get();
    }

    _reactor = _tl->getReactor(transport::TransportLayer::kNewReactor);
    auto typeFactory = std::make_unique<connection_pool_tl::TLTypeFactory>(
        _reactor, _tl, std::move(_onConnectHook), _connPoolOpts);
    _pool = std::make_shared<ConnectionPool>(
        std::move(typeFactory), std::string("NetworkInterfaceTL-") + _instanceName, _connPoolOpts);
    _ioThread = stdx::thread([this] {
        setThreadName(_instanceName);
        _run();
    });

    invariant(_state.swap(kStarted) == kDefault);
}

void NetworkInterfaceTL::_run() {
    LOG(2) << "The NetworkInterfaceTL reactor thread is spinning up";

    // This returns when the reactor is stopped in shutdown()
    _reactor->run();

    // Note that the pool will shutdown again when the ConnectionPool dtor runs
    // This prevents new timers from being set, calls all cancels via the factory registry, and
    // destructs all connections for all existing pools.
    _pool->shutdown();

    // Close out all remaining tasks in the reactor now that they've all been canceled.
    _reactor->drain();

    LOG(2) << "NetworkInterfaceTL shutdown successfully";
}

void NetworkInterfaceTL::shutdown() {
    if (_state.swap(kStopped) != kStarted)
        return;

    LOG(2) << "Shutting down network interface.";

    // Stop the reactor/thread first so that nothing runs on a partially dtor'd pool.
    _reactor->stop();

    _cancelAllAlarms();

    _ioThread.join();
}

bool NetworkInterfaceTL::inShutdown() const {
    return _state.load() == kStopped;
}

void NetworkInterfaceTL::waitForWork() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    MONGO_IDLE_THREAD_BLOCK;
    _workReadyCond.wait(lk, [this] { return _isExecutorRunnable; });
}

void NetworkInterfaceTL::waitForWorkUntil(Date_t when) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    MONGO_IDLE_THREAD_BLOCK;
    _workReadyCond.wait_until(lk, when.toSystemTimePoint(), [this] { return _isExecutorRunnable; });
}

void NetworkInterfaceTL::signalWorkAvailable() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (!_isExecutorRunnable) {
        _isExecutorRunnable = true;
        _workReadyCond.notify_one();
    }
}

Date_t NetworkInterfaceTL::now() {
    // TODO This check is because we set up NetworkInterfaces in MONGO_INITIALIZERS and then expect
    // this method to work before the NI is started.
    if (!_reactor) {
        return Date_t::now();
    }
    return _reactor->now();
}

NetworkInterfaceTL::CommandState::CommandState(NetworkInterfaceTL* interface_,
                                               RemoteCommandRequestOnAny request_,
                                               const TaskExecutor::CallbackHandle& cbHandle_,
                                               Promise<RemoteCommandOnAnyResponse> promise_)
    : interface(interface_),
      requestOnAny(std::move(request_)),
      cbHandle(cbHandle_),
      promise(std::move(promise_)) {}


auto NetworkInterfaceTL::CommandState::make(NetworkInterfaceTL* interface,
                                            RemoteCommandRequestOnAny request,
                                            const TaskExecutor::CallbackHandle& cbHandle,
                                            Promise<RemoteCommandOnAnyResponse> promise) {
    auto state =
        std::make_shared<CommandState>(interface, std::move(request), cbHandle, std::move(promise));

    {
        stdx::lock_guard lk(interface->_inProgressMutex);
        interface->_inProgress.insert({cbHandle, state});
    }

    return state;
}

NetworkInterfaceTL::CommandState::~CommandState() {
    invariant(interface);

    {
        stdx::lock_guard lk(interface->_inProgressMutex);
        interface->_inProgress.erase(cbHandle);
    }
}

Status NetworkInterfaceTL::startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                        RemoteCommandRequestOnAny& request,
                                        RemoteCommandCompletionFn&& onFinish,
                                        const BatonHandle& baton) {
    if (inShutdown()) {
        return {ErrorCodes::ShutdownInProgress, "NetworkInterface shutdown in progress"};
    }

    LOG(3) << "startCommand: " << redact(request.toString());

    if (_metadataHook) {
        BSONObjBuilder newMetadata(std::move(request.metadata));

        auto status = _metadataHook->writeRequestMetadata(request.opCtx, &newMetadata);
        if (!status.isOK()) {
            return status;
        }

        request.metadata = newMetadata.obj();
    }

    auto cmdPF = makePromiseFuture<RemoteCommandOnAnyResponse>();

    auto cmdState = CommandState::make(this, request, cbHandle, std::move(cmdPF.promise));
    cmdState->start = now();
    if (cmdState->requestOnAny.timeout != cmdState->requestOnAny.kNoTimeout) {
        cmdState->deadline = cmdState->start + cmdState->requestOnAny.timeout;
    }

    auto executor = baton ? ExecutorPtr(baton) : ExecutorPtr(_reactor);
    std::move(cmdPF.future)
        .thenRunOn(executor)
        .onError([requestId = cmdState->requestOnAny.id](auto error)
                     ->StatusWith<RemoteCommandOnAnyResponse> {
                         LOG(2) << "Failed to get connection from pool for request " << requestId
                                << ": " << redact(error);

                         // The TransportLayer has, for historical reasons returned SocketException
                         // for network errors, but sharding assumes HostUnreachable on network
                         // errors.
                         if (error == ErrorCodes::SocketException) {
                             error = Status(ErrorCodes::HostUnreachable, error.reason());
                         }
                         return error;
                     })
        .getAsync([ this, cmdState, onFinish = std::move(onFinish) ](
            StatusWith<RemoteCommandOnAnyResponse> response) {
            auto duration = now() - cmdState->start;
            if (!response.isOK()) {
                onFinish(RemoteCommandOnAnyResponse(boost::none, response.getStatus(), duration));
            } else {
                const auto& rs = response.getValue();
                LOG(2) << "Request " << cmdState->requestOnAny.id << " finished with response: "
                       << redact(rs.isOK() ? rs.data.toString() : rs.status.toString());
                onFinish(rs);
            }
        });

    if (MONGO_FAIL_POINT(networkInterfaceDiscardCommandsBeforeAcquireConn)) {
        log() << "Discarding command due to failpoint before acquireConn";
        return Status::OK();
    }

    auto[connPromise, connFuture] = makePromiseFuture<ConnectionPool::ConnectionHandle>();

    std::move(connFuture).thenRunOn(executor).getAsync([this, cmdState, baton](auto swConn) {
        auto status = swConn.getStatus();

        if (status.isOK()) {
            if (MONGO_FAIL_POINT(networkInterfaceDiscardCommandsAfterAcquireConn)) {
                log() << "Discarding command due to failpoint after acquireConn";
            } else {
                try {
                    _onAcquireConn(cmdState, std::move(swConn.getValue()), baton);
                } catch (const DBException& ex) {
                    status = ex.toStatus();
                }
            }
        }

        if (!status.isOK() && !cmdState->done.swap(true)) {
            cmdState->promise.setError(status);
        }
    });

    struct ConnState {
        explicit ConnState(size_t n, Promise<ConnectionPool::ConnectionHandle> p)
            : finishLine(n), promise(std::move(p)) {}

        StrongWeakFinishLine finishLine;
        Promise<ConnectionPool::ConnectionHandle> promise;
    };

    auto connState = std::make_shared<ConnState>(request.target.size(), std::move(connPromise));

    for (size_t idx = 0; idx < request.target.size() && !connState->finishLine.isReady(); ++idx) {
        auto getConnectionCallback = [ connState, cmdState, idx ](
            StatusWith<ConnectionPool::ConnectionHandle> swConn) noexcept {
            if (swConn.isOK()) {
                if (connState->finishLine.arriveStrongly()) {
                    cmdState->request.emplace(cmdState->requestOnAny, idx);
                    connState->promise.emplaceValue(std::move(swConn.getValue()));
                } else {
                    swConn.getValue()->indicateSuccess();
                }
            } else {
                LOG(2) << "Failed to get connection from pool for request "
                       << cmdState->requestOnAny.id << ": " << swConn.getStatus();

                if (connState->finishLine.arriveWeakly()) {
                    connState->promise.setError(swConn.getStatus());
                }
            }
        };

        if (auto semi = _pool->get(request.target[idx], request.sslMode, request.timeout);
            semi.isReady()) {
            // If we have a connection in hand, stay on thread and immediately handle it
            getConnectionCallback(std::move(semi).getNoThrow());
        } else {
            // Otherwise route all connection management over the networking reactor, to ensure we
            // promptly return connections to the pool that may not be needed
            std::move(semi)
                .thenRunOn(ExecutorPtr(_reactor))
                .getAsync(std::move(getConnectionCallback));
        }
    }

    return Status::OK();
}

// This is only called from within a then() callback on a future, so throwing is equivalent to
// returning a ready Future with a not-OK status.
void NetworkInterfaceTL::_onAcquireConn(std::shared_ptr<CommandState> state,
                                        ConnectionPool::ConnectionHandle conn,
                                        const BatonHandle& baton) {

    if (state->done.load()) {
        conn->indicateSuccess();
        uasserted(ErrorCodes::CallbackCanceled, "Command was canceled");
    }

    state->conn = std::move(conn);
    auto tlconn = checked_cast<connection_pool_tl::TLConnection*>(state->conn.get());
    auto client = tlconn->client();

    if (state->deadline != RemoteCommandRequest::kNoExpirationDate) {
        auto nowVal = now();
        if (nowVal >= state->deadline) {
            auto connDuration = nowVal - state->start;
            uasserted(ErrorCodes::NetworkInterfaceExceededTimeLimit,
                      str::stream() << "Remote command timed out while waiting to get a "
                                       "connection from the pool, took "
                                    << connDuration
                                    << ", timeout was set to "
                                    << state->requestOnAny.timeout);
        }

        state->timer = _reactor->makeTimer();
        state->timer->waitUntil(state->deadline, baton)
            .getAsync([this, client, state, baton](Status status) {
                if (status == ErrorCodes::CallbackCanceled) {
                    invariant(state->done.load());
                    return;
                }

                if (state->done.swap(true)) {
                    return;
                }

                if (getTestCommandsEnabled()) {
                    stdx::lock_guard<stdx::mutex> lk(_mutex);
                    _counters.timedOut++;
                }

                const std::string message = str::stream()
                    << "Request " << state->requestOnAny.id << " timed out"
                    << ", deadline was " << state->deadline.toString() << ", op was "
                    << redact(state->requestOnAny.toString());

                LOG(2) << message;
                state->promise.setError(
                    Status(ErrorCodes::NetworkInterfaceExceededTimeLimit, message));

                client->cancel(baton);
            });
    }

    client->runCommandRequest(*state->request, baton)
        .then([this, state](RemoteCommandResponse response) {
            if (state->done.load()) {
                uasserted(ErrorCodes::CallbackCanceled, "Callback was canceled");
            }

            const auto& target = state->conn->getHostAndPort();

            if (_metadataHook && response.status.isOK()) {
                response.status =
                    _metadataHook->readReplyMetadata(nullptr, target.toString(), response.data);
            }

            return RemoteCommandOnAnyResponse(target, std::move(response));
        })
        .getAsync([this, state, baton](StatusWith<RemoteCommandOnAnyResponse> swr) {
            if (!swr.isOK()) {
                state->conn->indicateFailure(swr.getStatus());
            } else if (!swr.getValue().isOK()) {
                state->conn->indicateFailure(swr.getValue().status);
            } else {
                state->conn->indicateUsed();
                state->conn->indicateSuccess();
            }

            if (state->done.swap(true)) {
                return;
            }

            if (getTestCommandsEnabled()) {
                stdx::lock_guard<stdx::mutex> lk(_mutex);
                if (swr.isOK() && swr.getValue().status.isOK()) {
                    _counters.succeeded++;
                } else {
                    _counters.failed++;
                }
            }

            if (state->timer) {
                state->timer->cancel(baton);
            }

            state->promise.setFromStatusWith(std::move(swr));
        });
}

void NetworkInterfaceTL::cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                       const BatonHandle& baton) {
    stdx::unique_lock<stdx::mutex> lk(_inProgressMutex);
    auto it = _inProgress.find(cbHandle);
    if (it == _inProgress.end()) {
        return;
    }
    auto state = it->second.lock();
    if (!state) {
        return;
    }

    _inProgress.erase(it);
    lk.unlock();

    if (state->done.swap(true)) {
        return;
    }

    if (getTestCommandsEnabled()) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _counters.canceled++;
    }

    LOG(2) << "Canceling operation; original request was: "
           << redact(state->requestOnAny.toString());
    state->promise.setError({ErrorCodes::CallbackCanceled,
                             str::stream() << "Command canceled; original request was: "
                                           << redact(state->requestOnAny.toString())});
    if (state->conn) {
        auto client = checked_cast<connection_pool_tl::TLConnection*>(state->conn.get());
        client->client()->cancel(baton);
    }
}

Status NetworkInterfaceTL::schedule(unique_function<void(Status)> action) {
    if (inShutdown()) {
        return {ErrorCodes::ShutdownInProgress, "NetworkInterface shutdown in progress"};
    }

    _reactor->schedule([action = std::move(action)](auto status) { action(status); });
    return Status::OK();
}

Status NetworkInterfaceTL::setAlarm(const TaskExecutor::CallbackHandle& cbHandle,
                                    Date_t when,
                                    unique_function<void(Status)> action) {
    if (inShutdown()) {
        return {ErrorCodes::ShutdownInProgress, "NetworkInterface shutdown in progress"};
    }

    if (when <= now()) {
        _reactor->schedule([action = std::move(action)](auto status) { action(status); });
        return Status::OK();
    }

    auto pf = makePromiseFuture<void>();
    std::move(pf.future).getAsync(std::move(action));

    auto alarmState =
        std::make_shared<AlarmState>(when, cbHandle, _reactor->makeTimer(), std::move(pf.promise));

    {
        stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);

        // If a user has already scheduled an alarm with a handle, make sure they intentionally
        // override it by canceling and setting a new one.
        auto alarmPair = std::make_pair(cbHandle, std::shared_ptr<AlarmState>(alarmState));
        auto && [ _, wasInserted ] = _inProgressAlarms.insert(std::move(alarmPair));
        invariant(wasInserted);
    }

    alarmState->timer->waitUntil(alarmState->when, nullptr).getAsync([
        this,
        state = std::move(alarmState)
    ](Status status) mutable { _answerAlarm(status, state); });

    return Status::OK();
}

void NetworkInterfaceTL::cancelAlarm(const TaskExecutor::CallbackHandle& cbHandle) {
    stdx::unique_lock<stdx::mutex> lk(_inProgressMutex);

    auto iter = _inProgressAlarms.find(cbHandle);

    if (iter == _inProgressAlarms.end()) {
        return;
    }

    auto alarmState = std::move(iter->second);

    _inProgressAlarms.erase(iter);

    lk.unlock();

    alarmState->timer->cancel();
    alarmState->promise.setError(Status(ErrorCodes::CallbackCanceled, "Alarm cancelled"));
}

void NetworkInterfaceTL::_cancelAllAlarms() {
    auto alarms = [&] {
        stdx::unique_lock<stdx::mutex> lk(_inProgressMutex);
        return std::exchange(_inProgressAlarms, {});
    }();

    for (auto && [ cbHandle, state ] : alarms) {
        state->timer->cancel();
        state->promise.setError(Status(ErrorCodes::CallbackCanceled, "Alarm cancelled"));
    }
}

void NetworkInterfaceTL::_answerAlarm(Status status, std::shared_ptr<AlarmState> state) {
    // Since the lock is released before canceling the timer, this thread can win the race with
    // cancelAlarm(). Thus if status is CallbackCanceled, then this alarm is already removed from
    // _inProgressAlarms.
    if (status == ErrorCodes::CallbackCanceled) {
        return;
    }

    // transport::Reactor timers do not involve spurious wake ups, however, this check is nearly
    // free and allows us to be resilient to a world where timers impls do have spurious wake ups.
    auto currentTime = now();
    if (status.isOK() && currentTime < state->when) {
        LOG(2) << "Alarm returned early. Expected at: " << state->when
               << ", fired at: " << currentTime;
        state->timer->waitUntil(state->when, nullptr).getAsync([
            this,
            state = std::move(state)
        ](Status status) mutable { _answerAlarm(status, state); });
        return;
    }

    // Erase the AlarmState from the map.
    {
        stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);

        auto iter = _inProgressAlarms.find(state->cbHandle);
        if (iter == _inProgressAlarms.end()) {
            return;
        }

        _inProgressAlarms.erase(iter);
    }

    // A not OK status here means the timer experienced a system error.
    // It is not reasonable to complete the promise on a reactor thread because there is likely no
    // properly functioning reactor.
    if (!status.isOK()) {
        state->promise.setError(status);
        return;
    }

    // Fulfill the promise on a reactor thread
    _reactor->schedule([state](auto status) {
        if (status.isOK()) {
            state->promise.emplaceValue();
        } else {
            state->promise.setError(status);
        }
    });
}

bool NetworkInterfaceTL::onNetworkThread() {
    return _reactor->onReactorThread();
}

void NetworkInterfaceTL::dropConnections(const HostAndPort& hostAndPort) {
    _pool->dropConnections(hostAndPort);
}

}  // namespace executor
}  // namespace mongo
