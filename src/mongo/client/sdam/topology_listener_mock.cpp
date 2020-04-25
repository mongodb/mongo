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
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/client/sdam/topology_listener_mock.h"

namespace mongo::sdam {

void TopologyListenerMock::onServerHeartbeatSucceededEvent(const ServerAddress& hostAndPort,
                                                           const BSONObj reply) {
    stdx::lock_guard lk(_mutex);
    auto it = _serverIsMasterReplies.find(hostAndPort);
    if (it != _serverIsMasterReplies.end()) {
        it->second.emplace_back(Status::OK());
    } else {
        _serverIsMasterReplies.emplace(hostAndPort, std::vector<Status>{Status::OK()});
    }
}

void TopologyListenerMock::onServerHeartbeatFailureEvent(Status errorStatus,
                                                         const ServerAddress& hostAndPort,
                                                         const BSONObj reply) {
    stdx::lock_guard lk(_mutex);
    // If the map already contains an element for hostAndPort, append to its already existing
    // vector. Otherwise, create a new vector.
    auto it = _serverIsMasterReplies.find(hostAndPort);
    if (it != _serverIsMasterReplies.end()) {
        it->second.emplace_back(errorStatus);
    } else {
        _serverIsMasterReplies.emplace(hostAndPort, std::vector<Status>{errorStatus});
    }
}

bool TopologyListenerMock::hasIsMasterResponse(const ServerAddress& hostAndPort) {
    stdx::lock_guard lock(_mutex);
    return _hasIsMasterResponse(lock, hostAndPort);
}

bool TopologyListenerMock::_hasIsMasterResponse(WithLock, const ServerAddress& hostAndPort) {
    return _serverIsMasterReplies.find(hostAndPort) != _serverIsMasterReplies.end();
}

std::vector<Status> TopologyListenerMock::getIsMasterResponse(const ServerAddress& hostAndPort) {
    stdx::lock_guard lock(_mutex);
    invariant(_hasIsMasterResponse(lock, hostAndPort));
    auto it = _serverIsMasterReplies.find(hostAndPort);
    auto statusWithIsMasterResponse = it->second;
    _serverIsMasterReplies.erase(it);
    return statusWithIsMasterResponse;
}

void TopologyListenerMock::onServerPingSucceededEvent(IsMasterRTT latency,
                                                      const ServerAddress& hostAndPort) {
    stdx::lock_guard lk(_mutex);
    auto it = _serverPingRTTs.find(hostAndPort);
    if (it != _serverPingRTTs.end()) {
        it->second.emplace_back(latency);
    } else {
        _serverPingRTTs.emplace(hostAndPort, std::vector<StatusWith<IsMasterRTT>>{latency});
    }
}

void TopologyListenerMock::onServerPingFailedEvent(const ServerAddress& hostAndPort,
                                                   const Status& errorStatus) {
    stdx::lock_guard lk(_mutex);
    // If the map already contains an element for hostAndPort, append to its already existing
    // vector. Otherwise, create a new vector.
    auto it = _serverPingRTTs.find(hostAndPort);
    if (it != _serverPingRTTs.end()) {
        it->second.emplace_back(errorStatus);
    } else {
        _serverPingRTTs.emplace(hostAndPort, std::vector<StatusWith<IsMasterRTT>>{errorStatus});
    }
}

bool TopologyListenerMock::hasPingResponse(const ServerAddress& hostAndPort) {
    stdx::lock_guard lock(_mutex);
    return _hasPingResponse(lock, hostAndPort);
}

bool TopologyListenerMock::_hasPingResponse(WithLock, const ServerAddress& hostAndPort) {
    return _serverPingRTTs.find(hostAndPort) != _serverPingRTTs.end();
}

std::vector<StatusWith<IsMasterRTT>> TopologyListenerMock::getPingResponse(
    const ServerAddress& hostAndPort) {
    stdx::lock_guard lock(_mutex);
    invariant(_hasPingResponse(lock, hostAndPort));
    auto it = _serverPingRTTs.find(hostAndPort);
    auto statusWithRTT = it->second;
    _serverPingRTTs.erase(it);
    return statusWithRTT;
}

}  // namespace mongo::sdam
