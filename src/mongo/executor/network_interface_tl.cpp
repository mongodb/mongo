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


#include "mongo/executor/network_interface_tl.h"

#include <fmt/format.h>

#include "mongo/config.h"
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/server_options.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/connection_pool_tl.h"
#include "mongo/executor/hedging_metrics.h"
#include "mongo/executor/network_interface_tl_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kASIO


namespace mongo {
namespace executor {

using namespace fmt::literals;

namespace {
static inline const std::string kMaxTimeMSOpOnlyField = "maxTimeMSOpOnly";

Status appendMetadata(RemoteCommandRequestOnAny* request,
                      const std::unique_ptr<rpc::EgressMetadataHook>& hook) {
    if (hook) {
        BSONObjBuilder bob(std::move(request->metadata));
        auto status = hook->writeRequestMetadata(request->opCtx, &bob);
        if (!status.isOK()) {
            return status;
        }
        request->metadata = bob.obj();
    }

    if (!request->opCtx)
        return Status::OK();

    request->validatedTenancyScope = auth::ValidatedTenancyScope::get(request->opCtx);

    return Status::OK();
}

/**
 * Invokes `f()`, and returns true if it succeeds.
 * Otherwise, we log the exception with a `hint` string and handle the error.
 * The exception handling has two possibilities, controlled by the server parameter
 * `suppressNetworkInterfaceTransportLayerExceptions`.
 * The old behavior is to simply rethrow the exception, which will crash the process.
 * The new behavior is to invoke `eh(err)` and return false. This gives the caller a way
 * to provide a route to propagate the exception as a Status (perhaps filling a promise
 * with it) and carry on.
 */
template <typename F, typename EH>
bool catchingInvoke(F&& f, EH&& eh, StringData hint) {
    try {
        std::forward<F>(f)();
        return true;
    } catch (...) {
        Status err = exceptionToStatus();
        LOGV2(5802401, "Callback failed", "msg"_attr = hint, "error"_attr = err);
        if (gSuppressNetworkInterfaceTransportLayerExceptions.isEnabledAndIgnoreFCV())
            std::forward<EH>(eh)(err);  // new server parameter protected behavior
        else
            throw;  // old behavior
        return false;
    }
}

}  // namespace

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
        } else if (ErrorCodes::isCancellationError(status)) {
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

namespace {
const Status kNetworkInterfaceShutdownInProgress = {ErrorCodes::ShutdownInProgress,
                                                    "NetworkInterface shutdown in progress"};
}

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
        if (TestingProctor::instance().isEnabled()) {
            LOGV2_WARNING(22601, "No TransportLayer configured during NetworkInterface startup");
        }
        _ownedTransportLayer =
            transport::TransportLayerManager::makeAndStartDefaultEgressTransportLayer();
        _tl = _ownedTransportLayer.get();
    }

    std::shared_ptr<const transport::SSLConnectionContext> transientSSLContext;
#ifdef MONGO_CONFIG_SSL
    if (_connPoolOpts.transientSSLParams) {
        auto statusOrContext =
            _tl->createTransientSSLContext(_connPoolOpts.transientSSLParams.get());
        uassertStatusOK(statusOrContext.getStatus());
        transientSSLContext = std::move(statusOrContext.getValue());
    }
#endif

    _reactor = _tl->getReactor(transport::TransportLayer::kNewReactor);
    auto typeFactory = std::make_unique<connection_pool_tl::TLTypeFactory>(
        _reactor, _tl, std::move(_onConnectHook), _connPoolOpts, transientSSLContext);
    _pool = std::make_shared<ConnectionPool>(
        std::move(typeFactory), std::string("NetworkInterfaceTL-") + _instanceName, _connPoolOpts);

    if (TestingProctor::instance().isEnabled()) {
        _counters = std::make_unique<SynchronizedCounters>();
    }
}

