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
#include "mongo/client/sdam/topology_listener_mock.h"

#include "mongo/util/assert_util.h"

#include <absl/container/node_hash_map.h>
#include <absl/meta/type_traits.h>
#include <boost/move/utility_core.hpp>

namespace mongo::sdam {

void TopologyListenerMock::onServerHeartbeatSucceededEvent(const HostAndPort& hostAndPort,
                                                           const BSONObj reply) {
    stdx::lock_guard lk(_mutex);
    auto it = _serverHelloReplies.find(hostAndPort);
    if (it != _serverHelloReplies.end()) {
        it->second.emplace_back(Status::OK());
    } else {
        _serverHelloReplies.emplace(hostAndPort, std::vector<Status>{Status::OK()});
    }
}

void TopologyListenerMock::onServerHeartbeatFailureEvent(Status errorStatus,
                                                         const HostAndPort& hostAndPort,
                                                         const BSONObj reply) {
    stdx::lock_guard lk(_mutex);
    // If the map already contains an element for hostAndPort, append to its already existing
    // vector. Otherwise, create a new vector.
    auto it = _serverHelloReplies.find(hostAndPort);
    if (it != _serverHelloReplies.end()) {
        it->second.emplace_back(errorStatus);
    } else {
        _serverHelloReplies.emplace(hostAndPort, std::vector<Status>{errorStatus});
    }
}

bool TopologyListenerMock::hasHelloResponse(const HostAndPort& hostAndPort) {
    stdx::lock_guard lock(_mutex);
    return _hasHelloResponse(lock, hostAndPort);
}

bool TopologyListenerMock::_hasHelloResponse(WithLock, const HostAndPort& hostAndPort) {
    return _serverHelloReplies.find(hostAndPort) != _serverHelloReplies.end();
}

std::vector<Status> TopologyListenerMock::getHelloResponse(const HostAndPort& hostAndPort) {
    stdx::lock_guard lock(_mutex);
    invariant(_hasHelloResponse(lock, hostAndPort));
    auto it = _serverHelloReplies.find(hostAndPort);
    auto statusWithHelloResponse = it->second;
    _serverHelloReplies.erase(it);
    return statusWithHelloResponse;
}

void TopologyListenerMock::onServerPingSucceededEvent(HelloRTT latency,
                                                      const HostAndPort& hostAndPort) {
    stdx::lock_guard lk(_mutex);
    auto it = _serverPingRTTs.find(hostAndPort);
    if (it != _serverPingRTTs.end()) {
        it->second.emplace_back(latency);
    } else {
        _serverPingRTTs.emplace(hostAndPort, std::vector<StatusWith<HelloRTT>>{latency});
    }
}

void TopologyListenerMock::onServerPingFailedEvent(const HostAndPort& hostAndPort,
                                                   const Status& errorStatus) {
    stdx::lock_guard lk(_mutex);
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
    stdx::lock_guard lock(_mutex);
    return _hasPingResponse(lock, hostAndPort);
}

bool TopologyListenerMock::_hasPingResponse(WithLock, const HostAndPort& hostAndPort) {
    return _serverPingRTTs.find(hostAndPort) != _serverPingRTTs.end();
}

std::vector<StatusWith<HelloRTT>> TopologyListenerMock::getPingResponse(
    const HostAndPort& hostAndPort) {
    stdx::lock_guard lock(_mutex);
    invariant(_hasPingResponse(lock, hostAndPort));
    auto it = _serverPingRTTs.find(hostAndPort);
    auto statusWithRTT = it->second;
    _serverPingRTTs.erase(it);
    return statusWithRTT;
}

}  // namespace mongo::sdam
