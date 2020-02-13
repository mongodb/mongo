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
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {
namespace executor {

/**
 * SynchronizedCounters is synchronized bucket of event counts for commands
 */
class NetworkInterfaceTL::SynchronizedCounters {
public:
    auto get() const {
        stdx::lock_guard lk(_mutex);
        return _data;
    }


    void recordResult(const Status& status) {
        stdx::lock_guard lk(_mutex);
        if (status.isOK()) {
            // Increment the count of commands that received a valid response
            ++_data.succeeded;
        } else if (ErrorCodes::isExceededTimeLimitError(status)) {
            // Increment the count of commands that experienced a local timeout
            // Note that these commands do not count as "failed".
            ++_data.timedOut;
        } else if (ErrorCodes::isCancelationError(status)) {
            // Increment the count of commands that were canceled locally
            ++_data.canceled;
        } else if (ErrorCodes::isShutdownError(status)) {
            // Increment the count of commands that received an unrecoverable response
            ++_data.failedRemotely;
        } else {
            // Increment the count of commands that experienced a network failure
            ++_data.failed;
        }
    }

    /**
     * Increment the count of commands sent over the network
     */
    void recordSent() {
        ++_data.sent;
    }

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0),
                                            "NetworkInterfaceTL::SynchronizedCounters::_mutex");
    Counters _data;
};

NetworkInterfaceTL::NetworkInterfaceTL(std::string instanceName,
                                       ConnectionPool::Options connPoolOpts,
                                       ServiceContext* svcCtx,
                                       std::unique_ptr<NetworkConnectionHook> onConnectHook,
                                       std::unique_ptr<rpc::EgressMetadataHook> metadataHook)
    : _instanceName(std::move(instanceName)),
      _svcCtx(svcCtx),
      _connPoolOpts(std::move(connPoolOpts)),
      _onConnectHook(std::move(onConnectHook)),
      _metadataHook(std::move(metadataHook)),
      _state(kDefault) {
    if (_svcCtx) {
        _tl = _svcCtx->getTransportLayer();
    }

    // Even if you have a service context, it may not have a transport layer (mostly for unittests).
    if (!_tl) {
        LOGV2_WARNING(22601, "No TransportLayer configured during NetworkInterface startup");
        _ownedTransportLayer =
            transport::TransportLayerManager::makeAndStartDefaultEgressTransportLayer();
        _tl = _ownedTransportLayer.get();
    }

    _reactor = _tl->getReactor(transport::TransportLayer::kNewReactor);
    auto typeFactory = std::make_unique<connection_pool_tl::TLTypeFactory>(
        _reactor, _tl, std::move(_onConnectHook), _connPoolOpts);
    _pool = std::make_shared<ConnectionPool>(
        std::move(typeFactory), std::string("NetworkInterfaceTL-") + _instanceName, _connPoolOpts);

    if (getTestCommandsEnabled()) {
        _counters = std::make_unique<SynchronizedCounters>();
    }
}

std::string NetworkInterfaceTL::getDiagnosticString() {
    return "DEPRECATED: getDiagnosticString is deprecated in NetworkInterfaceTL";
}

void NetworkInterfaceTL::appendConnectionStats(ConnectionPoolStats* stats) const {
    auto pool = [&] {
        stdx::lock_guard<Latch> lk(_mutex);
        return _pool.get();
    }();
    if (pool)
        pool->appendConnectionStats(stats);
}

NetworkInterface::Counters NetworkInterfaceTL::getCounters() const {
    invariant(_counters);
    return _counters->get();
}

std::string NetworkInterfaceTL::getHostName() {
    return getHostNameCached();
}

void NetworkInterfaceTL::startup() {
    stdx::lock_guard<Latch> lk(_mutex);

    _ioThread = stdx::thread([this] {
        setThreadName(_instanceName);
        _run();
    });

    invariant(_state.swap(kStarted) == kDefault);
}

void NetworkInterfaceTL::_run() {
    LOGV2_DEBUG(22592, 2, "The NetworkInterfaceTL reactor thread is spinning up");

    // This returns when the reactor is stopped in shutdown()
    _reactor->run();

    // Note that the pool will shutdown again when the ConnectionPool dtor runs
    // This prevents new timers from being set, calls all cancels via the factory registry, and
    // destructs all connections for all existing pools.
    _pool->shutdown();

    // Close out all remaining tasks in the reactor now that they've all been canceled.
    _reactor->drain();

    LOGV2_DEBUG(22593, 2, "NetworkInterfaceTL shutdown successfully");
}

