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

#include <array>
#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/async_client.h"
#include "mongo/db/baton.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/connection_pool_tl.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/strong_weak_finish_line.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"


namespace mongo {
namespace executor {

class NetworkInterfaceTL : public NetworkInterface {
public:
    NetworkInterfaceTL(std::string instanceName,
                       ConnectionPool::Options connPoolOpts,
                       ServiceContext* ctx,
                       std::unique_ptr<NetworkConnectionHook> onConnectHook,
                       std::unique_ptr<rpc::EgressMetadataHook> metadataHook);
    ~NetworkInterfaceTL() override;

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
                        RemoteCommandRequest& request,
                        RemoteCommandCompletionFn&& onFinish,
                        const BatonHandle& baton) override;
    Status startExhaustCommand(const TaskExecutor::CallbackHandle& cbHandle,
                               RemoteCommandRequest& request,
                               RemoteCommandOnReplyFn&& onReply,
                               const BatonHandle& baton) override;

    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                       const BatonHandle& baton) override;
    SemiFuture<void> setAlarm(
        Date_t when, const CancellationToken& token = CancellationToken::uncancelable()) override;

    Status schedule(unique_function<void(Status)> action) override;

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
    /**
     * For an RPC, an instance of `CommandState` is created to capture the state of the
     * remote command. As part of running a remote command, `NITL` sends out a request
     * to the specified target.
     */

    struct CommandStateBase : public std::enable_shared_from_this<CommandStateBase> {
        CommandStateBase(NetworkInterfaceTL* interface_,
                         RemoteCommandRequest request_,
                         const TaskExecutor::CallbackHandle& cbHandle_,
                         const BatonHandle& baton_);
        virtual ~CommandStateBase();

        using ConnectionHandle = std::shared_ptr<ConnectionPool::ConnectionHandle::element_type>;
        using WeakConnectionHandle = std::weak_ptr<ConnectionPool::ConnectionHandle::element_type>;

        SemiFuture<ConnectionPool::ConnectionHandle> getConnection(
            std::shared_ptr<ConnectionPool> pool);

        Status handleConnectionAcquisitionError(Status status);

        ExecutorFuture<RemoteCommandResponse> sendRequest(ConnectionPool::ConnectionHandle conn);
        virtual Future<RemoteCommandResponse> sendRequestImpl(RemoteCommandRequest toSend) = 0;

        /**
         * Return the current connection to the pool and unset it locally.
         *
         * This must be called from the networking thread (i.e. the reactor).
         */
        void returnConnection(Status status) noexcept;

        /**
         * Set a timer to cancel the request at the requested deadline, if any.
         * If no timeout was specified on the request, this is a noop.
         */
        void setTimer();

        /**
         * Return the client for a given connection
         */
        static AsyncDBClient* getClient(const ConnectionHandle& conn) noexcept;

        /**
         * Cancel the operation with the provided status.
         */
        void cancel(Status status);

        /**
         * Run the NetworkInterface's MetadataHook on a given request if this Command isn't already
         * finished.
         */
        Status doMetadataHook(const RemoteCommandResponse& response);

        NetworkInterfaceTL* interface;

        // Original request as received from the caller.
        const RemoteCommandRequest request;

        TaskExecutor::CallbackHandle cbHandle;

        ClockSource::StopWatch stopwatch;
        Date_t deadline = kNoExpirationDate;

        BatonHandle baton;
        std::unique_ptr<transport::ReactorTimer> timer;

        ConnectionHandle conn;

        CancellationSource _cancelSource;

        stdx::mutex cancelMutex;
        // Overwrites the generic cancellation status when the operation is cancelled.
        Status cancelStatus = Status::OK();
    };

    struct CommandState final : public CommandStateBase {
        CommandState(NetworkInterfaceTL* interface,
                     RemoteCommandRequest request,
                     const TaskExecutor::CallbackHandle& cbHandle,
                     const BatonHandle& baton)
            : CommandStateBase(interface, std::move(request), cbHandle, baton) {}
        ~CommandState() override = default;

