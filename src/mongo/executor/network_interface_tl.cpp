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
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/exhaust_response_reader_tl.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/testing_proctor.h"

#include <algorithm>
#include <memory>
#include <tuple>
#include <type_traits>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace executor {

namespace {
MONGO_FAIL_POINT_DEFINE(triggerSendRequestNetworkTimeout);
MONGO_FAIL_POINT_DEFINE(forceConnectionNetworkTimeout);
MONGO_FAIL_POINT_DEFINE(hangBeforeDrainingCommandStates);
MONGO_FAIL_POINT_DEFINE(increaseTimeoutOnKillOp);

auto& numConnectionNetworkTimeouts =
    *MetricBuilder<Counter64>("operation.numConnectionNetworkTimeouts");
auto& timeSpentWaitingBeforeConnectionTimeoutMillis =
    *MetricBuilder<Counter64>("operation.totalTimeWaitingBeforeConnectionTimeoutMillis");

void appendMetadata(RemoteCommandRequest* request,
                    const std::unique_ptr<rpc::EgressMetadataHook>& hook) {
    if (hook) {
        BSONObjBuilder bob(std::move(request->metadata));
        iassert(hook->writeRequestMetadata(request->opCtx, &bob));
        request->metadata = bob.obj();
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
        stdx::lock_guard lk(_mutex);
        ++_data.sent;
    }

private:
    mutable stdx::mutex _mutex;
    Counters _data;
};

namespace {
constexpr auto kShutdownInProgressMsg = "NetworkInterface shutdown in progress"_sd;
constexpr auto kNotYetStartedUpMsg = "NetworkInterface has not started yet"_sd;
}  // namespace

NetworkInterfaceTL::NetworkInterfaceTL(std::string instanceName,
                                       std::shared_ptr<AsyncClientFactory> factory,
                                       std::unique_ptr<rpc::EgressMetadataHook> metadataHook)
    : _instanceName(std::move(instanceName)),
      _clientFactory(std::move(factory)),
      _metadataHook(std::move(metadataHook)),
      _state(kDefault) {}

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
    if (MONGO_unlikely(!_initialized.load())) {
        return;
    }

    _clientFactory->appendConnectionStats(stats);
}

void NetworkInterfaceTL::appendStats(BSONObjBuilder& bob) const {
    if (MONGO_unlikely(!_initialized.load())) {
        return;
    }

    BSONObjBuilder builder = bob.subobjStart(_instanceName);
    _reactor->appendStats(builder);
    _clientFactory->appendStats(builder);
}

NetworkInterface::Counters NetworkInterfaceTL::getCounters() const {
    invariant(_counters);
    return _counters->get();
}

std::string NetworkInterfaceTL::getHostName() {
    return getHostNameCached();
}