void NetworkInterfaceTL::shutdown() {
    if (_state.swap(kStopped) != kStarted)
        return;

    LOGV2_DEBUG(22594, 2, "Shutting down network interface.");

    // Stop the reactor/thread first so that nothing runs on a partially dtor'd pool.
    _reactor->stop();

    _cancelAllAlarms();

    _ioThread.join();
}

bool NetworkInterfaceTL::inShutdown() const {
    return _state.load() == kStopped;
}

void NetworkInterfaceTL::waitForWork() {
    stdx::unique_lock<Latch> lk(_mutex);
    MONGO_IDLE_THREAD_BLOCK;
    _workReadyCond.wait(lk, [this] { return _isExecutorRunnable; });
}

void NetworkInterfaceTL::waitForWorkUntil(Date_t when) {
    stdx::unique_lock<Latch> lk(_mutex);
    MONGO_IDLE_THREAD_BLOCK;
    _workReadyCond.wait_until(lk, when.toSystemTimePoint(), [this] { return _isExecutorRunnable; });
}

void NetworkInterfaceTL::signalWorkAvailable() {
    stdx::unique_lock<Latch> lk(_mutex);
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
                                               const TaskExecutor::CallbackHandle& cbHandle_)
    : interface(interface_),
      requestOnAny(std::move(request_)),
      cbHandle(cbHandle_),
      finishLine(maxRequestFailures()) {}


auto NetworkInterfaceTL::CommandState::make(NetworkInterfaceTL* interface,
                                            RemoteCommandRequestOnAny request,
                                            const TaskExecutor::CallbackHandle& cbHandle) {
    auto state = std::make_shared<CommandState>(interface, std::move(request), cbHandle);
    auto [promise, future] = makePromiseFuture<RemoteCommandOnAnyResponse>();
    state->promise = std::move(promise);
    future = std::move(future)
                 .onError([state](Status error) {
                     // If command promise was canceled or timed out, wrap the error in a RCRsp
                     return RemoteCommandOnAnyResponse(
                         boost::none, std::move(error), state->stopwatch.elapsed());
                 })
                 .tapAll([state](const auto& swRequest) {
                     // swRequest is either populated from the success path or the value returning
                     // onError above. swRequest.isOK() should not be possible.
                     invariant(swRequest.isOK());

                     // At this point, the command has either been sent and returned an RCRsp or
                     // has received a local interruption that was wrapped in a RCRsp.
                     state->tryFinish(swRequest.getValue().status);
                 });

    {
        stdx::lock_guard lk(interface->_inProgressMutex);
        interface->_inProgress.insert({cbHandle, state});
    }

    return std::pair(state, std::move(future));
}

AsyncDBClient* NetworkInterfaceTL::RequestState::client() noexcept {
    if (!conn) {
        return nullptr;
    }

    return checked_cast<connection_pool_tl::TLConnection*>(conn.get())->client();
}

void NetworkInterfaceTL::CommandState::setTimer() {
    if (deadline == RemoteCommandRequest::kNoExpirationDate) {
        return;
    }

    const auto nowVal = interface->now();
    if (nowVal >= deadline) {
        auto connDuration = stopwatch.elapsed();
        uasserted(ErrorCodes::NetworkInterfaceExceededTimeLimit,
                  str::stream() << "Remote command timed out while waiting to get a "
                                   "connection from the pool, took "
                                << connDuration << ", timeout was set to " << requestOnAny.timeout);
    }

    // TODO reform with SERVER-41459
    timer = interface->_reactor->makeTimer();
    timer->waitUntil(deadline, baton).getAsync([this, anchor = shared_from_this()](Status status) {
        if (!status.isOK()) {
            return;
        }

        if (!finishLine.arriveStrongly()) {
            // If we didn't cross the command finishLine first, the promise is already fulfilled
            return;
        }

        const std::string message = str::stream() << "Request " << requestOnAny.id << " timed out"
                                                  << ", deadline was " << deadline.toString()
                                                  << ", op was " << redact(requestOnAny.toString());

        LOGV2_DEBUG(22595, 2, "{message}", "message"_attr = message);
        promise.setError(Status(ErrorCodes::NetworkInterfaceExceededTimeLimit, message));
    });
}

