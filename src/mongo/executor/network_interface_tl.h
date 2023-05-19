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

#include <boost/optional.hpp>

#include "mongo/client/async_client.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/connection_pool_tl.h"
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
    void appendStats(BSONObjBuilder&) const override;
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

    /**
     * NetworkInterfaceTL's implementation of a leased network-stream
     * provided for manual use outside of the NITL's usual RPC API.
     * When this type is destroyed, the destructor of the ConnectionHandle
     * member will return the connection to this NetworkInterface's ConnectionPool.
     */
    class LeasedStream : public NetworkInterface::LeasedStream {
    public:
        AsyncDBClient* getClient() override;

        LeasedStream(ConnectionPool::ConnectionHandle&& conn) : _conn{std::move(conn)} {}

        // These pass-through indications of the health of the leased
        // stream to the underlying ConnectionHandle
        void indicateSuccess() override;
        void indicateUsed() override;
        void indicateFailure(Status) override;

    private:
        ConnectionPool::ConnectionHandle _conn;
    };

    SemiFuture<std::unique_ptr<NetworkInterface::LeasedStream>> leaseStream(
        const HostAndPort& hostAndPort,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout) override;

private:
    struct RequestState;
    struct RequestManager;

    /**
     * For each logical RPC, an instance of `CommandState` is created to capture the state of the
     * remote command. As part of running a remote command, `NITL` sends out one or more requests
     * to the specified targets, and `RequestState` represents the state of each request.
     * `CommandState` owns a `RequestManager` that tracks individual requests. For each request sent
     * over the wire, `RequestManager` creates a `Context` that holds a weak pointer to the
     * `Request`, as well as the index of the target.
     */

    struct CommandStateBase : public std::enable_shared_from_this<CommandStateBase> {
        CommandStateBase(NetworkInterfaceTL* interface_,
                         RemoteCommandRequestOnAny request_,
                         const TaskExecutor::CallbackHandle& cbHandle_);
        virtual ~CommandStateBase() = default;

        /**
         * Use the current RequestState to send out a command request.
         */
        virtual Future<RemoteCommandResponse> sendRequest(
            std::shared_ptr<RequestState> requestState) = 0;

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

        /**
         * Return the maximum amount of requests that can come from this command.
         */
        size_t maxConcurrentRequests() const noexcept {
            if (!requestOnAny.options.hedgeOptions.isHedgeEnabled) {
                return 1;
            }

            return requestOnAny.options.hedgeOptions.hedgeCount + 1;
        }

        /**
         * Return the most connections we expect to be able to acquire.
         */
        size_t maxPossibleConns() const noexcept {
            return requestOnAny.target.size();
        }

        NetworkInterfaceTL* interface;

        RemoteCommandRequestOnAny requestOnAny;
        TaskExecutor::CallbackHandle cbHandle;
        Date_t deadline = kNoExpirationDate;

        ClockSource::StopWatch stopwatch;

        BatonHandle baton;
        std::unique_ptr<transport::ReactorTimer> timer;

        std::unique_ptr<RequestManager> requestManager;

        // TODO replace the finishLine with an atomic bool. It is no longer tracking allowed
        // failures accurately.
        StrongWeakFinishLine finishLine;

        boost::optional<UUID> operationKey;

        // Total time spent waiting for connections that eventually time out.
        Milliseconds connTimeoutWaitTime{0};
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

        Future<RemoteCommandResponse> sendRequest(
            std::shared_ptr<RequestState> requestState) override;

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

        Future<RemoteCommandResponse> sendRequest(
            std::shared_ptr<RequestState> requestState) override;

        void fulfillFinalPromise(StatusWith<RemoteCommandOnAnyResponse> response) override;

        void continueExhaustRequest(std::shared_ptr<RequestState> requestState,
                                    StatusWith<RemoteCommandResponse> swResponse);

        // Protects against race between reactor thread restarting stopwatch during exhaust
        // request and main thread reading stopwatch elapsed time during shutdown.
        Mutex stopwatchMutex = MONGO_MAKE_LATCH("NetworkInterfaceTL::ExhaustCommandState::mutex");

        Promise<void> promise;
        Promise<RemoteCommandResponse> finalResponsePromise;
        RemoteCommandOnReplyFn onReplyFn;
    };

    struct RequestManager {
        RequestManager(CommandStateBase* cmdState);

        void trySend(StatusWith<ConnectionPool::ConnectionHandle> swConn, size_t idx) noexcept;
        void cancelRequests();
        void killOperationsForPendingRequests();

        CommandStateBase* cmdState;

        /**
         * Holds context for individual requests, and is only valid if initialized.
         * `idx` maps the request to its target in the corresponding `cmdState`.
         */
        struct Context {
            bool initialized = false;
            size_t idx;
            std::weak_ptr<RequestState> request;
        };
        std::vector<Context> requests;

        Mutex mutex = MONGO_MAKE_LATCH("NetworkInterfaceTL::RequestManager::mutex");

        // Number of connections we've resolved.
        size_t connsResolved{0};

        // Number of sent requests.
        size_t sentIdx{0};

        // Set to true when the command finishes or is canceled to block remaining requests.
        bool isLocked{false};
    };

    struct RequestState final : public std::enable_shared_from_this<RequestState> {
        using ConnectionHandle = std::shared_ptr<ConnectionPool::ConnectionHandle::element_type>;
        using WeakConnectionHandle = std::weak_ptr<ConnectionPool::ConnectionHandle::element_type>;
        RequestState(RequestManager* mgr, std::shared_ptr<CommandStateBase> cmdState_)
            : cmdState{std::move(cmdState_)}, requestManager(mgr) {}

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

    Status _killOperation(CommandStateBase* cmdStateToKill, size_t idx);

    std::string _instanceName;
    ServiceContext* _svcCtx = nullptr;
    transport::TransportLayer* _tl = nullptr;
    // Will be created if ServiceContext is null, or if no TransportLayer was configured at startup
    std::unique_ptr<transport::TransportLayer> _ownedTransportLayer;
    transport::ReactorHandle _reactor;

    const ConnectionPool::Options _connPoolOpts;
    std::unique_ptr<NetworkConnectionHook> _onConnectHook;
    std::shared_ptr<ConnectionPool> _pool;

    class SynchronizedCounters;
    std::shared_ptr<SynchronizedCounters> _counters;

    std::unique_ptr<rpc::EgressMetadataHook> _metadataHook;

    // We start in kDefault, transition to kStarted after a call to startup completes.
    // Enter kStopping at the first call to shutdown and transition to kStopped
    // when the call completes.
    enum State {
        kDefault,
        kStarted,
        kStopping,
        kStopped,
    };

    friend StringData toString(State s) {
        return std::array{
            "Default"_sd,
            "Started"_sd,
            "Stopping"_sd,
            "Stopped"_sd,
        }
            .at(s);
    }

    // This condition variable is dedicated to block a thread calling this class
    // destructor, strictly when another thread is performing the network
    // interface shutdown which depends on the _ioThread termination and may
    // take an undeterministic amount of time to return.
    mutable stdx::mutex _stateMutex;  // NOLINT
    stdx::condition_variable _stoppedCV;
    State _state;

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
