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
#include "mongo/executor/hedging_metrics.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {
namespace executor {

namespace {
static inline const std::string kMaxTimeMSOptionName = "maxTimeMS";
}  // unnamed namespace

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

    _shutdownAllAlarms();

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

NetworkInterfaceTL::CommandStateBase::CommandStateBase(
    NetworkInterfaceTL* interface_,
    RemoteCommandRequestOnAny request_,
    const TaskExecutor::CallbackHandle& cbHandle_)
    : interface(interface_),
      requestOnAny(std::move(request_)),
      cbHandle(cbHandle_),
      finishLine(maxRequestFailures()),
      operationKey(request_.operationKey) {}

NetworkInterfaceTL::CommandState::CommandState(NetworkInterfaceTL* interface_,
                                               RemoteCommandRequestOnAny request_,
                                               const TaskExecutor::CallbackHandle& cbHandle_)
    : CommandStateBase(interface_, std::move(request_), cbHandle_),
      hedgeCount(requestOnAny.hedgeOptions ? requestOnAny.hedgeOptions->count + 1 : 1) {}

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

void NetworkInterfaceTL::CommandStateBase::setTimer() {
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
        fulfillFinalPromise(Status(ErrorCodes::NetworkInterfaceExceededTimeLimit, message));
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

void NetworkInterfaceTL::CommandStateBase::tryFinish(Status status) noexcept {
    invariant(finishLine.isReady());

    LOGV2_DEBUG(4646302, 2, "Finished request {request_id}", "request_id"_attr = requestOnAny.id);

    if (timer) {
        // The command has resolved one way or another,
        timer->cancel(baton);
    }

    if (!status.isOK()) {  // TODO: SERVER-46469: || (requestManager && requestManager->isHedging))
        if (requestManager) {
            requestManager->cancelRequests();
        }
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
    invariant(requestManager);
    {
        stdx::lock_guard<Latch> lk(requestManager->mutex);
        requestManager->isLocked = true;
        if (requestManager->sentNone()) {
            // We've canceled before any connections were acquired, we're all good.
            return;
        }
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
        return kNetworkInterfaceShutdownInProgress;
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
     * 3.  The timer in sendRequest() fires and the operation times out. ASIO timers run on the
     *     reactor.
     * 4.  AsyncDBClient::runCommandRequest() concludes. This path is sadly indeterminate since
     *     early failure can still be inline. The future chain is thenRunOn() either the baton or
     *     the reactor.
     *
     * The important bits to remember here:
     * - onFinish() is out-of-line
     * - Stay inline as long as feasible until sendRequest()---i.e. until network operations
     * - Baton execution *cannot* be relied upon at least until sendRequest()
     * - Connection failure and command failure are related but distinct
     */

    // When our command finishes, run onFinish
    std::move(future).getAsync([this, cmdState = cmdState, onFinish = std::move(onFinish)](
                                   StatusWith<RemoteCommandOnAnyResponse> swr) {
        invariant(swr.isOK());
        auto rs = std::move(swr.getValue());
        // The TransportLayer has, for historical reasons returned
        // SocketException for network errors, but sharding assumes
        // HostUnreachable on network errors.
        if (rs.status == ErrorCodes::SocketException) {
            rs.status = Status(ErrorCodes::HostUnreachable, rs.status.reason());
        }

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

    cmdState->requestManager = std::make_unique<RequestManager>(cmdState->hedgeCount, cmdState);

    std::vector<std::shared_ptr<NetworkInterfaceTL::RequestState>> requestStates;

    for (size_t i = 0; i < cmdState->hedgeCount; i++) {
        requestStates.emplace_back(
            cmdState->requestManager->makeRequest(cmdState->requestManager.get()));
    }

    invariant(cmdState->requestManager);
    RequestManager* rm = cmdState->requestManager.get();

    // Attempt to get a connection to every target host
    for (size_t idx = 0; idx < request.target.size() && !rm->usedAllConn(); ++idx) {
        auto connFuture = _pool->get(request.target[idx], request.sslMode, request.timeout);

        if (connFuture.isReady()) {
            rm->trySend(std::move(connFuture).getNoThrow(), idx);
            continue;
        }

        // For every connection future we didn't have immediately ready, schedule
        std::move(connFuture).thenRunOn(_reactor).getAsync([requestStates, rm, idx](auto swConn) {
            rm->trySend(std::move(swConn), idx);
        });
    }

    return Status::OK();
}

Future<RemoteCommandResponse> NetworkInterfaceTL::CommandState::sendRequest(size_t reqId) {
    auto requestState = requestManager->getRequest(reqId);
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
    promise.setFromStatusWith(std::move(response));
}

std::shared_ptr<NetworkInterfaceTL::RequestState> NetworkInterfaceTL::RequestManager::getRequest(
    size_t reqId) {
    invariant(requestCnt.load() > reqId);
    return requests[reqId].lock();
}

std::shared_ptr<NetworkInterfaceTL::RequestState>
NetworkInterfaceTL::RequestManager::getNextRequest() {
    stdx::lock_guard<Latch> lk(mutex);
    if (sentIdx.load() < requests.size()) {
        auto req = requests[sentIdx.fetchAndAdd(1)].lock();
        if (sentIdx.load() > 1) {
            req->isHedge = true;
        }
        return req;
    } else {
        return nullptr;
    }
}

void NetworkInterfaceTL::RequestManager::cancelRequests() {
    for (size_t i = 0; i < requests.size(); i++) {
        auto requestState = requests[i].lock();
        if (requestState) {
            LOGV2_DEBUG(4646301,
                        2,
                        "Cancelling request {request_id} with index {idx}",
                        "request_id"_attr = cmdState.lock()->requestOnAny.id,
                        "idx"_attr = i);
            requestState->cancel();
        }
    }
}

void NetworkInterfaceTL::RequestManager::trySend(
    StatusWith<ConnectionPool::ConnectionHandle> swConn, size_t idx) noexcept {
    auto cmdStatePtr = cmdState.lock();
    invariant(cmdStatePtr);
    // Our connection wasn't any good
    if (!swConn.isOK()) {
        connStatus[idx] = ConnStatus::Failed;
        if (!usedAllConn()) {
            return;
        }

        // We're the last one, set the promise if it hasn't already been set via cancel or timeout
        if (cmdStatePtr->finishLine.arriveStrongly()) {
            auto& reactor = cmdStatePtr->interface->_reactor;
            if (reactor->onReactorThread()) {
                cmdStatePtr->fulfillFinalPromise(swConn.getStatus());
            } else {
                ExecutorFuture<void>(reactor, swConn.getStatus())
                    .getAsync([this, cmdStatePtr](Status status) {
                        cmdStatePtr->fulfillFinalPromise(std::move(status));
                    });
            }
        }
        return;
    }

    connStatus[idx] = ConnStatus::OK;

    // Our command has already been satisfied
    {
        stdx::lock_guard<Latch> lk(mutex);
        if (cmdStatePtr->finishLine.isReady() || sentAll() || isLocked) {
            swConn.getValue()->indicateSuccess();
            return;
        }
    }

    LOGV2_DEBUG(4646300,
                2,
                "Sending request {request_id} with index {idx}",
                "request_id"_attr = cmdStatePtr->requestOnAny.id,
                "idx"_attr = idx);

    auto req = getNextRequest();
    if (req) {
        RemoteCommandRequest remoteReq({cmdStatePtr->requestOnAny, idx});
        if (remoteReq.hedgeOptions) {
            req->hasHedgeOptions = true;
        }
        if (req->isHedge) {  // this is a hedged read
            invariant(remoteReq.hedgeOptions);
            auto maxTimeMS = remoteReq.hedgeOptions->maxTimeMSForHedgedReads;
            if (remoteReq.timeout == remoteReq.kNoTimeout ||
                remoteReq.timeout > Milliseconds(maxTimeMS)) {
                BSONObjBuilder updatedCmdBuilder;
                for (const auto& elem : remoteReq.cmdObj) {
                    if (elem.fieldNameStringData() != kMaxTimeMSOptionName) {
                        updatedCmdBuilder.append(elem);
                    }
                }
                updatedCmdBuilder.append(kMaxTimeMSOptionName, maxTimeMS);

                remoteReq.cmdObj = updatedCmdBuilder.obj();
                LOGV2_DEBUG(
                    4647200,
                    2,
                    "Set  MaxTimeMS to {maxTimeMS} for request {request_id} with index {idx}",
                    "maxTimeMS"_attr = maxTimeMS,
                    "request_id"_attr = cmdStatePtr->requestOnAny.id,
                    "idx"_attr = idx);
            }
        }
        if (cmdStatePtr->interface->_svcCtx && remoteReq.hedgeOptions) {
            auto hm = HedgingMetrics::get(cmdStatePtr->interface->_svcCtx);
            invariant(hm);
            hm->incrementNumTotalOperations();
            if (req->isHedge) {
                hm->incrementNumTotalHedgedOperations();
            }
        }
        req->send(std::move(swConn), remoteReq);
    }
}

void NetworkInterfaceTL::RequestState::trySend(StatusWith<ConnectionPool::ConnectionHandle> swConn,
                                               size_t idx) noexcept {
    invariant(requestManager);
    requestManager->trySend(std::move(swConn), idx);
}

void NetworkInterfaceTL::RequestState::send(StatusWith<ConnectionPool::ConnectionHandle> swConn,
                                            RemoteCommandRequest remoteCommandRequest) noexcept {

    // We have a connection and the command hasn't already been attempted
    request.emplace(remoteCommandRequest);
    host = request.get().target;
    conn = std::move(swConn.getValue());

    networkInterfaceHangCommandsAfterAcquireConn.pauseWhileSet();

    if (MONGO_unlikely(networkInterfaceAfterAcquireConn.shouldFail())) {
        LOGV2(4630601,
              "Request {request_id} acquired a connection",
              "request_id"_attr = request.get().id);
    }

    if (auto counters = interface()->_counters) {
        counters->recordSent();
    }

    resolve(cmdState->sendRequest(reqId));
}

void NetworkInterfaceTL::RequestState::resolve(Future<RemoteCommandResponse> future) noexcept {
    auto& reactor = interface()->_reactor;
    auto& baton = cmdState->baton;

    isSent = true;

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

    if (baton) {
        // If we have a baton then use it for the promise and then switch to the reactor to return
        // our connection.
        std::move(anyFuture)
            .thenRunOn(baton)
            .onCompletion([ this, anchor = shared_from_this() ](auto swr) noexcept {
                auto response = uassertStatusOK(swr);
                auto status = swr.getValue().status;
                auto commandStatus = getStatusFromCommandResult(response.data);
                // ignore MaxTimeMS expiration errors for  hedged reads
                if (hasHedgeOptions && isHedge && commandStatus == ErrorCodes::MaxTimeMSExpired) {
                    LOGV2_DEBUG(4660700,
                                2,
                                "Hedged request {request_id} returned status {status}",
                                "request_id"_attr = request.get().id,
                                "status"_attr = commandStatus);
                } else {
                    if (cmdState->finishLine.arriveStrongly()) {
                        if (hasHedgeOptions && isHedge) {
                            auto hm = HedgingMetrics::get(cmdState->interface->_svcCtx);
                            invariant(hm);
                            hm->incrementNumAdvantageouslyHedgedOperations();
                        }
                        cmdState->fulfillFinalPromise(std::move(response));
                    }
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
                auto commandStatus = getStatusFromCommandResult(response.data);
                ON_BLOCK_EXIT([&] { returnConnection(status); });
                if (!cmdState->finishLine.arriveStrongly()) {
                    return;
                }
                // ignore MaxTimeMS expiration errors for  hedged reads
                if (hasHedgeOptions && isHedge && commandStatus == ErrorCodes::MaxTimeMSExpired) {
                    LOGV2_DEBUG(4660701,
                                2,
                                "Hedged request {request_id} returned status {status}",
                                "request_id"_attr = request.get().id,
                                "status"_attr = commandStatus);
                } else {
                    if (hasHedgeOptions && isHedge) {
                        auto hm = HedgingMetrics::get(cmdState->interface->_svcCtx);
                        invariant(hm);
                        hm->incrementNumAdvantageouslyHedgedOperations();
                    }
                    cmdState->fulfillFinalPromise(std::move(response));
                }
            });
    }
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
            stdx::lock_guard lk(state->_onReplyMutex);
            state->onReplyFn(RemoteCommandOnAnyResponse(
                                 boost::none, std::move(error), state->stopwatch.elapsed()),
                             false);
        })
        .getAsync([state](Status status) { state->tryFinish(status); });

    {
        stdx::lock_guard lk(interface->_inProgressMutex);
        interface->_inProgress.insert({cbHandle, state});
    }

    return state;
}

Future<RemoteCommandResponse> NetworkInterfaceTL::ExhaustCommandState::sendRequest(size_t reqId) {
    auto requestState = requestManager->getRequest(reqId);
    invariant(requestState);

    auto clientCallback = [this, requestState](const RemoteCommandResponse& response,
                                               bool isMoreToComeSet) {
        // Stash this response on the command state to be used to fulfill the promise.
        prevResponse = response;
        auto onAnyResponse = RemoteCommandOnAnyResponse(requestState->host, response);
        doMetadataHook(onAnyResponse);

        // If the command failed, we will call 'onReply' as a part of the future chain paired with
        // the promise. This is to be sure that all error paths will run 'onReply' only once upon
        // future completion.
        if (!getStatusFromCommandResult(response.data).isOK()) {
            // The moreToCome bit should *not* be set if the command failed
            invariant(!isMoreToComeSet);
            return;
        }

        // Reset the stopwatch to measure the correct duration for the folowing reply
        stopwatch.restart();
        setTimer();

        stdx::lock_guard lk(_onReplyMutex);
        onReplyFn(onAnyResponse, isMoreToComeSet);
    };

    return makeReadyFutureWith(
               [this, requestState, clientCallback = std::move(clientCallback)]() mutable {
                   setTimer();
                   return requestState->client()->runExhaustCommandRequest(
                       *requestState->request, std::move(clientCallback), baton);
               })
        .then([this, requestState] { return prevResponse; });
}

void NetworkInterfaceTL::ExhaustCommandState::fulfillFinalPromise(
    StatusWith<RemoteCommandOnAnyResponse> response) {
    auto status = !response.getStatus().isOK()
        ? response.getStatus()
        : getStatusFromCommandResult(response.getValue().data);

    if (!status.isOK()) {
        promise.setError(status);
        return;
    }

    promise.emplaceValue();
}

Status NetworkInterfaceTL::startExhaustCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                               RemoteCommandRequestOnAny& request,
                                               RemoteCommandOnReplyFn&& onReply,
                                               const BatonHandle& baton) {
    if (inShutdown()) {
        return {ErrorCodes::ShutdownInProgress, "NetworkInterface shutdown in progress"};
    }

    LOGV2_DEBUG(23909,
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

    auto cmdState = ExhaustCommandState::make(this, request, cbHandle, std::move(onReply));
    if (cmdState->requestOnAny.timeout != cmdState->requestOnAny.kNoTimeout) {
        cmdState->deadline = cmdState->stopwatch.start() + cmdState->requestOnAny.timeout;
    }
    cmdState->baton = baton;

    cmdState->requestManager = std::make_unique<RequestManager>(1, cmdState);
    auto requestState = cmdState->requestManager->makeRequest(cmdState->requestManager.get());

    // Attempt to get a connection to every target host
    for (size_t idx = 0;
         idx < request.target.size() && !requestState->requestManager->usedAllConn();
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

    if (!cmdStateToCancel->requestManager) {
        // The command was canceled before it has started.
        cmdStateToCancel->fulfillFinalPromise(
            {ErrorCodes::CallbackCanceled,
             str::stream() << "Command canceled; original request was: "
                           << redact(cmdStateToCancel->requestOnAny.toString())});
        return;
    }

    for (size_t i = 0; i < cmdStateToCancel->requestManager->requests.size(); i++) {
        auto requestStateToCancel = cmdStateToCancel->requestManager->requests[i].lock();

        if (!requestStateToCancel) {
            continue;
        }

        stdx::unique_lock<Latch> lk(requestStateToCancel->requestManager->mutex);
        bool hasAcquiredConn = !requestStateToCancel->requestManager->sentNone();
        requestStateToCancel->requestManager->isLocked = true;
        lk.unlock();

        // Only kill the command if it has an operation key and was attempted.
        bool shouldKillOp =
            cmdStateToCancel->operationKey && hasAcquiredConn && requestStateToCancel->request;

        if (!shouldKillOp) {
            // Satisfy the promise locally immediately.
            LOGV2_DEBUG(
                22599,
                2,
                "Canceling operation; original request was: {cmdStateToCancel_requestOnAny}",
                "cmdStateToCancel_requestOnAny"_attr =
                    redact(cmdStateToCancel->requestOnAny.toString()));
            cmdStateToCancel->fulfillFinalPromise(
                {ErrorCodes::CallbackCanceled,
                 str::stream() << "Command canceled; original request was: "
                               << redact(cmdStateToCancel->requestOnAny.toString())});
            return;
        }

        _killOperation(requestStateToCancel);
    }
}

void NetworkInterfaceTL::_killOperation(std::shared_ptr<RequestState> requestStateToKill) {
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
        [this, operationKey, cmdStateToKill](StatusWith<RemoteCommandOnAnyResponse> swr) {
            invariant(swr.isOK());
            auto rs = std::move(swr.getValue());
            LOGV2_DEBUG(
                51813,
                2,
                "Remote _killOperations request to cancel command with operationKey {operationKey}"
                " finished with response: {rsdata_or_status}",
                "operationKey"_attr = operationKey,
                "rsdata_or_status"_attr =
                    redact(rs.isOK() ? rs.data.toString() : rs.status.toString()));

            // Satisfy the promise locally.
            if (rs.isOK()) {
                // _killOperations succeeded but the operation interrupted error is expected to be
                // ignored in resolve() since cancelCommand() crossed the command finishLine first.
                cmdStateToKill->fulfillFinalPromise(
                    {ErrorCodes::CallbackCanceled,
                     str::stream() << "cancelCommand successfully issued remote interruption"});
            } else {
                // _killOperations timed out or failed due to other errors.
                rs.status.addContext("operation's client canceled by cancelCommand");
                cmdStateToKill->fulfillFinalPromise(std::move(rs.status));
            }
        });

    killOpCmdState->requestManager = std::make_unique<RequestManager>(1, killOpCmdState);

    invariant(killOpCmdState->requestManager->connStatus.size() == 1);
    auto killOpRequestState =
        killOpCmdState->requestManager->makeRequest(killOpCmdState->requestManager.get());

    // Send the _killOperations request.
    auto connFuture = _pool->get(target, sslMode, killOpRequest.kNoTimeout);
    if (connFuture.isReady()) {
        killOpRequestState->trySend(std::move(connFuture).getNoThrow(), 0);
        return;
    }

    std::move(connFuture)
        .thenRunOn(_reactor)
        .getAsync([this, killOpRequestState, killOpRequest](auto swConn) {
            killOpRequestState->trySend(std::move(swConn), 0);
        });
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
    if (ErrorCodes::isCancelationError(status)) {
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
