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

#include "mongo/client/remote_command_targeter_factory_mock.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/operation_context.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"

#include <utility>
#include <vector>

namespace mongo {
namespace {

class TargeterProxy final : public RemoteCommandTargeter {
public:
    TargeterProxy(std::shared_ptr<RemoteCommandTargeter> mock) : _mock(mock) {}

    ConnectionString connectionString() override {
        return _mock->connectionString();
    }

    StatusWith<HostAndPort> findHost(OperationContext* opCtx,
                                     const ReadPreferenceSetting& readPref,
                                     const TargetingMetadata& targetingMetadata) override {
        return _mock->findHost(opCtx, readPref, targetingMetadata);
    }

    SemiFuture<HostAndPort> findHost(const ReadPreferenceSetting& readPref,
                                     const CancellationToken& cancelToken,
                                     const TargetingMetadata& targetingMetadata) override {
        return _mock->findHost(readPref, cancelToken, targetingMetadata);
    }

    SemiFuture<std::vector<HostAndPort>> findHosts(const ReadPreferenceSetting& readPref,
                                                   const CancellationToken& cancelToken) override {
        return _mock->findHosts(readPref, cancelToken);
    }

    void markHostNotPrimary(const HostAndPort& host, const Status& status) override {
        _mock->markHostNotPrimary(host, status);
    }

    void markHostUnreachable(const HostAndPort& host, const Status& status) override {
        _mock->markHostUnreachable(host, status);
    }

    void markHostShuttingDown(const HostAndPort& host, const Status& status) override {
        _mock->markHostShuttingDown(host, status);
    }

private:
    const std::shared_ptr<RemoteCommandTargeter> _mock;
};

}  // namespace

RemoteCommandTargeterFactoryMock::RemoteCommandTargeterFactoryMock() = default;

RemoteCommandTargeterFactoryMock::~RemoteCommandTargeterFactoryMock() = default;

std::unique_ptr<RemoteCommandTargeter> RemoteCommandTargeterFactoryMock::create(
    const ConnectionString& connStr) {
    auto it = _mockTargeters.find(connStr);
    if (it != _mockTargeters.end()) {
        return std::make_unique<TargeterProxy>(it->second);
    }

    return std::make_unique<RemoteCommandTargeterMock>();
}

void RemoteCommandTargeterFactoryMock::addTargeterToReturn(
    const ConnectionString& connStr, std::unique_ptr<RemoteCommandTargeterMock> mockTargeter) {
    _mockTargeters[connStr] = std::move(mockTargeter);
}

void RemoteCommandTargeterFactoryMock::removeTargeterToReturn(const ConnectionString& connStr) {
    MockTargetersMap::iterator it = _mockTargeters.find(connStr);

    invariant(it != _mockTargeters.end());
    invariant(it->second.use_count() == 1);

    _mockTargeters.erase(it);
}

}  // namespace mongo
