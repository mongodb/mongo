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

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
#include <algorithm>
#include <memory>
#include <tuple>
#include <type_traits>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/validated_tenancy_scope.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/connection_pool_tl.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_tl_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/ssl_connection_context.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kASIO


namespace mongo {
namespace executor {


void OpportunisticSecondaryTargetingParameter::append(OperationContext*,
                                                      BSONObjBuilder* b,
                                                      StringData name,
                                                      const boost::optional<TenantId>&) {
    return;
}

Status OpportunisticSecondaryTargetingParameter::set(const BSONElement& newValueElement,
                                                     const boost::optional<TenantId>&) {
    LOGV2_WARNING(
        9206304,
        "Opportunistic secondary targeting has been deprecated and the "
        "opportunisticSecondaryTargeting parameter has no effect. For more information please "
        "see https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

Status OpportunisticSecondaryTargetingParameter::setFromString(StringData modeStr,
                                                               const boost::optional<TenantId>&) {
    LOGV2_WARNING(
        9206305,
        "Opportunistic secondary targeting has been deprecated and the "
        "opportunisticSecondaryTargeting parameter has no effect. For more information please "
        "see https://dochub.mongodb.org/core/hedged-reads-deprecated");

    return Status::OK();
}

using namespace fmt::literals;

namespace {
MONGO_FAIL_POINT_DEFINE(triggerSendRequestNetworkTimeout);
MONGO_FAIL_POINT_DEFINE(forceConnectionNetworkTimeout);
MONGO_FAIL_POINT_DEFINE(waitForShutdownBeforeSendRequest);
MONGO_FAIL_POINT_DEFINE(networkInterfaceSendsRequestsOnReactorThread);

auto& numConnectionNetworkTimeouts =
    *MetricBuilder<Counter64>("operation.numConnectionNetworkTimeouts");
auto& timeSpentWaitingBeforeConnectionTimeoutMillis =
    *MetricBuilder<Counter64>("operation.totalTimeWaitingBeforeConnectionTimeoutMillis");

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

    return Status::OK();
}

template <typename IA, typename IB, typename F>
int compareTransformed(IA a1, IA a2, IB b1, IB b2, F&& f) {
    for (;; ++a1, ++b1)
        if (a1 == a2)
            return b1 == b2 ? 0 : -1;
        else if (b1 == b2)
            return 1;
        else if (int r = f(*a1) - f(*b1))
            return r;
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
        stdx::lock_guard lk(_mutex);
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
        _tl = _svcCtx->getTransportLayerManager();
    }

    // Even if you have a service context, it may not have a transport layer (mostly for unittests).
    if (!_tl) {
        if (TestingProctor::instance().isEnabled()) {
            LOGV2_WARNING(22601, "No TransportLayer configured during NetworkInterface startup");
        }
        _ownedTransportLayer =
            transport::TransportLayerManagerImpl::makeAndStartDefaultEgressTransportLayer();
        _tl = _ownedTransportLayer.get();
    }

    std::shared_ptr<const transport::SSLConnectionContext> transientSSLContext;
#ifdef MONGO_CONFIG_SSL
    if (_connPoolOpts.transientSSLParams) {
        auto statusOrContext = _tl->getEgressLayer()->createTransientSSLContext(
            _connPoolOpts.transientSSLParams.value());
        uassertStatusOK(statusOrContext.getStatus());
        transientSSLContext = std::move(statusOrContext.getValue());
    }
#endif

    _reactor = _tl->getEgressLayer()->getReactor(transport::TransportLayer::kNewReactor);
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
        stdx::unique_lock lk(_mutex);
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
    _pool->appendConnectionStats(stats);
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
    _ioThread = stdx::thread([this] {
        setThreadName(_instanceName);
        _run();
    });

    stdx::lock_guard lk(_mutex);
    invariant(_state == kDefault, "Network interface has already started");
    _state = kStarted;
}

void NetworkInterfaceTL::_run() {
    LOGV2_DEBUG(22592, 2, "The NetworkInterfaceTL reactor thread is spinning up");

    // This returns when the reactor is stopped in shutdown()
    _reactor->run();

    // Close out all remaining tasks in the reactor now that they've all been canceled.
    _reactor->drain();

    LOGV2_DEBUG(22593, 2, "NetworkInterfaceTL shutdown successfully");
}

void NetworkInterfaceTL::shutdown() {
    decltype(_inProgress) inProgress;
    {
        stdx::lock_guard lk(_mutex);
        switch (_state) {
            case kDefault:
                _state = kStopped;
                // If we never started, there aren't any commands running.
                invariant(_inProgress.empty());
                _stoppedCV.notify_one();
                return;
            case kStarted:
                _state = kStopping;
                // Grab a copy of the remaining commands. Any attempt to register new commands will
                // throw, so only these need to be cancelled.
                inProgress = _inProgress;
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

    _shutdownAllAlarms();

    const ScopeGuard finallySetStopped = [&] {
        stdx::lock_guard lk(_mutex);
        _state = kStopped;
        invariant(_inProgress.size() == 0);
        _stoppedCV.notify_one();
    };

    for (auto& [_, weakCmdState] : inProgress) {
        auto cmdState = weakCmdState.lock();
        if (!cmdState) {
            continue;
        }

        if (!cmdState->promiseFulfilling.swap(true)) {
            cmdState->fulfillFinalPromise(kNetworkInterfaceShutdownInProgress);
        }

        // Ensure each command has its future's promise fulfilled before shutting down the reactor.
        // Future continuations may try to schedule guaranteed work on the reactor, and if it's
        // shutdown, the work will be rejected leading to an invariant. If we fulfilled the promise
        // above, this will return immediately; otherwise, it will block on the thread that claimed
        // responsibility for fulfilling the promise.
        cmdState->promiseFulfilled.get();
    }

    // This prevents new timers from being set, cancels any ongoing operations on all connections,
    // and destructs all connections for all existing pools.
    _pool->shutdown();

    // Now that the commands have been canceled, ensure they've fully finished and cleaned up before
    // stopping the reactor.
    {
        stdx::unique_lock lk(_mutex);
        LOGV2_DEBUG(9213400,
                    2,
                    "Waiting for any pending network interface operations to complete",
                    "numPending"_attr = _inProgress.size());
        invariant(_state == kStopping);
        _stoppedCV.wait(lk, [&] { return _inProgress.size() == 0; });
    }

    _reactor->stop();

    _ioThread.join();
}

bool NetworkInterfaceTL::inShutdown() const {
    stdx::lock_guard lk(_mutex);
    return _inShutdown_inlock(lk);
}

bool NetworkInterfaceTL::_inShutdown_inlock(WithLock lk) const {
    return _state == kStopping || _state == kStopped;
}

void NetworkInterfaceTL::waitForWork() {
    // waitForWork should only be used by network-mocking code and should not be reachable in the
    // NetworkInterfaceTL.
    MONGO_UNREACHABLE;
}

void NetworkInterfaceTL::waitForWorkUntil(Date_t when) {
    // waitForWorkUntil should only be used by network-mocking code and should not be reachable in
    // the NetworkInterfaceTL.
    MONGO_UNREACHABLE;
}

// This is a no-op in the NetworkInterfaceTL since the waitForWork API is unreachable here.
void NetworkInterfaceTL::signalWorkAvailable() {}

Date_t NetworkInterfaceTL::now() {
    // TODO This check is because we set up NetworkInterfaces in MONGO_INITIALIZERS and then expect
    // this method to work before the NI is started.
    if (!_reactor) {
        return Date_t::now();
    }
    return _reactor->now();
}

void NetworkInterfaceTL::_registerCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                          std::shared_ptr<CommandStateBase> cmdState) {
    stdx::lock_guard lk(_mutex);

    if (_inShutdown_inlock(lk)) {
        uassertStatusOK(kNetworkInterfaceShutdownInProgress);
    }

    _inProgress.insert({cbHandle, cmdState});
}

NetworkInterfaceTL::CommandStateBase::CommandStateBase(
    NetworkInterfaceTL* interface_,
    RemoteCommandRequestOnAny request_,
    const TaskExecutor::CallbackHandle& cbHandle_)
    : interface(interface_),
      request(RemoteCommandRequest(std::move(request_), 0)),
      requestToSend(request),
      cbHandle(cbHandle_),
      timer(interface->_reactor->makeTimer()),
      operationKey(request.operationKey) {}

NetworkInterfaceTL::CommandStateBase::~CommandStateBase() {
    invariant(!conn);
    interface->_unregisterCommand(cbHandle);
}

NetworkInterfaceTL::CommandState::CommandState(NetworkInterfaceTL* interface_,
                                               RemoteCommandRequestOnAny request_,
                                               const TaskExecutor::CallbackHandle& cbHandle_)
    : CommandStateBase(interface_, std::move(request_), cbHandle_) {}

auto NetworkInterfaceTL::CommandState::make(NetworkInterfaceTL* interface,
                                            RemoteCommandRequestOnAny request,
                                            const TaskExecutor::CallbackHandle& cbHandle) {
    auto state = std::make_shared<CommandState>(interface, std::move(request), cbHandle);
    auto [promise, future] = makePromiseFuture<RemoteCommandOnAnyResponse>();
    state->promise = std::move(promise);

    interface->_registerCommand(cbHandle, state);

    // Set the callbacks after successfully registering the command, since since the reference cycle
    // can only be broken if this future chain is fulfilled.
    future = std::move(future)
                 .onError([state](Status error) {
                     // If command promise was canceled or timed out, wrap the error in a RCRsp
                     return RemoteCommandOnAnyResponse(
                         boost::none, std::move(error), state->stopwatch.elapsed());
                 })
                 .tapAll([state](const auto& swRequest) {
                     // swRequest is either populated from the success path or the value
                     // returning onError above. swRequest.isOK() should not be possible.
                     invariant(swRequest.getStatus());

                     // At this point, the command has either been sent and returned an RCRsp or
                     // has received a local interruption that was wrapped in a RCRsp.
                     state->tryFinish(swRequest.getValue().status);
                 });
    return std::pair(state, std::move(future));
}

AsyncDBClient* NetworkInterfaceTL::CommandStateBase::getClient(
    const ConnectionHandle& conn) noexcept {
    if (!conn) {
        return nullptr;
    }

    return checked_cast<connection_pool_tl::TLConnection*>(conn.get())->client();
}

void NetworkInterfaceTL::CommandStateBase::setTimer() {
    auto nowVal = interface->now();

    triggerSendRequestNetworkTimeout.executeIf(
        [&](const BSONObj& data) {
            LOGV2(6496503,
                  "triggerSendRequestNetworkTimeout failpoint enabled, timing out request",
                  "request"_attr = request.cmdObj.toString());
            // Sleep to make sure the elapsed wait time for connection timeout is > 1 millisecond.
            sleepmillis(100);
            deadline = nowVal;
        },
        [&](const BSONObj& data) {
            return data["collectionNS"].valueStringData() ==
                request.cmdObj.firstElement().valueStringData();
        });

    if (deadline == kNoExpirationDate || !request.enforceLocalTimeout) {
        return;
    }

    const auto timeoutCode =
        request.timeoutCode.get_value_or(ErrorCodes::NetworkInterfaceExceededTimeLimit);

    // We don't need to capture an anchor for the CommandStateBase (i.e. this). If the request gets
    // fulfilled and misses cancelling the timer (i.e. we can't lock the weak_ptr), we just want to
    // return. Ideally we'd ensure that cancellation could never miss timers, but since they will
    // eventually fire anyways it's not a huge deal that we don't.
    timer->waitUntil(deadline, baton)
        .getAsync([this, weakState = weak_from_this(), timeoutCode](Status status) {
            if (!status.isOK()) {
                return;
            }

            auto cmdState = weakState.lock();
            if (!cmdState) {
                return;
            }

            if (promiseFulfilling.swap(true)) {
                return;
            }

            const std::string message = str::stream() << "Request " << request.id << " timed out"
                                                      << ", deadline was " << deadline.toString()
                                                      << ", op was " << redact(request.toString());

            LOGV2_DEBUG(22595,
                        2,
                        "Request timed out",
                        "requestId"_attr = request.id,
                        "deadline"_attr = deadline,
                        "request"_attr = request);
            fulfillFinalPromise(StatusWith<RemoteCommandOnAnyResponse>(RemoteCommandOnAnyResponse(
                request.target, Status(timeoutCode, message), stopwatch.elapsed())));
        });
}

void NetworkInterfaceTL::CommandStateBase::returnConnection(Status status) noexcept {
    auto connToReturn = [this] {
        stdx::unique_lock<Latch> lk(mutex);
        invariant(conn);
        return std::exchange(conn, {});
    }();

    if (!status.isOK()) {
        connToReturn->indicateFailure(std::move(status));
        return;
    }

    connToReturn->indicateUsed();
    connToReturn->indicateSuccess();
}

void NetworkInterfaceTL::CommandStateBase::tryFinish(Status status) noexcept {
    invariant(promiseFulfilling.load());

    LOGV2_DEBUG(
        4646302, 2, "Finished request", "requestId"_attr = request.id, "status"_attr = status);

    // The command has resolved one way or another.
    timer->cancel(baton);

    if (interface->_counters) {
        // Increment our counters for the integration test
        interface->_counters->recordResult(status);
    }

    if (operationKey) {
        // Kill operations for the request that we didn't use to fulfill the promise.
        killOperation();
    }

    if (!status.isOK()) {
        // Cancel after we issue _killOperations
        cancel();
    }

    networkInterfaceCommandsFailedWithErrorCode.shouldFail([&](const BSONObj& data) {
        const auto errorCode = data.getIntField("errorCode");
        if (errorCode != status.code()) {
            return false;
        }

        const std::string requestCmdName = request.cmdObj.firstElement().fieldName();
        for (auto&& cmdName : data.getObjectField("cmdNames")) {
            if (cmdName.type() == String && cmdName.valueStringData() == requestCmdName) {
                return true;
            }
        }

        return false;
    });
}

void NetworkInterfaceTL::_unregisterCommand(const TaskExecutor::CallbackHandle& cbHandle) {
    stdx::lock_guard lk(_mutex);
    if (!_inProgress.erase(cbHandle)) {
        // We never made it into the inProgress list.
        return;
    }
    if (_state == State::kStopping && _inProgress.size() == 0) {
        _stoppedCV.notify_one();
    }
}

void NetworkInterfaceTL::CommandStateBase::cancel() noexcept {
    LOGV2_DEBUG(4646301, 2, "Cancelling request", "requestId"_attr = request.id);

    auto connToCancel = [this] {
        stdx::lock_guard<Latch> lk(mutex);
        return conn;
    }();
    if (auto clientPtr = getClient(connToCancel)) {
        // If we have a client, cancel it
        clientPtr->cancel(baton);
    }
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

    request.target.resize(1);
    auto targetNode = request.target.front();

    auto [cmdState, future] = CommandState::make(this, request, cbHandle);
    if (cmdState->request.timeout != cmdState->request.kNoTimeout) {
        cmdState->deadline = cmdState->stopwatch.start() + cmdState->request.timeout;
    }
    cmdState->baton = baton;

    // When our command finishes, run onFinish out of line.
    std::move(future)
        // Run the callback on the baton if it exists and is not shut down, and run on the reactor
        // otherwise.
        .thenRunOn(makeGuaranteedExecutor(baton, _reactor))
        .getAsync([cmdState = cmdState,
                   onFinish = std::move(onFinish)](StatusWith<RemoteCommandOnAnyResponse> swr) {
            invariant(swr.getStatus(),
                      "Remote command response failed with an error: {}"_format(
                          swr.getStatus().toString()));
            auto rs = std::move(swr.getValue());
            // The TransportLayer has, for historical reasons returned
            // SocketException for network errors, but sharding assumes
            // HostUnreachable on network errors.
            if (rs.status == ErrorCodes::SocketException) {
                rs.status = Status(ErrorCodes::HostUnreachable, rs.status.reason());
            }

            // Time limit exceeded from ConnectionPool waiting to acquire a connection.
            if (rs.status == ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit) {
                numConnectionNetworkTimeouts.increment(1);
                timeSpentWaitingBeforeConnectionTimeoutMillis.increment(
                    durationCount<Milliseconds>(cmdState->connTimeoutWaitTime));
                auto timeoutCode = cmdState->request.timeoutCode;
                if (timeoutCode && cmdState->connTimeoutWaitTime >= cmdState->request.timeout) {
                    rs.status = Status(*timeoutCode, rs.status.reason());
                }
                if (gEnableDetailedConnectionHealthMetricLogLines.load()) {
                    LOGV2(6496500,
                          "Operation timed out while waiting to acquire connection",
                          "requestId"_attr = cmdState->request.id,
                          "duration"_attr = cmdState->connTimeoutWaitTime);
                }
            }

            LOGV2_DEBUG(22597,
                        2,
                        "Request finished with response",
                        "requestId"_attr = cmdState->request.id,
                        "isOK"_attr = rs.isOK(),
                        "response"_attr =
                            redact(rs.isOK() ? rs.data.toString() : rs.status.toString()));

            onFinish(std::move(rs));
        });

    if (MONGO_unlikely(networkInterfaceDiscardCommandsBeforeAcquireConn.shouldFail())) {
        LOGV2(22598, "Discarding command due to failpoint before acquireConn");
        return Status::OK();
    }

    auto connFuture = _pool->get(targetNode, request.sslMode, request.timeout);

    if (connFuture.isReady() &&
        !MONGO_unlikely(networkInterfaceSendsRequestsOnReactorThread.shouldFail())) {
        cmdState->trySend(std::move(connFuture).getNoThrow());
    } else {
        // Otherwise, schedule the request.
        std::move(connFuture).thenRunOn(_reactor).getAsync([cmdState = cmdState](auto swConn) {
            cmdState->trySend(std::move(swConn));
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

Future<RemoteCommandResponse> NetworkInterfaceTL::CommandState::sendRequest() {
    return makeReadyFutureWith([this] {
               setTimer();
               const auto connAcquiredTimer =
                   checked_cast<connection_pool_tl::TLConnection*>(conn.get())
                       ->getConnAcquiredTimer();
               return getClient(conn)->runCommandRequest(
                   requestToSend, baton, std::move(connAcquiredTimer));
           })
        .then([this](RemoteCommandResponse response) {
            uassertStatusOK(doMetadataHook(RemoteCommandOnAnyResponse(request.target, response)));
            return response;
        });
}

Status NetworkInterfaceTL::CommandStateBase::doMetadataHook(
    const RemoteCommandOnAnyResponse& response) {
    if (auto& hook = interface->_metadataHook; hook && !promiseFulfilling.load()) {
        invariant(response.target);
        return hook->readReplyMetadata(nullptr, response.data);
    }
    return Status::OK();
}

void NetworkInterfaceTL::CommandState::fulfillFinalPromise(
    StatusWith<RemoteCommandOnAnyResponse> response) {
    promise.setFrom(std::move(response));
    promiseFulfilled.set();
}


void NetworkInterfaceTL::CommandStateBase::killOperation() {
    {
        stdx::lock_guard<Latch> lk(mutex);
        if (!conn) {
            return;
        }
    }

    if (auto status = interface->_killOperation(this); !status.isOK()) {
        LOGV2_DEBUG(4664810, 2, "Failed to send remote _killOperations", "error"_attr = status);
    }
}

void NetworkInterfaceTL::CommandStateBase::trySend(
    StatusWith<ConnectionPool::ConnectionHandle> swConn) noexcept {
    forceConnectionNetworkTimeout.executeIf(
        [&](const BSONObj& data) {
            LOGV2(6496502,
                  "forceConnectionNetworkTimeout failpoint enabled, timing out request",
                  "request"_attr = request.cmdObj.toString());
            swConn =
                Status(ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit,
                       "PooledConnectionAcquisitionExceededTimeLimit triggered via fail point.");
        },
        [&](const BSONObj& data) {
            return data["collectionNS"].valueStringData() ==
                request.cmdObj.firstElement().valueStringData();
        });

    // Our connection wasn't any good
    if (!swConn.isOK()) {
        {
            stdx::lock_guard<Latch> lk(mutex);
            invariant(!conn);
        }

        // We're the last one, set the promise if it hasn't already been set via cancel or timeout
        if (!promiseFulfilling.swap(true)) {
            if (swConn.getStatus() == ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit) {
                connTimeoutWaitTime = stopwatch.elapsed();
            }

            auto& reactor = interface->_reactor;
            boost::optional<HostAndPort> target = request.target;
            if (reactor->onReactorThread()) {
                fulfillFinalPromise(StatusWith<RemoteCommandOnAnyResponse>(
                    RemoteCommandOnAnyResponse(target, std::move(swConn.getStatus()))));
            } else {
                ExecutorFuture<void>(reactor, swConn.getStatus())
                    .getAsync([this, anchor = shared_from_this(), target](Status status) {
                        fulfillFinalPromise(StatusWith<RemoteCommandOnAnyResponse>(
                            RemoteCommandOnAnyResponse(target, std::move(status))));
                    });
            }
        }
        return;
    }

    checked_cast<connection_pool_tl::TLConnection*>(swConn.getValue().get())
        ->startConnAcquiredTimer();

    bool logSetMaxTimeMS = false;
    {
        stdx::lock_guard<Latch> lk(mutex);

        invariant(!conn);

        // Set conn under the lock so they will always be observed during cancel.
        conn = std::move(swConn.getValue());

        if (interface->_svcCtx && request.timeout != RemoteCommandRequest::kNoTimeout &&
            WireSpec::getWireSpec(interface->_svcCtx).get()->isInternalClient) {
            logSetMaxTimeMS = true;
            BSONObjBuilder updatedCmdBuilder;
            updatedCmdBuilder.appendElements(requestToSend.cmdObj);
            updatedCmdBuilder.append("maxTimeMSOpOnly", requestToSend.timeout.count());
            requestToSend.cmdObj = updatedCmdBuilder.obj();
        }
    }

    LOGV2_DEBUG(4646300,
                2,
                "Sending request",
                "requestId"_attr = request.id,
                "target"_attr = request.target);

    if (logSetMaxTimeMS) {
        LOGV2_DEBUG(4924402,
                    2,
                    "Set maxTimeMSOpOnly for request",
                    "maxTimeMSOpOnly"_attr = request.timeout,
                    "requestId"_attr = request.id,
                    "target"_attr = request.target);
    }

    LOGV2_DEBUG(4630601,
                2,
                "Request acquired a connection",
                "requestId"_attr = request.id,
                "target"_attr = request.target);

    networkInterfaceHangCommandsAfterAcquireConn.pauseWhileSet();

    // An attempt to avoid sending a request after its command has been canceled or already executed
    // using another connection. Just a best effort to mitigate unnecessary resource consumption if
    // possible, and allow deterministic cancellation of requests in testing.
    if (promiseFulfilling.load()) {
        LOGV2_DEBUG(5813901,
                    2,
                    "Skipping request as it has already been fulfilled or canceled",
                    "requestId"_attr = request.id,
                    "target"_attr = request.target);
        returnConnection(Status::OK());
        return;
    }

    if (auto counters = interface->_counters) {
        counters->recordSent();
    }

    if (waitForShutdownBeforeSendRequest.shouldFail()) {
        invariant(!interface->onNetworkThread());
        promiseFulfilled.get();
    }

    resolve(sendRequest());
}

void NetworkInterfaceTL::CommandStateBase::resolve(Future<RemoteCommandResponse> future) noexcept {
    // Convert the RemoteCommandResponse to a RemoteCommandOnAnyResponse and wrap any error
    auto anyFuture = std::move(future)
                         .then([this, anchor = shared_from_this()](RemoteCommandResponse response) {
                             // The RCRq ran successfully, wrap the result with the host in question
                             return RemoteCommandOnAnyResponse(request.target, std::move(response));
                         })
                         .onError([this, anchor = shared_from_this()](Status error) {
                             // The RCRq failed, wrap the error into a RCRsp with the host and
                             // duration
                             return RemoteCommandOnAnyResponse(
                                 request.target, std::move(error), stopwatch.elapsed());
                         });

    std::move(anyFuture)
        .thenRunOn(
            makeGuaranteedExecutor(baton, interface->_reactor))  // Switch to the baton/reactor.
        .getAsync([this, anchor = shared_from_this()](auto swr) noexcept {
            auto response = uassertStatusOK(swr);
            auto status = response.status;

            returnConnection(status);

            if (promiseFulfilling.swap(true)) {
                LOGV2_DEBUG(4754301,
                            2,
                            "Skipping the response because the operation was cancelled",
                            "requestId"_attr = request.id,
                            "target"_attr = request.target,
                            "status"_attr = response.status,
                            "response"_attr = redact(response.data));

                return;
            }

            fulfillFinalPromise(std::move(response));
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
                                                   RemoteCommandOnReplyFn&& onReply,
                                                   const BatonHandle& baton) {
    auto state = std::make_shared<ExhaustCommandState>(
        interface, std::move(request), cbHandle, std::move(onReply));
    auto [promise, future] = makePromiseFuture<void>();
    state->promise = std::move(promise);

    interface->_registerCommand(cbHandle, state);

    // Set the callbacks after successfully registering the command, since since the reference cycle
    // can only be broken if this future chain is fulfilled.
    std::move(future)
        .thenRunOn(makeGuaranteedExecutor(baton, interface->_reactor))
        .onError([state](Status error) {
            stdx::lock_guard<Latch> lk(state->stopwatchMutex);
            state->onReplyFn(RemoteCommandOnAnyResponse(
                boost::none, std::move(error), state->stopwatch.elapsed()));
        })
        .getAsync([state](Status status) {
            state->tryFinish(
                Status{ErrorCodes::ExhaustCommandFinished, "Exhaust command finished"});
        });

    return state;
}

Future<RemoteCommandResponse> NetworkInterfaceTL::ExhaustCommandState::sendRequest() try {
    auto [promise, future] = makePromiseFuture<RemoteCommandResponse>();
    finalResponsePromise = std::move(promise);

    setTimer();
    getClient(conn)
        ->beginExhaustCommandRequest(request, baton)
        .thenRunOn(interface->_reactor)
        .getAsync([this](StatusWith<RemoteCommandResponse> swResponse) mutable {
            continueExhaustRequest(swResponse);
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
    promiseFulfilled.set();
}

void NetworkInterfaceTL::ExhaustCommandState::continueExhaustRequest(
    StatusWith<RemoteCommandResponse> swResponse) {
    RemoteCommandResponse response;
    if (!swResponse.isOK()) {
        response = RemoteCommandResponse(std::move(swResponse.getStatus()));
    } else {
        response = std::move(swResponse.getValue());
    }

    if (interface->inShutdown() || ErrorCodes::isCancellationError(response.status)) {
        finalResponsePromise.emplaceValue(response);
        return;
    }

    auto onAnyResponse = RemoteCommandOnAnyResponse(request.target, response);
    if (Status metadataHookStatus = doMetadataHook(onAnyResponse); !metadataHookStatus.isOK()) {
        finalResponsePromise.setError(metadataHookStatus);
        return;
    }

    // If the command failed, we will call 'onReply' as a part of the future chain paired with
    // the promise. This is to be sure that all error paths will run 'onReply' only once upon
    // future completion.
    if (!response.status.isOK() || !getStatusFromCommandResult(response.data).isOK()) {
        // The moreToCome bit should *not* be set if the command failed
        invariant(!response.moreToCome);

        finalResponsePromise.emplaceValue(response);
        return;
    }

    onReplyFn(onAnyResponse);

    // Reset the stopwatch to measure the correct duration for the following reply
    {
        stdx::lock_guard<Latch> lk(stopwatchMutex);
        stopwatch.restart();
    }
    if (deadline != kNoExpirationDate) {
        deadline = stopwatch.start() + request.timeout;
    }

    setTimer();

    getClient(conn)
        ->awaitExhaustCommand(baton)
        .thenRunOn(interface->_reactor)
        .getAsync([this](StatusWith<RemoteCommandResponse> swResponse) mutable {
            continueExhaustRequest(swResponse);
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

    auto cmdState = ExhaustCommandState::make(this, request, cbHandle, std::move(onReply), baton);
    if (cmdState->request.timeout != cmdState->request.kNoTimeout) {
        cmdState->deadline = cmdState->stopwatch.start() + cmdState->request.timeout;
    }
    cmdState->baton = baton;

    // Attempt to get a connection to the target host
    auto connFuture = _pool->get(request.target.front(), request.sslMode, request.timeout);

    if (connFuture.isReady()) {
        cmdState->trySend(std::move(connFuture).getNoThrow());
    } else {
        // For every connection future we didn't have immediately ready, schedule
        std::move(connFuture).thenRunOn(_reactor).getAsync([cmdState](auto swConn) {
            cmdState->trySend(std::move(swConn));
        });
    }

    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

void NetworkInterfaceTL::cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                       const BatonHandle&) {
    std::shared_ptr<NetworkInterfaceTL::CommandStateBase> cmdStateToCancel;
    {
        stdx::unique_lock<Mutex> lk(_mutex);
        auto it = _inProgress.find(cbHandle);
        if (it == _inProgress.end()) {
            return;
        }
        cmdStateToCancel = it->second.lock();
        if (!cmdStateToCancel) {
            return;
        }
    }

    if (!cmdStateToCancel->promiseFulfilling.swap(true)) {
        LOGV2_DEBUG(22599,
                    2,
                    "Canceling operation for request",
                    "request"_attr = redact(cmdStateToCancel->request.cmdObj));
        cmdStateToCancel->fulfillFinalPromise({ErrorCodes::CallbackCanceled,
                                               str::stream()
                                                   << "Command canceled; original request was: "
                                                   << redact(cmdStateToCancel->request.cmdObj)});
    }
}

Status NetworkInterfaceTL::_killOperation(CommandStateBase* cmdStateToKill) try {
    auto [target, sslMode] = [&] {
        const auto& request = cmdStateToKill->request;
        return std::make_pair(request.target, request.sslMode);
    }();

    auto operationKey = cmdStateToKill->operationKey.value();
    LOGV2_DEBUG(4664801,
                2,
                "Sending remote _killOperations request to cancel command",
                "operationKey"_attr = operationKey,
                "target"_attr = target);

    // Make a request state for _killOperations.
    executor::RemoteCommandRequest killOpRequest(
        target,
        DatabaseName::kAdmin,
        BSON("_killOperations" << 1 << "operationKeys" << BSON_ARRAY(operationKey)),
        nullptr,
        kCancelCommandTimeout);

    auto cbHandle = executor::TaskExecutor::CallbackHandle();
    auto [killOpCmdState, future] = CommandState::make(this, killOpRequest, cbHandle);
    killOpCmdState->deadline = killOpCmdState->stopwatch.start() + killOpRequest.timeout;

    std::move(future).getAsync(
        [this, operationKey, killOpRequest](StatusWith<RemoteCommandOnAnyResponse> swr) {
            invariant(swr.getStatus());
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
            killOpCmdState->trySend(std::move(swConn));
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

SemiFuture<void> NetworkInterfaceTL::setAlarm(Date_t when, const CancellationToken& token) {
    if (inShutdown()) {
        // Pessimistically check if we're in shutdown and save some work
        return kNetworkInterfaceShutdownInProgress;
    }

    if (when <= now()) {
        return Status::OK();
    }

    auto id = nextAlarmId.fetchAndAdd(1);
    auto alarmState = std::make_shared<AlarmState>(this, id, _reactor->makeTimer(), token);

    {
        stdx::lock_guard<Mutex> lk(_mutex);

        if (_inShutdown_inlock(lk)) {
            // Check that we've won any possible race with _shutdownAllAlarms();
            return kNetworkInterfaceShutdownInProgress;
        }

        // If a user has already scheduled an alarm with a handle, make sure they intentionally
        // override it by canceling and setting a new one.
        auto&& [it, wasInserted] =
            _inProgressAlarms.emplace(alarmState->id, std::weak_ptr(alarmState));
        invariant(wasInserted);
    }

    auto future =
        alarmState->timer->waitUntil(when, nullptr).tapAll([alarmState](Status status) {});

    alarmState->source.token().onCancel().thenRunOn(_reactor).getAsync(
        [this, weakAlarmState = std::weak_ptr(alarmState)](Status status) {
            if (!status.isOK()) {
                return;
            }

            auto alarmState = weakAlarmState.lock();
            if (!alarmState) {
                return;
            }

            alarmState->timer->cancel();
        });

    return std::move(future).semi();
}

void NetworkInterfaceTL::_shutdownAllAlarms() {
    auto alarms = [&] {
        stdx::unique_lock<Mutex> lk(_mutex);
        invariant(_state == kStopping);
        return _inProgressAlarms;
    }();

    for (auto&& [_, weakState] : alarms) {
        auto alarmState = weakState.lock();
        if (!alarmState) {
            continue;
        }
        alarmState->source.cancel();
    }

    {
        stdx::unique_lock<Mutex> lk(_mutex);
        _stoppedCV.wait(lk, [&] { return _inProgressAlarms.empty(); });
    }
}

void NetworkInterfaceTL::_removeAlarm(std::uint64_t id) {
    stdx::lock_guard<Mutex> lk(_mutex);
    auto it = _inProgressAlarms.find(id);
    invariant(it != _inProgressAlarms.end());
    _inProgressAlarms.erase(it);
    if (_inShutdown_inlock(lk) && _inProgressAlarms.empty()) {
        _stoppedCV.notify_all();
    }
}

bool NetworkInterfaceTL::onNetworkThread() {
    return _reactor->onReactorThread();
}

void NetworkInterfaceTL::dropConnections(const HostAndPort& hostAndPort) {
    _pool->dropConnections(hostAndPort);
}

AsyncDBClient* NetworkInterfaceTL::LeasedStream::getClient() {
    return checked_cast<connection_pool_tl::TLConnection*>(_conn.get())->client();
}

void NetworkInterfaceTL::LeasedStream::indicateSuccess() {
    return _conn->indicateSuccess();
}

void NetworkInterfaceTL::LeasedStream::indicateFailure(Status status) {
    _conn->indicateFailure(status);
}

void NetworkInterfaceTL::LeasedStream::indicateUsed() {
    _conn->indicateUsed();
}

SemiFuture<std::unique_ptr<NetworkInterface::LeasedStream>> NetworkInterfaceTL::leaseStream(
    const HostAndPort& hostAndPort, transport::ConnectSSLMode sslMode, Milliseconds timeout) {

    return _pool->lease(hostAndPort, sslMode, timeout)
        .thenRunOn(_reactor)
        .then([](auto conn) -> std::unique_ptr<NetworkInterface::LeasedStream> {
            auto ptr = std::make_unique<NetworkInterfaceTL::LeasedStream>(std::move(conn));
            return ptr;
        })
        .semi();
}
}  // namespace executor
}  // namespace mongo