void NetworkInterfaceTL::RequestState::returnConnection(Status status) noexcept {
    // Settle the connection object on the reactor
    invariant(conn);
    invariant(interface()->_reactor->onReactorThread());

    auto connToReturn = std::exchange(conn, {});

    if (!status.isOK()) {
        connToReturn->indicateFailure(std::move(status));
        return;
    }

    connToReturn->indicateUsed();
    connToReturn->indicateSuccess();
}

void NetworkInterfaceTL::CommandState::tryFinish(Status status) noexcept {
    invariant(finishLine.isReady());

    if (timer) {
        // The command has resolved one way or another,
        timer->cancel(baton);
    }

    if (auto requestState = requestStatePtr.lock(); requestState && !status.isOK()) {
        requestState->cancel();
    }

    if (interface->_counters) {
        // Increment our counters for the integration test
        interface->_counters->recordResult(status);
    }

    {
        // We've finished, we're not in progress anymore
        stdx::lock_guard lk(interface->_inProgressMutex);
        interface->_inProgress.erase(cbHandle);
    }
}

void NetworkInterfaceTL::RequestState::cancel() noexcept {
    if (connFinishLine.arriveStrongly()) {
        // We've canceled before any connections were acquired, we're all good.
        return;
    }

    auto& reactor = interface()->_reactor;

    // If we failed, then get the client to finish up.
    // Note: CommandState::returnConnection() and CommandState::cancel() run on the reactor
    // thread only. One goes first and then the other, so there isn't a risk of canceling
    // the next command to run on the connection.
    if (reactor->onReactorThread()) {
        if (auto clientPtr = client()) {
            // If we have a client, cancel it
            clientPtr->cancel(cmdState->baton);
        }
    } else {
        ExecutorFuture<void>(reactor).getAsync([this, anchor = shared_from_this()](Status status) {
            invariant(status.isOK());
            if (auto clientPtr = client()) {
                // If we have a client, cancel it
                clientPtr->cancel(cmdState->baton);
            }
        });
    }
}

NetworkInterfaceTL::RequestState::~RequestState() {
    invariant(!conn);
}

Status NetworkInterfaceTL::startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                        RemoteCommandRequestOnAny& request,
                                        RemoteCommandCompletionFn&& onFinish,
                                        const BatonHandle& baton) {
    if (inShutdown()) {
        return {ErrorCodes::ShutdownInProgress, "NetworkInterface shutdown in progress"};
    }

    LOGV2_DEBUG(22596,
                logSeverityV1toV2(kDiagnosticLogLevel).toInt(),
                "startCommand: {request}",
                "request"_attr = redact(request.toString()));

    if (_metadataHook) {
        BSONObjBuilder newMetadata(std::move(request.metadata));

        auto status = _metadataHook->writeRequestMetadata(request.opCtx, &newMetadata);
        if (!status.isOK()) {
            return status;
        }

        request.metadata = newMetadata.obj();
    }

    auto [cmdState, future] = CommandState::make(this, request, cbHandle);
    if (cmdState->requestOnAny.timeout != cmdState->requestOnAny.kNoTimeout) {
        cmdState->deadline = cmdState->stopwatch.start() + cmdState->requestOnAny.timeout;
    }
    cmdState->baton = baton;

    /**
     * It is important that onFinish() runs out of line. That said, we can't thenRunOn() arbitrarily
     * without doing extra context switches and delaying execution. The cmdState promise can be
     * fulfilled in these paths:
     *
     * 1.  There are available connections to all nodes but they're all bad. This path is inline so
     *     it then schedules onto the reactor to finish.
     * 2.  All nodes are bad but some needed new connections. The reaction to the new connection
     *     needs to be scheduled onto the reactor.
     * 3.  The timer in onAcquireConn() fires and the operation times out. ASIO timers run on the
     *     reactor.
     * 4.  AsyncDBClient::runCommandRequest() concludes. This path is sadly indeterminate since
     *     early failure can still be inline. The future chain is thenRunOn() either the baton or
     *     the reactor.
     *
     * The important bits to remember here:
     * - onFinish() is out-of-line
     * - Stay inline as long as feasible until onAcquireConn()---i.e. until network operations
     * - Baton execution *cannot* be relied upon at least until onAcquireConn()
     * - Connection failure and command failure are related but distinct
     */

    // When our command finishes, run onFinish
    std::move(future).getAsync([this, cmdState = cmdState, onFinish = std::move(onFinish)](
                                   StatusWith<RemoteCommandOnAnyResponse> swr) {
        invariant(swr.isOK());
        auto rs = std::move(swr.getValue());
        LOGV2_DEBUG(22597,
                    2,
                    "Request {cmdState_requestOnAny_id} finished with response: "
                    "{rs_isOK_rs_data_rs_status_toString}",
                    "cmdState_requestOnAny_id"_attr = cmdState->requestOnAny.id,
                    "rs_isOK_rs_data_rs_status_toString"_attr =
                        redact(rs.isOK() ? rs.data.toString() : rs.status.toString()));
        onFinish(std::move(rs));
    });

    if (MONGO_unlikely(networkInterfaceDiscardCommandsBeforeAcquireConn.shouldFail())) {
        LOGV2(22598, "Discarding command due to failpoint before acquireConn");
        return Status::OK();
    }

    auto requestState = std::make_shared<RequestState>(cmdState);
    cmdState->requestStatePtr = requestState;

    // Attempt to get a connection to every target host
    for (size_t idx = 0; idx < request.target.size() && !requestState->connFinishLine.isReady();
         ++idx) {
        auto connFuture = _pool->get(request.target[idx], request.sslMode, request.timeout);

        if (connFuture.isReady()) {
            requestState->trySend(std::move(connFuture).getNoThrow(), idx);
            continue;
        }

        // For every connection future we didn't have immediately ready, schedule
        std::move(connFuture).thenRunOn(_reactor).getAsync([requestState, idx](auto swConn) {
            requestState->trySend(std::move(swConn), idx);
        });
    }

    return Status::OK();
}

