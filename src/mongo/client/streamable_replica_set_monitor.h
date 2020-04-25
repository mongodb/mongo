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
#include "mongo/client/sdam/sdam.h"
#include "mongo/client/server_is_master_monitor.h"
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
class StreamableReplicaSetMonitor
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
                                std::shared_ptr<executor::EgressTagCloser> connectionManager);

    void init();

    void drop();

    static ReplicaSetMonitorPtr make(const MongoURI& uri,
                                     std::shared_ptr<executor::TaskExecutor> executor,
                                     std::shared_ptr<executor::EgressTagCloser> connectionCloser);

    SemiFuture<HostAndPort> getHostOrRefresh(const ReadPreferenceSetting& readPref,
                                             Milliseconds maxWait = kDefaultFindHostTimeout);

    SemiFuture<std::vector<HostAndPort>> getHostsOrRefresh(
        const ReadPreferenceSetting& readPref, Milliseconds maxWait = kDefaultFindHostTimeout);

    HostAndPort getMasterOrUassert();

    void failedHost(const HostAndPort& host, const Status& status) override;
    void failedHostPreHandshake(const HostAndPort& host,
                                const Status& status,
                                BSONObj bson) override;
    void failedHostPostHandshake(const HostAndPort& host,
                                 const Status& status,
                                 BSONObj bson) override;

    bool isPrimary(const HostAndPort& host) const;

    bool isHostUp(const HostAndPort& host) const;

    int getMinWireVersion() const;

    int getMaxWireVersion() const;

    std::string getName() const;

    std::string getServerAddress() const;

    const MongoURI& getOriginalUri() const;

    sdam::TopologyEventsPublisherPtr getEventsPublisher();

    bool contains(const HostAndPort& server) const;

    void appendInfo(BSONObjBuilder& b, bool forFTDC = false) const;

    bool isKnownToHaveGoodPrimary() const;
    void runScanForMockReplicaSet() override;

private:
    class StreamableReplicaSetMonitorQueryProcessor;
    using StreamableReplicaSetMontiorQueryProcessorPtr =
        std::shared_ptr<StreamableReplicaSetMonitor::StreamableReplicaSetMonitorQueryProcessor>;

    struct HostQuery {
        Date_t deadline;
        executor::TaskExecutor::CallbackHandle deadlineHandle;
        ReadPreferenceSetting criteria;
        Date_t start = Date_t::now();
        bool done = false;
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
        WithLock, const ReadPreferenceSetting& criteria, const Date_t& deadline);

    std::vector<HostAndPort> _extractHosts(
        const std::vector<sdam::ServerDescriptionPtr>& serverDescriptions);

    boost::optional<std::vector<HostAndPort>> _getHosts(const TopologyDescriptionPtr& topology,
                                                        const ReadPreferenceSetting& criteria);
    boost::optional<std::vector<HostAndPort>> _getHosts(const ReadPreferenceSetting& criteria);

    // Incoming Events
    void onTopologyDescriptionChangedEvent(UUID topologyId,
                                           sdam::TopologyDescriptionPtr previousDescription,
                                           sdam::TopologyDescriptionPtr newDescription) override;

    void onServerHeartbeatSucceededEvent(const sdam::ServerAddress& hostAndPort,
                                         const BSONObj reply) override;

    void onServerHandshakeFailedEvent(const sdam::ServerAddress& address,
                                      const Status& status,
                                      const BSONObj reply) override;

    void onServerHeartbeatFailureEvent(Status errorStatus,
                                       const ServerAddress& hostAndPort,
                                       const BSONObj reply) override;

    void onServerPingFailedEvent(const sdam::ServerAddress& hostAndPort,
                                 const Status& status) override;

    void onServerPingSucceededEvent(sdam::IsMasterRTT durationMS,
                                    const sdam::ServerAddress& hostAndPort) override;

    void onServerHandshakeCompleteEvent(sdam::IsMasterRTT durationMs,
                                        const ServerAddress& hostAndPort,
                                        const BSONObj reply) override;

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
    bool _hasMembershipChange(sdam::TopologyDescriptionPtr oldDescription,
                              sdam::TopologyDescriptionPtr newDescription);
    void _setConfirmedNotifierState(WithLock, const ServerDescriptionPtr& primaryDescription);

    Status _makeUnsatisfiedReadPrefError(const ReadPreferenceSetting& criteria) const;
    Status _makeReplicaSetMonitorRemovedError() const;

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
    ServerIsMasterMonitorPtr _isMasterMonitor;
    std::shared_ptr<ServerPingMonitor> _pingMonitor;

    // This object will be registered as a TopologyListener if there are
    // any outstanding queries for this RSM instance.
    StreamableReplicaSetMontiorQueryProcessorPtr _queryProcessor;

    const MongoURI _uri;

    std::shared_ptr<executor::EgressTagCloser> _connectionManager;
    std::shared_ptr<executor::TaskExecutor> _executor;

    AtomicWord<bool> _isDropped{true};

    mutable Mutex _mutex = MONGO_MAKE_LATCH("ReplicaSetMonitor");
    std::vector<HostQueryPtr> _outstandingQueries;
    boost::optional<ChangeNotifierState> _confirmedNotifierState;
    mutable PseudoRandom _random;

    static inline const auto kServerSelectionConfig =
        sdam::ServerSelectionConfiguration::defaultConfiguration();
    static constexpr auto kDefaultLogLevel = 0;
    static constexpr auto kLowerLogLevel = 1;
    static constexpr auto kLogPrefix = "[ReplicaSetMonitor]";
};
}  // namespace mongo
