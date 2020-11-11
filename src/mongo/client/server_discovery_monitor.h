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
#include "mongo/client/mongo_uri.h"
#include "mongo/client/sdam/sdam.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
using namespace sdam;

class SingleServerDiscoveryMonitor
    : public std::enable_shared_from_this<SingleServerDiscoveryMonitor> {
public:
    explicit SingleServerDiscoveryMonitor(const MongoURI& setUri,
                                          const HostAndPort& host,
                                          boost::optional<TopologyVersion> topologyVersion,
                                          const SdamConfiguration& sdamConfig,
                                          TopologyEventsPublisherPtr eventListener,
                                          std::shared_ptr<executor::TaskExecutor> executor);

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

    // Sent in the initial hello request when using the streamable exhaust protocol. The max
    // duration a server should wait for a significant topology change before sending a response.
    static constexpr Milliseconds kMaxAwaitTime = Milliseconds(10000);

private:
    void _scheduleNextHello(WithLock, Milliseconds delay);
    void _rescheduleNextHello(WithLock, Milliseconds delay);
    void _doRemoteCommand();

    // Use the awaitable hello protocol with the exhaust bit set. Attach _topologyVersion and
    // kMaxAwaitTimeMS to the request.
    StatusWith<executor::TaskExecutor::CallbackHandle> _scheduleStreamableHello();

    // Use the old hello protocol. Do not attach _topologyVersion or kMaxAwaitTimeMS to the
    // request.
    StatusWith<executor::TaskExecutor::CallbackHandle> _scheduleSingleHello();

    void _onHelloSuccess(const BSONObj bson);
    void _onHelloFailure(const Status& status, const BSONObj bson);

    Milliseconds _overrideRefreshPeriod(Milliseconds original);
    Milliseconds _currentRefreshPeriod(WithLock, bool scheduleImmediately);
    void _cancelOutstandingRequest(WithLock);

    boost::optional<Milliseconds> _timeSinceLastCheck() const;

    static constexpr auto kLogLevel = 0;

    Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(4), "SingleServerDiscoveryMonitor::mutex");
    HostAndPort _host;
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
                           std::shared_ptr<executor::TaskExecutor> executor = nullptr);

    virtual ~ServerDiscoveryMonitor() {}

    void shutdown();

    /**
     * Request an immediate check of each member in the replica set.
     */
    void requestImmediateCheck();

    /**
     * Add/Remove Single Monitors based on the current topology membership.
     */
    void onTopologyDescriptionChangedEvent(TopologyDescriptionPtr previousDescription,
                                           TopologyDescriptionPtr newDescription) override;

    void disableExpeditedChecking();

private:
    /**
     * If the provided executor exists, use that one (for testing). Otherwise create a new one.
     */
    std::shared_ptr<executor::TaskExecutor> _setupExecutor(
        const std::shared_ptr<executor::TaskExecutor>& executor);
    void _disableExpeditedChecking(WithLock);

    static constexpr auto kLogLevel = 0;

    Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(5), "ServerDiscoveryMonitor::mutex");
    SdamConfiguration _sdamConfiguration;
    TopologyEventsPublisherPtr _eventPublisher;
    std::shared_ptr<executor::TaskExecutor> _executor;
    stdx::unordered_map<HostAndPort, SingleServerDiscoveryMonitorPtr> _singleMonitors;
    bool _isShutdown;
    MongoURI _setUri;
};
using ServerDiscoveryMonitorPtr = std::shared_ptr<ServerDiscoveryMonitor>;
}  // namespace mongo
