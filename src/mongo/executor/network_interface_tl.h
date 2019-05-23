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
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/transport_layer.h"

namespace mongo {
namespace executor {

class NetworkInterfaceTL : public NetworkInterface {
public:
    NetworkInterfaceTL(std::string instanceName,
                       ConnectionPool::Options connPoolOpts,
                       ServiceContext* ctx,
                       std::unique_ptr<NetworkConnectionHook> onConnectHook,
                       std::unique_ptr<rpc::EgressMetadataHook> metadataHook);

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
                        RemoteCommandRequest& request,
                        RemoteCommandCompletionFn&& onFinish,
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

private:
    struct CommandState {
        CommandState(NetworkInterfaceTL* interface_,
                     RemoteCommandRequest request_,
                     const TaskExecutor::CallbackHandle& cbHandle_,
                     Promise<RemoteCommandResponse> promise_);
        ~CommandState();

        // Create a new CommandState in a shared_ptr
        // Prefer this over raw construction
        static auto make(NetworkInterfaceTL* interface,
                         RemoteCommandRequest request,
                         const TaskExecutor::CallbackHandle& cbHandle,
                         Promise<RemoteCommandResponse> promise);

        NetworkInterfaceTL* interface;

        RemoteCommandRequest request;
        TaskExecutor::CallbackHandle cbHandle;
        Date_t deadline = RemoteCommandRequest::kNoExpirationDate;
        Date_t start;

        ConnectionPool::ConnectionHandle conn;
        std::unique_ptr<transport::ReactorTimer> timer;

        AtomicWord<bool> done;
        Promise<RemoteCommandResponse> promise;
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

        Promise<void> promise;
    };

    void _cancelAllAlarms();
    void _answerAlarm(Status status, std::shared_ptr<AlarmState> state);

    void _run();
    void _onAcquireConn(std::shared_ptr<CommandState> state,
                        ConnectionPool::ConnectionHandle conn,
                        const BatonHandle& baton);

    std::string _instanceName;
    ServiceContext* _svcCtx;
    transport::TransportLayer* _tl;
    // Will be created if ServiceContext is null, or if no TransportLayer was configured at startup
    std::unique_ptr<transport::TransportLayer> _ownedTransportLayer;
    transport::ReactorHandle _reactor;

    mutable stdx::mutex _mutex;
    ConnectionPool::Options _connPoolOpts;
    std::unique_ptr<NetworkConnectionHook> _onConnectHook;
    std::shared_ptr<ConnectionPool> _pool;
    Counters _counters;

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

    stdx::mutex _inProgressMutex;
    stdx::unordered_map<TaskExecutor::CallbackHandle, std::weak_ptr<CommandState>> _inProgress;
    stdx::unordered_map<TaskExecutor::CallbackHandle, std::shared_ptr<AlarmState>>
        _inProgressAlarms;

    stdx::condition_variable _workReadyCond;
    bool _isExecutorRunnable = false;
};

}  // namespace executor
}  // namespace mongo