void NetworkInterfaceTL::startup() {
    stdx::lock_guard lk(_mutex);
    invariant(_state != kStarted, "NetworkInterface has already started");

    if (_state != kDefault) {
        LOGV2_INFO(9446800,
                   "Skipping NetworkInterface startup due to shutdown",
                   "state"_attr = toString(_state));
        return;
    }

    _svcCtx = getGlobalServiceContext();
    auto tlm = _svcCtx->getTransportLayerManager();
    invariant(tlm, "Cannot start NetworkInterface without a TransportLayer!");

    auto tl = tlm->getTransportLayer(_clientFactory->getTransportProtocol());
    invariant(tl && tl->isEgress());
    _reactor = tl->getReactor(transport::TransportLayer::kNewReactor);
    _clientFactory->startup(_svcCtx, tl, _reactor);
    _initialized.store(true);

    if (TestingProctor::instance().isEnabled()) {
        _counters = std::make_unique<SynchronizedCounters>();
    }

    _ioThread = stdx::thread([this] {
        setThreadName(_instanceName);
        _run();
    });

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

        cmdState->cancel({ErrorCodes::ShutdownInProgress, kShutdownInProgressMsg});
    }

    // This prevents new timers from being set, cancels any ongoing operations on all connections,
    // and destructs all connections for all existing pools.
    _clientFactory->shutdown();

    // Now that the commands have been canceled, ensure they've fully finished and cleaned up before
    // stopping the reactor.
    {
        hangBeforeDrainingCommandStates.pauseWhileSet();
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

Status NetworkInterfaceTL::_verifyRunning() const {
    stdx::lock_guard lk(_mutex);
    switch (_state) {
        case kStopping:
        case kStopped:
            return {ErrorCodes::ShutdownInProgress, kShutdownInProgressMsg};
        case kDefault:
            return {ErrorCodes::NotYetInitialized, kNotYetStartedUpMsg};
        case kStarted:
            return Status::OK();
    }
    MONGO_UNREACHABLE;
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
    if (MONGO_unlikely(!_initialized.load())) {
        return Date_t::now();
    }
    return _reactor->now();
}

void NetworkInterfaceTL::_registerCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                          std::shared_ptr<CommandStateBase> cmdState) {
    {
        stdx::lock_guard lk(_mutex);

        if (_inShutdown_inlock(lk)) {
            uassertStatusOK({ErrorCodes::ShutdownInProgress, kShutdownInProgressMsg});
        }

        _inProgress.insert({cbHandle, cmdState});
    }

    if (cmdState->request.timeout != RemoteCommandRequest::kNoTimeout) {
        cmdState->deadline = cmdState->stopwatch.start() + cmdState->request.timeout;
    }

    // Okay to inline this callback since all it does is log.
    cmdState->cancelSource.token().onCancel().unsafeToInlineFuture().getAsync(
        [id = cmdState->request.id](Status s) {
            if (!s.isOK()) {
                return;
            }
            LOGV2_DEBUG(4646301, 2, "Cancelling request", "requestId"_attr = id);
        });
}

NetworkInterfaceTL::CommandStateBase::CommandStateBase(
    NetworkInterfaceTL* interface_,
    RemoteCommandRequest request_,
    const TaskExecutor::CallbackHandle& cbHandle_,
    const BatonHandle& baton_,
    const CancellationToken& token)
    : interface(interface_),
      request(std::move(request_)),
      cbHandle(cbHandle_),
      baton(baton_),
      timer(interface->_reactor->makeTimer()),
      cancelSource(token) {}

NetworkInterfaceTL::CommandStateBase::~CommandStateBase() {
    invariant(!clientHandle);
    interface->_unregisterCommand(cbHandle);
}

void NetworkInterfaceTL::CommandStateBase::cancel(Status status) {
    invariant(!status.isOK());
    {
        stdx::lock_guard<stdx::mutex> lk(cancelMutex);
        if (!cancelStatus.isOK()) {
            LOGV2_DEBUG(9257001,
                        2,
                        "Skipping redundant cancellation",
                        "requestId"_attr = request.id,
                        "request"_attr = redact(request.toString()),
                        "originalReason"_attr = cancelStatus,
                        "redundantReason"_attr = status);
            return;
        }

        cancelStatus = status;
        LOGV2_DEBUG(9257002,
                    2,
                    "Cancelling command with reason",
                    "requestId"_attr = request.id,
                    "request"_attr = redact(request.toString()),
                    "reason"_attr = status);
    }
    cancelSource.cancel();
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

            const std::string message = str::stream() << "Request " << request.id << " timed out"
                                                      << ", deadline was " << deadline.toString()
                                                      << ", op was " << redact(request.toString());

            LOGV2_DEBUG(22595,
                        2,
                        "Request timed out",
                        "requestId"_attr = request.id,
                        "deadline"_attr = deadline,
                        "request"_attr = request);
            cancel({timeoutCode, message});
        });
}

