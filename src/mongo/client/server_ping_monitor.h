// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_listener.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <mutex>

namespace mongo {

/**
 * Manages server monitoring for a single server. Broadcasts the RTT (Round Trip Time) to a
 * listener.
 */
class SingleServerPingMonitor : public std::enable_shared_from_this<SingleServerPingMonitor> {
public:
    explicit SingleServerPingMonitor(const MongoURI& setUri,
                                     const HostAndPort& hostAndPort,
                                     sdam::TopologyListener* rttListener,
                                     Milliseconds pingFrequency,
                                     std::shared_ptr<executor::TaskExecutor> executor);

    /**
     * Starts the pinging loop.
     */
    void init();

    /**
     * Signals that the SingleServerPingMonitor has been dropped and should cancel any outstanding
     * pings scheduled to execute in the future. Contract: Once drop() is completed, the
     * SingleServerPingMonitor will stop broadcasting results to the listener.
     */
    void drop();

private:
    Date_t now() const {
        return _executor ? _executor->now() : Date_t::now();
    }

    /**
     * Wraps the callback and schedules it to run at some time.
     *
     * The callback wrapper does the following:
     * * Returns before running cb if isDropped is true.
     * * Returns before running cb if the handle was canceled.
     * * Locks before running cb and unlocks after.
     */
    template <typename Callback>
    auto _scheduleWorkAt(Date_t when, Callback&& cb) const;

    /**
     * Schedules the next ping request at _nextPingStartDate.
     */
    void _scheduleServerPing();

    /**
     * Sends a ping to the server and processes the response. If the ping was successful, broadcasts
     * the RTT (Round Trip Time) and schedules the next ping. Otherwise, broadcasts the error status
     * and returns.
     */
    void _doServerPing();

    MongoURI _setUri;

    HostAndPort _hostAndPort;

    /**
     * Listens for when new RTT (Round Trip Time) values are published.
     */
    sdam::TopologyListener* _rttListener;

    /**
     * The frequency at which ping requests should be sent to measure the round trip time.
     */
    Milliseconds _pingFrequency;

    std::shared_ptr<executor::TaskExecutor> _executor;

    /**
     * The time at which the next ping should be scheduled. Pings should be sent uniformly across
     * time at _pingFrequency.
     */
    Date_t _nextPingStartDate{};

    static constexpr auto kLogLevel = 0;

    /**
     * Must be held to access any of the member variables below.
     */
    mutable std::mutex _mutex;

    /**
     * Enables a scheduled or outgoing ping to be cancelled upon drop().
     */
    executor::TaskExecutor::CallbackHandle _pingHandle;

    /**
     * Indicates if the server has been dropped and should no longer be monitored.
     */
    bool _isDropped = false;
};


/**
 * Monitors the RTT (Round Trip Time) for a set of servers.
 */
class ServerPingMonitor : public sdam::TopologyListener {
    ServerPingMonitor(const ServerPingMonitor&) = delete;
    ServerPingMonitor& operator=(const ServerPingMonitor&) = delete;

public:
    ServerPingMonitor(const MongoURI& setUri,
                      sdam::TopologyListener* rttListener,
                      Milliseconds pingFrequency,
                      std::shared_ptr<executor::TaskExecutor> executor);
    ~ServerPingMonitor() override;

    /**
     * Drops all SingleServerMonitors and shuts down the task executor.
     */
    void shutdown();

    /**
     * The first "hello" exchange for a connection to the server succeeded. Creates a new
     * SingleServerPingMonitor to monitor the new replica set member.
     */
    void onServerHandshakeCompleteEvent(sdam::HelloRTT durationMs,
                                        const HostAndPort& address,
                                        BSONObj reply = BSONObj()) override;

    /**
     * Drop corresponding SingleServerPingMonitors if the server is not included in the
     * newDescritpion.
     */
    void onTopologyDescriptionChangedEvent(sdam::TopologyDescriptionPtr previousDescription,
                                           sdam::TopologyDescriptionPtr newDescription) override;

private:
    MongoURI _setUri;

    /**
     * Listens for when new RTT (Round Trip Time) values are published.
     */
    sdam::TopologyListener* _rttListener;

    /**
     * The interval at which ping requests should be sent to measure the RTT (Round Trip Time).
     */
    Milliseconds _pingFrequency;

    /**
     * Executor for performing server monitoring pings for all of the replica set members.
     */
    std::shared_ptr<executor::TaskExecutor> _executor;

    static constexpr auto kLogLevel = 0;

    mutable std::mutex _mutex;

    /**
     * Maps each server to a SingleServerPingMonitor.
     * Note: SingleServerPingMonitor's drop() should always be called before removing it from the
     * _serverPingMonitorMap.
     */
    stdx::unordered_map<HostAndPort, std::shared_ptr<SingleServerPingMonitor>>
        _serverPingMonitorMap;

    bool _isShutdown{false};
};

}  // namespace mongo
