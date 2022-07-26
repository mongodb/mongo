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

#include "mongo/client/server_discovery_monitor.h"

#include <algorithm>
#include <iterator>

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_server_parameters.h"
#include "mongo/client/sdam/sdam.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(overrideMaxAwaitTimeMS);

using executor::NetworkInterface;
using executor::NetworkInterfaceThreadPool;
using executor::TaskExecutor;
using executor::ThreadPoolTaskExecutor;

const Milliseconds kZeroMs = Milliseconds{0};

/**
 * Given the TopologyVersion corresponding to a remote host, determines if exhaust is enabled.
 */
bool exhaustEnabled(boost::optional<TopologyVersion> topologyVersion) {
    return (topologyVersion &&
            gReplicaSetMonitorProtocol == ReplicaSetMonitorProtocol::kStreamable);
}

}  // namespace

SingleServerDiscoveryMonitor::SingleServerDiscoveryMonitor(
    const MongoURI& setUri,
    const HostAndPort& host,
    boost::optional<TopologyVersion> topologyVersion,
    const SdamConfiguration& sdamConfig,
    sdam::TopologyEventsPublisherPtr eventListener,
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<ReplicaSetMonitorStats> stats)
    : _host(host),
      _stats(stats),
      _topologyVersion(topologyVersion),
      _eventListener(eventListener),
      _executor(executor),
      _heartbeatFrequency(_overrideRefreshPeriod(sdamConfig.getHeartBeatFrequency())),
      _connectTimeout(sdamConfig.getConnectionTimeout()),
      _isExpedited(true),
      _isShutdown(true),
      _setUri(setUri) {
    LOGV2_DEBUG(4333217,
                kLogLevel + 1,
                "RSM {replicaSet} monitoring {host}",
                "RSM monitoring host",
                "host"_attr = host,
                "replicaSet"_attr = _setUri.getSetName());
}

void SingleServerDiscoveryMonitor::init() {
    stdx::lock_guard lock(_mutex);
    _isShutdown = false;
    _scheduleNextHello(lock, Milliseconds(0));
}

void SingleServerDiscoveryMonitor::requestImmediateCheck() {
    stdx::lock_guard lock(_mutex);
    if (_isShutdown)
        return;

    // The previous refresh period may or may not have been expedited.
    // Saving the value here before we change to expedited mode.
    const auto previousRefreshPeriod = _currentRefreshPeriod(lock, false);

    if (!_isExpedited) {
        // save some log lines.
        LOGV2_DEBUG(
            4333227,
            kLogLevel,
            "RSM {replicaSet} monitoring {host} in expedited mode until we detect a primary.",
            "RSM monitoring host in expedited mode until we detect a primary",
            "host"_attr = _host,
            "replicaSet"_attr = _setUri.getSetName());

        // This will change the _currentRefreshPeriod to the shorter expedited duration.
        _isExpedited = true;
    }

    // Get the new expedited refresh period.
    const auto expeditedRefreshPeriod = _currentRefreshPeriod(lock, false);

    if (_helloOutstanding) {
        LOGV2_DEBUG(
            4333216,
            kLogLevel + 2,
            "RSM {replicaSet} immediate hello check requested, but there "
            "is already an outstanding request",
            "RSM immediate hello check requested, but there is already an outstanding request",
            "replicaSet"_attr = _setUri.getSetName());
        return;
    }

    if (const auto maybeDelayUntilNextCheck = calculateExpeditedDelayUntilNextCheck(
            _timeSinceLastCheck(), expeditedRefreshPeriod, previousRefreshPeriod)) {
        _rescheduleNextHello(lock, *maybeDelayUntilNextCheck);
    }
}