NetworkInterfaceTL::~NetworkInterfaceTL() {
    shutdown();

    {
        stdx::unique_lock lk(_stateMutex);
        _stoppedCV.wait(lk, [&] { return _state == kStopped; });
    }

    // Because we quick exit on shutdown, these invariants are usually checked only in ASAN builds
    // and integration/unit tests.
    invariant(_inProgress.empty());
    invariant(_inProgressAlarms.empty());
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

void NetworkInterfaceTL::appendStats(BSONObjBuilder& bob) const {
    BSONObjBuilder builder = bob.subobjStart(_instanceName);
    _reactor->appendStats(builder);
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

    stdx::lock_guard stateLock(_stateMutex);
    invariant(_state == kDefault, "Network interface has already started");
    _state = kStarted;
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

    {
        stdx::lock_guard lk(_stateMutex);
        switch (_state) {
            case kDefault:
                _state = kStopped;
                _stoppedCV.notify_one();
                return;
            case kStarted:
                _state = kStopping;
                break;
            case kStopping:
            case kStopped:
                LOGV2_INFO(6529201,
                           "Network interface redundant shutdown",
                           "state"_attr = toString(_state));
                return;
        }
    }

    LOGV2_DEBUG(22594, 2, "Shutting down network interface.");

    const ScopeGuard finallySetStopped = [&] {
        stdx::lock_guard lk(_stateMutex);
        _state = kStopped;
        _stoppedCV.notify_one();
    };

    // Cancel any remaining commands. Any attempt to register new commands will throw.
    auto inProgress = [&] {
        stdx::lock_guard lk(_inProgressMutex);
        return std::exchange(_inProgress, {});
    }();

    for (auto& [_, weakCmdState] : inProgress) {
        auto cmdState = weakCmdState.lock();
        if (!cmdState) {
            continue;
        }

        if (!cmdState->finishLine.arriveStrongly()) {
            continue;
        }

        cmdState->fulfillFinalPromise(kNetworkInterfaceShutdownInProgress);
    }

    // Stop the reactor/thread first so that nothing runs on a partially dtor'd pool.
    _reactor->stop();

    _shutdownAllAlarms();

    _ioThread.join();
}

bool NetworkInterfaceTL::inShutdown() const {
    stdx::lock_guard lk(_stateMutex);
    return _state == kStopping || _state == kStopped;
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

NetworkInterfaceTL::CommandStateBase::CommandStateBase(
    NetworkInterfaceTL* interface_,
    RemoteCommandRequestOnAny request_,
    const TaskExecutor::CallbackHandle& cbHandle_)
    : interface(interface_),
      requestOnAny(std::move(request_)),
      cbHandle(cbHandle_),
      timer(interface->_reactor->makeTimer()),
      finishLine(1),
      operationKey(requestOnAny.operationKey) {}

NetworkInterfaceTL::CommandState::CommandState(NetworkInterfaceTL* interface_,
                                               RemoteCommandRequestOnAny request_,
                                               const TaskExecutor::CallbackHandle& cbHandle_)
    : CommandStateBase(interface_, std::move(request_), cbHandle_),
      hedgeCount(requestOnAny.options.isHedgeEnabled ? requestOnAny.options.hedgeCount + 1 : 1) {}

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

    state->requestManager = std::make_unique<RequestManager>(state.get());

    {
        stdx::lock_guard lk(interface->_inProgressMutex);
        if (interface->inShutdown()) {
            // If we're in shutdown, we can't add a new command.
            uassertStatusOK(kNetworkInterfaceShutdownInProgress);
        }

        interface->_inProgress.insert({cbHandle, state});
    }

    return std::pair(state, std::move(future));
}

AsyncDBClient* NetworkInterfaceTL::RequestState::getClient(const ConnectionHandle& conn) noexcept {
    if (!conn) {
        return nullptr;
    }

    return checked_cast<connection_pool_tl::TLConnection*>(conn.get())->client();
}

void NetworkInterfaceTL::CommandStateBase::setTimer() {
    if (deadline == kNoExpirationDate || !requestOnAny.enforceLocalTimeout) {
        return;
    }

    const auto timeoutCode = requestOnAny.timeoutCode;
    const auto nowVal = interface->now();
    if (nowVal >= deadline) {
        auto connDuration = stopwatch.elapsed();
        uasserted(timeoutCode,
                  str::stream() << "Remote command timed out while waiting to get a "
                                   "connection from the pool, took "
                                << connDuration << ", timeout was set to " << requestOnAny.timeout);
    }

    // TODO reform with SERVER-41459
    timer->waitUntil(deadline, baton)
        .getAsync([this, anchor = shared_from_this(), timeoutCode](Status status) {
            if (!status.isOK()) {
                return;
            }
            if (!finishLine.arriveStrongly()) {
                // If we didn't cross the command finishLine first, the promise is already fulfilled
                return;
            }

            const std::string message = str::stream()
                << "Request " << requestOnAny.id << " timed out"
                << ", deadline was " << deadline.toString() << ", op was "
                << redact(requestOnAny.toString());

            LOGV2_DEBUG(22595,
                        2,
                        "Request timed out",
                        "requestId"_attr = requestOnAny.id,
                        "deadline"_attr = deadline,
                        "request"_attr = requestOnAny);
            fulfillFinalPromise(Status(timeoutCode, message));
        });
}

void NetworkInterfaceTL::RequestState::returnConnection(Status status) noexcept {
    invariant(conn);

    auto connToReturn = std::exchange(conn, {});

    if (!status.isOK()) {
        connToReturn->indicateFailure(std::move(status));
        return;
    }

    connToReturn->indicateUsed();
    connToReturn->indicateSuccess();
}

void NetworkInterfaceTL::CommandStateBase::tryFinish(Status status) noexcept {
    invariant(finishLine.isReady());

    LOGV2_DEBUG(
        4646302, 2, "Finished request", "requestId"_attr = requestOnAny.id, "status"_attr = status);

    // The command has resolved one way or another.
    timer->cancel(baton);

    if (interface->_counters) {
        // Increment our counters for the integration test
        interface->_counters->recordResult(status);
    }

    {
        // We've finished, we're not in progress anymore
        stdx::lock_guard lk(interface->_inProgressMutex);
        interface->_inProgress.erase(cbHandle);
    }

    invariant(requestManager);
    if (operationKey &&
        !MONGO_unlikely(networkInterfaceShouldNotKillPendingRequests.shouldFail())) {
        // Kill operations for requests that we didn't use to fulfill the promise.
        requestManager->killOperationsForPendingRequests();
    }

    if (!status.isOK()) {
        // We cancel after we issue _killOperations because, if we cancel before, existing
        // RequestStates may finish and destruct to quickly.
        requestManager->cancelRequests();
    }

    networkInterfaceCommandsFailedWithErrorCode.shouldFail([&](const BSONObj& data) {
        const auto errorCode = data.getIntField("errorCode");
        if (errorCode != status.code()) {
            return false;
        }

        const std::string requestCmdName = requestOnAny.cmdObj.firstElement().fieldName();
        for (auto&& cmdName : data.getObjectField("cmdNames")) {
            if (cmdName.type() == String && cmdName.valueStringData() == requestCmdName) {
                return true;
            }
        }

        return false;
    });
}

void NetworkInterfaceTL::RequestState::cancel() noexcept {
    auto connToCancel = weakConn.lock();
    if (auto clientPtr = getClient(connToCancel)) {
        // If we have a client, cancel it
        clientPtr->cancel(cmdState->baton);
    }
}

NetworkInterfaceTL::RequestState::~RequestState() {
    invariant(!conn);
}

Status NetworkInterfaceTL::startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                        RemoteCommandRequestOnAny& request,
                                        RemoteCommandCompletionFn&& onFinish,
                                        const BatonHandle& baton) try {
    if (inShutdown()) {
        return kNetworkInterfaceShutdownInProgress;
    }

    LOGV2_DEBUG(
        22596, kDiagnosticLogLevel, "startCommand", "request"_attr = redact(request.toString()));

    auto status = appendMetadata(&request, _metadataHook);
    if (!status.isOK()) {
        return status;
    }

    bool targetHostsInAlphabeticalOrder =
        MONGO_unlikely(networkInterfaceSendRequestsToTargetHostsInAlphabeticalOrder.shouldFail(
            [request](const BSONObj&) { return request.options.isHedgeEnabled; }));

    if (targetHostsInAlphabeticalOrder) {
        // Sort the target hosts by host names.
        std::sort(request.target.begin(),
                  request.target.end(),
                  [](const HostAndPort& target1, const HostAndPort& target2) {
                      return target1.toString() < target2.toString();
                  });
    }

    if ((request.target.size() > 1) && !request.options.isHedgeEnabled &&
        !gOpportunisticSecondaryTargeting.load()) {
        request.target.resize(1);
    }

    auto [cmdState, future] = CommandState::make(this, request, cbHandle);
    if (cmdState->requestOnAny.timeout != cmdState->requestOnAny.kNoTimeout) {
        cmdState->deadline = cmdState->stopwatch.start() + cmdState->requestOnAny.timeout;
    }
    cmdState->baton = baton;

    if (_svcCtx && cmdState->requestOnAny.options.isHedgeEnabled) {
        auto hm = HedgingMetrics::get(_svcCtx);
        invariant(hm);
        hm->incrementNumTotalOperations();
    }

    // When our command finishes, run onFinish out of line.
    std::move(future)
        // Run the callback on the baton if it exists and is not shut down, and run on the reactor
        // otherwise.
        .thenRunOn(makeGuaranteedExecutor(baton, _reactor))
        .getAsync([cmdState = cmdState,
                   onFinish = std::move(onFinish)](StatusWith<RemoteCommandOnAnyResponse> swr) {
            invariant(swr.isOK(),
                      "Remote command response failed with an error: {}"_format(
                          swr.getStatus().toString()));
            auto rs = std::move(swr.getValue());
            // The TransportLayer has, for historical reasons returned
            // SocketException for network errors, but sharding assumes
            // HostUnreachable on network errors.
            if (rs.status == ErrorCodes::SocketException) {
                rs.status = Status(ErrorCodes::HostUnreachable, rs.status.reason());
            }

            LOGV2_DEBUG(22597,
                        2,
                        "Request finished with response",
                        "requestId"_attr = cmdState->requestOnAny.id,
                        "isOK"_attr = rs.isOK(),
                        "response"_attr =
                            redact(rs.isOK() ? rs.data.toString() : rs.status.toString()));
            catchingInvoke([&] { onFinish(std::move(rs)); },
                           [&](Status& err) { cmdState->fulfillFinalPromise(err); },
                           "The finish callback failed. Aborting exhaust command");
        });

    if (MONGO_unlikely(networkInterfaceDiscardCommandsBeforeAcquireConn.shouldFail())) {
        LOGV2(22598, "Discarding command due to failpoint before acquireConn");
        return Status::OK();
    }

    // Attempt to get a connection to every target host
    for (size_t idx = 0; idx < request.target.size(); ++idx) {
        auto connFuture =
            _pool->get(request.target[idx], request.sslMode, request.timeout, request.timeoutCode);

        // If connection future is ready or requests should be sent in order, send the request
        // immediately.
        if (connFuture.isReady() || targetHostsInAlphabeticalOrder) {
            cmdState->requestManager->trySend(std::move(connFuture).getNoThrow(), idx);
            continue;
        }

        // Otherwise, schedule the request.
        std::move(connFuture).thenRunOn(_reactor).getAsync([cmdState = cmdState, idx](auto swConn) {
            cmdState->requestManager->trySend(std::move(swConn), idx);
        });
    }

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

void NetworkInterfaceTL::testEgress(const HostAndPort& hostAndPort,
                                    transport::ConnectSSLMode sslMode,
                                    Milliseconds timeout,
                                    Status status) {
    auto handle = _pool->get(hostAndPort, sslMode, timeout).get();
    if (status.isOK()) {
        handle->indicateSuccess();
    } else {
        handle->indicateFailure(status);
    }
}

Future<RemoteCommandResponse> NetworkInterfaceTL::CommandState::sendRequest(
    std::shared_ptr<RequestState> requestState) {
    return makeReadyFutureWith([this, requestState] {
               setTimer();
               return RequestState::getClient(requestState->conn)
                   ->runCommandRequest(*requestState->request, baton);
           })
        .then([this, requestState](RemoteCommandResponse response) {
            catchingInvoke(
                [&] { doMetadataHook(RemoteCommandOnAnyResponse(requestState->host, response)); },
                [&](Status& err) { promise.setError(err); },
                "Metadata hook readReplyMetadata");
            return response;
        });
}

void NetworkInterfaceTL::CommandStateBase::doMetadataHook(
    const RemoteCommandOnAnyResponse& response) {
    if (auto& hook = interface->_metadataHook; hook && !finishLine.isReady()) {
        invariant(response.target);

        uassertStatusOK(
            hook->readReplyMetadata(nullptr, response.target->toString(), response.data));
    }
}

void NetworkInterfaceTL::CommandState::fulfillFinalPromise(
    StatusWith<RemoteCommandOnAnyResponse> response) {
    promise.setFrom(std::move(response));
}

NetworkInterfaceTL::RequestManager::RequestManager(CommandStateBase* cmdState_)
    : cmdState{cmdState_},
      requests(cmdState->maxConcurrentRequests(), std::weak_ptr<RequestState>()) {}

void NetworkInterfaceTL::RequestManager::cancelRequests() {
    {
        stdx::lock_guard<Latch> lk(mutex);
        isLocked = true;

        if (sentIdx == 0) {
            // We've canceled before any connections were acquired.
            return;
        }
    }

    for (size_t i = 0; i < requests.size(); i++) {
        // This may cause the connection to be discarded before it receives the response to an
        // earlier `_killOperations` command.
        if (auto requestState = requests[i].lock()) {
            LOGV2_DEBUG(4646301,
                        2,
                        "Cancelling request",
                        "requestId"_attr = cmdState->requestOnAny.id,
                        "index"_attr = i);
            requestState->cancel();
        }
    }
}

void NetworkInterfaceTL::RequestManager::killOperationsForPendingRequests() {
    {
        stdx::lock_guard<Latch> lk(mutex);
        isLocked = true;

        if (sentIdx == 0) {
            // We've canceled before any connections were acquired.
            return;
        }
    }

    for (size_t i = 0; i < requests.size(); i++) {
        auto requestState = requests[i].lock();
        if (!requestState || requestState->fulfilledPromise) {
            continue;
        }

        auto conn = requestState->weakConn.lock();
        if (!conn) {
            // If there is nothing from weakConn, the networking has already finished.
            continue;
        }

        // If the request was sent, send a remote command request to the target host
        // to kill the operation started by the request.

        LOGV2_DEBUG(4664801,
                    2,
                    "Sending remote _killOperations request to cancel command",
                    "operationKey"_attr = cmdState->operationKey,
                    "target"_attr = requestState->request->target,
                    "requestId"_attr = requestState->request->id);

        auto status = requestState->interface()->_killOperation(requestState);
        if (!status.isOK()) {
            LOGV2_DEBUG(4664810, 2, "Failed to send remote _killOperations", "error"_attr = status);
        }
    }
}

void NetworkInterfaceTL::RequestManager::trySend(
    StatusWith<ConnectionPool::ConnectionHandle> swConn, size_t idx) noexcept {
    // Our connection wasn't any good
    if (!swConn.isOK()) {
        {
            stdx::lock_guard<Latch> lk(mutex);

            auto currentConnsResolved = ++connsResolved;
            if (currentConnsResolved < cmdState->maxPossibleConns()) {
                // If we still have connections outstanding, we don't need to fail the promise.
                return;
            }

            if (sentIdx > 0) {
                // If a request has been sent, we shouldn't fail the promise.
                return;
            }

            if (isLocked) {
                // If we've finished, obviously we don't need to fail the promise.
                return;
            }
        }

        // We're the last one, set the promise if it hasn't already been set via cancel or timeout
        if (cmdState->finishLine.arriveStrongly()) {
            auto& reactor = cmdState->interface->_reactor;
            if (reactor->onReactorThread()) {
                cmdState->fulfillFinalPromise(std::move(swConn.getStatus()));
            } else {
                ExecutorFuture<void>(reactor, swConn.getStatus())
                    .getAsync([this, anchor = cmdState->shared_from_this()](Status status) {
                        cmdState->fulfillFinalPromise(std::move(status));
                    });
            }
        }
        return;
    }

    std::shared_ptr<RequestState> requestState;

    {
        stdx::lock_guard<Latch> lk(mutex);

        // Increment the number of conns we were able to resolve.
        ++connsResolved;

        auto haveSentAll = sentIdx >= cmdState->maxConcurrentRequests();
        if (haveSentAll || isLocked) {
            // Our command has already been satisfied or we have already sent out all
            // the requests.
            swConn.getValue()->indicateSuccess();
            return;
        }

        auto currentSentIdx = sentIdx++;

        requestState = std::make_shared<RequestState>(this, cmdState->shared_from_this(), idx);
        requestState->isHedge = currentSentIdx > 0;

        // Set conn/weakConn+request under the lock so they will always be observed during cancel.
        requestState->conn = std::move(swConn.getValue());
        requestState->weakConn = requestState->conn;

        requestState->request = RemoteCommandRequest(cmdState->requestOnAny, idx);
        requestState->host = requestState->request->target;

        requests.at(currentSentIdx) = requestState;
    }

    LOGV2_DEBUG(4646300,
                2,
                "Sending request",
                "requestId"_attr = cmdState->requestOnAny.id,
                "target"_attr = cmdState->requestOnAny.target[idx]);

    auto request = &requestState->request.get();

    if (requestState->isHedge) {
        invariant(request->options.isHedgeEnabled);
        invariant(WireSpec::instance().get()->isInternalClient);

        auto hedgingMaxTimeMS = Milliseconds(request->options.maxTimeMSForHedgedReads);
        if (request->timeout == RemoteCommandRequest::kNoTimeout ||
            hedgingMaxTimeMS < request->timeout) {
            LOGV2_DEBUG(4647200,
                        2,
                        "Set maxTimeMSOpOnly for hedged request",
                        "originalMaxTime"_attr = request->timeout,
                        "reducedMaxTime"_attr = hedgingMaxTimeMS,
                        "requestId"_attr = cmdState->requestOnAny.id,
                        "target"_attr = cmdState->requestOnAny.target[idx]);
            request->timeout = hedgingMaxTimeMS;
        }

        if (cmdState->interface->_svcCtx) {
            auto hm = HedgingMetrics::get(cmdState->interface->_svcCtx);
            invariant(hm);
            hm->incrementNumTotalHedgedOperations();
        }
    }

    if (request->timeout != RemoteCommandRequest::kNoTimeout &&
        WireSpec::instance().get()->isInternalClient) {
        LOGV2_DEBUG(4924402,
                    2,
                    "Set maxTimeMSOpOnly for request",
                    "maxTimeMSOpOnly"_attr = request->timeout,
                    "requestId"_attr = cmdState->requestOnAny.id,
                    "target"_attr = cmdState->requestOnAny.target[idx]);

        BSONObjBuilder updatedCmdBuilder;
        updatedCmdBuilder.appendElements(request->cmdObj);
        updatedCmdBuilder.append(kMaxTimeMSOpOnlyField, request->timeout.count());
        request->cmdObj = updatedCmdBuilder.obj();
    }

    networkInterfaceHangCommandsAfterAcquireConn.pauseWhileSet();

    // We have a connection and the command hasn't already been attempted
    LOGV2_DEBUG(4630601,
                2,
                "Request acquired a connection",
                "requestId"_attr = requestState->request->id,
                "target"_attr = requestState->request->target);

    // An attempt to avoid sending a request after its command has been canceled or already executed
    // using another connection. Just a best effort to mitigate unnecessary resource consumption if
    // possible, and allow deterministic cancellation of requests in testing.
    if (cmdState->finishLine.isReady()) {
        LOGV2_DEBUG(5813901,
                    2,
                    "Skipping request as it has already been fulfilled or canceled",
                    "requestId"_attr = requestState->request->id,
                    "target"_attr = requestState->request->target);
        requestState->returnConnection(Status::OK());
        return;
    }

    if (auto counters = cmdState->interface->_counters) {
        counters->recordSent();
    }

    requestState->resolve(cmdState->sendRequest(requestState));
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
                return RemoteCommandOnAnyResponse(host, std::move(error), stopwatch.elapsed());
            });

    std::move(anyFuture)                                    //
        .thenRunOn(makeGuaranteedExecutor(baton, reactor))  // Switch to the baton/reactor.
        .getAsync([ this, anchor = shared_from_this() ](auto swr) noexcept {
            auto response = uassertStatusOK(swr);
            auto status = response.status;

            returnConnection(status);

            const auto commandStatus = getStatusFromCommandResult(response.data);
            if (isHedge) {
                // Ignore maxTimeMS expiration or any sharding "retargeting needed" error category
                // for hedged reads without triggering the finish line.
                if (commandStatus == ErrorCodes::MaxTimeMSExpired ||
                    commandStatus == ErrorCodes::StaleDbVersion ||
                    ErrorCodes::isStaleShardVersionError(commandStatus)) {
                    LOGV2_DEBUG(4660701,
                                2,
                                "Hedged request returned status",
                                "requestId"_attr = request->id,
                                "target"_attr = request->target,
                                "status"_attr = commandStatus);
                    return;
                }
            }

            if (!cmdState->finishLine.arriveStrongly()) {
                LOGV2_DEBUG(4754301,
                            2,
                            "Skipping the response because it was already received from other node",
                            "requestId"_attr = request->id,
                            "target"_attr = request->target,
                            "status"_attr = commandStatus);

                return;
            }

            if (isHedge) {
                auto hm = HedgingMetrics::get(cmdState->interface->_svcCtx);
                invariant(hm);
                hm->incrementNumAdvantageouslyHedgedOperations();
            }
            fulfilledPromise = true;
            cmdState->fulfillFinalPromise(std::move(response));
        });
}

