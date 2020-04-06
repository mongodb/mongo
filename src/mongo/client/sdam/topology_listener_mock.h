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

#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_listener.h"
#include "mongo/util/uuid.h"

namespace mongo::sdam {

class TopologyListenerMock : public TopologyListener {
public:
    TopologyListenerMock() = default;
    virtual ~TopologyListenerMock() = default;

    void onServerHeartbeatSucceededEvent(const ServerAddress& hostAndPort,
                                         const BSONObj reply) override;

    void onServerHeartbeatFailureEvent(Status errorStatus,
                                       const ServerAddress& hostAndPort,
                                       const BSONObj reply) override;

    /**
     * Returns true if _serverIsMasterReplies contains an element corresponding to hostAndPort.
     */
    bool hasIsMasterResponse(const ServerAddress& hostAndPort);
    bool _hasIsMasterResponse(WithLock, const ServerAddress& hostAndPort);

    /**
     * Returns the responses for the most recent onServerHeartbeat events.
     */
    std::vector<Status> getIsMasterResponse(const ServerAddress& hostAndPort);

    void onServerPingSucceededEvent(IsMasterRTT latency, const ServerAddress& hostAndPort) override;

    void onServerPingFailedEvent(const ServerAddress& hostAndPort, const Status& status) override;

    /**
     * Returns true if _serverPingRTTs contains an element corresponding to hostAndPort.
     */
    bool hasPingResponse(const ServerAddress& hostAndPort);
    bool _hasPingResponse(WithLock, const ServerAddress& hostAndPort);

    /**
     * Returns the responses for the most recent onServerPing events.
     */
    std::vector<StatusWith<IsMasterRTT>> getPingResponse(const ServerAddress& hostAndPort);

private:
    Mutex _mutex;
    stdx::unordered_map<ServerAddress, std::vector<Status>> _serverIsMasterReplies;
    stdx::unordered_map<ServerAddress, std::vector<StatusWith<IsMasterRTT>>> _serverPingRTTs;
};

}  // namespace mongo::sdam
