/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_rs.h"

#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {

RemoteCommandTargeterRS::RemoteCommandTargeterRS(const std::string& rsName,
                                                 const std::vector<HostAndPort>& seedHosts)
    : _rsName(rsName) {

    std::set<HostAndPort> seedServers(seedHosts.begin(), seedHosts.end());
    _rsMonitor = ReplicaSetMonitor::createIfNeeded(rsName, seedServers);

    LOG(1) << "Started targeter for "
           << ConnectionString::forReplicaSet(
                  rsName, std::vector<HostAndPort>(seedServers.begin(), seedServers.end()));
}

ConnectionString RemoteCommandTargeterRS::connectionString() {
    return uassertStatusOK(ConnectionString::parse(_rsMonitor->getServerAddress()));
}

StatusWith<HostAndPort> RemoteCommandTargeterRS::findHostWithMaxWait(
    const ReadPreferenceSetting& readPref, Milliseconds maxWait) {
    return _rsMonitor->getHostOrRefresh(readPref, maxWait);
}

StatusWith<HostAndPort> RemoteCommandTargeterRS::findHost(OperationContext* txn,
                                                          const ReadPreferenceSetting& readPref) {
    auto clock = txn->getServiceContext()->getFastClockSource();
    auto startDate = clock->now();
    while (true) {
        const auto interruptStatus = txn->checkForInterruptNoAssert();
        if (!interruptStatus.isOK()) {
            return interruptStatus;
        }
        const auto host = _rsMonitor->getHostOrRefresh(readPref, Milliseconds::zero());
        if (host.getStatus() != ErrorCodes::FailedToSatisfyReadPreference) {
            return host;
        }
        // Enforce a 20-second ceiling on the time spent looking for a host. This conforms with the
        // behavior used throughout mongos prior to version 3.4, but is not fundamentally desirable.
        // See comment in remote_command_targeter.h for details.
        if (clock->now() - startDate > Seconds{20}) {
            return host;
        }
        sleepFor(Milliseconds{500});
    }
}

void RemoteCommandTargeterRS::markHostNotMaster(const HostAndPort& host) {
    invariant(_rsMonitor);

    _rsMonitor->failedHost(host);
}

void RemoteCommandTargeterRS::markHostUnreachable(const HostAndPort& host) {
    invariant(_rsMonitor);

    _rsMonitor->failedHost(host);
}

}  // namespace mongo