NetworkInterfaceTL::ExhaustCommandState::ExhaustCommandState(
    NetworkInterfaceTL* interface_,
    RemoteCommandRequestOnAny request_,
    const TaskExecutor::CallbackHandle& cbHandle_,
    RemoteCommandOnReplyFn&& onReply_)
    : CommandStateBase(interface_, std::move(request_), cbHandle_),
      onReplyFn(std::move(onReply_)) {}

auto NetworkInterfaceTL::ExhaustCommandState::make(NetworkInterfaceTL* interface,
                                                   RemoteCommandRequestOnAny request,
                                                   const TaskExecutor::CallbackHandle& cbHandle,
                                                   RemoteCommandOnReplyFn&& onReply) {
    auto state = std::make_shared<ExhaustCommandState>(
        interface, std::move(request), cbHandle, std::move(onReply));
    auto [promise, future] = makePromiseFuture<void>();
    state->promise = std::move(promise);
    std::move(future)
        .onError([state](Status error) {
            state->onReplyFn(RemoteCommandOnAnyResponse(
                boost::none, std::move(error), state->stopwatch.elapsed()));
        })
        .getAsync([state](Status status) {
            state->tryFinish(
                Status{ErrorCodes::ExhaustCommandFinished, "Exhaust command finished"});
        });

    state->requestManager = std::make_unique<RequestManager>(state.get());

    {
        stdx::lock_guard lk(interface->_inProgressMutex);
        if (interface->inShutdown()) {
            // If we're in shutdown, we can't add a new command.
            uassertStatusOK(kNetworkInterfaceShutdownInProgress);
        }
        interface->_inProgress.insert({cbHandle, state});
    }

    return state;
}

