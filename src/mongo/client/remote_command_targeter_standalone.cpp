// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/remote_command_targeter_standalone.h"

#include "mongo/base/status_with.h"
#include "mongo/client/connection_string.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {

RemoteCommandTargeterStandalone::RemoteCommandTargeterStandalone(const HostAndPort& hostAndPort)
    : _hostAndPort(hostAndPort) {}

ConnectionString RemoteCommandTargeterStandalone::connectionString() {
    return ConnectionString(_hostAndPort);
}

SemiFuture<HostAndPort> RemoteCommandTargeterStandalone::findHost(
    const ReadPreferenceSetting& readPref,
    const CancellationToken& cancelToken,
    const TargetingMetadata& targetingMetadata) {
    return {_hostAndPort};
}

SemiFuture<std::vector<HostAndPort>> RemoteCommandTargeterStandalone::findHosts(
    const ReadPreferenceSetting& readPref,
    const TargetingMetadata& targetingMetadata,
    const CancellationToken& cancelToken) {
    return {{_hostAndPort}};
}

StatusWith<HostAndPort> RemoteCommandTargeterStandalone::findHost(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const TargetingMetadata& targetingMetadata) {
    return _hostAndPort;
}

void RemoteCommandTargeterStandalone::markHostNotPrimary(const HostAndPort& host,
                                                         const Status& status) {
    dassert(host == _hostAndPort);
}

void RemoteCommandTargeterStandalone::markHostUnreachable(const HostAndPort& host,
                                                          const Status& status) {
    dassert(host == _hostAndPort);
}

void RemoteCommandTargeterStandalone::markHostShuttingDown(const HostAndPort& host,
                                                           const Status& status) {
    dassert(host == _hostAndPort);
}

}  // namespace mongo
