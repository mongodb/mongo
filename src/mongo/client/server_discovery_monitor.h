// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_monitor_stats.h"
#include "mongo/client/sdam/election_id_set_version_pair.h"
#include "mongo/client/sdam/sdam.h"
#include "mongo/client/sdam/sdam_configuration.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_listener.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <mutex>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo::sdam {

class SingleServerDiscoveryMonitor
    : public std::enable_shared_from_this<SingleServerDiscoveryMonitor> {
public:
    explicit SingleServerDiscoveryMonitor(const MongoURI& setUri,
                                          const HostAndPort& host,
                                          boost::optional<TopologyVersion> topologyVersion,
                                          const SdamConfiguration& sdamConfig,
                                          TopologyEventsPublisherPtr eventListener,
                                          std::shared_ptr<executor::TaskExecutor> executor,
                                          std::shared_ptr<ReplicaSetMonitorStats> stats);

    void init();
    void shutdown();

    /**
     * Request an immediate check. The server will be checked immediately if we haven't completed
     * a hello less than SdamConfiguration::kMinHeartbeatFrequency ago. Otherwise,
     * we schedule a check that runs after SdamConfiguration::kMinHeartbeatFrequency since
     * the last hello.
     */
    void requestImmediateCheck();
    void disableExpeditedChecking();

    /**
     * Calculates the timing of the next Hello request when moving to expedited mode. Returns
     * boost::none if the existing schedule should be maintained.
     */
    static boost::optional<Milliseconds> calculateExpeditedDelayUntilNextCheck(
        const boost::optional<Milliseconds>& maybeTimeSinceLastCheck,
        const Milliseconds& expeditedRefreshPeriod,
        const Milliseconds& previousRefreshPeriod);

    /**
     * Sent in the initial hello request when using the streamable exhaust protocol. The max
     * duration a server should wait for a significant topology change before sending a response.
     */
    static constexpr Milliseconds kMaxAwaitTime = Milliseconds(10000);

private:
    void _scheduleNextHello(WithLock, Milliseconds delay);
    void _rescheduleNextHello(WithLock, Milliseconds delay);
    void _doRemoteCommand();

    /**
     * Uses the awaitable hello protocol with the exhaust bit set. Attaches _topologyVersion and
     * kMaxAwaitTimeMS to the request.
     */
    StatusWith<executor::TaskExecutor::CallbackHandle> _scheduleStreamableHello();

    /**
     * Uses the old hello protocol. Does not attach _topologyVersion or kMaxAwaitTimeMS to the
     * request.
     */
    StatusWith<executor::TaskExecutor::CallbackHandle> _scheduleSingleHello();

    /**
     * Dispatches to one of `_onHelloSuccess` or `_onHelloFailure` depending on the remote command
     * response.
     */
    void _onHello(const executor::RemoteCommandResponse&);
    void _onHelloSuccess(BSONObj bson);
    void _onHelloFailure(const Status& status, BSONObj bson);

    Milliseconds _overrideRefreshPeriod(Milliseconds original);
    Milliseconds _currentRefreshPeriod(WithLock, bool scheduleImmediately);
    void _cancelOutstandingRequest(WithLock);

    boost::optional<Milliseconds> _timeSinceLastCheck() const;

    static constexpr auto kLogLevel = 0;

    const HostAndPort _host;
    const std::shared_ptr<ReplicaSetMonitorStats> _stats;

    std::mutex _mutex;
    boost::optional<TopologyVersion> _topologyVersion;
    TopologyEventsPublisherPtr _eventListener;
    std::shared_ptr<executor::TaskExecutor> _executor;
    Milliseconds _heartbeatFrequency;
    Milliseconds _connectTimeout;

    boost::optional<Date_t> _lastHelloAt;
    bool _helloOutstanding = false;
    bool _isExpedited;
    executor::TaskExecutor::CallbackHandle _nextHelloHandle;
    executor::TaskExecutor::CallbackHandle _remoteCommandHandle;

    bool _isShutdown;
    MongoURI _setUri;
};
using SingleServerDiscoveryMonitorPtr = std::shared_ptr<SingleServerDiscoveryMonitor>;


class ServerDiscoveryMonitor : public TopologyListener {
public:
    ServerDiscoveryMonitor(const MongoURI& setUri,
                           const SdamConfiguration& sdamConfiguration,
                           TopologyEventsPublisherPtr eventsPublisher,
                           TopologyDescriptionPtr initialTopologyDescription,
                           std::shared_ptr<ReplicaSetMonitorStats> stats,
                           std::shared_ptr<executor::TaskExecutor> executor = nullptr);

    ~ServerDiscoveryMonitor() override {}

    void shutdown();

    /** Requests an immediate check of each member in the replica set. */
    void requestImmediateCheck();

    /** Adds/Removes Single Monitors based on the current topology membership. */
    void onTopologyDescriptionChangedEvent(TopologyDescriptionPtr previousDescription,
                                           TopologyDescriptionPtr newDescription) override;

    void disableExpeditedChecking();

private:
    /**
     * If the provided executor exists, uses that one (for testing). Otherwise creates a new one.
     */
    std::shared_ptr<executor::TaskExecutor> _setupExecutor(
        const std::shared_ptr<executor::TaskExecutor>& executor);
    void _disableExpeditedChecking(WithLock);

    static constexpr auto kLogLevel = 0;

    const std::shared_ptr<ReplicaSetMonitorStats> _stats;

    std::mutex _mutex;
    SdamConfiguration _sdamConfiguration;
    TopologyEventsPublisherPtr _eventPublisher;
    std::shared_ptr<executor::TaskExecutor> _executor;
    stdx::unordered_map<HostAndPort, SingleServerDiscoveryMonitorPtr> _singleMonitors;
    bool _isShutdown;
    MongoURI _setUri;
};
using ServerDiscoveryMonitorPtr = std::shared_ptr<ServerDiscoveryMonitor>;
}  // namespace mongo::sdam