void NetworkInterfaceTL::CommandStateBase::releaseClientHandle(Status status) {
    invariant(clientHandle);

    auto clientHandleToRelease = std::exchange(clientHandle, {});

    if (!status.isOK()) {
        clientHandleToRelease->indicateFailure(std::move(status));
        return;
    }

    clientHandleToRelease->indicateUsed();
    clientHandleToRelease->indicateSuccess();
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

SemiFuture<RemoteCommandResponse> NetworkInterfaceTL::startCommand(
    const TaskExecutor::CallbackHandle& cbHandle,
    RemoteCommandRequest& request,
    const BatonHandle& baton,
    const CancellationToken& token) {
    uassertStatusOK(_verifyRunning());

    LOGV2_DEBUG(
        22596, kDiagnosticLogLevel, "startCommand", "request"_attr = redact(request.toString()));

    appendMetadata(&request, _metadataHook);

    auto cmdState = std::make_shared<CommandState>(this, request, cbHandle, baton, token);
    _registerCommand(cmdState->cbHandle, cmdState);

    return _runCommand(cmdState).semi();
}

void NetworkInterfaceTL::testEgress(const HostAndPort& hostAndPort,
                                    transport::ConnectSSLMode sslMode,
                                    Milliseconds timeout,
                                    Status status) {
    uassert(ErrorCodes::NotYetInitialized, kNotYetStartedUpMsg, _initialized.load());

    auto handle = _clientFactory->get(hostAndPort, sslMode, timeout).get();
    if (status.isOK()) {
        handle->indicateSuccess();
    } else {
        handle->indicateFailure(status);
    }
}

ExecutorFuture<RemoteCommandResponse> NetworkInterfaceTL::CommandState::sendRequestImpl(
    RemoteCommandRequest req) {
    return makeReadyFutureWith([this, req = std::move(req)] {
               const auto connAcquiredTimer = clientHandle->getAcquiredTimer();
               return clientHandle->getClient().runCommandRequest(
                   std::move(req), baton, std::move(connAcquiredTimer), cancelSource.token());
           })
        .thenRunOn(makeGuaranteedExecutor())
        .onCompletion(
            [this, anchor = shared_from_this()](StatusWith<RemoteCommandResponse> swResp) {
                auto status = swResp.isOK() ? swResp.getValue().status : swResp.getStatus();
                releaseClientHandle(status);
                return swResp;
            });
}

void NetworkInterfaceTL::CommandStateBase::doMetadataHook(const RemoteCommandResponse& response) {
    if (auto& hook = interface->_metadataHook; hook && response.isOK()) {
        uassertStatusOK(hook->readReplyMetadata(nullptr, response.data));
    }
}

ExecutorFuture<RemoteCommandResponse> NetworkInterfaceTL::ExhaustCommandState::sendRequestImpl(
    RemoteCommandRequest req) try {
    return clientHandle->getClient()
        .beginExhaustCommandRequest(req, baton, cancelSource.token())
        .thenRunOn(makeGuaranteedExecutor());
} catch (const DBException& ex) {
    return ExecutorFuture<RemoteCommandResponse>(makeGuaranteedExecutor(), ex.toStatus());
}

SemiFuture<std::shared_ptr<NetworkInterface::ExhaustResponseReader>>
NetworkInterfaceTL::startExhaustCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                        RemoteCommandRequest& request,
                                        const BatonHandle& baton,
                                        const CancellationToken& cancelToken) {
    uassertStatusOK(_verifyRunning());

    LOGV2_DEBUG(23909,
                kDiagnosticLogLevel,
                "startExhaustCommand",
                "request"_attr = redact(request.toString()));

    appendMetadata(&request, _metadataHook);

    auto cmdState =
        std::make_shared<ExhaustCommandState>(this, request, cbHandle, baton, cancelToken);
    _registerCommand(cbHandle, cmdState);
    return _runCommand(cmdState)
        .onCompletion([this, cmdState, cancelToken](StatusWith<RemoteCommandResponse> swr)
                          -> StatusWith<std::shared_ptr<ExhaustResponseReader>> {
            invariant(swr);
            auto& resp = swr.getValue();

            if (!resp.isOK()) {
                if (cmdState->clientHandle) {
                    cmdState->releaseClientHandle(resp.status);
                }
                return resp.status;
            }

            invariant(cmdState->clientHandle);
            return std::make_shared<ExhaustResponseReaderTL>(
                cmdState->request,
                resp,
                std::exchange(cmdState->clientHandle, {}),
                cmdState->baton,
                _reactor,
                cancelToken);
        })
        .semi();
}

void NetworkInterfaceTL::cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                       const BatonHandle&) {
    std::shared_ptr<NetworkInterfaceTL::CommandStateBase> cmdStateToCancel;
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        auto it = _inProgress.find(cbHandle);
        if (it == _inProgress.end()) {
            return;
        }
        cmdStateToCancel = it->second.lock();
        if (!cmdStateToCancel) {
            return;
        }
    }

    cmdStateToCancel->cancel({ErrorCodes::CallbackCanceled,
                              str::stream() << "Command canceled; original request was: "
                                            << redact(cmdStateToCancel->request.cmdObj)});
}

