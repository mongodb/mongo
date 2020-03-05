/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/server_ping_monitor.h"

#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"

namespace mongo {

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutor;
using executor::ThreadPoolTaskExecutor;
using CallbackArgs = TaskExecutor::CallbackArgs;
using CallbackHandle = TaskExecutor::CallbackHandle;

SingleServerPingMonitor::SingleServerPingMonitor(sdam::ServerAddress hostAndPort,
                                                 sdam::TopologyListener* rttListener,
                                                 Seconds pingFrequency,
                                                 std::shared_ptr<TaskExecutor> executor)
    : _hostAndPort(hostAndPort),
      _rttListener(rttListener),
      _pingFrequency(pingFrequency),
      _executor(executor) {}

void SingleServerPingMonitor::init() {
    _scheduleServerPing();
}

void SingleServerPingMonitor::drop() {
    stdx::lock_guard lk(_mutex);
    _isDropped = true;
    if (auto handle = std::exchange(_pingHandle, {})) {
        _executor->cancel(handle);
    }
}

template <typename Callback>
auto SingleServerPingMonitor::_scheduleWorkAt(Date_t when, Callback&& cb) const {
    auto wrappedCallback = [cb = std::forward<Callback>(cb),
                            anchor = shared_from_this()](const CallbackArgs& cbArgs) mutable {
        if (ErrorCodes::isCancelationError(cbArgs.status)) {
            return;
        }

        stdx::lock_guard lk(anchor->_mutex);
        if (anchor->_isDropped) {
            return;
        }
        cb(cbArgs);
    };
    return _executor->scheduleWorkAt(std::move(when), std::move(wrappedCallback));
}

void SingleServerPingMonitor::_scheduleServerPing() {
    auto schedulePingHandle = _scheduleWorkAt(
        _nextPingStartDate, [anchor = shared_from_this()](const CallbackArgs& cbArgs) mutable {
            anchor->_doServerPing();
        });

    stdx::lock_guard lk(_mutex);
    if (_isDropped) {
        return;
    }

    if (ErrorCodes::isShutdownError(schedulePingHandle.getStatus().code())) {
        LOGV2_DEBUG(23727,
                    1,
                    "Can't schedule ping for {hostAndPort}. Executor shutdown in progress",
                    "hostAndPort"_attr = _hostAndPort);
        return;
    }

    if (!schedulePingHandle.isOK()) {
        LOGV2_FATAL(23732,
                    "Can't continue scheduling pings to {hostAndPort} due to "
                    "{schedulePingHandle_getStatus}",
                    "hostAndPort"_attr = _hostAndPort,
                    "schedulePingHandle_getStatus"_attr = redact(schedulePingHandle.getStatus()));
        fassertFailed(31434);
    }

    _pingHandle = std::move(schedulePingHandle.getValue());
}

void SingleServerPingMonitor::_doServerPing() {
    // Compute _nextPingStartDate before making the request to ensure the next ping is scheduled
    // _pingFrequency in the future and independent of how long it takes to ping the server and get
    // a response.
    _nextPingStartDate = now() + _pingFrequency;

    // Ensure the ping request will timeout if it exceeds _pingFrequency time to complete.
    auto request = executor::RemoteCommandRequest(
        HostAndPort(_hostAndPort), "admin", BSON("ping" << 1), nullptr, _pingFrequency);

    auto remotePingHandle = _executor->scheduleRemoteCommand(
        std::move(request),
        [anchor = shared_from_this(),
         timer = Timer()](const executor::TaskExecutor::RemoteCommandCallbackArgs& result) mutable {
            if (ErrorCodes::isCancelationError(result.response.status)) {
                // Do no more work if the SingleServerPingMonitor is removed or the request is
                // canceled.
                return;
            }
            {
                stdx::lock_guard lk(anchor->_mutex);
                if (anchor->_isDropped) {
                    return;
                }

                if (!result.response.isOK()) {
                    anchor->_rttListener->onServerPingFailedEvent(anchor->_hostAndPort,
                                                                  result.response.status);
                } else {
                    auto rtt = sdam::IsMasterRTT(timer.micros());
                    anchor->_rttListener->onServerPingSucceededEvent(rtt, anchor->_hostAndPort);
                }
            }
            anchor->_scheduleServerPing();
        });

    if (ErrorCodes::isShutdownError(remotePingHandle.getStatus().code())) {
        LOGV2_DEBUG(23728,
                    1,
                    "Can't ping {hostAndPort}. Executor shutdown in progress",
                    "hostAndPort"_attr = _hostAndPort);
        return;
    }

    if (!remotePingHandle.isOK()) {
        LOGV2_FATAL(23733,
                    "Can't continue pinging {hostAndPort} due to {remotePingHandle_getStatus}",
                    "hostAndPort"_attr = _hostAndPort,
                    "remotePingHandle_getStatus"_attr = redact(remotePingHandle.getStatus()));
        fassertFailed(31435);
    }

    // Update the _pingHandle so the ping can be canceled if the SingleServerPingMonitor gets
    // dropped.
    _pingHandle = std::move(remotePingHandle.getValue());
}

ServerPingMonitor::ServerPingMonitor(sdam::TopologyListener* rttListener,
                                     Seconds pingFrequency,
                                     boost::optional<std::shared_ptr<TaskExecutor>> executor)
    : _rttListener(rttListener),
      _pingFrequency(pingFrequency),
      _executor(executor.get_value_or({})) {}

ServerPingMonitor::~ServerPingMonitor() {
    shutdown();
}

void ServerPingMonitor::shutdown() {
    decltype(_serverPingMonitorMap) serverPingMonitorMap;
    decltype(_executor) executor;
    {
        stdx::lock_guard<Latch> lk(_mutex);
        if (std::exchange(_isShutdown, true)) {
            return;
        }

        // Store _serverPingMonitorMap locally to prevent calling drop() on an already destroyed
        // SingleServerPingMonitor once the lock is released.
        serverPingMonitorMap = std::exchange(_serverPingMonitorMap, {});
        executor = std::exchange(_executor, {});
    }

    for (auto& [hostAndPort, singleMonitor] : serverPingMonitorMap) {
        singleMonitor->drop();
    }

    if (executor) {
        executor->shutdown();
        executor->join();
    }
}

void ServerPingMonitor::_setupTaskExecutor_inlock() {
    if (_isShutdown || _executor) {
        // Do not restart the _executor if it is in shutdown or already provided from a test.
        return;
    } else {
        auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
        auto net = executor::makeNetworkInterface(
            "ServerPingMonitor-TaskExecutor", nullptr, std::move(hookList));
        auto pool = std::make_unique<NetworkInterfaceThreadPool>(net.get());
        _executor = std::make_shared<ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
        _executor->startup();
    }
}

void ServerPingMonitor::onServerHandshakeCompleteEvent(sdam::IsMasterRTT durationMs,
                                                       const sdam::ServerAddress& address,
                                                       const BSONObj reply) {
    stdx::lock_guard lk(_mutex);
    uassert(ErrorCodes::ShutdownInProgress,
            str::stream() << "ServerPingMonitor is unable to start monitoring '" << address
                          << "' due to shutdown",
            !_isShutdown);

    _setupTaskExecutor_inlock();
    invariant(_serverPingMonitorMap.find(address) == _serverPingMonitorMap.end());
    auto newSingleMonitor =
        std::make_shared<SingleServerPingMonitor>(address, _rttListener, _pingFrequency, _executor);
    _serverPingMonitorMap[address] = newSingleMonitor;
    newSingleMonitor->init();
    LOGV2_DEBUG(
        23729, 1, "ServerPingMonitor is now monitoring {address}", "address"_attr = address);
}

void ServerPingMonitor::onServerClosedEvent(const sdam::ServerAddress& address, OID topologyId) {
    stdx::lock_guard lk(_mutex);
    if (_isShutdown) {
        LOGV2_DEBUG(23730,
                    1,
                    "ServerPingMonitor is in shutdown and will stop monitoring {address} if it has "
                    "not already done so.",
                    "address"_attr = address);
        return;
    }
    auto it = _serverPingMonitorMap.find(address);
    invariant(it != _serverPingMonitorMap.end());
    it->second->drop();
    _serverPingMonitorMap.erase(it);
    LOGV2_DEBUG(
        23731, 1, "ServerPingMonitor stopped  monitoring {address}", "address"_attr = address);
}


}  // namespace mongo
