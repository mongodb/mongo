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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/server_ping_monitor.h"

#include "mongo/client/sdam/sdam.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(serverPingMonitorFailWithHostUnreachable);

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutor;
using executor::ThreadPoolTaskExecutor;
using CallbackArgs = TaskExecutor::CallbackArgs;
using CallbackHandle = TaskExecutor::CallbackHandle;

SingleServerPingMonitor::SingleServerPingMonitor(const MongoURI& setUri,
                                                 const sdam::ServerAddress& hostAndPort,
                                                 sdam::TopologyListener* rttListener,
                                                 Milliseconds pingFrequency,
                                                 std::shared_ptr<TaskExecutor> executor)
    : _setUri(setUri),
      _hostAndPort(hostAndPort),
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
                    kLogLevel,
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
    request.sslMode = _setUri.getSSLMode();

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

                if (MONGO_unlikely(serverPingMonitorFailWithHostUnreachable.shouldFail(
                        [&](const BSONObj& data) {
                            return anchor->_hostAndPort == data.getStringField("hostAndPort");
                        }))) {
                    const std::string reason = str::stream()
                        << "Failing the ping command to " << (anchor->_hostAndPort);
                    anchor->_rttListener->onServerPingFailedEvent(
                        anchor->_hostAndPort, {ErrorCodes::HostUnreachable, reason});
                } else if (!result.response.isOK()) {
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
                    kLogLevel,
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

ServerPingMonitor::ServerPingMonitor(const MongoURI& setUri,
                                     sdam::TopologyListener* rttListener,
                                     Milliseconds pingFrequency,
                                     std::shared_ptr<TaskExecutor> executor)
    : _setUri(setUri),
      _rttListener(rttListener),
      _pingFrequency(pingFrequency),
      _executor(executor) {}

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
}

void ServerPingMonitor::onServerHandshakeCompleteEvent(sdam::IsMasterRTT durationMs,
                                                       const sdam::ServerAddress& address,
                                                       const BSONObj reply) {
    stdx::lock_guard lk(_mutex);
    if (_isShutdown) {
        return;
    }

    if (_serverPingMonitorMap.find(address) != _serverPingMonitorMap.end()) {
        LOGV2_DEBUG(466811,
                    kLogLevel + 1,
                    "ServerPingMonitor already monitoring {address}",
                    "address"_attr = address);
        return;
    }
    auto newSingleMonitor = std::make_shared<SingleServerPingMonitor>(
        _setUri, address, _rttListener, _pingFrequency, _executor);
    _serverPingMonitorMap[address] = newSingleMonitor;
    newSingleMonitor->init();
    LOGV2_DEBUG(23729,
                kLogLevel,
                "ServerPingMonitor is now monitoring {address}",
                "address"_attr = address);
}

void ServerPingMonitor::onTopologyDescriptionChangedEvent(
    UUID topologyId,
    sdam::TopologyDescriptionPtr previousDescription,
    sdam::TopologyDescriptionPtr newDescription) {
    stdx::lock_guard lk(_mutex);
    if (_isShutdown) {
        return;
    }

    // Remove monitors that are missing from the topology.
    auto it = _serverPingMonitorMap.begin();
    while (it != _serverPingMonitorMap.end()) {
        const auto& serverAddress = it->first;
        if (newDescription->findServerByAddress(serverAddress) == boost::none) {
            auto& singleMonitor = _serverPingMonitorMap[serverAddress];
            singleMonitor->drop();
            LOGV2_DEBUG(462899,
                        kLogLevel,
                        "ServerPingMonitor for host {addr} was removed from being monitored.",
                        "addr"_attr = serverAddress);
            it = _serverPingMonitorMap.erase(it, ++it);
        } else {
            ++it;
        }
    }
}

}  // namespace mongo
