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

#include <functional>
#include <memory>
#include <set>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/client/mongo_uri.h"
#include "mongo/client/replica_set_change_notifier.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_stats.h"
#include "mongo/client/sdam/sdam.h"
#include "mongo/client/server_discovery_monitor.h"
#include "mongo/client/server_ping_monitor.h"
#include "mongo/client/streamable_replica_set_monitor_error_handler.h"
#include "mongo/executor/egress_tag_closer.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {

class BSONObj;
class ReplicaSetMonitor;
class ReplicaSetMonitorTest;
struct ReadPreferenceSetting;
using ReplicaSetMonitorPtr = std::shared_ptr<ReplicaSetMonitor>;

/**
 * Replica set monitor implementation backed by the classes in the mongo::sdam namespace.
 *
 * All methods perform the required synchronization to allow callers from multiple threads.
 */
class StreamableReplicaSetMonitor final
    : public ReplicaSetMonitor,
      public sdam::TopologyListener,
      public std::enable_shared_from_this<StreamableReplicaSetMonitor> {

    StreamableReplicaSetMonitor(const ReplicaSetMonitor&) = delete;
    StreamableReplicaSetMonitor& operator=(const ReplicaSetMonitor&) = delete;

public:
    class Refresher;

    static constexpr auto kExpeditedRefreshPeriod = Milliseconds(500);
    static constexpr auto kCheckTimeout = Seconds(5);

    StreamableReplicaSetMonitor(const MongoURI& uri,
                                std::shared_ptr<executor::TaskExecutor> executor,
                                std::shared_ptr<executor::EgressTagCloser> connectionManager,
                                std::function<void()> cleanupCallback,
                                std::shared_ptr<ReplicaSetMonitorManagerStats> managerStats);

    ~StreamableReplicaSetMonitor() override;

    void init() override;

    void initForTesting(sdam::TopologyManagerPtr topologyManager);

    void drop() override;

    static ReplicaSetMonitorPtr make(const MongoURI& uri,
                                     std::shared_ptr<executor::TaskExecutor> executor,
                                     std::shared_ptr<executor::EgressTagCloser> connectionCloser,
                                     std::function<void()> cleanupCallback,
                                     std::shared_ptr<ReplicaSetMonitorManagerStats> managerStats);

    SemiFuture<HostAndPort> getHostOrRefresh(const ReadPreferenceSetting& readPref,
                                             const std::vector<HostAndPort>& excludedHosts,
                                             const CancellationToken& cancelToken) override;

    SemiFuture<std::vector<HostAndPort>> getHostsOrRefresh(
        const ReadPreferenceSetting& readPref,
        const std::vector<HostAndPort>& excludedHosts,
        const CancellationToken& cancelToken) override;

    HostAndPort getPrimaryOrUassert() override;

    void failedHost(const HostAndPort& host, const Status& status) override;
    void failedHostPreHandshake(const HostAndPort& host,
                                const Status& status,
                                BSONObj bson) override;
    void failedHostPostHandshake(const HostAndPort& host,
                                 const Status& status,
                                 BSONObj bson) override;

    bool isPrimary(const HostAndPort& host) const override;

    bool isHostUp(const HostAndPort& host) const override;

    int getMinWireVersion() const override;

    int getMaxWireVersion() const override;

    std::string getName() const override;

    std::string getServerAddress() const override;

    const MongoURI& getOriginalUri() const override;

    sdam::TopologyEventsPublisherPtr getEventsPublisher();

    bool contains(const HostAndPort& server) const override;

    void appendInfo(BSONObjBuilder& b, bool forFTDC = false) const override;

    bool isKnownToHaveGoodPrimary() const override;
    void runScanForMockReplicaSet() override;
    class StreamableReplicaSetMonitorDiscoveryTimeProcessor;

private:
    class StreamableReplicaSetMonitorQueryProcessor;
    using StreamableReplicaSetMontiorQueryProcessorPtr =
        std::shared_ptr<StreamableReplicaSetMonitor::StreamableReplicaSetMonitorQueryProcessor>;

    using StreamableReplicaSetMonitorDiscoveryTimeProcessorPtr = std::shared_ptr<
        StreamableReplicaSetMonitor::StreamableReplicaSetMonitorDiscoveryTimeProcessor>;

    struct HostQuery {
        HostQuery(std::shared_ptr<ReplicaSetMonitorStats> stats)
            : statsCollector(stats->collectGetHostAndRefreshStats()) {}

        ~HostQuery() {
            invariant(hasBeenResolved());
        }

        bool hasBeenResolved() {
            return done.load();
        }

        /**
         * Tries to mark the query as done and resolve its promise with an error status, and returns
         * whether or not it was able to do so.
         */
        bool tryCancel(Status status) {
            invariant(!status.isOK());
            auto wasAlreadyDone = done.swap(true);
            if (!wasAlreadyDone) {
                promise.setError(status);
                deadlineCancelSource.cancel();
            }
            return !wasAlreadyDone;
        }

        /**
         * Tries to mark the query as done and resolve its promise with a successful result, and
         * returns whether or not it was able to do so.
         */
        bool tryResolveWithSuccess(std::vector<HostAndPort>&& result) {
            auto wasAlreadyDone = done.swap(true);
            if (!wasAlreadyDone) {
                promise.emplaceValue(std::move(result));
                deadlineCancelSource.cancel();
            }
            return !wasAlreadyDone;
        }

        const mongo::ScopeGuard<std::function<void()>> statsCollector;

        CancellationSource deadlineCancelSource;

        ReadPreferenceSetting criteria;

        std::vector<HostAndPort> excludedHosts;

        // Used to compute latency.
        Date_t start;

        AtomicWord<bool> done{false};

        Promise<std::vector<HostAndPort>> promise;
    };

    using HostQueryPtr = std::shared_ptr<HostQuery>;

    // Information collected from the primary ServerDescription to be published via the
    // ReplicaSetChangeNotifier
    struct ChangeNotifierState {
        HostAndPort primaryAddress;
        std::set<HostAndPort> passives;
        ConnectionString connectionString;
    };

    SemiFuture<std::vector<HostAndPort>> _enqueueOutstandingQuery(
        WithLock,
        const ReadPreferenceSetting& criteria,
        const std::vector<HostAndPort>& excludedHosts,
        const CancellationToken& cancelToken,
        const Date_t& deadline);

    // Removes the query pointed to by iter and returns an iterator to the next item in the list.
    std::list<HostQueryPtr>::iterator _eraseQueryFromOutstandingQueries(
        WithLock, std::list<HostQueryPtr>::iterator iter);

    std::vector<HostAndPort> _extractHosts(
        const std::vector<sdam::ServerDescriptionPtr>& serverDescriptions);

    boost::optional<std::vector<HostAndPort>> _getHosts(
        const TopologyDescriptionPtr& topology,
        const ReadPreferenceSetting& criteria,
        const std::vector<HostAndPort>& excludedHosts = std::vector<HostAndPort>());
    boost::optional<std::vector<HostAndPort>> _getHosts(
        const ReadPreferenceSetting& criteria,
        const std::vector<HostAndPort>& excludedHosts = std::vector<HostAndPort>());

    // Incoming Events
    void onTopologyDescriptionChangedEvent(sdam::TopologyDescriptionPtr previousDescription,
                                           sdam::TopologyDescriptionPtr newDescription) override;

    void onServerHeartbeatSucceededEvent(const HostAndPort& hostAndPort, BSONObj reply) override;

    void onServerHandshakeFailedEvent(const HostAndPort& address,
                                      const Status& status,
                                      BSONObj reply) override;

    void onServerHeartbeatFailureEvent(Status errorStatus,
                                       const HostAndPort& hostAndPort,
                                       BSONObj reply) override;

    void onServerPingFailedEvent(const HostAndPort& hostAndPort, const Status& status) override;

    void onServerPingSucceededEvent(sdam::HelloRTT durationMS,
                                    const HostAndPort& hostAndPort) override;

    void onServerHandshakeCompleteEvent(sdam::HelloRTT durationMs,
                                        const HostAndPort& hostAndPort,
                                        BSONObj reply) override;

    // Get a pointer to the current primary's ServerDescription
    // To ensure a consistent view of the Topology either _currentPrimary or _currentTopology should
    // be called (not both) since the topology can change between the function invocations.
    boost::optional<sdam::ServerDescriptionPtr> _currentPrimary() const;

    // Get the current TopologyDescription
    // Note that most functions will want to save the result of this function once per computation
    // so that we are operating on a consistent read-only view of the topology.
    sdam::TopologyDescriptionPtr _currentTopology() const;

    std::string _logPrefix();

    void _failOutstandingWithStatus(WithLock, Status status);

    void _setConfirmedNotifierState(WithLock, const ServerDescriptionPtr& primaryDescription);

    // Try to satisfy the outstanding queries for this instance with the given topology information.
    void _processOutstanding(const TopologyDescriptionPtr& topologyDescription);

    // Take action on error for the given host.
    void _doErrorActions(
        const HostAndPort& host,
        const StreamableReplicaSetMonitorErrorHandler::ErrorActions& errorActions) const;

    void _failedHost(const HostAndPort& host,
                     const Status& status,
                     BSONObj bson,
                     StreamableReplicaSetMonitorErrorHandler::HandshakeStage stage,
                     bool isApplicationOperation);

    sdam::SdamConfiguration _sdamConfig;
    sdam::TopologyManagerPtr _topologyManager;
    sdam::ServerSelectorPtr _serverSelector;
    sdam::TopologyEventsPublisherPtr _eventsPublisher;
    std::unique_ptr<StreamableReplicaSetMonitorErrorHandler> _errorHandler;
    ServerDiscoveryMonitorPtr _serverDiscoveryMonitor;
    std::shared_ptr<ServerPingMonitor> _pingMonitor;

    // This object will be registered as a TopologyListener if there are
    // any outstanding queries for this RSM instance.
    StreamableReplicaSetMontiorQueryProcessorPtr _queryProcessor;

    StreamableReplicaSetMonitorDiscoveryTimeProcessorPtr _primaryDiscoveryTimeProcessor;
    const MongoURI _uri;

    std::shared_ptr<executor::EgressTagCloser> _connectionManager;
    std::shared_ptr<executor::TaskExecutor> _executor;

    AtomicWord<bool> _isDropped{true};

    mutable Mutex _mutex = MONGO_MAKE_LATCH("ReplicaSetMonitor");
    std::list<HostQueryPtr> _outstandingQueries;
    boost::optional<ChangeNotifierState> _confirmedNotifierState;
    std::shared_ptr<ReplicaSetMonitorStats> _stats;

    static constexpr auto kDefaultLogLevel = 0;
    static constexpr auto kLowerLogLevel = 1;
    static constexpr auto kLogPrefix = "[ReplicaSetMonitor]";
};
}  // namespace mongo