Future<RemoteCommandResponse> NetworkInterfaceTL::CommandState::sendRequest() {
    auto requestState = requestStatePtr.lock();
    invariant(requestState);

    return makeReadyFutureWith([this, requestState] {
               setTimer();

               return requestState->client()->runCommandRequest(*requestState->request, baton);
           })
        .then([this, requestState](RemoteCommandResponse response) {
            doMetadataHook(RemoteCommandOnAnyResponse(requestState->host, response));
            return response;
        });
}

void NetworkInterfaceTL::CommandState::doMetadataHook(const RemoteCommandOnAnyResponse& response) {
    if (auto& hook = interface->_metadataHook; hook && !finishLine.isReady()) {
        invariant(response.target);

        uassertStatusOK(
            hook->readReplyMetadata(nullptr, response.target->toString(), response.data));
    }
}

void NetworkInterfaceTL::RequestState::trySend(StatusWith<ConnectionPool::ConnectionHandle> swConn,
                                               size_t idx) noexcept {
    // Our connection wasn't any good
    if (!swConn.isOK()) {
        if (!connFinishLine.arriveWeakly()) {
            // There are others!
            return;
        }

        // We're the last one, set the promise if it hasn't already been set via cancel or timeout
        if (cmdState->finishLine.arriveStrongly()) {
            auto& reactor = interface()->_reactor;
            if (reactor->onReactorThread()) {
                cmdState->promise.setError(swConn.getStatus());
            } else {
                ExecutorFuture<void>(reactor, swConn.getStatus()).getAsync([this](Status status) {
                    cmdState->promise.setError(std::move(status));
                });
            }
        }
        return;
    }

    // Our command has already been attempted or satisfied
    if (!connFinishLine.arriveStrongly() || cmdState->finishLine.isReady()) {
        swConn.getValue()->indicateSuccess();
        return;
    }

    // We have a connection and the command hasn't already been attempted
    request.emplace(cmdState->requestOnAny, idx);
    host = cmdState->requestOnAny.target[idx];
    conn = std::move(swConn.getValue());

    networkInterfaceDiscardCommandsAfterAcquireConn.pauseWhileSet();

    if (auto counters = interface()->_counters) {
        counters->recordSent();
    }

    resolve(cmdState->sendRequest());
}

