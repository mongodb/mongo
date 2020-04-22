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

#pragma once

#include <deque>

#include "mongo/client/async_client.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/strong_weak_finish_line.h"

namespace mongo {
namespace executor {

class NetworkInterfaceTL : public NetworkInterface {
    static constexpr int kDiagnosticLogLevel = 4;

public:
    NetworkInterfaceTL(std::string instanceName,
                       ConnectionPool::Options connPoolOpts,
                       ServiceContext* ctx,
                       std::unique_ptr<NetworkConnectionHook> onConnectHook,
                       std::unique_ptr<rpc::EgressMetadataHook> metadataHook);
    ~NetworkInterfaceTL();

    constexpr static Milliseconds kCancelCommandTimeout{1000};

    std::string getDiagnosticString() override;
    void appendConnectionStats(ConnectionPoolStats* stats) const override;
    std::string getHostName() override;
    Counters getCounters() const override;

    void startup() override;
    void shutdown() override;
    bool inShutdown() const override;
    void waitForWork() override;
    void waitForWorkUntil(Date_t when) override;
    void signalWorkAvailable() override;
    Date_t now() override;
    Status startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                        RemoteCommandRequestOnAny& request,
                        RemoteCommandCompletionFn&& onFinish,
                        const BatonHandle& baton) override;
    Status startExhaustCommand(const TaskExecutor::CallbackHandle& cbHandle,
                               RemoteCommandRequestOnAny& request,
                               RemoteCommandOnReplyFn&& onReply,
                               const BatonHandle& baton) override;

    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                       const BatonHandle& baton) override;
    Status setAlarm(const TaskExecutor::CallbackHandle& cbHandle,
                    Date_t when,
                    unique_function<void(Status)> action) override;

    Status schedule(unique_function<void(Status)> action) override;

    void cancelAlarm(const TaskExecutor::CallbackHandle& cbHandle) override;

    bool onNetworkThread() override;

    void dropConnections(const HostAndPort& hostAndPort) override;

    void testEgress(const HostAndPort& hostAndPort,
                    transport::ConnectSSLMode sslMode,
                    Milliseconds timeout,
                    Status status) override;

private:
    struct RequestState;
    struct RequestManager;

    struct CommandStateBase : public std::enable_shared_from_this<CommandStateBase> {
        CommandStateBase(NetworkInterfaceTL* interface_,
                         RemoteCommandRequestOnAny request_,
                         const TaskExecutor::CallbackHandle& cbHandle_);
        virtual ~CommandStateBase() = default;

        /**
         * Use the current RequestState to send out a command request.
         */
        virtual Future<RemoteCommandResponse> sendRequest(size_t reqId) = 0;

        /**
         * Return the maximum number of request failures this Command can tolerate
         */
        virtual size_t maxRequestFailures() {
            return 1;
        }

        /**
         * Set a timer to fulfill the promise with a timeout error.
         */
        virtual void setTimer();

        /**
         * Fulfill the promise with the response.
         */
        virtual void fulfillFinalPromise(StatusWith<RemoteCommandOnAnyResponse> response) = 0;

        /**
         * Fulfill the promise for the Command.
         *
         * This will throw/invariant if called multiple times. In an ideal world, this would do the
         * swap on CommandState::done for you and return early if it was already true. It does not
         * do so currently.
         */
        void tryFinish(Status status) noexcept;

        /**
         * Run the NetworkInterface's MetadataHook on a given request if this Command isn't already
         * finished.
         */
        void doMetadataHook(const RemoteCommandOnAnyResponse& response);

        NetworkInterfaceTL* interface;

        RemoteCommandRequestOnAny requestOnAny;
        TaskExecutor::CallbackHandle cbHandle;
        Date_t deadline = kNoExpirationDate;

        ClockSource::StopWatch stopwatch;

        BatonHandle baton;
        std::unique_ptr<transport::ReactorTimer> timer;

        std::unique_ptr<RequestManager> requestManager;

        StrongWeakFinishLine finishLine;

