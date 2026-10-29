// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/remote_command_targeter_factory_mock.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/targeting_metadata.h"
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
                                                   const TargetingMetadata& targetingMetadata,
                                                   const CancellationToken& cancelToken) override {
        return _mock->findHosts(readPref, targetingMetadata, cancelToken);
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
