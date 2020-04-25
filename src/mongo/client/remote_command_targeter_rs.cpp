/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/client/remote_command_targeter_rs.h"

#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"

namespace mongo {

RemoteCommandTargeterRS::RemoteCommandTargeterRS(const std::string& rsName,
                                                 const std::vector<HostAndPort>& seedHosts)
    : _rsName(rsName) {

    std::set<HostAndPort> seedServers(seedHosts.begin(), seedHosts.end());
    _rsMonitor = ReplicaSetMonitor::createIfNeeded(rsName, seedServers);

    LOGV2_DEBUG(
        20157,
        1,
        "Started targeter for "
        "{ConnectionString_forReplicaSet_rsName_std_vector_HostAndPort_seedServers_begin_"
        "seedServers_end}",
        "ConnectionString_forReplicaSet_rsName_std_vector_HostAndPort_seedServers_begin_seedServers_end"_attr =
            ConnectionString::forReplicaSet(
                rsName, std::vector<HostAndPort>(seedServers.begin(), seedServers.end())));
}

ConnectionString RemoteCommandTargeterRS::connectionString() {
    return uassertStatusOK(ConnectionString::parse(_rsMonitor->getServerAddress()));
}

SemiFuture<HostAndPort> RemoteCommandTargeterRS::findHostWithMaxWait(
    const ReadPreferenceSetting& readPref, Milliseconds maxWait) {
    return _rsMonitor->getHostOrRefresh(readPref, maxWait);
}

SemiFuture<std::vector<HostAndPort>> RemoteCommandTargeterRS::findHostsWithMaxWait(
    const ReadPreferenceSetting& readPref, Milliseconds maxWait) {
    return _rsMonitor->getHostsOrRefresh(readPref, maxWait);
}

StatusWith<HostAndPort> RemoteCommandTargeterRS::findHost(OperationContext* opCtx,
                                                          const ReadPreferenceSetting& readPref) {
    const auto interruptStatus = opCtx->checkForInterruptNoAssert();
    if (!interruptStatus.isOK()) {
        return interruptStatus;
    }

    // Enforce a 20-second ceiling on the time spent looking for a host. This conforms with the
    // behavior used throughout mongos prior to version 3.4, but is not fundamentally desirable.
    // See comment in remote_command_targeter.h for details.
    return _rsMonitor
        ->getHostOrRefresh(readPref,
                           std::min<Milliseconds>(opCtx->getRemainingMaxTimeMillis(), Seconds(20)))
        .getNoThrow(opCtx);
}

void RemoteCommandTargeterRS::markHostNotMaster(const HostAndPort& host, const Status& status) {
    invariant(_rsMonitor);

    _rsMonitor->failedHost(host, status);
}

void RemoteCommandTargeterRS::markHostUnreachable(const HostAndPort& host, const Status& status) {
    invariant(_rsMonitor);

    _rsMonitor->failedHost(host, status);
}

}  // namespace mongo
