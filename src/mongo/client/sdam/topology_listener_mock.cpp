// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/client/sdam/topology_listener_mock.h"

#include "mongo/util/assert_util.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>

namespace mongo::sdam {

void TopologyListenerMock::onServerHeartbeatSucceededEvent(const HostAndPort& hostAndPort,
                                                           const BSONObj reply) {
    std::lock_guard lk(_mutex);
    auto it = _serverHelloReplies.find(hostAndPort);
    if (it != _serverHelloReplies.end()) {
        it->second.emplace_back(Status::OK());
    } else {
        _serverHelloReplies.emplace(hostAndPort, std::vector<Status>{Status::OK()});
    }
    ++_serverHeartbeatCounts.successes;
}

void TopologyListenerMock::onServerHeartbeatFailureEvent(Status errorStatus,
                                                         const HostAndPort& hostAndPort,
                                                         const BSONObj reply) {
    std::lock_guard lk(_mutex);
    // If the map already contains an element for hostAndPort, append to its already existing
    // vector. Otherwise, create a new vector.
    auto it = _serverHelloReplies.find(hostAndPort);
    if (it != _serverHelloReplies.end()) {
        it->second.emplace_back(errorStatus);
    } else {
        _serverHelloReplies.emplace(hostAndPort, std::vector<Status>{errorStatus});
    }
    ++_serverHeartbeatCounts.failures;
}

TopologyListenerMock::Counts TopologyListenerMock::serverHeartbeatCounts() const {
    return _serverHeartbeatCounts;
}

bool TopologyListenerMock::hasHelloResponse(const HostAndPort& hostAndPort) {
    std::lock_guard lock(_mutex);
    return _hasHelloResponse(lock, hostAndPort);
}

bool TopologyListenerMock::_hasHelloResponse(WithLock, const HostAndPort& hostAndPort) {
    return _serverHelloReplies.find(hostAndPort) != _serverHelloReplies.end();
}

std::vector<Status> TopologyListenerMock::getHelloResponse(const HostAndPort& hostAndPort) {
    std::lock_guard lock(_mutex);
    invariant(_hasHelloResponse(lock, hostAndPort));
    auto it = _serverHelloReplies.find(hostAndPort);
    auto statusWithHelloResponse = it->second;
    _serverHelloReplies.erase(it);
    return statusWithHelloResponse;
}

void TopologyListenerMock::onServerPingSucceededEvent(HelloRTT latency,
                                                      const HostAndPort& hostAndPort) {
    std::lock_guard lk(_mutex);
    auto it = _serverPingRTTs.find(hostAndPort);
    if (it != _serverPingRTTs.end()) {
        it->second.emplace_back(latency);
    } else {
        _serverPingRTTs.emplace(hostAndPort, std::vector<StatusWith<HelloRTT>>{latency});
    }
}

void TopologyListenerMock::onServerPingFailedEvent(const HostAndPort& hostAndPort,
                                                   const Status& errorStatus) {
    std::lock_guard lk(_mutex);
    // If the map already contains an element for hostAndPort, append to its already existing
    // vector. Otherwise, create a new vector.
    auto it = _serverPingRTTs.find(hostAndPort);
    if (it != _serverPingRTTs.end()) {
        it->second.emplace_back(errorStatus);
    } else {
        _serverPingRTTs.emplace(hostAndPort, std::vector<StatusWith<HelloRTT>>{errorStatus});
    }
}

bool TopologyListenerMock::hasPingResponse(const HostAndPort& hostAndPort) {
    std::lock_guard lock(_mutex);
    return _hasPingResponse(lock, hostAndPort);
}

bool TopologyListenerMock::_hasPingResponse(WithLock, const HostAndPort& hostAndPort) {
    return _serverPingRTTs.find(hostAndPort) != _serverPingRTTs.end();
}

std::vector<StatusWith<HelloRTT>> TopologyListenerMock::getPingResponse(
    const HostAndPort& hostAndPort) {
    std::lock_guard lock(_mutex);
    invariant(_hasPingResponse(lock, hostAndPort));
    auto it = _serverPingRTTs.find(hostAndPort);
    auto statusWithRTT = it->second;
    _serverPingRTTs.erase(it);
    return statusWithRTT;
}

}  // namespace mongo::sdam