void NetworkInterfaceTL::_killOperation(CommandStateBase* cmdStateToKill) try {
    auto operationKey = cmdStateToKill->request.operationKey;
    invariant(operationKey);

    LOGV2_DEBUG(4664801,
                2,
                "Sending remote _killOperations request to cancel command",
                "target"_attr = cmdStateToKill->request.target,
                "cancelledRequestId"_attr = cmdStateToKill->request.id,
                "canelledRequest"_attr = redact(cmdStateToKill->request.toString()),
                "operationKey"_attr = operationKey);

    executor::RemoteCommandRequest killOpRequest(
        cmdStateToKill->request.target,
        DatabaseName::kAdmin,
        BSON("_killOperations" << 1 << "operationKeys" << BSON_ARRAY(*operationKey)),
        nullptr,
        increaseTimeoutOnKillOp.shouldFail() ? kCancelCommandTimeout_forTest
                                             : kCancelCommandTimeout);
    auto cbHandle = executor::TaskExecutor::CallbackHandle();
    auto killOpCmdState = std::make_shared<CommandState>(
        this, killOpRequest, cbHandle, nullptr, CancellationToken::uncancelable());
    _registerCommand(cbHandle, killOpCmdState);
    _runCommand(killOpCmdState)
        .getAsync([this, operationKey, killOpRequest](StatusWith<RemoteCommandResponse> swr) {
            invariant(swr);
            auto rs = std::move(swr.getValue());
            LOGV2_DEBUG(51813,
                        2,
                        "Remote _killOperations request to cancel command finished with response",
                        "operationKey"_attr = operationKey,
                        "target"_attr = killOpRequest.target,
                        "response"_attr =
                            redact(rs.isOK() ? rs.data.toString() : rs.status.toString()));
        });
} catch (const DBException& ex) {
    LOGV2_DEBUG(4664810, 2, "Failed to send remote _killOperations", "error"_attr = ex.toStatus());
    return;
}

Status NetworkInterfaceTL::schedule(unique_function<void(Status)> action) {
    if (Status running = _verifyRunning(); !running.isOK()) {
        return running;
    }

    _reactor->schedule([action = std::move(action)](auto status) { action(status); });
    return Status::OK();
}

SemiFuture<void> NetworkInterfaceTL::setAlarm(Date_t when, const CancellationToken& token) {
    if (Status running = _verifyRunning(); !running.isOK()) {
        // Pessimistically check if we're in shutdown and save some work
        return running;
    }

    if (when <= now()) {
        return Status::OK();
    }

    auto id = nextAlarmId.fetchAndAdd(1);
    auto alarmState = std::make_shared<AlarmState>(this, id, _reactor->makeTimer(), token);

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (_inShutdown_inlock(lk)) {
            // Check that we've won any possible race with _shutdownAllAlarms();
            return Status(ErrorCodes::ShutdownInProgress, kShutdownInProgressMsg);
        }

        // If a user has already scheduled an alarm with a handle, make sure they intentionally
        // override it by canceling and setting a new one.
        auto&& [it, wasInserted] =
            _inProgressAlarms.emplace(alarmState->id, std::weak_ptr(alarmState));
        invariant(wasInserted);
    }

    auto future =
        alarmState->timer->waitUntil(when, nullptr).tapAll([alarmState](Status status) {});

    alarmState->source.token().onCancel().unsafeToInlineFuture().getAsync(
        [this, weakAlarmState = std::weak_ptr(alarmState)](Status status) {
            if (!status.isOK()) {
                return;
            }

            _reactor->schedule([this, weakAlarmState = std::move(weakAlarmState)](Status s) {
                if (!s.isOK()) {
                    return;
                }
                auto alarmState = weakAlarmState.lock();
                if (!alarmState) {
                    return;
                }

                alarmState->timer->cancel();
            });
        });

    return std::move(future).semi();
}

void NetworkInterfaceTL::_shutdownAllAlarms() {
    auto alarms = [&] {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
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
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _stoppedCV.wait(lk, [&] { return _inProgressAlarms.empty(); });
    }
}

