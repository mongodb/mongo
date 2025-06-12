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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/async_client.h"
#include "mongo/db/baton.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/duration.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <array>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {
namespace executor {

class NetworkInterfaceTL : public NetworkInterface {
public:
    NetworkInterfaceTL(std::string instanceName,
                       std::shared_ptr<AsyncClientFactory> factory,
                       std::unique_ptr<rpc::EgressMetadataHook> metadataHook);
    ~NetworkInterfaceTL() override;

    constexpr static Milliseconds kCancelCommandTimeout{1000};
    constexpr static Milliseconds kCancelCommandTimeout_forTest{5000};

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
    SemiFuture<TaskExecutor::ResponseStatus> startCommand(
        const TaskExecutor::CallbackHandle& cbHandle,
        RemoteCommandRequest& request,
        const BatonHandle& baton,
        const CancellationToken& token) override;
    SemiFuture<std::shared_ptr<NetworkInterface::ExhaustResponseReader>> startExhaustCommand(
        const TaskExecutor::CallbackHandle& cbHandle,
        RemoteCommandRequest& request,
        const BatonHandle& baton,
        const CancellationToken& cancelToken) override;

    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                       const BatonHandle& baton) override;
    SemiFuture<void> setAlarm(
        Date_t when, const CancellationToken& token = CancellationToken::uncancelable()) override;

    Status schedule(unique_function<void(Status)> action) override;

    bool onNetworkThread() override;

    void dropConnections(const HostAndPort& target, const Status& status) override;

    void testEgress(const HostAndPort& hostAndPort,
                    transport::ConnectSSLMode sslMode,
                    Milliseconds timeout,
                    Status status) override;

    const AsyncClientFactory& getClientFactory_forTest() const {
        return *_clientFactory;
    }

    /**
     * NetworkInterfaceTL's implementation of a leased network-stream
     * provided for manual use outside of the NITL's usual RPC API.
     * When this type is destroyed, the destructor of the AsyncClientHandle
     * member will return the connection to this NetworkInterface's AsyncClientFactory.
     */
    class LeasedStream : public NetworkInterface::LeasedStream {
    public:
        AsyncDBClient* getClient() override;

        LeasedStream(std::shared_ptr<AsyncClientFactory::AsyncClientHandle> client)
            : _clientHandle{std::move(client)} {}

        // These pass-through indications of the health of the leased
        // stream to the underlying AsyncClientFactory.
        void indicateSuccess() override;
        void indicateUsed() override;
        void indicateFailure(Status) override;

    private:
        std::shared_ptr<AsyncClientFactory::AsyncClientHandle> _clientHandle;
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
                         const BatonHandle& baton_,
                         const CancellationToken& token);
        virtual ~CommandStateBase();

        SemiFuture<std::shared_ptr<AsyncClientFactory::AsyncClientHandle>> getClient(
            AsyncClientFactory& factory);

        Status handleClientAcquisitionError(Status status);

        ExecutorFuture<RemoteCommandResponse> sendRequest(
            std::shared_ptr<AsyncClientFactory::AsyncClientHandle> client);
        virtual ExecutorFuture<RemoteCommandResponse> sendRequestImpl(
            RemoteCommandRequest toSend) = 0;

        /**
         * Release the current client handle back to its factory.
         *
         * This must be called from the networking thread (i.e. the reactor).
         */
        void releaseClientHandle(Status status);

        /**
         * Set a timer to cancel the request at the requested deadline, if any.
         * If no timeout was specified on the request, this is a noop.
         */
        void setTimer();

        /**
         * Cancel the operation with the provided status.
         */
        void cancel(Status status);

        /**
         * Run the NetworkInterface's MetadataHook on a given request if this Command isn't already
         * finished.
         */
        void doMetadataHook(const RemoteCommandResponse& response);

        /**
         * Returns a GuaranteedExecutor that schedules work on the Baton associated with this
         * CommandStateBase if available or on the reactor otherwise.
         */
        ExecutorPtr makeGuaranteedExecutor();

        NetworkInterfaceTL* interface;

        // Original request as received from the caller.
        const RemoteCommandRequest request;

        TaskExecutor::CallbackHandle cbHandle;

        ClockSource::StopWatch stopwatch;
        Date_t deadline = kNoExpirationDate;

        BatonHandle baton;
        std::unique_ptr<transport::ReactorTimer> timer;

        std::shared_ptr<AsyncClientFactory::AsyncClientHandle> clientHandle;

        CancellationSource cancelSource;

        stdx::mutex cancelMutex;
        // Overwrites the generic cancellation status when the operation is cancelled.
        Status cancelStatus = Status::OK();
    };

    struct CommandState final : public CommandStateBase {
        using CommandStateBase::CommandStateBase;
        ~CommandState() override = default;
        ExecutorFuture<RemoteCommandResponse> sendRequestImpl(RemoteCommandRequest toSend) override;
    };

    struct ExhaustCommandState final : public CommandStateBase {
        using CommandStateBase::CommandStateBase;
        ~ExhaustCommandState() override = default;

        ExecutorFuture<RemoteCommandResponse> sendRequestImpl(RemoteCommandRequest toSend) override;

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

    Status _verifyRunning() const;

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
    transport::ReactorHandle _reactor;
    std::shared_ptr<AsyncClientFactory> _clientFactory;

    // A lock-free way to check that the reactor and connection pool have been initialized. We need
    // this and the _state enum to prevent null pointer dereferencing on the hot path without
    // acquiring an extra lock.
    Atomic<bool> _initialized{false};

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

    // Guards _svcCtx, _state, _inProgress, and _inProgressAlarms.
    mutable stdx::mutex _mutex;

    ServiceContext* _svcCtx = nullptr;

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
