/**
 *    Copyright (C) 2018 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
                        const RemoteCommandCompletionFn& onFinish,
                        const transport::BatonHandle& baton) override;

    void cancelCommand(const TaskExecutor::CallbackHandle& cbHandle,
                       const transport::BatonHandle& baton) override;
    Status setAlarm(Date_t when,
                    const stdx::function<void()>& action,
                    const transport::BatonHandle& baton) override;

    bool onNetworkThread() override;

    void dropConnections(const HostAndPort& hostAndPort) override;

private:
    struct CommandState {
        CommandState(RemoteCommandRequest request_, TaskExecutor::CallbackHandle cbHandle_)
            : request(std::move(request_)), cbHandle(std::move(cbHandle_)) {}

        RemoteCommandRequest request;
        TaskExecutor::CallbackHandle cbHandle;
        Date_t deadline = RemoteCommandRequest::kNoExpirationDate;
        Date_t start;

        struct Deleter {
            ConnectionPool::ConnectionHandleDeleter returner;
            transport::ReactorHandle reactor;

            void operator()(ConnectionPool::ConnectionInterface* ptr) const {
                reactor->schedule(transport::Reactor::kDispatch,
                                  [ ret = returner, ptr ] { ret(ptr); });
            }
        };
        using ConnHandle = std::unique_ptr<ConnectionPool::ConnectionInterface, Deleter>;

        ConnHandle conn;
        std::unique_ptr<transport::ReactorTimer> timer;

        AtomicBool done;
        Promise<RemoteCommandResponse> promise;
        Future<RemoteCommandResponse> mergedFuture;
    };

    void _eraseInUseConn(const TaskExecutor::CallbackHandle& handle);
    Future<RemoteCommandResponse> _onAcquireConn(std::shared_ptr<CommandState> state,
                                                 CommandState::ConnHandle conn,
                                                 const transport::BatonHandle& baton);

    std::string _instanceName;
    ServiceContext* _svcCtx;
    transport::TransportLayer* _tl;
    // Will be created if ServiceContext is null, or if no TransportLayer was configured at startup
    std::unique_ptr<transport::TransportLayer> _ownedTransportLayer;
    transport::ReactorHandle _reactor;

    mutable stdx::mutex _mutex;
    ConnectionPool::Options _connPoolOpts;
    std::unique_ptr<NetworkConnectionHook> _onConnectHook;
    std::unique_ptr<ConnectionPool> _pool;
    Counters _counters;

    std::unique_ptr<rpc::EgressMetadataHook> _metadataHook;
    AtomicBool _inShutdown;
    stdx::thread _ioThread;

    stdx::mutex _inProgressMutex;
    stdx::unordered_map<TaskExecutor::CallbackHandle, std::shared_ptr<CommandState>> _inProgress;
    stdx::unordered_set<std::shared_ptr<transport::ReactorTimer>> _inProgressAlarms;

    stdx::condition_variable _workReadyCond;
    bool _isExecutorRunnable = false;
};

}  // namespace executor
}  // namespace mongo