Future<RemoteCommandResponse> NetworkInterfaceTL::ExhaustCommandState::sendRequest(
    std::shared_ptr<RequestState> requestState) try {
    auto [promise, future] = makePromiseFuture<RemoteCommandResponse>();
    finalResponsePromise = std::move(promise);

    setTimer();
    requestState->getClient(requestState->conn)
        ->beginExhaustCommandRequest(*requestState->request, baton)
        .thenRunOn(requestState->interface()->_reactor)
        .getAsync([this, requestState](StatusWith<RemoteCommandResponse> swResponse) mutable {
            continueExhaustRequest(std::move(requestState), swResponse);
        });
    return std::move(future).then([this](const auto& finalResponse) { return finalResponse; });
} catch (const DBException& ex) {
    return ex.toStatus();
}

void NetworkInterfaceTL::ExhaustCommandState::fulfillFinalPromise(
    StatusWith<RemoteCommandOnAnyResponse> swr) {
    promise.setFrom([&] {
        if (!swr.isOK())
            return swr.getStatus();
        auto response = swr.getValue();
        if (!response.isOK())
            return response.status;
        return getStatusFromCommandResult(response.data);
    }());
}

void NetworkInterfaceTL::ExhaustCommandState::continueExhaustRequest(
    std::shared_ptr<RequestState> requestState, StatusWith<RemoteCommandResponse> swResponse) {
    RemoteCommandResponse response;
    if (!swResponse.isOK()) {
        response = RemoteCommandResponse(std::move(swResponse.getStatus()));
    } else {
        response = std::move(swResponse.getValue());
    }

    if (requestState->interface()->inShutdown() ||
        ErrorCodes::isCancellationError(response.status)) {
        finalResponsePromise.emplaceValue(response);
        return;
    }

    auto onAnyResponse = RemoteCommandOnAnyResponse(requestState->host, response);
    if (!catchingInvoke([&] { doMetadataHook(onAnyResponse); },
                        [&](Status& err) { finalResponsePromise.setError(err); },
                        "Exhaust command metadata hook readReplyMetadata"))
        return;

    // If the command failed, we will call 'onReply' as a part of the future chain paired with
    // the promise. This is to be sure that all error paths will run 'onReply' only once upon
    // future completion.
    if (!response.status.isOK() || !getStatusFromCommandResult(response.data).isOK()) {
        // The moreToCome bit should *not* be set if the command failed
        invariant(!response.moreToCome);

        finalResponsePromise.emplaceValue(response);
        return;
    }

    if (!catchingInvoke([&] { onReplyFn(onAnyResponse); },
                        [&](Status& err) { finalResponsePromise.setError(err); },
                        "Exhaust command onReplyFn"))
        return;

    // Reset the stopwatch to measure the correct duration for the following reply
    stopwatch.restart();
    if (deadline != kNoExpirationDate) {
        deadline = stopwatch.start() + requestOnAny.timeout;
    }
    if (!catchingInvoke([&] { setTimer(); },
                        [&](Status& err) { finalResponsePromise.setError(err); },
                        "Exhaust command setTimer"))
        return;

    requestState->getClient(requestState->conn)
        ->awaitExhaustCommand(baton)
        .thenRunOn(requestState->interface()->_reactor)
        .getAsync([this, requestState](StatusWith<RemoteCommandResponse> swResponse) mutable {
            continueExhaustRequest(std::move(requestState), swResponse);
        });
}