boost::optional<Milliseconds> SingleServerDiscoveryMonitor::calculateExpeditedDelayUntilNextCheck(
    const boost::optional<Milliseconds>& maybeTimeSinceLastCheck,
    const Milliseconds& expeditedRefreshPeriod,
    const Milliseconds& previousRefreshPeriod) {
    invariant(expeditedRefreshPeriod.count() <= previousRefreshPeriod.count());

    const auto timeSinceLastCheck =
        (maybeTimeSinceLastCheck) ? *maybeTimeSinceLastCheck : Milliseconds::max();
    invariant(timeSinceLastCheck.count() >= 0);

    if (timeSinceLastCheck == previousRefreshPeriod)
        return boost::none;

    if (timeSinceLastCheck > expeditedRefreshPeriod)
        return Milliseconds(0);

    const auto delayUntilExistingRequest = previousRefreshPeriod - timeSinceLastCheck;

    // Calculate when the next hello should be scheduled.
    const Milliseconds delayUntilNextCheck = expeditedRefreshPeriod - timeSinceLastCheck;

    // Do nothing if the time would be greater-than or equal to the existing request.
    return (delayUntilNextCheck >= delayUntilExistingRequest)
        ? boost::none
        : boost::optional<Milliseconds>(delayUntilNextCheck);
}

boost::optional<Milliseconds> SingleServerDiscoveryMonitor::_timeSinceLastCheck() const {
    // Since the system clock is not monotonic, the returned value can be negative. In this case we
    // choose a conservative estimate of 0ms as the time we last checked.
    return (_lastHelloAt)
        ? boost::optional<Milliseconds>(std::max(Milliseconds(0), _executor->now() - *_lastHelloAt))
        : boost::none;
}

void SingleServerDiscoveryMonitor::_rescheduleNextHello(WithLock lock, Milliseconds delay) {
    LOGV2_DEBUG(4333218,
                kLogLevel,
                "Rescheduling the next replica set monitoring request",
                "replicaSet"_attr = _setUri.getSetName(),
                "host"_attr = _host,
                "delay"_attr = delay);
    _cancelOutstandingRequest(lock);
    _scheduleNextHello(lock, delay);
}

void SingleServerDiscoveryMonitor::_scheduleNextHello(WithLock, Milliseconds delay) {
    if (_isShutdown)
        return;

    invariant(!_helloOutstanding);

    auto swCbHandle = _executor->scheduleWorkAt(
        _executor->now() + delay,
        [self = shared_from_this()](const executor::TaskExecutor::CallbackArgs& cbData) {
            if (!cbData.status.isOK()) {
                return;
            }

            self->_doRemoteCommand();
        });

    if (!swCbHandle.isOK()) {
        _onHelloFailure(swCbHandle.getStatus(), BSONObj());
        return;
    }

    _nextHelloHandle = swCbHandle.getValue();
}

void SingleServerDiscoveryMonitor::_doRemoteCommand() {
    stdx::lock_guard lock(_mutex);
    if (_isShutdown)
        return;

    StatusWith<executor::TaskExecutor::CallbackHandle> swCbHandle = [&]() {
        if (exhaustEnabled(_topologyVersion)) {
            return _scheduleStreamableHello();
        }
        return _scheduleSingleHello();
    }();

    if (!swCbHandle.isOK()) {
        _onHelloFailure(swCbHandle.getStatus(), BSONObj());
        uasserted(4615612, swCbHandle.getStatus().toString());
    }

    _helloOutstanding = true;
    _remoteCommandHandle = swCbHandle.getValue();
}