void NetworkInterfaceTL::_removeAlarm(std::uint64_t id) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
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

void NetworkInterfaceTL::dropConnections(const HostAndPort& target, const Status& status) {
    if (MONGO_unlikely(!_initialized.load())) {
        return;
    }

    _clientFactory->dropConnections(target, status);
}

AsyncDBClient* NetworkInterfaceTL::LeasedStream::getClient() {
    return &_clientHandle->getClient();
}

void NetworkInterfaceTL::LeasedStream::indicateSuccess() {
    return _clientHandle->indicateSuccess();
}

void NetworkInterfaceTL::LeasedStream::indicateFailure(Status status) {
    _clientHandle->indicateFailure(status);
}

void NetworkInterfaceTL::LeasedStream::indicateUsed() {
    _clientHandle->indicateUsed();
}

SemiFuture<std::unique_ptr<NetworkInterface::LeasedStream>> NetworkInterfaceTL::leaseStream(
    const HostAndPort& hostAndPort, transport::ConnectSSLMode sslMode, Milliseconds timeout) {
    invariant(_initialized.load());

    return _clientFactory->lease(hostAndPort, sslMode, timeout)
        .thenRunOn(_reactor)
        .then([](auto conn) -> std::unique_ptr<NetworkInterface::LeasedStream> {
            auto ptr = std::make_unique<NetworkInterfaceTL::LeasedStream>(std::move(conn));
            return ptr;
        })
        .semi();
}

SemiFuture<std::shared_ptr<AsyncClientFactory::AsyncClientHandle>>
NetworkInterfaceTL::CommandStateBase::getClient(AsyncClientFactory& factory) {
    Status failPointStatus = Status::OK();
    forceConnectionNetworkTimeout.executeIf(
        [&](const BSONObj& data) {
            LOGV2(6496502,
                  "forceConnectionNetworkTimeout failpoint enabled, timing out request",
                  "request"_attr = request.cmdObj.toString());
            failPointStatus = {
                ErrorCodes::PooledConnectionAcquisitionExceededTimeLimit,
                "PooledConnectionAcquisitionExceededTimeLimit triggered via fail point."};
        },
        [&](const BSONObj& data) {
            return data["collectionNS"].valueStringData() ==
                request.cmdObj.firstElement().valueStringData();
        });
    if (!failPointStatus.isOK()) {
        return failPointStatus;
    }
    return factory.get(request.target, request.sslMode, request.timeout, cancelSource.token());
}

ExecutorFuture<RemoteCommandResponse> NetworkInterfaceTL::CommandStateBase::sendRequest(
    std::shared_ptr<AsyncClientFactory::AsyncClientHandle> retrievedClient) {
    retrievedClient->startAcquiredTimer();

    LOGV2_DEBUG(4630601,
                2,
                "Request acquired a connection",
                "requestId"_attr = request.id,
                "target"_attr = request.target);

    RemoteCommandRequest requestToSend = request;

    clientHandle = std::move(retrievedClient);

    if (interface->_svcCtx && requestToSend.timeout != RemoteCommandRequest::kNoTimeout &&
        WireSpec::getWireSpec(interface->_svcCtx).isInternalClient()) {
        BSONObjBuilder updatedCmdBuilder;
        updatedCmdBuilder.appendElements(request.cmdObj);
        updatedCmdBuilder.append("maxTimeMSOpOnly", request.timeout.count());
        requestToSend.cmdObj = updatedCmdBuilder.obj();

        LOGV2_DEBUG(4924402,
                    2,
                    "Set maxTimeMSOpOnly for request",
                    "maxTimeMSOpOnly"_attr = request.timeout,
                    "requestId"_attr = request.id,
                    "target"_attr = request.target);
    }

    networkInterfaceHangCommandsAfterAcquireConn.pauseWhileSet();

    LOGV2_DEBUG(4646300,
                2,
                "Sending request",
                "requestId"_attr = request.id,
                "target"_attr = request.target);

    if (auto counters = interface->_counters) {
        counters->recordSent();
    }

    setTimer();
    return sendRequestImpl(std::move(requestToSend));
}