Status NetworkInterfaceTL::startExhaustCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                               RemoteCommandRequestOnAny& request,
                                               RemoteCommandOnReplyFn&& onReply,
                                               const BatonHandle& baton) try {
    if (inShutdown()) {
        return {ErrorCodes::ShutdownInProgress, "NetworkInterface shutdown in progress"};
    }

    LOGV2_DEBUG(
        23909, kDiagnosticLogLevel, "startCommand", "request"_attr = redact(request.toString()));

    auto status = appendMetadata(&request, _metadataHook);
    if (!status.isOK()) {
        return status;
    }

    auto cmdState = ExhaustCommandState::make(this, request, cbHandle, std::move(onReply));
    if (cmdState->requestOnAny.timeout != cmdState->requestOnAny.kNoTimeout) {
        cmdState->deadline = cmdState->stopwatch.start() + cmdState->requestOnAny.timeout;
    }
    cmdState->baton = baton;
    cmdState->requestManager = std::make_unique<RequestManager>(cmdState.get());

    // Attempt to get a connection to every target host
    for (size_t idx = 0; idx < request.target.size(); ++idx) {
        auto connFuture = _pool->get(request.target[idx], request.sslMode, request.timeout);

        if (connFuture.isReady()) {
            cmdState->requestManager->trySend(std::move(connFuture).getNoThrow(), idx);
            continue;
        }

        // For every connection future we didn't have immediately ready, schedule
        std::move(connFuture).thenRunOn(_reactor).getAsync([cmdState, idx](auto swConn) {
            cmdState->requestManager->trySend(std::move(swConn), idx);
        });
    }

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

void NetworkInterfaceTL::cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                       const BatonHandle&) {
    stdx::unique_lock<Latch> lk(_inProgressMutex);
    auto it = _inProgress.find(cbHandle);
    if (it == _inProgress.end()) {
        return;
    }
    auto cmdStateToCancel = it->second.lock();
    if (!cmdStateToCancel) {
        return;
    }

    _inProgress.erase(it);
    lk.unlock();

    if (!cmdStateToCancel->finishLine.arriveStrongly()) {
        // If we didn't cross the command finishLine first, the promise is already fulfilled
        return;
    }

    LOGV2_DEBUG(22599,
                2,
                "Canceling operation for request",
                "request"_attr = redact(cmdStateToCancel->requestOnAny.toString()));
    cmdStateToCancel->fulfillFinalPromise(
        {ErrorCodes::CallbackCanceled,
         str::stream() << "Command canceled; original request was: "
                       << redact(cmdStateToCancel->requestOnAny.toString())});
}