StatusWith<TaskExecutor::CallbackHandle> SingleServerDiscoveryMonitor::_scheduleStreamableHello() {
    auto maxAwaitTimeMS = durationCount<Milliseconds>(kMaxAwaitTime);
    overrideMaxAwaitTimeMS.execute([&](const BSONObj& data) {
        maxAwaitTimeMS =
            durationCount<Milliseconds>(Milliseconds(data["maxAwaitTimeMS"].numberInt()));
    });

    BSONObjBuilder bob;
    bob.append("isMaster", 1);
    bob.append("maxAwaitTimeMS", maxAwaitTimeMS);
    bob.append("topologyVersion", _topologyVersion->toBSON());

    if (auto wireSpec = WireSpec::instance().get(); wireSpec->isInternalClient) {
        WireSpec::appendInternalClientWireVersion(wireSpec->outgoing, &bob);
    }

    const auto timeoutMS = _connectTimeout + kMaxAwaitTime;
    auto request =
        executor::RemoteCommandRequest(HostAndPort(_host), "admin", bob.obj(), nullptr, timeoutMS);
    request.sslMode = _setUri.getSSLMode();

    auto swCbHandle = _executor->scheduleExhaustRemoteCommand(
        std::move(request),
        [self = shared_from_this(), helloStats = _stats->collectHelloStats()](
            const executor::TaskExecutor::RemoteCommandCallbackArgs& result) mutable {
            Milliseconds nextRefreshPeriod;
            {
                stdx::lock_guard lk(self->_mutex);

                if (self->_isShutdown) {
                    self->_helloOutstanding = false;
                    LOGV2_DEBUG(4495400,
                                kLogLevel,
                                "RSM {replicaSet} not processing response: {error}",
                                "RSM not processing response",
                                "error"_attr = result.response.status,
                                "replicaSet"_attr = self->_setUri.getSetName());
                    return;
                }

                auto responseTopologyVersion = result.response.data.getField("topologyVersion");
                if (responseTopologyVersion) {
                    self->_topologyVersion = TopologyVersion::parse(
                        IDLParserContext("TopologyVersion"), responseTopologyVersion.Obj());
                } else {
                    self->_topologyVersion = boost::none;
                }

                self->_lastHelloAt = self->_executor->now();
                if (!result.response.isOK() || !result.response.moreToCome) {
                    self->_helloOutstanding = false;
                    nextRefreshPeriod = self->_currentRefreshPeriod(lk, result.response.isOK());
                    self->_scheduleNextHello(lk, nextRefreshPeriod);
                }
            }

            if (result.response.isOK()) {
                self->_onHelloSuccess(result.response.data);
            } else {
                self->_onHelloFailure(result.response.status, result.response.data);
            }
        });

    return swCbHandle;
}

StatusWith<TaskExecutor::CallbackHandle> SingleServerDiscoveryMonitor::_scheduleSingleHello() {
    BSONObjBuilder bob;
    bob.append("isMaster", 1);
    if (auto wireSpec = WireSpec::instance().get(); wireSpec->isInternalClient) {
        WireSpec::appendInternalClientWireVersion(wireSpec->outgoing, &bob);
    }

    auto request = executor::RemoteCommandRequest(
        HostAndPort(_host), "admin", bob.obj(), nullptr, _connectTimeout);
    request.sslMode = _setUri.getSSLMode();

    auto swCbHandle = _executor->scheduleRemoteCommand(
        std::move(request),
        [self = shared_from_this(), helloStats = _stats->collectHelloStats()](
            const executor::TaskExecutor::RemoteCommandCallbackArgs& result) mutable {
            Milliseconds nextRefreshPeriod;
            {
                stdx::lock_guard lk(self->_mutex);
                self->_helloOutstanding = false;

                if (self->_isShutdown) {
                    LOGV2_DEBUG(4333219,
                                kLogLevel,
                                "RSM {replicaSet} not processing response: {error}",
                                "RSM not processing response",
                                "error"_attr = result.response.status,
                                "replicaSet"_attr = self->_setUri.getSetName());
                    return;
                }

                self->_lastHelloAt = self->_executor->now();

                auto responseTopologyVersion = result.response.data.getField("topologyVersion");
                if (responseTopologyVersion) {
                    self->_topologyVersion = TopologyVersion::parse(
                        IDLParserContext("TopologyVersion"), responseTopologyVersion.Obj());
                } else {
                    self->_topologyVersion = boost::none;
                }

                if (!result.response.isOK() || !result.response.moreToCome) {
                    self->_helloOutstanding = false;

                    // Prevent immediate rescheduling when exhaust is not supported.
                    auto scheduleImmediately =
                        (exhaustEnabled(self->_topologyVersion)) ? result.response.isOK() : false;
                    nextRefreshPeriod = self->_currentRefreshPeriod(lk, scheduleImmediately);
                    self->_scheduleNextHello(lk, nextRefreshPeriod);
                }
            }

            if (result.response.isOK()) {
                self->_onHelloSuccess(result.response.data);
            } else {
                self->_onHelloFailure(result.response.status, result.response.data);
            }
        });

    return swCbHandle;
}