        boost::optional<UUID> operationKey;
    };

    struct CommandState final : public CommandStateBase {
        CommandState(NetworkInterfaceTL* interface_,
                     RemoteCommandRequestOnAny request_,
                     const TaskExecutor::CallbackHandle& cbHandle_);
        ~CommandState() = default;

        // Create a new CommandState in a shared_ptr
        // Prefer this over raw construction
        static auto make(NetworkInterfaceTL* interface,
                         RemoteCommandRequestOnAny request,
                         const TaskExecutor::CallbackHandle& cbHandle);

        Future<RemoteCommandResponse> sendRequest(size_t reqId) override;

        void fulfillFinalPromise(StatusWith<RemoteCommandOnAnyResponse> response) override;

        Promise<RemoteCommandOnAnyResponse> promise;

        const size_t hedgeCount;
    };

    struct ExhaustCommandState final : public CommandStateBase {
        ExhaustCommandState(NetworkInterfaceTL* interface_,
                            RemoteCommandRequestOnAny request_,
                            const TaskExecutor::CallbackHandle& cbHandle_,
                            RemoteCommandOnReplyFn&& onReply_);
        virtual ~ExhaustCommandState() = default;

        // Create a new ExhaustCommandState in a shared_ptr
        // Prefer this over raw construction
        static auto make(NetworkInterfaceTL* interface,
                         RemoteCommandRequestOnAny request,
                         const TaskExecutor::CallbackHandle& cbHandle,
                         RemoteCommandOnReplyFn&& onReply);

        Future<RemoteCommandResponse> sendRequest(size_t reqId) override;

        void fulfillFinalPromise(StatusWith<RemoteCommandOnAnyResponse> response) override;

        void continueExhaustRequest(std::shared_ptr<RequestState> requestState,
                                    StatusWith<RemoteCommandResponse> swResponse);

        Promise<void> promise;
        Promise<RemoteCommandResponse> finalResponsePromise;
        RemoteCommandOnReplyFn onReplyFn;
    };

    enum class ConnStatus { Unset, OK, Failed };

    struct RequestManager {
        RequestManager(size_t numHedges, std::shared_ptr<CommandStateBase> cmdState_)
            : connStatus(cmdState_->requestOnAny.target.size(), ConnStatus::Unset),
              requests(numHedges),
              cmdState(cmdState_){};

        std::shared_ptr<RequestState> makeRequest();
        std::shared_ptr<RequestState> getRequest(size_t reqId);
        std::shared_ptr<RequestState> getNextRequest();

        void trySend(StatusWith<ConnectionPool::ConnectionHandle> swConn, size_t idx) noexcept;
        void cancelRequests();
        void killOperationsForPendingRequests();

        bool sentNone() const;
        bool sentAll() const;

        ConnStatus getConnStatus(size_t reqId);
        bool usedAllConn() const;

        std::vector<ConnStatus> connStatus;
        std::vector<std::weak_ptr<RequestState>> requests;
        std::weak_ptr<CommandStateBase> cmdState;

        // Number of sent requests.
        AtomicWord<size_t> sentIdx{0};

        // Number of requests to send.
        AtomicWord<size_t> requestCount{0};

        // Set to true when the command finishes or is canceled to block remaining requests.
        bool isLocked{false};

        Mutex mutex = MONGO_MAKE_LATCH("NetworkInterfaceTL::RequestManager::mutex");
    };

    struct RequestState final : public std::enable_shared_from_this<RequestState> {
        using ConnectionHandle = std::shared_ptr<ConnectionPool::ConnectionHandle::element_type>;
        using WeakConnectionHandle = std::weak_ptr<ConnectionPool::ConnectionHandle::element_type>;
        RequestState(RequestManager* mgr, std::shared_ptr<CommandStateBase> cmdState_, size_t id)
            : cmdState{std::move(cmdState_)}, requestManager(mgr), reqId(id) {}

        ~RequestState();