Status NetworkInterfaceTL::_killOperation(std::shared_ptr<RequestState> requestStateToKill) try {
    auto [target, sslMode] = [&] {
        invariant(requestStateToKill->request);
        auto request = requestStateToKill->request.get();
        return std::make_pair(request.target, request.sslMode);
    }();
    auto cmdStateToKill = requestStateToKill->cmdState;
    auto operationKey = cmdStateToKill->operationKey.get();

    // Make a request state for _killOperations.
    executor::RemoteCommandRequest killOpRequest(
        target,
        "admin",
        BSON("_killOperations" << 1 << "operationKeys" << BSON_ARRAY(operationKey)),
        nullptr,
        kCancelCommandTimeout);

    auto cbHandle = executor::TaskExecutor::CallbackHandle();
    auto [killOpCmdState, future] = CommandState::make(this, killOpRequest, cbHandle);
    killOpCmdState->deadline = killOpCmdState->stopwatch.start() + killOpRequest.timeout;

    std::move(future).getAsync(
        [this, operationKey, killOpRequest](StatusWith<RemoteCommandOnAnyResponse> swr) {
            invariant(swr.isOK());
            auto rs = std::move(swr.getValue());
            LOGV2_DEBUG(51813,
                        2,
                        "Remote _killOperations request to cancel command finished with response",
                        "operationKey"_attr = operationKey,
                        "target"_attr = killOpRequest.target,
                        "response"_attr =
                            redact(rs.isOK() ? rs.data.toString() : rs.status.toString()));
        });

    // Send the _killOperations request.
    auto connFuture = _pool->get(target, sslMode, killOpRequest.kNoTimeout);
    std::move(connFuture)
        .thenRunOn(_reactor)
        .getAsync([this, killOpCmdState = killOpCmdState](auto swConn) {
            killOpCmdState->requestManager->trySend(std::move(swConn), 0);
        });
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

Status NetworkInterfaceTL::schedule(unique_function<void(Status)> action) {
    if (inShutdown()) {
        return kNetworkInterfaceShutdownInProgress;
    }

    _reactor->schedule([action = std::move(action)](auto status) { action(status); });
    return Status::OK();
}

Status NetworkInterfaceTL::setAlarm(const TaskExecutor::CallbackHandle& cbHandle,
                                    Date_t when,
                                    unique_function<void(Status)> action) {
    if (inShutdown()) {
        // Pessimistically check if we're in shutdown and save some work
        return kNetworkInterfaceShutdownInProgress;
    }

    if (when <= now()) {
        _reactor->schedule([action = std::move(action)](auto status) { action(status); });
        return Status::OK();
    }

    auto pf = makePromiseFuture<void>();
    std::move(pf.future).getAsync(std::move(action));

    auto alarmState =
        std::make_shared<AlarmState>(when, cbHandle, _reactor->makeTimer(), std::move(pf.promise));

    auto weakAlarmState = std::weak_ptr<AlarmState>(alarmState);

    {
        stdx::lock_guard<Latch> lk(_inProgressMutex);

        if (_inProgressAlarmsInShutdown) {
            // Check that we've won any possible race with _shutdownAllAlarms();
            return kNetworkInterfaceShutdownInProgress;
        }

        // If a user has already scheduled an alarm with a handle, make sure they intentionally
        // override it by canceling and setting a new one.
        auto&& [_, wasInserted] = _inProgressAlarms.emplace(cbHandle, alarmState);
        invariant(wasInserted);
    }

    alarmState->timer->waitUntil(alarmState->when, nullptr)
        .getAsync([this, weakAlarmState](Status status) mutable {
            auto state = weakAlarmState.lock();
            if (!state) {
                LOGV2_DEBUG(4511701, 4, "AlarmState destroyed before timer callback finished");
                return;
            }

            _answerAlarm(status, std::move(state));
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

    if (alarmState->done.swap(true)) {
        return;
    }

    alarmState->timer->cancel();
    alarmState->promise.setError(Status(ErrorCodes::CallbackCanceled, "Alarm cancelled"));
}

void NetworkInterfaceTL::_shutdownAllAlarms() {
    auto alarms = [&] {
        stdx::unique_lock<Latch> lk(_inProgressMutex);

        // Prevent any more alarms from registering
        _inProgressAlarmsInShutdown = true;

        return std::exchange(_inProgressAlarms, {});
    }();

    for (auto&& [cbHandle, state] : alarms) {
        if (state->done.swap(true)) {
            continue;
        }

        state->timer->cancel();
        state->promise.setError(Status(ErrorCodes::CallbackCanceled, "Alarm cancelled"));
    }
}

void NetworkInterfaceTL::_answerAlarm(Status status, std::shared_ptr<AlarmState> state) {
    // Since the lock is released before canceling the timer, this thread can win the race with
    // cancelAlarm(). Thus if status is CallbackCanceled, then this alarm is already removed from
    // _inProgressAlarms.
    if (ErrorCodes::isCancellationError(status)) {
        return;
    }

    if (inShutdown()) {
        // No alarms get processed in shutdown
        return;
    }

    // transport::Reactor timers do not involve spurious wake ups, however, this check is nearly
    // free and allows us to be resilient to a world where timers impls do have spurious wake ups.
    auto currentTime = now();
    if (status.isOK() && currentTime < state->when) {
        LOGV2_DEBUG(22600,
                    2,
                    "Alarm returned early",
                    "expectedTime"_attr = state->when,
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

    if (state->done.swap(true)) {
        return;
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