void NetworkInterfaceTL::RequestState::resolve(Future<RemoteCommandResponse> future) noexcept {
    auto& reactor = interface()->_reactor;
    auto& baton = cmdState->baton;

    // Convert the RemoteCommandResponse to a RemoteCommandOnAnyResponse and wrap any error
    auto anyFuture =
        std::move(future)
            .then([this, anchor = shared_from_this()](RemoteCommandResponse response) {
                // The RCRq ran successfully, wrap the result with the host in question
                return RemoteCommandOnAnyResponse(host, std::move(response));
            })
            .onError([this, anchor = shared_from_this()](Status error) {
                // The RCRq failed, wrap the error into a RCRsp with the host and duration

                // The TransportLayer has, for historical reasons returned
                // SocketException for network errors, but sharding assumes
                // HostUnreachable on network errors.
                if (error == ErrorCodes::SocketException) {
                    error = Status(ErrorCodes::HostUnreachable, error.reason());
                }

                return RemoteCommandOnAnyResponse(host, std::move(error), stopwatch.elapsed());
            });

    if (baton) {
        // If we have a baton then use it for the promise and then switch to the reactor to return
        // our connection.
        std::move(anyFuture)
            .thenRunOn(baton)
            .onCompletion([ this, anchor = shared_from_this() ](auto swr) noexcept {
                auto response = uassertStatusOK(swr);
                auto status = swr.getValue().status;
                if (cmdState->finishLine.arriveStrongly()) {
                    cmdState->promise.emplaceValue(std::move(response));
                }

                return status;
            })
            .thenRunOn(reactor)
            .getAsync(
                [this, anchor = shared_from_this()](Status status) { returnConnection(status); });
    } else {
        // If we do not have a baton, then we can fulfill the promise and return our connection in
        // the same callback
        std::move(anyFuture).thenRunOn(reactor).getAsync(
            [ this, anchor = shared_from_this() ](auto swr) noexcept {
                auto response = uassertStatusOK(swr);
                auto status = response.status;
                ON_BLOCK_EXIT([&] { returnConnection(status); });
                if (!cmdState->finishLine.arriveStrongly()) {
                    return;
                }

                cmdState->promise.emplaceValue(std::move(response));
            });
    }
}

void NetworkInterfaceTL::cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                       const BatonHandle&) {
    stdx::unique_lock<Latch> lk(_inProgressMutex);
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

    if (!state->finishLine.arriveStrongly()) {
        // If we didn't cross the command finishLine first, the promise is already fulfilled
        return;
    }

    // Satisfy the promise locally
    LOGV2_DEBUG(22599,
                2,
                "Canceling operation; original request was: {state_requestOnAny}",
                "state_requestOnAny"_attr = redact(state->requestOnAny.toString()));
    state->promise.setError({ErrorCodes::CallbackCanceled,
                             str::stream() << "Command canceled; original request was: "
                                           << redact(state->requestOnAny.toString())});
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
        stdx::lock_guard<Latch> lk(_inProgressMutex);

        // If a user has already scheduled an alarm with a handle, make sure they intentionally
        // override it by canceling and setting a new one.
        auto alarmPair = std::make_pair(cbHandle, std::shared_ptr<AlarmState>(alarmState));
        auto&& [_, wasInserted] = _inProgressAlarms.insert(std::move(alarmPair));
        invariant(wasInserted);
    }

    alarmState->timer->waitUntil(alarmState->when, nullptr)
        .getAsync([this, state = std::move(alarmState)](Status status) mutable {
            _answerAlarm(status, state);
        });

    return Status::OK();
}

void NetworkInterfaceTL::cancelAlarm(const TaskExecutor::CallbackHandle& cbHandle) {
    stdx::unique_lock<Latch> lk(_inProgressMutex);

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
        stdx::unique_lock<Latch> lk(_inProgressMutex);
        return std::exchange(_inProgressAlarms, {});
    }();

    for (auto&& [cbHandle, state] : alarms) {
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
        LOGV2_DEBUG(22600,
                    2,
                    "Alarm returned early. Expected at: {state_when}, fired at: {currentTime}",
                    "state_when"_attr = state->when,
                    "currentTime"_attr = currentTime);
        state->timer->waitUntil(state->when, nullptr)
            .getAsync([this, state = std::move(state)](Status status) mutable {
                _answerAlarm(status, state);
            });
        return;
    }

    // Erase the AlarmState from the map.
    {
        stdx::lock_guard<Latch> lk(_inProgressMutex);

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