        /**
         * Return the client for a given connection
         */
        static AsyncDBClient* getClient(const ConnectionHandle& conn) noexcept;

        /**
         * Cancel the current client operation or do nothing if there is no client.
         */
        void cancel() noexcept;

        /**
         * Return the current connection to the pool and unset it locally.
         *
         * This must be called from the networking thread (i.e. the reactor).
         */
        void returnConnection(Status status) noexcept;

        /**
         * Attempt to send a request using the given connection
         */
        void trySend(StatusWith<ConnectionPool::ConnectionHandle> swConn, size_t idx) noexcept;

        void send(StatusWith<ConnectionPool::ConnectionHandle> swConn,
                  RemoteCommandRequest remoteCommandRequest) noexcept;

        /**
         * Resolve an eventual response
         */
        void resolve(Future<RemoteCommandResponse> future) noexcept;

        NetworkInterfaceTL* interface() noexcept {
            return cmdState->interface;
        }

        std::shared_ptr<CommandStateBase> cmdState;

        ClockSource::StopWatch stopwatch;

        RequestManager* const requestManager{nullptr};

        boost::optional<RemoteCommandRequest> request;
        HostAndPort host;
        ConnectionHandle conn;
        WeakConnectionHandle weakConn;

        // Internal id of this request as tracked by the RequestManager.
        size_t reqId;

        // True if this request is an additional request sent to hedge the operation.
        bool isHedge{false};

        // Set to true if the response to the request is used to fulfill the command's
        // promise (i.e. arrives before the responses to all other requests and is not
        // a MaxTimeMSExpired error response if this is a hedged request).
        bool fulfilledPromise{false};
    };

    struct AlarmState {
        AlarmState(Date_t when_,
                   TaskExecutor::CallbackHandle cbHandle_,
                   std::unique_ptr<transport::ReactorTimer> timer_,
                   Promise<void> promise_)
            : cbHandle(std::move(cbHandle_)),
              when(when_),
              timer(std::move(timer_)),
              promise(std::move(promise_)) {}

        TaskExecutor::CallbackHandle cbHandle;
        Date_t when;
        std::unique_ptr<transport::ReactorTimer> timer;

        AtomicWord<bool> done;
        Promise<void> promise;
    };

    void _shutdownAllAlarms();
    void _answerAlarm(Status status, std::shared_ptr<AlarmState> state);

    void _run();

    Status _killOperation(std::shared_ptr<RequestState> requestStateToKill);

    std::string _instanceName;
    ServiceContext* _svcCtx = nullptr;
    transport::TransportLayer* _tl = nullptr;
    // Will be created if ServiceContext is null, or if no TransportLayer was configured at startup
    std::unique_ptr<transport::TransportLayer> _ownedTransportLayer;
    transport::ReactorHandle _reactor;

    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(3), "NetworkInterfaceTL::_mutex");
    ConnectionPool::Options _connPoolOpts;
    std::unique_ptr<NetworkConnectionHook> _onConnectHook;
    std::shared_ptr<ConnectionPool> _pool;

    class SynchronizedCounters;
    std::shared_ptr<SynchronizedCounters> _counters;

    std::unique_ptr<rpc::EgressMetadataHook> _metadataHook;

    // We start in kDefault, transition to kStarted after startup() is complete and enter kStopped
    // at the first call to shutdown()
    enum State : int {
        kDefault,
        kStarted,
        kStopped,
    };
    AtomicWord<State> _state;
    stdx::thread _ioThread;

    Mutex _inProgressMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "NetworkInterfaceTL::_inProgressMutex");
    stdx::unordered_map<TaskExecutor::CallbackHandle, std::weak_ptr<CommandStateBase>> _inProgress;

    bool _inProgressAlarmsInShutdown = false;
    stdx::unordered_map<TaskExecutor::CallbackHandle, std::shared_ptr<AlarmState>>
        _inProgressAlarms;

    stdx::condition_variable _workReadyCond;
    bool _isExecutorRunnable = false;
};

}  // namespace executor
}  // namespace mongo