void SingleServerDiscoveryMonitor::shutdown() {
    stdx::lock_guard lock(_mutex);
    if (std::exchange(_isShutdown, true)) {
        return;
    }

    LOGV2_DEBUG(4333220,
                kLogLevel + 1,
                "RSM {replicaSet} Closing host {host}",
                "RSM closing host",
                "host"_attr = _host,
                "replicaSet"_attr = _setUri.getSetName());

    _cancelOutstandingRequest(lock);

    LOGV2_DEBUG(4333229,
                kLogLevel + 1,
                "RSM {replicaSet} Done Closing host {host}",
                "RSM done closing host",
                "host"_attr = _host,
                "replicaSet"_attr = _setUri.getSetName());
}

void SingleServerDiscoveryMonitor::_cancelOutstandingRequest(WithLock) {
    if (_remoteCommandHandle) {
        _executor->cancel(_remoteCommandHandle);
    }

    if (_nextHelloHandle) {
        _executor->cancel(_nextHelloHandle);
    }

    _helloOutstanding = false;
}

void SingleServerDiscoveryMonitor::_onHelloSuccess(const BSONObj bson) {
    LOGV2_DEBUG(4333221,
                kLogLevel + 1,
                "RSM {replicaSet} received successful hello for server {host}: {helloReply}",
                "RSM received successful hello",
                "host"_attr = _host,
                "replicaSet"_attr = _setUri.getSetName(),
                "helloReply"_attr = bson);

    _eventListener->onServerHeartbeatSucceededEvent(_host, bson);
}

void SingleServerDiscoveryMonitor::_onHelloFailure(const Status& status, const BSONObj bson) {
    LOGV2_DEBUG(4333222,
                kLogLevel,
                "RSM {replicaSet} received error response from server {host}: {error}: {response}",
                "RSM received error response",
                "host"_attr = _host,
                "error"_attr = status.toString(),
                "replicaSet"_attr = _setUri.getSetName(),
                "response"_attr = bson);

    _eventListener->onServerHeartbeatFailureEvent(status, _host, bson);
}

Milliseconds SingleServerDiscoveryMonitor::_overrideRefreshPeriod(Milliseconds original) {
    Milliseconds r = original;
    static constexpr auto kPeriodField = "period"_sd;
    if (auto modifyReplicaSetMonitorDefaultRefreshPeriod =
            globalFailPointRegistry().find("modifyReplicaSetMonitorDefaultRefreshPeriod")) {
        modifyReplicaSetMonitorDefaultRefreshPeriod->executeIf(
            [&r](const BSONObj& data) {
                r = duration_cast<Milliseconds>(Seconds{data.getIntField(kPeriodField)});
            },
            [](const BSONObj& data) { return data.hasField(kPeriodField); });
    }
    return r;
}

Milliseconds SingleServerDiscoveryMonitor::_currentRefreshPeriod(WithLock,
                                                                 bool scheduleImmediately) {
    if (scheduleImmediately)
        return Milliseconds(0);

    // The _overrideRefreshPeriod() supports fail injection.
    return (_isExpedited) ? sdam::SdamConfiguration::kMinHeartbeatFrequency
                          : _overrideRefreshPeriod(_heartbeatFrequency);
}

void SingleServerDiscoveryMonitor::disableExpeditedChecking() {
    stdx::lock_guard lock(_mutex);
    _isExpedited = false;
}


ServerDiscoveryMonitor::ServerDiscoveryMonitor(
    const MongoURI& setUri,
    const sdam::SdamConfiguration& sdamConfiguration,
    sdam::TopologyEventsPublisherPtr eventsPublisher,
    sdam::TopologyDescriptionPtr initialTopologyDescription,
    std::shared_ptr<ReplicaSetMonitorStats> stats,
    std::shared_ptr<executor::TaskExecutor> executor)
    : _stats(stats),
      _sdamConfiguration(sdamConfiguration),
      _eventPublisher(eventsPublisher),
      _executor(_setupExecutor(executor)),
      _isShutdown(false),
      _setUri(setUri) {
    LOGV2_DEBUG(4333223,
                kLogLevel,
                "RSM {replicaSet} monitoring {nReplicaSetMembers} members.",
                "RSM now monitoring replica set",
                "replicaSet"_attr = _setUri.getSetName(),
                "nReplicaSetMembers"_attr = initialTopologyDescription->getServers().size());
    onTopologyDescriptionChangedEvent(nullptr, initialTopologyDescription);
}

