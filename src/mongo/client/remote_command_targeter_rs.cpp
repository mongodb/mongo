// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/client/remote_command_targeter_rs.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/replica_set_monitor_server_parameters_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"

#include <set>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {

namespace {

/**
 * To be used on mongod only. Returns the host and port for this mongod as specified in the current
 * replica set configuration.
 */
HostAndPort getLocalHostAndPort(ServiceContext* serviceContext) {
    auto localHostAndPort = repl::ReplicationCoordinator::get(serviceContext)->getMyHostAndPort();
    // The host and port would be empty if the replica set configuration has not been initialized or
    // if this mongod is no longer part of the replica set it was in. Returns a retryable error to
    // make the external client retry.
    uassert(ErrorCodes::HostNotFound,
            "Cannot find the host and port for this node in the replica set configuration.",
            !localHostAndPort.empty());
    return localHostAndPort;
}

}  // namespace

RemoteCommandTargeterRS::RemoteCommandTargeterRS(const std::string& rsName,
                                                 const std::vector<HostAndPort>& seedHosts)
    : _serviceContext(getGlobalServiceContext()), _rsName(rsName) {

    std::set<HostAndPort> seedServers(seedHosts.begin(), seedHosts.end());
    _rsMonitor = ReplicaSetMonitor::createIfNeeded(rsName, seedServers);

    if (serverGlobalParams.clusterRole.has(ClusterRole::ShardServer)) {
        auto replCoord = repl::ReplicationCoordinator::get(_serviceContext);
        _isTargetingLocalRS = replCoord->getConfig().getReplSetName() == _rsName;
    }

    LOGV2_DEBUG(20157,
                1,
                "Started targeter",
                "connectionString"_attr = ConnectionString::forReplicaSet(
                    rsName, std::vector<HostAndPort>(seedServers.begin(), seedServers.end())));
}

ConnectionString RemoteCommandTargeterRS::connectionString() {
    return uassertStatusOK(ConnectionString::parse(_rsMonitor->getServerAddress()));
}

bool RemoteCommandTargeterRS::_mustTargetLocalHost(const ReadPreferenceSetting& readPref) const {
    return _isTargetingLocalRS && readPref.isPretargeted;
}

SemiFuture<HostAndPort> RemoteCommandTargeterRS::findHost(
    const ReadPreferenceSetting& readPref,
    const CancellationToken& cancelToken,
    const TargetingMetadata& targetingMetadata) {
    if (_mustTargetLocalHost(readPref)) {
        return getLocalHostAndPort(_serviceContext);
    }

    return _rsMonitor->getHostOrRefresh(readPref, targetingMetadata, cancelToken).semi();
}

SemiFuture<std::vector<HostAndPort>> RemoteCommandTargeterRS::findHosts(
    const ReadPreferenceSetting& readPref,
    const TargetingMetadata& targetingMetadata,
    const CancellationToken& cancelToken) {
    if (_mustTargetLocalHost(readPref)) {
        return std::vector<HostAndPort>{getLocalHostAndPort(_serviceContext)};
    }
    return _rsMonitor->getHostsOrRefresh(readPref, targetingMetadata, cancelToken).semi();
}

StatusWith<HostAndPort> RemoteCommandTargeterRS::findHost(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const TargetingMetadata& targetingMetadata) {
    const auto interruptStatus = opCtx->checkForInterruptNoAssert();
    if (!interruptStatus.isOK()) {
        return interruptStatus;
    }

    if (_mustTargetLocalHost(readPref)) {
        return getLocalHostAndPort(_serviceContext);
    }

    bool maxTimeMsLesser = (opCtx->getRemainingMaxTimeMillis() <
                            Milliseconds(gDefaultFindReplicaSetHostTimeoutMS.load()));
    auto swHostAndPort =
        _rsMonitor->getHostOrRefresh(readPref, targetingMetadata, opCtx->getCancellationToken())
            .getNoThrow(opCtx);

    // If opCtx is interrupted, getHostOrRefresh may be canceled through the token (rather than
    // opCtx) and therefore we may get a generic FailedToSatisfyReadPreference as tokens do not
    // propagate errors.
    // TODO SERVER-95226 : Check if this override is still needed.
    if (auto status = swHostAndPort.getStatus();
        status == ErrorCodes::FailedToSatisfyReadPreference) {
        if (auto ctxStatus = opCtx->checkForInterruptNoAssert(); !ctxStatus.isOK()) {
            return ctxStatus;
        } else if (maxTimeMsLesser) {
            return Status(ErrorCodes::MaxTimeMSExpired,
                          str::stream() << "operation timed out: " << status.reason());
        }
    }

    return swHostAndPort;
}

void RemoteCommandTargeterRS::markHostNotPrimary(const HostAndPort& host, const Status& status) {
    invariant(_rsMonitor);

    _rsMonitor->failedHost(host, status);
}

void RemoteCommandTargeterRS::markHostUnreachable(const HostAndPort& host, const Status& status) {
    invariant(_rsMonitor);

    _rsMonitor->failedHost(host, status);
}

void RemoteCommandTargeterRS::markHostShuttingDown(const HostAndPort& host, const Status& status) {
    invariant(_rsMonitor);

    _rsMonitor->failedHost(host, status);
}

}  // namespace mongo
