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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork
#include "mongo/platform/basic.h"

#include "mongo/client/streamable_replica_set_monitor.h"

#include <algorithm>
#include <limits>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/client/connpool.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/streamable_replica_set_monitor_query_processor.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/bson_extract_optime.h"
#include "mongo/db/server_options.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/string_map.h"
#include "mongo/util/timer.h"

namespace mongo {
using namespace mongo::sdam;

using std::numeric_limits;
using std::set;
using std::shared_ptr;
using std::string;
using std::vector;

namespace {
// Pull nested types to top-level scope
using executor::EgressTagCloser;
using executor::TaskExecutor;
using CallbackArgs = TaskExecutor::CallbackArgs;
using CallbackHandle = TaskExecutor::CallbackHandle;
using HandshakeStage = StreamableReplicaSetMonitorErrorHandler::HandshakeStage;

const ReadPreferenceSetting kPrimaryOnlyReadPreference(ReadPreference::PrimaryOnly, TagSet());

// Utility functions to use when finding servers
bool minWireCompare(const ServerDescriptionPtr& a, const ServerDescriptionPtr& b) {
    return a->getMinWireVersion() < b->getMinWireVersion();
}

bool maxWireCompare(const ServerDescriptionPtr& a, const ServerDescriptionPtr& b) {
    return a->getMaxWireVersion() < b->getMaxWireVersion();
}

bool secondaryPredicate(const ServerDescriptionPtr& server) {
    return server->getType() == ServerType::kRSSecondary;
}

bool primaryOrSecondaryPredicate(const ServerDescriptionPtr& server) {
    const auto serverType = server->getType();
    return serverType == ServerType::kRSPrimary || serverType == ServerType::kRSSecondary;
}

std::string readPrefToStringWithMinOpTime(const ReadPreferenceSetting& readPref) {
    BSONObjBuilder builder;
    readPref.toInnerBSON(&builder);
    if (!readPref.minOpTime.isNull()) {
        builder.append("minOpTime", readPref.minOpTime.toBSON());
    }
    return builder.obj().toString();
}

std::string hostListToString(boost::optional<std::vector<HostAndPort>> x) {
    std::stringstream s;
    if (x) {
        for (auto h : *x) {
            s << h.toString() << "; ";
        }
    }
    return s.str();
}

int32_t pingTimeMillis(const ServerDescriptionPtr& serverDescription) {
    static const Milliseconds maxLatency = Milliseconds::max();
    const auto& serverRtt = serverDescription->getRtt();
    auto latencyMillis = (serverRtt) ? duration_cast<Milliseconds>(*serverRtt) : maxLatency;
    return std::min(latencyMillis, maxLatency).count();
}

constexpr auto kZeroMs = Milliseconds(0);
}  // namespace


/*
 * The concurrency control for this class is outlined below:
 *
 * The _mutex member variable is used to protect access to the _outstandingQueries list. This list
 * is accessed when the RSM instance cannot immediately satisfy a query.
 *
 * The TopologyManager holds a pointer to the current topology, and it is responsible for making
 * sure that concurrent access to topology is safe. In practice, this means that methods that access
 * the topology information should first obtain a copy of the current topology information (via
 * _currentTopology) and maintain a copy of it in it's stack so that the TopologyDescription will
 * not be destroyed.
 *
 * Additionally, the atomic bool value _isDropped is used to determine if we are shutting down. In
 * the getHostsOrRefresh method, _isDropped is checked in the normal case when we can satisfy a
 * query immediately, and if not, the mutex is taken to add the query to the outstanding list. This
 * implies that getHostsOrRefresh should avoid accessing any mutable state before the lock is taken
 * when enqueing the outstanding query.
 *
 * All child classes (_topologyManager, _serverMonitor, _pingMonitor, _eventsPublisher) handle their
 * own concurrency control, and effectively provide serialized access to their respective
 * functionality. Once they are shutdown in the drop() method the operations exposed via their api
 * are effectively no-ops.
 */
StreamableReplicaSetMonitor::StreamableReplicaSetMonitor(
    const MongoURI& uri,
    std::shared_ptr<TaskExecutor> executor,
    std::shared_ptr<executor::EgressTagCloser> connectionManager)
    : _serverSelector(std::make_unique<SdamServerSelector>(kServerSelectionConfig)),
      _errorHandler(std::make_unique<SdamErrorHandler>(uri.getSetName())),
      _queryProcessor(std::make_shared<StreamableReplicaSetMonitorQueryProcessor>()),
      _uri(uri),
      _connectionManager(connectionManager),
      _executor(executor),
      _random(PseudoRandom(SecureRandom().nextInt64())) {

    // TODO SERVER-45395: sdam should use the HostAndPort type for ServerAddress
    std::vector<ServerAddress> seeds;
    for (const auto& seed : uri.getServers()) {
        seeds.push_back(seed.toString());
    }

    _sdamConfig = SdamConfiguration(seeds);
}

ReplicaSetMonitorPtr StreamableReplicaSetMonitor::make(
    const MongoURI& uri,
    std::shared_ptr<TaskExecutor> executor,
    std::shared_ptr<executor::EgressTagCloser> connectionManager) {
    auto result = std::make_shared<StreamableReplicaSetMonitor>(uri, executor, connectionManager);
    result->init();
    return result;
}

void StreamableReplicaSetMonitor::init() {
    stdx::lock_guard lock(_mutex);
    LOGV2_DEBUG(4333206, kLowerLogLevel, "Starting Replica Set Monitor {uri}", "uri"_attr = _uri);

    _eventsPublisher = std::make_shared<sdam::TopologyEventsPublisher>(_executor);
    _topologyManager = std::make_unique<TopologyManager>(
        _sdamConfig, getGlobalServiceContext()->getPreciseClockSource(), _eventsPublisher);

    _eventsPublisher->registerListener(shared_from_this());

    _pingMonitor =
        std::make_unique<ServerPingMonitor>(_uri,
                                            _eventsPublisher.get(),
                                            sdam::SdamConfiguration::kDefaultHeartbeatFrequencyMs,
                                            _executor);
    _eventsPublisher->registerListener(_pingMonitor);

    _isMasterMonitor = std::make_unique<ServerIsMasterMonitor>(
        _uri, _sdamConfig, _eventsPublisher, _topologyManager->getTopologyDescription(), _executor);
    _eventsPublisher->registerListener(_isMasterMonitor);

    _isDropped.store(false);

    ReplicaSetMonitorManager::get()->getNotifier().onFoundSet(getName());
}

void StreamableReplicaSetMonitor::drop() {
    {
        stdx::lock_guard lock(_mutex);
        if (_isDropped.swap(true))
            return;

        _eventsPublisher->close();
        _failOutstandingWithStatus(
            lock, Status{ErrorCodes::ShutdownInProgress, "the ReplicaSetMonitor is shutting down"});
    }

    LOGV2(4333209, "Closing Replica Set Monitor {replicaSet}", "replicaSet"_attr = getName());
    _queryProcessor->shutdown();
    _pingMonitor->shutdown();
    _isMasterMonitor->shutdown();

    ReplicaSetMonitorManager::get()->getNotifier().onDroppedSet(getName());
    LOGV2(4333210, "Done closing Replica Set Monitor {replicaSet}", "replicaSet"_attr = getName());
}

SemiFuture<HostAndPort> StreamableReplicaSetMonitor::getHostOrRefresh(
    const ReadPreferenceSetting& criteria, Milliseconds maxWait) {
    return getHostsOrRefresh(criteria, maxWait)
        .thenRunOn(_executor)
        .then([self = shared_from_this()](const std::vector<HostAndPort>& result) {
            invariant(result.size());
            return result[self->_random.nextInt64(result.size())];
        })
        .semi();
}

std::vector<HostAndPort> StreamableReplicaSetMonitor::_extractHosts(
    const std::vector<ServerDescriptionPtr>& serverDescriptions) {
    std::vector<HostAndPort> result;
    for (const auto& server : serverDescriptions) {
        result.emplace_back(server->getAddress());
    }
    return result;
}

SemiFuture<std::vector<HostAndPort>> StreamableReplicaSetMonitor::getHostsOrRefresh(
    const ReadPreferenceSetting& criteria, Milliseconds maxWait) {
    // In the fast case (stable topology), we avoid mutex acquisition.
    if (_isDropped.load()) {
        return _makeReplicaSetMonitorRemovedError();
    }

    // start counting from the beginning of the operation
    const auto deadline = _executor->now() + ((maxWait > kZeroMs) ? maxWait : kZeroMs);

    // try to satisfy query immediately
    auto immediateResult = _getHosts(criteria);
    if (immediateResult) {
        return {*immediateResult};
    }

    _isMasterMonitor->requestImmediateCheck();
    LOGV2_DEBUG(4333212,
                kLowerLogLevel,
                "RSM {replicaSet} start async getHosts with {readPref}",
                "replicaSet"_attr = getName(),
                "readPref"_attr = readPrefToStringWithMinOpTime(criteria));

    // fail fast on timeout
    const Date_t& now = _executor->now();
    if (deadline <= now) {
        return _makeUnsatisfiedReadPrefError(criteria);
    }

    {
        stdx::lock_guard lk(_mutex);

        // We check if we are closed under the mutex here since someone could have called
        // close() concurrently with the code above.
        if (_isDropped.load()) {
            return _makeReplicaSetMonitorRemovedError();
        }

        return _enqueueOutstandingQuery(lk, criteria, deadline);
    }
}

SemiFuture<std::vector<HostAndPort>> StreamableReplicaSetMonitor::_enqueueOutstandingQuery(
    WithLock, const ReadPreferenceSetting& criteria, const Date_t& deadline) {
    using HostAndPortList = std::vector<HostAndPort>;
    Future<HostAndPortList> result;

    auto query = std::make_shared<HostQuery>();
    query->criteria = criteria;
    query->deadline = deadline;

    auto pf = makePromiseFuture<HostAndPortList>();
    query->promise = std::move(pf.promise);

    auto deadlineCb =
        [this, query, self = shared_from_this()](const TaskExecutor::CallbackArgs& cbArgs) {
            stdx::lock_guard lock(_mutex);
            if (query->done) {
                return;
            }

            const auto cbStatus = cbArgs.status;
            if (!cbStatus.isOK()) {
                query->promise.setError(cbStatus);
                query->done = true;
                return;
            }

            const auto errorStatus = _makeUnsatisfiedReadPrefError(query->criteria);
            query->promise.setError(errorStatus);
            query->done = true;
            LOGV2_INFO(4333208,
                       "RSM {replicaSet} host selection timeout: {status}",
                       "replicaSet"_attr = getName(),
                       "error"_attr = errorStatus.toString());
        };
    auto swDeadlineHandle = _executor->scheduleWorkAt(query->deadline, deadlineCb);

    if (!swDeadlineHandle.isOK()) {
        LOGV2_INFO(4333207,
                   "RSM {replicaSet} error scheduling deadline handler: {status}",
                   "replicaSet"_attr = getName(),
                   "error"_attr = swDeadlineHandle.getStatus());
        return SemiFuture<HostAndPortList>::makeReady(swDeadlineHandle.getStatus());
    }
    query->deadlineHandle = swDeadlineHandle.getValue();
    _outstandingQueries.push_back(query);

    // Send topology changes to the query processor to satisfy the future.
    // It will be removed as a listener when all waiting queries have been satisfied.
    _eventsPublisher->registerListener(_queryProcessor);

    return std::move(pf.future).semi();
}  // namespace mongo

boost::optional<std::vector<HostAndPort>> StreamableReplicaSetMonitor::_getHosts(
    const TopologyDescriptionPtr& topology, const ReadPreferenceSetting& criteria) {
    auto result = _serverSelector->selectServers(topology, criteria);
    if (!result)
        return boost::none;
    return _extractHosts(*result);
}

boost::optional<std::vector<HostAndPort>> StreamableReplicaSetMonitor::_getHosts(
    const ReadPreferenceSetting& criteria) {
    return _getHosts(_currentTopology(), criteria);
}

HostAndPort StreamableReplicaSetMonitor::getMasterOrUassert() {
    return getHostOrRefresh(kPrimaryOnlyReadPreference).get();
}

sdam::TopologyEventsPublisherPtr StreamableReplicaSetMonitor::getEventsPublisher() {
    return _eventsPublisher;
}

void StreamableReplicaSetMonitor::failedHost(const HostAndPort& host, const Status& status) {
    failedHostPostHandshake(host, status, BSONObj());
}

void StreamableReplicaSetMonitor::failedHostPreHandshake(const HostAndPort& host,
                                                         const Status& status,
                                                         BSONObj bson) {
    _failedHost(host, status, bson, HandshakeStage::kPreHandshake, true);
}

void StreamableReplicaSetMonitor::failedHostPostHandshake(const HostAndPort& host,
                                                          const Status& status,
                                                          BSONObj bson) {
    _failedHost(host, status, bson, HandshakeStage::kPostHandshake, true);
}

void StreamableReplicaSetMonitor::_failedHost(const HostAndPort& host,
                                              const Status& status,
                                              BSONObj bson,
                                              HandshakeStage stage,
                                              bool isApplicationOperation) {
    if (_isDropped.load())
        return;

    _doErrorActions(
        host,
        _errorHandler->computeErrorActions(host, status, stage, isApplicationOperation, bson));
}

void StreamableReplicaSetMonitor::_doErrorActions(
    const HostAndPort& host,
    const StreamableReplicaSetMonitorErrorHandler::ErrorActions& errorActions) const {
    {
        stdx::lock_guard lock(_mutex);
        if (_isDropped.load())
            return;

        if (errorActions.dropConnections)
            _connectionManager->dropConnections(host);

        if (errorActions.requestImmediateCheck)
            _isMasterMonitor->requestImmediateCheck();
    }

    // Call outside of the lock since this may generate a topology change event.
    if (errorActions.isMasterOutcome)
        _topologyManager->onServerDescription(*errorActions.isMasterOutcome);
}

boost::optional<ServerDescriptionPtr> StreamableReplicaSetMonitor::_currentPrimary() const {
    return _currentTopology()->getPrimary();
}

bool StreamableReplicaSetMonitor::isPrimary(const HostAndPort& host) const {
    const auto currentPrimary = _currentPrimary();
    return (currentPrimary ? (*currentPrimary)->getAddress() == host.toString() : false);
}

bool StreamableReplicaSetMonitor::isHostUp(const HostAndPort& host) const {
    auto currentTopology = _currentTopology();
    const auto& serverDescription = currentTopology->findServerByAddress(host.toString());
    return serverDescription && (*serverDescription)->getType() != ServerType::kUnknown;
}

int StreamableReplicaSetMonitor::getMinWireVersion() const {
    auto currentTopology = _currentTopology();
    const std::vector<ServerDescriptionPtr>& servers = currentTopology->getServers();
    if (servers.size() > 0) {
        const auto& serverDescription =
            *std::min_element(servers.begin(), servers.end(), minWireCompare);
        return serverDescription->getMinWireVersion();
    } else {
        return 0;
    }
}

int StreamableReplicaSetMonitor::getMaxWireVersion() const {
    auto currentTopology = _currentTopology();
    const std::vector<ServerDescriptionPtr>& servers = currentTopology->getServers();
    if (servers.size() > 0) {
        const auto& serverDescription =
            *std::max_element(servers.begin(), servers.end(), maxWireCompare);
        return serverDescription->getMaxWireVersion();
    } else {
        return std::numeric_limits<int>::max();
    }
}

std::string StreamableReplicaSetMonitor::getName() const {
    return _uri.getSetName();
}

std::string StreamableReplicaSetMonitor::getServerAddress() const {
    const auto topologyDescription = _currentTopology();
    const auto servers = topologyDescription->getServers();

    std::stringstream output;
    output << _uri.getSetName() << "/";

    for (const auto& server : servers) {
        output << server->getAddress();
        if (&server != &servers.back())
            output << ",";
    }

    auto result = output.str();
    return output.str();
}

const MongoURI& StreamableReplicaSetMonitor::getOriginalUri() const {
    return _uri;
}

bool StreamableReplicaSetMonitor::contains(const HostAndPort& host) const {
    return static_cast<bool>(_currentTopology()->findServerByAddress(host.toString()));
}

void StreamableReplicaSetMonitor::appendInfo(BSONObjBuilder& bsonObjBuilder, bool forFTDC) const {
    auto topologyDescription = _currentTopology();

    BSONObjBuilder monitorInfo(bsonObjBuilder.subobjStart(getName()));
    if (forFTDC) {
        for (auto serverDescription : topologyDescription->getServers()) {
            monitorInfo.appendNumber(serverDescription->getAddress(),
                                     pingTimeMillis(serverDescription));
        }
        return;
    }

    // NOTE: the format here must be consistent for backwards compatibility
    BSONArrayBuilder hosts(monitorInfo.subarrayStart("hosts"));
    for (const auto& serverDescription : topologyDescription->getServers()) {
        bool isUp = false;
        bool isMaster = false;
        bool isSecondary = false;
        bool isHidden = false;

        switch (serverDescription->getType()) {
            case ServerType::kRSPrimary:
                isUp = true;
                isMaster = true;
                break;
            case ServerType::kRSSecondary:
                isUp = true;
                isSecondary = true;
                break;
            case ServerType::kStandalone:
                isUp = true;
                break;
            case ServerType::kMongos:
                isUp = true;
                break;
            case ServerType::kRSGhost:
                isHidden = true;
                break;
            case ServerType::kRSArbiter:
                isHidden = true;
                break;
            default:
                break;
        }

        BSONObjBuilder builder(hosts.subobjStart());
        builder.append("addr", serverDescription->getAddress());
        builder.append("ok", isUp);
        builder.append("ismaster", isMaster);  // intentionally not camelCase
        builder.append("hidden", isHidden);
        builder.append("secondary", isSecondary);
        builder.append("pingTimeMillis", pingTimeMillis(serverDescription));

        if (serverDescription->getTags().size()) {
            BSONObjBuilder tagsBuilder(builder.subobjStart("tags"));
            serverDescription->appendBsonTags(tagsBuilder);
        }
    }
}

bool StreamableReplicaSetMonitor::isKnownToHaveGoodPrimary() const {
    return static_cast<bool>(_currentPrimary());
}

sdam::TopologyDescriptionPtr StreamableReplicaSetMonitor::_currentTopology() const {
    return _topologyManager->getTopologyDescription();
}

void StreamableReplicaSetMonitor::_setConfirmedNotifierState(
    WithLock, const ServerDescriptionPtr& primaryDescription) {
    invariant(primaryDescription && primaryDescription->getType() == sdam::ServerType::kRSPrimary);

    const auto& hosts = primaryDescription->getHosts();
    const auto& passives = primaryDescription->getPassives();

    // TODO SERVER-45395: remove need for HostAndPort conversion
    std::set<HostAndPort> confirmedHosts;
    std::transform(hosts.begin(),
                   hosts.end(),
                   std::inserter(confirmedHosts, confirmedHosts.end()),
                   [](const ServerAddress& addr) -> HostAndPort { return HostAndPort(addr); });

    std::set<HostAndPort> confirmedPassives;
    std::transform(passives.begin(),
                   passives.end(),
                   std::inserter(confirmedPassives, confirmedPassives.end()),
                   [](const ServerAddress& addr) -> HostAndPort { return HostAndPort(addr); });

    confirmedHosts.insert(confirmedPassives.begin(), confirmedPassives.end());

    _confirmedNotifierState = ChangeNotifierState{
        HostAndPort(primaryDescription->getAddress()),
        confirmedPassives,
        ConnectionString::forReplicaSet(
            getName(), std::vector<HostAndPort>(confirmedHosts.begin(), confirmedHosts.end()))};
}

void StreamableReplicaSetMonitor::onTopologyDescriptionChangedEvent(
    UUID topologyId,
    TopologyDescriptionPtr previousDescription,
    TopologyDescriptionPtr newDescription) {
    stdx::lock_guard lock(_mutex);
    if (_isDropped.load())
        return;

    // Notify external components if there are membership changes in the topology.
    if (_hasMembershipChange(previousDescription, newDescription)) {
        LOGV2(4333213,
              "RSM {replicaSet} Topology Change: {topologyDescription}",
              "replicaSet"_attr = getName(),
              "topologyDescription"_attr = newDescription->toString());

        auto maybePrimary = newDescription->getPrimary();
        if (maybePrimary) {
            _setConfirmedNotifierState(lock, *maybePrimary);

            ReplicaSetMonitorManager::get()->getNotifier().onConfirmedSet(
                _confirmedNotifierState->connectionString,
                _confirmedNotifierState->primaryAddress,
                _confirmedNotifierState->passives);
        } else {
            if (_confirmedNotifierState) {
                const auto& connectionString = _confirmedNotifierState->connectionString;
                ReplicaSetMonitorManager::get()->getNotifier().onPossibleSet(connectionString);
            } else {
                // No confirmed hosts yet, just send list of hosts that are routable base on type.
                const auto& primaryAndSecondaries =
                    newDescription->findServers(primaryOrSecondaryPredicate);
                if (primaryAndSecondaries.size() == 0) {
                    LOGV2_DEBUG(4645401,
                                kLowerLogLevel,
                                "Skip publishing unconfirmed replica set members since there are "
                                "no primaries or secondaries in the new topology",
                                "replicaSet"_attr = getName());
                    return;
                }

                const auto connectionString = ConnectionString::forReplicaSet(
                    getName(), _extractHosts(primaryAndSecondaries));
                ReplicaSetMonitorManager::get()->getNotifier().onPossibleSet(connectionString);
            }
        }
    }
}

void StreamableReplicaSetMonitor::onServerHeartbeatSucceededEvent(const ServerAddress& hostAndPort,
                                                                  const BSONObj reply) {
    // After the inital handshake, isMasterResponses should not update the RTT with durationMs.
    IsMasterOutcome outcome(hostAndPort, reply, boost::none);
    _topologyManager->onServerDescription(outcome);
}

void StreamableReplicaSetMonitor::onServerHeartbeatFailureEvent(Status errorStatus,
                                                                const ServerAddress& hostAndPort,
                                                                const BSONObj reply) {
    _failedHost(
        HostAndPort(hostAndPort), errorStatus, reply, HandshakeStage::kPostHandshake, false);
}

void StreamableReplicaSetMonitor::onServerPingFailedEvent(const ServerAddress& hostAndPort,
                                                          const Status& status) {
    _failedHost(HostAndPort(hostAndPort), status, BSONObj(), HandshakeStage::kPostHandshake, false);
}

void StreamableReplicaSetMonitor::onServerHandshakeFailedEvent(const sdam::ServerAddress& address,
                                                               const Status& status,
                                                               const BSONObj reply) {
    _failedHost(HostAndPort(address), status, reply, HandshakeStage::kPreHandshake, false);
};

void StreamableReplicaSetMonitor::onServerPingSucceededEvent(sdam::IsMasterRTT durationMS,
                                                             const ServerAddress& hostAndPort) {
    LOGV2_DEBUG(4668132,
                kLowerLogLevel,
                "ReplicaSetMonitor ping success",
                "host"_attr = hostAndPort,
                "replicaSet"_attr = getName(),
                "duration"_attr = durationMS);
    _topologyManager->onServerRTTUpdated(hostAndPort, durationMS);
}

void StreamableReplicaSetMonitor::onServerHandshakeCompleteEvent(sdam::IsMasterRTT durationMs,
                                                                 const ServerAddress& hostAndPort,
                                                                 const BSONObj reply) {
    IsMasterOutcome outcome(hostAndPort, reply, durationMs);
    _topologyManager->onServerDescription(outcome);
}

std::string StreamableReplicaSetMonitor::_logPrefix() {
    return str::stream() << kLogPrefix << " [" << getName() << "] ";
}

void StreamableReplicaSetMonitor::_failOutstandingWithStatus(WithLock, Status status) {
    for (const auto& query : _outstandingQueries) {
        if (query->done)
            continue;

        query->done = true;
        _executor->cancel(query->deadlineHandle);
        query->promise.setError(status);
    }
    _outstandingQueries.clear();
}

bool StreamableReplicaSetMonitor::_hasMembershipChange(
    sdam::TopologyDescriptionPtr oldDescription, sdam::TopologyDescriptionPtr newDescription) {

    if (oldDescription->getServers().size() != newDescription->getServers().size())
        return true;

    for (const auto& server : oldDescription->getServers()) {
        const auto newServer = newDescription->findServerByAddress(server->getAddress());
        if (!newServer)
            return true;
        const ServerDescription& s = *server;
        const ServerDescription& ns = **newServer;
        if (s != ns)
            return true;
    }

    for (const auto& server : newDescription->getServers()) {
        auto oldServer = oldDescription->findServerByAddress(server->getAddress());
        if (!oldServer)
            return true;
    }

    return false;
}

void StreamableReplicaSetMonitor::_processOutstanding(
    const TopologyDescriptionPtr& topologyDescription) {

    // Note that a possible performance optimization is:
    // instead of calling _getHosts for every outstanding query, we could
    // first group into equivalence classes then call _getHosts once per class.

    stdx::lock_guard lock(_mutex);

    bool shouldRemove;
    auto it = _outstandingQueries.begin();
    while (it != _outstandingQueries.end()) {
        auto& query = *it;
        shouldRemove = false;

        if (query->done) {
            shouldRemove = true;
        } else {
            auto result = _getHosts(topologyDescription, query->criteria);
            if (result) {
                _executor->cancel(query->deadlineHandle);
                query->done = true;
                query->promise.emplaceValue(std::move(*result));
                const auto latency = _executor->now() - query->start;
                LOGV2_DEBUG(433214,
                            kLowerLogLevel,
                            "RSM {replicaSet} finished async getHosts: {readPref} ({latency})",
                            "replicaSet"_attr = getName(),
                            "readPref"_attr = readPrefToStringWithMinOpTime(query->criteria),
                            "latency"_attr = latency.toString());
                shouldRemove = true;
            }
        }

        it = (shouldRemove) ? _outstandingQueries.erase(it) : ++it;
    }

    if (_outstandingQueries.size()) {
        // enable expedited mode
        _isMasterMonitor->requestImmediateCheck();
    } else {
        // if no more outstanding queries, no need to listen for topology changes in
        // this monitor.
        _eventsPublisher->removeListener(_queryProcessor);
    }
}

Status StreamableReplicaSetMonitor::_makeUnsatisfiedReadPrefError(
    const ReadPreferenceSetting& criteria) const {
    return Status(ErrorCodes::FailedToSatisfyReadPreference,
                  str::stream() << "Could not find host matching read preference "
                                << criteria.toString() << " for set " << getName());
}

Status StreamableReplicaSetMonitor::_makeReplicaSetMonitorRemovedError() const {
    return Status(ErrorCodes::ReplicaSetMonitorRemoved,
                  str::stream() << "ReplicaSetMonitor for set " << getName() << " is removed");
}

void StreamableReplicaSetMonitor::runScanForMockReplicaSet() {
    MONGO_UNREACHABLE;
}
}  // namespace mongo
