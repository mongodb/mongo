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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_listener.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <vector>

namespace mongo::sdam {

class TopologyListenerMock : public TopologyListener {
public:
    TopologyListenerMock() = default;
    ~TopologyListenerMock() override = default;

    void onServerHeartbeatSucceededEvent(const HostAndPort& hostAndPort, BSONObj reply) override;

    void onServerHeartbeatFailureEvent(Status errorStatus,
                                       const HostAndPort& hostAndPort,
                                       BSONObj reply) override;

    /**
     * Returns true if _serverHelloReplies contains an element corresponding to hostAndPort.
     */
    bool hasHelloResponse(const HostAndPort& hostAndPort);
    bool _hasHelloResponse(WithLock, const HostAndPort& hostAndPort);

    /**
     * Returns the responses for the most recent onServerHeartbeat events.
     */
    std::vector<Status> getHelloResponse(const HostAndPort& hostAndPort);

    void onServerPingSucceededEvent(HelloRTT latency, const HostAndPort& hostAndPort) override;

    void onServerPingFailedEvent(const HostAndPort& hostAndPort, const Status& status) override;

    /**
     * Returns true if _serverPingRTTs contains an element corresponding to hostAndPort.
     */
    bool hasPingResponse(const HostAndPort& hostAndPort);
    bool _hasPingResponse(WithLock, const HostAndPort& hostAndPort);

    /**
     * Returns the responses for the most recent onServerPing events.
     */
    std::vector<StatusWith<HelloRTT>> getPingResponse(const HostAndPort& hostAndPort);

private:
    stdx::mutex _mutex;
    stdx::unordered_map<HostAndPort, std::vector<Status>> _serverHelloReplies;
    stdx::unordered_map<HostAndPort, std::vector<StatusWith<HelloRTT>>> _serverPingRTTs;
};

}  // namespace mongo::sdam
