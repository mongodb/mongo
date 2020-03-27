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
#include "mongo/executor/task_executor.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

/**
 * Manages server monitoring for a single server. Broadcasts the RTT (Round Trip Time) to a
 * listener.
 */
class SingleServerPingMonitor : public std::enable_shared_from_this<SingleServerPingMonitor> {
public:
    explicit SingleServerPingMonitor(sdam::ServerAddress hostAndPort,
                                     sdam::TopologyListener* rttListener,
                                     Seconds pingFrequency,
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

    sdam::ServerAddress _hostAndPort;

    /**
     * Listens for when new RTT (Round Trip Time) values are published.
     */
    sdam::TopologyListener* _rttListener;

    /**
     * The frequency at which ping requests should be sent to measure the round trip time.
     */
    Seconds _pingFrequency;

    std::shared_ptr<executor::TaskExecutor> _executor;

    /**
     * The time at which the next ping should be scheduled. Pings should be sent uniformly across
     * time at _pingFrequency.
     */
    Date_t _nextPingStartDate{};

    /**
     * Must be held to access any of the member variables below.
     */
    mutable Mutex _mutex = MONGO_MAKE_LATCH("SingleServerPingMonitor::mutex");

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
    /**
     * Note: The ServerPingMonitor creates its own executor by default. It takes in an executor for
     * testing only.
     */
    ServerPingMonitor(
        sdam::TopologyListener* rttListener,
        Seconds pingFrequency,
        boost::optional<std::shared_ptr<executor::TaskExecutor>> executor = boost::none);
    ~ServerPingMonitor();

    /**
     * Drops all SingleServerMonitors and shuts down the task executor.
     */
    void shutdown();

    /**
     * The first isMaster exchange for a server succeeded. Creates a new
     * SingleServerPingMonitor to monitor the new replica set member.
     */
    void onServerHandshakeCompleteEvent(sdam::IsMasterRTT durationMs,
                                        const sdam::ServerAddress& address,
                                        const BSONObj reply = BSONObj());

    /**
     * The connection to the server was closed. Removes the server from the ServerPingMonitorList.
     */
    void onServerClosedEvent(const sdam::ServerAddress& address, OID topologyId);

private:
    /**
     * Sets up and starts up the _executor if it did not already exist.
     */
    void _setupTaskExecutor_inlock();
    /**
     * Listens for when new RTT (Round Trip Time) values are published.
     */
    sdam::TopologyListener* _rttListener;

    /**
     * The interval at which ping requests should be sent to measure the RTT (Round Trip Time).
     */
    Seconds _pingFrequency;

    /**
     * Executor for performing server monitoring pings for all of the replica set members.
     */
    std::shared_ptr<executor::TaskExecutor> _executor;

    mutable Mutex _mutex = MONGO_MAKE_LATCH("ServerPingMonitor::mutex");

    /**
     * Maps each server to a SingleServerPingMonitor.
     * Note: SingleServerPingMonitor's drop() should always be called before removing it from the
     * _serverPingMonitorMap.
     */
    stdx::unordered_map<sdam::ServerAddress, std::shared_ptr<SingleServerPingMonitor>>
        _serverPingMonitorMap;

    bool _isShutdown{false};
};

}  // namespace mongo
