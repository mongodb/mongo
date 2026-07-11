// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_listener.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <mutex>
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

    struct Counts {
        unsigned successes = 0;
        unsigned failures = 0;
    };

    Counts serverHeartbeatCounts() const;

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
    std::mutex _mutex;
    stdx::unordered_map<HostAndPort, std::vector<Status>> _serverHelloReplies;
    stdx::unordered_map<HostAndPort, std::vector<StatusWith<HelloRTT>>> _serverPingRTTs;
    Counts _serverHeartbeatCounts;
};

}  // namespace mongo::sdam