void ServerDiscoveryMonitor::shutdown() {
    stdx::lock_guard lock(_mutex);
    if (_isShutdown)
        return;

    _isShutdown = true;
    for (auto singleMonitor : _singleMonitors) {
        singleMonitor.second->shutdown();
    }
}

void ServerDiscoveryMonitor::onTopologyDescriptionChangedEvent(
    sdam::TopologyDescriptionPtr previousDescription, sdam::TopologyDescriptionPtr newDescription) {
    stdx::lock_guard lock(_mutex);
    if (_isShutdown)
        return;

    const auto newType = newDescription->getType();
    using sdam::TopologyType;

    if (newType == TopologyType::kSingle || newType == TopologyType::kReplicaSetWithPrimary ||
        newType == TopologyType::kSharded) {
        _disableExpeditedChecking(lock);
    }

    // remove monitors that are missing from the topology
    auto it = _singleMonitors.begin();
    while (it != _singleMonitors.end()) {
        const auto& serverAddress = it->first;
        if (newDescription->findServerByAddress(serverAddress) == boost::none) {
            auto& singleMonitor = _singleMonitors[serverAddress];
            singleMonitor->shutdown();
            LOGV2_DEBUG(4333225,
                        kLogLevel,
                        "RSM {replicaSet} host {host} was removed from the topology",
                        "RSM host was removed from the topology",
                        "replicaSet"_attr = _setUri.getSetName(),
                        "addr"_attr = serverAddress);
            it = _singleMonitors.erase(it, std::next(it));
        } else {
            ++it;
        }
    }

    // add new monitors
    const auto servers = newDescription->getServers();
    std::for_each(servers.begin(),
                  servers.end(),
                  [this](const sdam::ServerDescriptionPtr& serverDescription) {
                      const auto& serverAddress = serverDescription->getAddress();
                      bool isMissing = _singleMonitors.find(serverDescription->getAddress()) ==
                          _singleMonitors.end();
                      if (isMissing) {
                          LOGV2_DEBUG(4333226,
                                      kLogLevel,
                                      "RSM {replicaSet} {host} was added to the topology",
                                      "RSM host was added to the topology",
                                      "replicaSet"_attr = _setUri.getSetName(),
                                      "host"_attr = serverAddress);
                          _singleMonitors[serverAddress] =
                              std::make_shared<SingleServerDiscoveryMonitor>(
                                  _setUri,
                                  serverAddress,
                                  serverDescription->getTopologyVersion(),
                                  _sdamConfiguration,
                                  _eventPublisher,
                                  _executor,
                                  _stats);
                          _singleMonitors[serverAddress]->init();
                      }
                  });

    invariant(_singleMonitors.size() == servers.size());
}

std::shared_ptr<executor::TaskExecutor> ServerDiscoveryMonitor::_setupExecutor(
    const std::shared_ptr<executor::TaskExecutor>& executor) {
    if (executor)
        return executor;

    auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
    auto net = executor::makeNetworkInterface(
        "ServerDiscoveryMonitor-TaskExecutor", nullptr, std::move(hookList));
    auto pool = std::make_unique<executor::NetworkInterfaceThreadPool>(net.get());
    auto result = std::make_shared<ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
    result->startup();
    return result;
}

void ServerDiscoveryMonitor::requestImmediateCheck() {
    stdx::lock_guard lock(_mutex);
    if (_isShutdown)
        return;

    for (auto& addressAndMonitor : _singleMonitors) {
        addressAndMonitor.second->requestImmediateCheck();
    }
}

void ServerDiscoveryMonitor::disableExpeditedChecking() {
    stdx::lock_guard lock(_mutex);
    _disableExpeditedChecking(lock);
}

void ServerDiscoveryMonitor::_disableExpeditedChecking(WithLock) {
    for (auto& addressAndMonitor : _singleMonitors) {
        addressAndMonitor.second->disableExpeditedChecking();
    }
}
}  // namespace mongo
