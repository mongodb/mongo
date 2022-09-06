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

#include "mongo/platform/basic.h"

#include "mongo/client/streamable_replica_set_monitor.h"

#include <algorithm>
#include <limits>
#include <set>

#include "mongo/bson/simple_bsonelement_comparator.h"
#include "mongo/client/connpool.h"
#include "mongo/client/global_conn_pool.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/replica_set_monitor_server_parameters_gen.h"
#include "mongo/client/streamable_replica_set_monitor_discovery_time_processor.h"
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
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/string_map.h"
#include "mongo/util/timer.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


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

std::string readPrefToStringFull(const ReadPreferenceSetting& readPref) {
    BSONObjBuilder builder;
    readPref.toInnerBSON(&builder);
    if (!readPref.minClusterTime.isNull()) {
        builder.append("minClusterTime", readPref.minClusterTime.toBSON());
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

double pingTimeMillis(const ServerDescriptionPtr& serverDescription) {
    const auto& serverRtt = serverDescription->getRtt();
    // Convert to micros so we don't lose information if under a ms
    return (serverRtt) ? durationCount<Microseconds>(*serverRtt) / 1000.0
                       : durationCount<Milliseconds>(Milliseconds::max());
}

constexpr auto kZeroMs = Milliseconds(0);

Status makeUnsatisfiedReadPrefError(const std::string& name,
                                    const ReadPreferenceSetting& criteria) {
    return Status(ErrorCodes::FailedToSatisfyReadPreference,
                  str::stream() << "Could not find host matching read preference "
                                << criteria.toString() << " for set " << name);
}

Status makeReplicaSetMonitorRemovedError(const std::string& name) {
    return Status(ErrorCodes::ShutdownInProgress,
                  str::stream() << "ReplicaSetMonitor for set " << name << " is removed");
}

bool hasMembershipChange(sdam::TopologyDescriptionPtr oldDescription,
                         sdam::TopologyDescriptionPtr newDescription) {
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
    std::shared_ptr<executor::EgressTagCloser> connectionManager,
    std::function<void()> cleanupCallback,
    std::shared_ptr<ReplicaSetMonitorManagerStats> managerStats)
    : ReplicaSetMonitor(cleanupCallback),
      _errorHandler(std::make_unique<SdamErrorHandler>(uri.getSetName())),
      _queryProcessor(std::make_shared<StreamableReplicaSetMonitorQueryProcessor>()),
      _primaryDiscoveryTimeProcessor(
          std::make_shared<StreamableReplicaSetMonitorDiscoveryTimeProcessor>()),
      _uri(uri),
      _connectionManager(connectionManager),
      _executor(executor),
      _stats(std::make_shared<ReplicaSetMonitorStats>(managerStats)) {
    // Maintain order of original seed list
    std::vector<HostAndPort> seedsNoDups;
    std::set<HostAndPort> alreadyAdded;

    const auto& seeds = uri.getServers();
    for (const auto& seed : seeds) {
        if (alreadyAdded.find(seed) == alreadyAdded.end()) {
            seedsNoDups.push_back(seed);
            alreadyAdded.insert(seed);
        }
    }

    // StreamableReplicaSetMonitor cannot be used with kSingle type, thus we know that the type is
    // kReplicaSetNoPrimary. We need to save the expected set name to avoid the case when the
    // provided seed address contains a ReplicaSet with different name (deployment mistake).
    _sdamConfig =
        SdamConfiguration(seedsNoDups, TopologyType::kReplicaSetNoPrimary, _uri.getSetName());
    _serverSelector = std::make_unique<SdamServerSelector>(_sdamConfig);
}

StreamableReplicaSetMonitor::~StreamableReplicaSetMonitor() {
    // `drop` is idempotent and a duplicate call from ReplicaSetMonitorManager::removeMonitor() is
    // safe.
    drop();
}

ReplicaSetMonitorPtr StreamableReplicaSetMonitor::make(
    const MongoURI& uri,
    std::shared_ptr<TaskExecutor> executor,
    std::shared_ptr<executor::EgressTagCloser> connectionManager,
    std::function<void()> cleanupCallback,
    std::shared_ptr<ReplicaSetMonitorManagerStats> managerStats) {
    auto result = std::make_shared<StreamableReplicaSetMonitor>(
        uri, executor, connectionManager, cleanupCallback, managerStats);
    result->init();
    invariant(managerStats);
    return result;
}

void StreamableReplicaSetMonitor::init() {
    stdx::lock_guard lock(_mutex);
    LOGV2_DEBUG(4333206,
                kLowerLogLevel,
                "Starting Replica Set Monitor {uri}",
                "Starting Replica Set Monitor",
                "uri"_attr = _uri,
                "config"_attr = _sdamConfig.toBson());

    invariant(shared_from_this().use_count() > 1,
              "StreamableReplicaSetMonitor::init() is invoked when there is no owner");

    _eventsPublisher = std::make_shared<sdam::TopologyEventsPublisher>(_executor);
    _topologyManager = std::make_unique<TopologyManagerImpl>(
        _sdamConfig, getGlobalServiceContext()->getPreciseClockSource(), _eventsPublisher);

    _eventsPublisher->registerListener(weak_from_this());

    _pingMonitor = std::make_unique<ServerPingMonitor>(
        _uri, _eventsPublisher.get(), _sdamConfig.getHeartBeatFrequency(), _executor);
    _eventsPublisher->registerListener(_pingMonitor);

    _serverDiscoveryMonitor =
        std::make_unique<ServerDiscoveryMonitor>(_uri,
                                                 _sdamConfig,
                                                 _eventsPublisher,
                                                 _topologyManager->getTopologyDescription(),
                                                 _stats,
                                                 _executor);
    _eventsPublisher->registerListener(_serverDiscoveryMonitor);

    _eventsPublisher->registerListener(_queryProcessor);

    _eventsPublisher->registerListener(_primaryDiscoveryTimeProcessor);

    _isDropped.store(false);

    ReplicaSetMonitorManager::get()->getNotifier().onFoundSet(getName());
}

void StreamableReplicaSetMonitor::initForTesting(sdam::TopologyManagerPtr topologyManager) {
    stdx::lock_guard lock(_mutex);

    _eventsPublisher = std::make_shared<sdam::TopologyEventsPublisher>(_executor);
    _topologyManager = std::move(topologyManager);

    _eventsPublisher->registerListener(weak_from_this());

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

    LOGV2(4333209,
          "Closing Replica Set Monitor {replicaSet}",
          "Closing Replica Set Monitor",
          "replicaSet"_attr = getName());
    _queryProcessor->shutdown();

    if (_pingMonitor) {
        _pingMonitor->shutdown();
    }

    if (_serverDiscoveryMonitor) {
        _serverDiscoveryMonitor->shutdown();
    }

    ReplicaSetMonitorManager::get()->getNotifier().onDroppedSet(getName());
    LOGV2(4333210,
          "Done closing Replica Set Monitor {replicaSet}",
          "Done closing Replica Set Monitor",
          "replicaSet"_attr = getName());
}

SemiFuture<HostAndPort> StreamableReplicaSetMonitor::getHostOrRefresh(
    const ReadPreferenceSetting& criteria,
    const std::vector<HostAndPort>& excludedHosts,
    const CancellationToken& cancelToken) {
    return getHostsOrRefresh(criteria, excludedHosts, cancelToken)
        .thenRunOn(_executor)
        .then([self = shared_from_this()](const std::vector<HostAndPort>& result) {
            invariant(!result.empty());
            // We do a random shuffle when we get the hosts so we can just pick the first one
            return result[0];
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
    const ReadPreferenceSetting& criteria,
    const std::vector<HostAndPort>& excludedHosts,
    const CancellationToken& cancelToken) {
    // In the fast case (stable topology), we avoid mutex acquisition.
    if (_isDropped.load()) {
        return makeReplicaSetMonitorRemovedError(getName());
    }

    // start counting from the beginning of the operation
    const auto deadline =
        _executor->now() + Milliseconds(gDefaultFindReplicaSetHostTimeoutMS.load());

    // try to satisfy query immediately
    auto immediateResult = _getHosts(criteria, excludedHosts);
    if (immediateResult) {
        return {*immediateResult};
    }

    if (_serverDiscoveryMonitor) {
        _serverDiscoveryMonitor->requestImmediateCheck();
    }

    LOGV2_DEBUG(4333212,
                kLowerLogLevel,
                "RSM {replicaSet} start async getHosts with {readPref}",
                "RSM start async getHosts",
                "replicaSet"_attr = getName(),
                "readPref"_attr = readPrefToStringFull(criteria));

    // Fail fast on timeout or cancellation.
    const Date_t& now = _executor->now();
    if (deadline <= now || cancelToken.isCanceled()) {
        return makeUnsatisfiedReadPrefError(getName(), criteria);
    }

    return _topologyManager->executeWithLock([this, criteria, cancelToken, deadline, excludedHosts](
                                                 const TopologyDescriptionPtr& topologyDescription)
                                                 -> SemiFuture<std::vector<HostAndPort>> {
        stdx::lock_guard lk(_mutex);

        // We check if we are closed under the mutex here since someone could have called
        // close() concurrently with the code above.
        if (_isDropped.load()) {
            return makeReplicaSetMonitorRemovedError(getName());
        }
        // try to satisfy the query again while holding both the StreamableRSM mutex and
        // TopologyManager mutex to avoid missing any topology change that has occurred
        // since the last check.
        auto immediateResult = _getHosts(topologyDescription, criteria, excludedHosts);
        if (immediateResult) {
            return {*immediateResult};
        }

        return _enqueueOutstandingQuery(lk, criteria, excludedHosts, cancelToken, deadline);
    });
}

SemiFuture<std::vector<HostAndPort>> StreamableReplicaSetMonitor::_enqueueOutstandingQuery(
    WithLock,
    const ReadPreferenceSetting& criteria,
    const std::vector<HostAndPort>& excludedHosts,
    const CancellationToken& cancelToken,
    const Date_t& deadline) {

    auto query = std::make_shared<HostQuery>(_stats);
    query->criteria = criteria;
    query->excludedHosts = excludedHosts;

    auto pf = makePromiseFuture<std::vector<HostAndPort>>();
    query->promise = std::move(pf.promise);

    // Make the deadline task cancelable for when the query is satisfied or when the input
    // cancelToken is canceled.
    query->deadlineCancelSource = CancellationSource(cancelToken);
    query->start = _executor->now();

    // Add the query to the list of outstanding queries.
    auto queryIter = _outstandingQueries.insert(_outstandingQueries.end(), query);

    // After a deadline or when the input cancellation token is canceled, cancel this query. If the
    // query completes first, the deadlineCancelSource will be used to cancel this task.
    _executor->sleepUntil(deadline, query->deadlineCancelSource.token())
        .getAsync([this, query, queryIter, self = shared_from_this(), cancelToken](Status status) {
            // If the deadline was reached or cancellation occurred on the input cancellation token,
            // mark the query as canceled. Otherwise, the deadlineCancelSource must have been
            // canceled due to the query completing successfully.
            if (status.isOK() || cancelToken.isCanceled()) {
                auto errorStatus = makeUnsatisfiedReadPrefError(self->getName(), query->criteria);
                // Mark query as done, and if it wasn't already done, remove it from the list of
                // outstanding queries.
                if (query->tryCancel(errorStatus)) {
                    LOGV2_INFO(4333208,
                               "RSM {replicaSet} host selection timeout: {error}",
                               "RSM host selection timeout",
                               "replicaSet"_attr = self->getName(),
                               "error"_attr = errorStatus.toString());

                    stdx::lock_guard lk(_mutex);
                    // Check that the RSM hasn't been dropped (and _outstandingQueries has not
                    // been cleared) before erasing.
                    if (!_isDropped.load()) {
                        invariant(_outstandingQueries.size() > 0);
                        _eraseQueryFromOutstandingQueries(lk, queryIter);
                    }
                }
            }
        });

    return std::move(pf.future).semi();
}

boost::optional<std::vector<HostAndPort>> StreamableReplicaSetMonitor::_getHosts(
    const TopologyDescriptionPtr& topology,
    const ReadPreferenceSetting& criteria,
    const std::vector<HostAndPort>& excludedHosts) {
    auto result = _serverSelector->selectServers(topology, criteria, excludedHosts);
    if (!result)
        return boost::none;
    return _extractHosts(*result);
}

boost::optional<std::vector<HostAndPort>> StreamableReplicaSetMonitor::_getHosts(
    const ReadPreferenceSetting& criteria, const std::vector<HostAndPort>& excludedHosts) {
    return _getHosts(_currentTopology(), criteria, excludedHosts);
}

HostAndPort StreamableReplicaSetMonitor::getPrimaryOrUassert() {
    return ReplicaSetMonitorInterface::getHostOrRefresh(kPrimaryOnlyReadPreference,
                                                        CancellationToken::uncancelable())
        .get();
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

        if (errorActions.requestImmediateCheck && _serverDiscoveryMonitor)
            _serverDiscoveryMonitor->requestImmediateCheck();
    }

    // Call outside of the lock since this may generate a topology change event.
    if (errorActions.helloOutcome)
        _topologyManager->onServerDescription(*errorActions.helloOutcome);
}

boost::optional<ServerDescriptionPtr> StreamableReplicaSetMonitor::_currentPrimary() const {
    return _currentTopology()->getPrimary();
}

bool StreamableReplicaSetMonitor::isPrimary(const HostAndPort& host) const {
    const auto currentPrimary = _currentPrimary();
    return (currentPrimary ? (*currentPrimary)->getAddress() == host : false);
}

bool StreamableReplicaSetMonitor::isHostUp(const HostAndPort& host) const {
    auto currentTopology = _currentTopology();
    const auto& serverDescription = currentTopology->findServerByAddress(host);
    return serverDescription && (*serverDescription)->getType() != ServerType::kUnknown;
}

int StreamableReplicaSetMonitor::getMinWireVersion() const {
    auto currentTopology = _currentTopology();
    const std::vector<ServerDescriptionPtr>& servers = currentTopology->findServers(
        [](const ServerDescriptionPtr& s) { return s->getType() != ServerType::kUnknown; });
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
    const std::vector<ServerDescriptionPtr>& servers = currentTopology->findServers(
        [](const ServerDescriptionPtr& s) { return s->getType() != ServerType::kUnknown; });
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
    return static_cast<bool>(_currentTopology()->findServerByAddress(host));
}

void StreamableReplicaSetMonitor::appendInfo(BSONObjBuilder& bsonObjBuilder, bool forFTDC) const {
    auto topologyDescription = _currentTopology();

    BSONObjBuilder monitorInfo(bsonObjBuilder.subobjStart(getName()));
    if (forFTDC) {
        for (auto serverDescription : topologyDescription->getServers()) {
            monitorInfo.appendNumber(serverDescription->getAddress().toString(),
                                     pingTimeMillis(serverDescription));
        }
        return;
    }

    // NOTE: the format here must be consistent for backwards compatibility
    BSONArrayBuilder hosts(monitorInfo.subarrayStart("hosts"));
    for (const auto& serverDescription : topologyDescription->getServers()) {
        bool isUp = false;
        bool isWritablePrimary = false;
        bool isSecondary = false;
        bool isHidden = false;

        switch (serverDescription->getType()) {
            case ServerType::kRSPrimary:
                isUp = true;
                isWritablePrimary = true;
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
        builder.append("addr", serverDescription->getAddress().toString());
        builder.append("ok", isUp);
        builder.append("ismaster", isWritablePrimary);  // intentionally not camelCase
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

    auto hosts = primaryDescription->getHosts();
    auto passives = primaryDescription->getPassives();
    hosts.insert(passives.begin(), passives.end());

    _confirmedNotifierState =
        ChangeNotifierState{primaryDescription->getAddress(),
                            passives,
                            ConnectionString::forReplicaSet(
                                getName(), std::vector<HostAndPort>(hosts.begin(), hosts.end()))};
}

void StreamableReplicaSetMonitor::onTopologyDescriptionChangedEvent(
    TopologyDescriptionPtr previousDescription, TopologyDescriptionPtr newDescription) {
    stdx::unique_lock<Latch> lock(_mutex);
    if (_isDropped.load())
        return;

    // Notify external components if there are membership changes in the topology.
    if (hasMembershipChange(previousDescription, newDescription)) {
        LOGV2(4333213,
              "RSM {replicaSet} Topology Change: {newTopologyDescription}",
              "RSM Topology Change",
              "replicaSet"_attr = getName(),
              "newTopologyDescription"_attr = newDescription->toBSON(),
              "previousTopologyDescription"_attr = previousDescription->toBSON());

        auto maybePrimary = newDescription->getPrimary();
        if (maybePrimary) {
            _setConfirmedNotifierState(lock, *maybePrimary);

            lock.unlock();
            ReplicaSetMonitorManager::get()->getNotifier().onConfirmedSet(
                _confirmedNotifierState->connectionString,
                _confirmedNotifierState->primaryAddress,
                _confirmedNotifierState->passives);
        } else {
            if (_confirmedNotifierState) {
                const auto& connectionString = _confirmedNotifierState->connectionString;
                lock.unlock();
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

                lock.unlock();
                ReplicaSetMonitorManager::get()->getNotifier().onPossibleSet(connectionString);
            }
        }
    }

    if (auto previousMaxElectionIdSetVersionPair =
            previousDescription->getMaxElectionIdSetVersionPair(),
        newMaxElectionIdSetVersionPair = newDescription->getMaxElectionIdSetVersionPair();
        setVersionWentBackwards(previousMaxElectionIdSetVersionPair,
                                newMaxElectionIdSetVersionPair)) {
        // The previous primary was unable to reach consensus for the config with
        // higher version and it was abandoned after failover.
        LOGV2(5940902,
              "Max known Set version coming from new primary forces to rollback it backwards",
              "replicaSet"_attr = getName(),
              "newElectionIdSetVersion"_attr = newMaxElectionIdSetVersionPair.setVersion,
              "previousMaxElectionIdSetVersion"_attr =
                  previousMaxElectionIdSetVersionPair.setVersion);
    }
}

void StreamableReplicaSetMonitor::onServerHeartbeatSucceededEvent(const HostAndPort& hostAndPort,
                                                                  const BSONObj reply) {
    // After the inital handshake, hello responses should not update the RTT with durationMs.
    HelloOutcome outcome(hostAndPort, reply, boost::none);
    _topologyManager->onServerDescription(outcome);
}

void StreamableReplicaSetMonitor::onServerHeartbeatFailureEvent(Status errorStatus,
                                                                const HostAndPort& hostAndPort,
                                                                const BSONObj reply) {
    _failedHost(
        HostAndPort(hostAndPort), errorStatus, reply, HandshakeStage::kPostHandshake, false);
}

void StreamableReplicaSetMonitor::onServerPingFailedEvent(const HostAndPort& hostAndPort,
                                                          const Status& status) {
    _failedHost(HostAndPort(hostAndPort), status, BSONObj(), HandshakeStage::kPostHandshake, false);
}

void StreamableReplicaSetMonitor::onServerHandshakeFailedEvent(const HostAndPort& address,
                                                               const Status& status,
                                                               const BSONObj reply) {
    _failedHost(HostAndPort(address), status, reply, HandshakeStage::kPreHandshake, false);
};

void StreamableReplicaSetMonitor::onServerPingSucceededEvent(sdam::HelloRTT durationMS,
                                                             const HostAndPort& hostAndPort) {
    LOGV2_DEBUG(4668132,
                kLowerLogLevel,
                "ReplicaSetMonitor ping success",
                "host"_attr = hostAndPort,
                "replicaSet"_attr = getName(),
                "duration"_attr = durationMS);
    _topologyManager->onServerRTTUpdated(hostAndPort, durationMS);
}

void StreamableReplicaSetMonitor::onServerHandshakeCompleteEvent(sdam::HelloRTT durationMs,
                                                                 const HostAndPort& hostAndPort,
                                                                 const BSONObj reply) {
    HelloOutcome outcome(hostAndPort, reply, durationMs);
    _topologyManager->onServerDescription(outcome);
}

std::string StreamableReplicaSetMonitor::_logPrefix() {
    return str::stream() << kLogPrefix << " [" << getName() << "] ";
}

void StreamableReplicaSetMonitor::_failOutstandingWithStatus(WithLock, Status status) {
    for (const auto& query : _outstandingQueries) {
        (void)query->tryCancel(status);
    }
    _outstandingQueries.clear();
}

std::list<StreamableReplicaSetMonitor::HostQueryPtr>::iterator
StreamableReplicaSetMonitor::_eraseQueryFromOutstandingQueries(
    WithLock, std::list<HostQueryPtr>::iterator iter) {
    return _outstandingQueries.erase(iter);
}

void StreamableReplicaSetMonitor::_processOutstanding(
    const TopologyDescriptionPtr& topologyDescription) {

    // Note that a possible performance optimization is:
    // instead of calling _getHosts for every outstanding query, we could
    // first group into equivalence classes then call _getHosts once per class.

    stdx::lock_guard lock(_mutex);

    auto it = _outstandingQueries.begin();
    bool hadUnresolvedQuery{false};

    // Iterate through the outstanding queries and try to resolve them via calls to _getHosts. If we
    // succeed in resolving a query, the query is removed from the list. If a query has already been
    // canceled, or there are no results, it will be skipped. Cancellation logic elsewhere will
    // handle removing the canceled queries from the list.
    while (it != _outstandingQueries.end()) {
        auto& query = *it;

        // If query has not been canceled yet, try to satisfy it.
        if (!query->hasBeenResolved()) {
            auto result = _getHosts(topologyDescription, query->criteria, query->excludedHosts);
            if (result) {
                if (query->tryResolveWithSuccess(std::move(*result))) {
                    const auto latency = _executor->now() - query->start;
                    LOGV2_DEBUG(433214,
                                kLowerLogLevel,
                                "RSM {replicaSet} finished async getHosts: {readPref} ({duration})",
                                "RSM finished async getHosts",
                                "replicaSet"_attr = getName(),
                                "readPref"_attr = readPrefToStringFull(query->criteria),
                                "duration"_attr = Milliseconds(latency));

                    it = _eraseQueryFromOutstandingQueries(lock, it);
                } else {
                    // The query was canceled, so skip to the next entry without erasing it.
                    ++it;
                }
            } else {
                // Results were not available, so skip to the next entry without erasing it.
                ++it;
                hadUnresolvedQuery = true;
            }
        } else {
            // The query was canceled, so skip to the next entry without erasing it.
            ++it;
        }
    }

    // If there remain unresolved queries, enable expedited mode.
    if (hadUnresolvedQuery && _serverDiscoveryMonitor) {
        _serverDiscoveryMonitor->requestImmediateCheck();
    }
}

void StreamableReplicaSetMonitor::runScanForMockReplicaSet() {
    MONGO_UNREACHABLE;
}
}  // namespace mongo
