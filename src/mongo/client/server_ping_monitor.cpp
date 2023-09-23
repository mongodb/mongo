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


#include <boost/optional.hpp>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/server_ping_monitor.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

MONGO_FAIL_POINT_DEFINE(serverPingMonitorFailWithHostUnreachable);
MONGO_FAIL_POINT_DEFINE(serverPingMonitorSetRTT);

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutor;
using executor::ThreadPoolTaskExecutor;
using CallbackArgs = TaskExecutor::CallbackArgs;
using CallbackHandle = TaskExecutor::CallbackHandle;

SingleServerPingMonitor::SingleServerPingMonitor(const MongoURI& setUri,
                                                 const HostAndPort& hostAndPort,
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
    if (std::exchange(_isDropped, true)) {
        return;
    }
    if (auto handle = std::exchange(_pingHandle, {})) {
        _executor->cancel(handle);
    }
}

template <typename Callback>
auto SingleServerPingMonitor::_scheduleWorkAt(Date_t when, Callback&& cb) const {
    auto wrappedCallback = [cb = std::forward<Callback>(cb),
                            anchor = shared_from_this()](const CallbackArgs& cbArgs) mutable {
        if (ErrorCodes::isCancellationError(cbArgs.status)) {
            LOGV2(7926101,
                  "ServerPingMonitor stopping pings to host because request was cancelled",
                  "host"_attr = anchor->_hostAndPort,
                  "status"_attr = cbArgs.status);
            return;
        }

        stdx::lock_guard lk(anchor->_mutex);
        if (anchor->_isDropped) {
            LOGV2(7926102,
                  "ServerPingMonitor stopping pings to host because the component was shutdown",
                  "host"_attr = anchor->_hostAndPort);
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
                    "Can't schedule ping for {host}. Executor shutdown in progress",
                    "Can't schedule ping for host. Executor shutdown in progress",
                    "host"_attr = _hostAndPort,
                    "replicaSet"_attr = _setUri.getSetName());
        return;
    }

    if (!schedulePingHandle.isOK()) {
        LOGV2_FATAL(23732,
                    "Can't continue scheduling pings to {host} due to {error}",
                    "Can't continue scheduling pings to host",
                    "host"_attr = _hostAndPort,
                    "error"_attr = redact(schedulePingHandle.getStatus()),
                    "replicaSet"_attr = _setUri.getSetName());
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
    auto request = executor::RemoteCommandRequest(HostAndPort(_hostAndPort),
                                                  DatabaseName::kAdmin,
                                                  BSON("ping" << 1),
                                                  nullptr,
                                                  _pingFrequency);
    request.sslMode = _setUri.getSSLMode();

    auto remotePingHandle = _executor->scheduleRemoteCommand(
        std::move(request),
        [anchor = shared_from_this(),
         timer = Timer()](const executor::TaskExecutor::RemoteCommandCallbackArgs& result) mutable {
            if (ErrorCodes::isCancellationError(result.response.status)) {
                // Do no more work if the SingleServerPingMonitor is removed or the request is
                // canceled.
                LOGV2(7926103,
                      "ServerPingMonitor stopping pings to host because monitor was removed or "
                      "request was cancelled",
                      "host"_attr = anchor->_hostAndPort,
                      "status"_attr = result.response.status);
                return;
            }
            {
                stdx::lock_guard lk(anchor->_mutex);
                int rttValue = 0;
                if (anchor->_isDropped) {
                    LOGV2(7926104,
                          "ServerPingMonitor stopping pings to host because the component was "
                          "shutdown",
                          "host"_attr = anchor->_hostAndPort);
                    return;
                }

                if (MONGO_unlikely(serverPingMonitorFailWithHostUnreachable.shouldFail(
                        [&](const BSONObj& data) {
                            return anchor->_hostAndPort.toString() ==
                                data.getStringField("hostAndPort");
                        }))) {
                    const std::string reason = str::stream()
                        << "Failing the ping command to " << (anchor->_hostAndPort);
                    anchor->_rttListener->onServerPingFailedEvent(
                        anchor->_hostAndPort, {ErrorCodes::HostUnreachable, reason});
                } else if (MONGO_unlikely(
                               serverPingMonitorSetRTT.shouldFail([&](const BSONObj& data) {
                                   if (data.hasField(anchor->_hostAndPort.toString())) {
                                       rttValue = data.getIntField(anchor->_hostAndPort.toString());
                                       return true;
                                   }
                                   return false;
                               }))) {
                    anchor->_rttListener->onServerPingSucceededEvent(Microseconds(rttValue),
                                                                     anchor->_hostAndPort);
                } else if (!result.response.isOK()) {
                    anchor->_rttListener->onServerPingFailedEvent(anchor->_hostAndPort,
                                                                  result.response.status);
                } else {
                    auto rtt = Microseconds(timer.micros());
                    anchor->_rttListener->onServerPingSucceededEvent(rtt, anchor->_hostAndPort);
                }
            }
            anchor->_scheduleServerPing();
        });

    if (ErrorCodes::isShutdownError(remotePingHandle.getStatus().code())) {
        LOGV2_DEBUG(23728,
                    kLogLevel,
                    "Can't ping {host}. Executor shutdown in progress",
                    "Can't ping host. Executor shutdown in progress",
                    "host"_attr = _hostAndPort,
                    "replicaSet"_attr = _setUri.getSetName());
        return;
    }

    if (!remotePingHandle.isOK()) {
        LOGV2_FATAL(23733,
                    "Can't continue pinging {host} due to {error}",
                    "Can't continue pinging host",
                    "host"_attr = _hostAndPort,
                    "error"_attr = redact(remotePingHandle.getStatus()),
                    "replicaSet"_attr = _setUri.getSetName());
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

void ServerPingMonitor::onServerHandshakeCompleteEvent(sdam::HelloRTT durationMs,
                                                       const HostAndPort& address,
                                                       const BSONObj reply) {
    stdx::lock_guard lk(_mutex);
    if (_isShutdown) {
        return;
    }

    if (_serverPingMonitorMap.find(address) != _serverPingMonitorMap.end()) {
        LOGV2_DEBUG(466811,
                    kLogLevel + 1,
                    "ServerPingMonitor already monitoring {host}",
                    "ServerPingMonitor already monitoring host",
                    "host"_attr = address,
                    "replicaSet"_attr = _setUri.getSetName());
        return;
    }
    auto newSingleMonitor = std::make_shared<SingleServerPingMonitor>(
        _setUri, address, _rttListener, _pingFrequency, _executor);
    _serverPingMonitorMap[address] = newSingleMonitor;
    newSingleMonitor->init();
    LOGV2_DEBUG(23729,
                kLogLevel,
                "ServerPingMonitor is now monitoring {host}",
                "ServerPingMonitor is now monitoring host",
                "host"_attr = address,
                "replicaSet"_attr = _setUri.getSetName());
}

void ServerPingMonitor::onTopologyDescriptionChangedEvent(
    sdam::TopologyDescriptionPtr previousDescription, sdam::TopologyDescriptionPtr newDescription) {
    stdx::lock_guard lk(_mutex);
    if (_isShutdown) {
        return;
    }

    const auto startingSize = _serverPingMonitorMap.size();
    size_t numRemoved = 0;

    // Remove monitors that are missing from the topology.
    auto it = _serverPingMonitorMap.begin();
    while (it != _serverPingMonitorMap.end()) {
        const auto& serverAddress = it->first;
        if (newDescription->findServerByAddress(serverAddress) == boost::none) {
            auto& singleMonitor = _serverPingMonitorMap[serverAddress];
            singleMonitor->drop();
            LOGV2_DEBUG(462899,
                        kLogLevel,
                        "ServerPingMonitor for host {host} was removed from being monitored",
                        "ServerPingMonitor for host was removed from being monitored",
                        "host"_attr = serverAddress,
                        "replicaSet"_attr = _setUri.getSetName());
            it = _serverPingMonitorMap.erase(it, std::next(it));
            numRemoved++;
        } else {
            ++it;
        }
    }

    invariant(_serverPingMonitorMap.size() == (startingSize - numRemoved));
}

}  // namespace mongo