Status NetworkInterfaceTL::CommandStateBase::handleClientAcquisitionError(Status status) {
    if (!ErrorCodes::isExceededTimeLimitError(status)) {
        return status;
    }

    auto connTimeoutWaitTime = stopwatch.elapsed();
    numConnectionNetworkTimeouts.increment(1);
    timeSpentWaitingBeforeConnectionTimeoutMillis.increment(
        durationCount<Milliseconds>(connTimeoutWaitTime));

    auto timeoutCode = request.timeoutCode;
    if (timeoutCode && connTimeoutWaitTime >= request.timeout) {
        status = Status(*timeoutCode, status.reason());
    }

    if (gEnableDetailedConnectionHealthMetricLogLines.load()) {
        LOGV2(6496500,
              "Operation timed out while waiting to acquire connection",
              "requestId"_attr = request.id,
              "duration"_attr = connTimeoutWaitTime);
    }

    return status;
}

ExecutorPtr NetworkInterfaceTL::CommandStateBase::makeGuaranteedExecutor() {
    return mongo::makeGuaranteedExecutor(baton, interface->_reactor);
}

ExecutorFuture<RemoteCommandResponse> NetworkInterfaceTL::_runCommand(
    std::shared_ptr<CommandStateBase> cmdState) {
    return cmdState->getClient(*_clientFactory)
        .thenRunOn(cmdState->makeGuaranteedExecutor())
        .onError([cmdState](Status status)
                     -> StatusWith<std::shared_ptr<AsyncClientFactory::AsyncClientHandle>> {
            return cmdState->handleClientAcquisitionError(status);
        })
        .then(
            [this, cmdState](std::shared_ptr<AsyncClientFactory::AsyncClientHandle> retrievedConn) {
                return cmdState->sendRequest(std::move(retrievedConn))
                    .then([cmdState](RemoteCommandResponse resp) {
                        cmdState->doMetadataHook(resp);
                        return resp;
                    })
                    .onError([this, cmdState](Status err) -> StatusWith<RemoteCommandResponse> {
                        if (auto opKey = cmdState->request.operationKey) {
                            _killOperation(cmdState.get());
                        }
                        return err;
                    });
            })
        .onCompletion([cmdState, this](StatusWith<RemoteCommandResponse> swResponse) {
            // If the command was cancelled for a reason, return a status that reflects that.
            if (swResponse == ErrorCodes::CallbackCanceled) {
                stdx::lock_guard<stdx::mutex> lk(cmdState->cancelMutex);
                if (!cmdState->cancelStatus.isOK()) {
                    swResponse = cmdState->cancelStatus;
                }
            }

            auto response = [&]() -> RemoteCommandResponse {
                if (swResponse.isOK()) {
                    return swResponse.getValue();
                } else {
                    return RemoteCommandResponse(cmdState->request.target,
                                                 std::move(swResponse.getStatus()),
                                                 cmdState->stopwatch.elapsed());
                }
            }();

            // The command has resolved one way or another.
            cmdState->timer->cancel(cmdState->baton);

            if (_counters) {
                // Increment our counters for the integration test
                _counters->recordResult(response.status);
            }

            networkInterfaceCommandsFailedWithErrorCode.shouldFail([&](const BSONObj& data) {
                const auto errorCode = data.getIntField("errorCode");
                if (errorCode != response.status.code()) {
                    return false;
                }

                const std::string requestCmdName =
                    cmdState->request.cmdObj.firstElement().fieldName();
                for (auto&& cmdName : data.getObjectField("cmdNames")) {
                    if (cmdName.type() == BSONType::string &&
                        cmdName.valueStringData() == requestCmdName) {
                        return true;
                    }
                }

                return false;
            });

            // The TransportLayer has, for historical reasons returned SocketException for network
            // errors, but sharding assumes HostUnreachable on network errors.
            if (response.status == ErrorCodes::SocketException) {
                response.status = Status(ErrorCodes::HostUnreachable, response.status.reason());
            }

            LOGV2_DEBUG(22597,
                        2,
                        "Request finished",
                        "requestId"_attr = cmdState->request.id,
                        "isOK"_attr = response.isOK(),
                        "response"_attr = redact(response.isOK() ? response.data.toString()
                                                                 : response.status.toString()));

            return response;
        });
}
}  // namespace executor
}  // namespace mongo
