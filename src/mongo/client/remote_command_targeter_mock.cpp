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

#include "mongo/client/remote_command_targeter_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_impl.h"

#include <mutex>
#include <utility>

#include <boost/move/utility_core.hpp>

namespace mongo {

RemoteCommandTargeterMock::RemoteCommandTargeterMock()
    : _findHostReturnValue(Status(ErrorCodes::InternalError, "No return value set")) {}

RemoteCommandTargeterMock::~RemoteCommandTargeterMock() = default;

std::shared_ptr<RemoteCommandTargeterMock> RemoteCommandTargeterMock::get(
    std::shared_ptr<RemoteCommandTargeter> targeter) {
    auto mock = std::dynamic_pointer_cast<RemoteCommandTargeterMock>(targeter);
    invariant(mock);

    return mock;
}

ConnectionString RemoteCommandTargeterMock::connectionString() {
    return _connectionStringReturnValue;
}

StatusWith<HostAndPort> RemoteCommandTargeterMock::findHost(
    OperationContext* opCtx,
    const ReadPreferenceSetting& readPref,
    const TargetingMetadata& targetingMetadata) {
    if (!_findHostReturnValue.isOK()) {
        return _findHostReturnValue.getStatus();
    }

    return _findHostReturnValue.getValue()[0];
}

SemiFuture<HostAndPort> RemoteCommandTargeterMock::findHost(const ReadPreferenceSetting&,
                                                            const CancellationToken&,
                                                            const TargetingMetadata&) {
    if (!_findHostReturnValue.isOK()) {
        return _findHostReturnValue.getStatus();
    }

    return _findHostReturnValue.getValue()[0];
}

SemiFuture<std::vector<HostAndPort>> RemoteCommandTargeterMock::findHosts(
    const ReadPreferenceSetting&, const CancellationToken&) {

    return _findHostReturnValue;
}

void RemoteCommandTargeterMock::markHostNotPrimary(const HostAndPort& host, const Status& status) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _hostsMarkedDown.insert(host);
}

void RemoteCommandTargeterMock::markHostUnreachable(const HostAndPort& host, const Status& status) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _hostsMarkedDown.insert(host);
}

void RemoteCommandTargeterMock::markHostShuttingDown(const HostAndPort& host,
                                                     const Status& status) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _hostsMarkedDown.insert(host);
}

void RemoteCommandTargeterMock::setConnectionStringReturnValue(ConnectionString returnValue) {
    _connectionStringReturnValue = std::move(returnValue);
}

void RemoteCommandTargeterMock::setFindHostReturnValue(StatusWith<HostAndPort> returnValue) {
    if (!returnValue.isOK()) {
        _findHostReturnValue = returnValue.getStatus();
    } else {
        _findHostReturnValue = std::vector{returnValue.getValue()};
    }
}

void RemoteCommandTargeterMock::setFindHostsReturnValue(
    StatusWith<std::vector<HostAndPort>> returnValue) {
    _findHostReturnValue = std::move(returnValue);
}

std::set<HostAndPort> RemoteCommandTargeterMock::getAndClearMarkedDownHosts() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    auto hostsMarkedDown = _hostsMarkedDown;
    _hostsMarkedDown.clear();
    return hostsMarkedDown;
}

}  // namespace mongo
