// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/remote_command_targeter_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/targeting_metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_impl.h"

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

    return _findHostReturnValue.getValue().front();
}

SemiFuture<HostAndPort> RemoteCommandTargeterMock::findHost(
    const ReadPreferenceSetting&,
    const CancellationToken&,
    const TargetingMetadata& targetingMetadata) {
    if (!_findHostReturnValue.isOK()) {
        return _findHostReturnValue.getStatus();
    }

    return _findHostReturnValue.getValue().front();
}

SemiFuture<std::vector<HostAndPort>> RemoteCommandTargeterMock::findHosts(
    const ReadPreferenceSetting&, const TargetingMetadata&, const CancellationToken&) {

    return _findHostReturnValue;
}

void RemoteCommandTargeterMock::markHostNotPrimary(const HostAndPort& host, const Status& status) {
    std::lock_guard<std::mutex> lg(_mutex);
    _hostsMarkedDown.insert(host);
}

void RemoteCommandTargeterMock::markHostUnreachable(const HostAndPort& host, const Status& status) {
    std::lock_guard<std::mutex> lg(_mutex);
    _hostsMarkedDown.insert(host);
}

void RemoteCommandTargeterMock::markHostShuttingDown(const HostAndPort& host,
                                                     const Status& status) {
    std::lock_guard<std::mutex> lg(_mutex);
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
    std::lock_guard<std::mutex> lg(_mutex);
    auto hostsMarkedDown = _hostsMarkedDown;
    _hostsMarkedDown.clear();
    return hostsMarkedDown;
}

}  // namespace mongo