        Future<RemoteCommandResponse> sendRequestImpl(RemoteCommandRequest toSend) override;
    };

    struct ExhaustCommandState final : public CommandStateBase {
        ExhaustCommandState(NetworkInterfaceTL* interface,
                            RemoteCommandRequest request,
                            const TaskExecutor::CallbackHandle& cbHandle,
                            const BatonHandle& baton,
                            RemoteCommandOnReplyFn&& onReply)
            : CommandStateBase(interface, std::move(request), cbHandle, baton),
              onReplyFn(std::move(onReply)) {}
        ~ExhaustCommandState() override = default;

        Future<RemoteCommandResponse> sendRequestImpl(RemoteCommandRequest toSend) override;

        void continueExhaustRequest(StatusWith<RemoteCommandResponse> swResponse);

        // Protects against race between reactor thread restarting stopwatch during exhaust
        // request and main thread reading stopwatch elapsed time during shutdown.
        stdx::mutex stopwatchMutex;

        Promise<RemoteCommandResponse> finalResponsePromise;
        RemoteCommandOnReplyFn onReplyFn;
    };

    struct AlarmState {
        AlarmState(NetworkInterfaceTL* interface_,
                   std::uint64_t id_,
                   std::unique_ptr<transport::ReactorTimer> timer_,
                   const CancellationToken& token)
            : interface(interface_), id(id_), timer(std::move(timer_)), source(token) {}
        ~AlarmState() {
            interface->_removeAlarm(id);
        }

        NetworkInterfaceTL* interface;
        std::uint64_t id;
        std::unique_ptr<transport::ReactorTimer> timer;
        CancellationSource source;
    };

    bool _inShutdown_inlock(WithLock lk) const;

    void _shutdownAllAlarms();

    void _run();

    void _killOperation(CommandStateBase* cmdStateToKill);

    /**
     * Adds the provided cmdState to the list of in-progress commands.
     * Throws an exception and does not add the state to the list if shutdown has started or
     * completed.
     */
    void _registerCommand(const TaskExecutor::CallbackHandle& cbHandle,
                          std::shared_ptr<CommandStateBase> cmdState);
    /**
     * Removes the provided cmdState to the list of in-progress commands.
     * This is invoked by the CommandStateBase destructor.
     * Has no effect if the command was never registered.
     */
    void _unregisterCommand(const TaskExecutor::CallbackHandle& cbHandle);

    /**
     * Removes an alarm from the in progress alarms by its ID.
     */
    void _removeAlarm(std::uint64_t id);

    ExecutorFuture<RemoteCommandResponse> _runCommand(std::shared_ptr<CommandStateBase> cmdState);

    std::string _instanceName;
    ServiceContext* _svcCtx = nullptr;
    transport::TransportLayerManager* _tl = nullptr;
    // Will be created if ServiceContext is null, or if no TransportLayer was configured at startup.
    std::unique_ptr<transport::TransportLayerManager> _ownedTransportLayer;
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

    // Guards _state, _inProgress, and _inProgressAlarms.
    mutable Mutex _mutex;

    // This condition variable is dedicated to block a thread calling this class
    // destructor, strictly when another thread is performing the network
    // interface shutdown which depends on the _ioThread termination and may
    // take an undeterministic amount of time to return.
    stdx::condition_variable _stoppedCV;
    State _state;

    stdx::thread _ioThread;

    // New entries cannot be added once shutdown has begun.
    // CommandStateBase instances will remove themselves from this map upon destruction.
    // shutdown() will block until this list is empty.
    stdx::unordered_map<TaskExecutor::CallbackHandle, std::weak_ptr<CommandStateBase>> _inProgress;

    AtomicWord<std::uint64_t> nextAlarmId{0};
    stdx::unordered_map<std::uint64_t, std::weak_ptr<AlarmState>> _inProgressAlarms;

    stdx::condition_variable _workReadyCond;
    bool _isExecutorRunnable = false;
};
}  // namespace executor
}  // namespace mongo
