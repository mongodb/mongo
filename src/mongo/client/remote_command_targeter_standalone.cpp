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
    const ReadPreferenceSetting& readPref, const CancellationToken& cancelToken) {
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
