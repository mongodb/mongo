// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/async_remote_command_targeter_adapter.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <memory>
#include <set>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace async_rpc {
namespace {

class AsyncRemoteCommandTargeterAdapterTest : public unittest::Test {
public:
    const std::vector<HostAndPort> kHosts{HostAndPort("FakeHost1", 12345),
                                          HostAndPort("FakeHost2", 12345)};

    void setUp() override {
        auto factory = RemoteCommandTargeterFactoryMock();
        _targeter = factory.create(ConnectionString::forStandalones(kHosts));

        auto targeterMock = getTargeterMock();
        targeterMock->setFindHostsReturnValue(kHosts);
    }

    std::shared_ptr<RemoteCommandTargeter> getTargeter() {
        return _targeter;
    }

    std::shared_ptr<RemoteCommandTargeterMock> getTargeterMock() {
        return RemoteCommandTargeterMock::get(_targeter);
    }

private:
    std::shared_ptr<RemoteCommandTargeter> _targeter;
};

/**
 * AsyncRemoteCommandTargeterAdapter resolves to the correct underlying HostAndPort.
 */
TEST_F(AsyncRemoteCommandTargeterAdapterTest, TargeterResolvesCorrectly) {
    ReadPreferenceSetting readPref;
    auto targeter = AsyncRemoteCommandTargeterAdapter(readPref, getTargeter());

    auto resolveFuture = targeter.resolve(CancellationToken::uncancelable(), TargetingMetadata{});

    ASSERT_EQUALS(resolveFuture.get(), kHosts[0]);
}

/**
 * When onRemoteCommandError is called, the targeter updates its view of the underlying topology
 * correctly.
 */
TEST_F(AsyncRemoteCommandTargeterAdapterTest, OnRemoteErrorUpdatesTopology) {
    ReadPreferenceSetting readPref;
    AsyncRemoteCommandTargeterAdapter targeter =
        AsyncRemoteCommandTargeterAdapter(readPref, getTargeter());

    [[maybe_unused]] auto commandErrorResponse = targeter.onRemoteCommandError(
        kHosts[0], Status(ErrorCodes::NotPrimaryNoSecondaryOk, "mock"));

    auto markedDownHosts = getTargeterMock()->getAndClearMarkedDownHosts();
    auto markedDownHost = *markedDownHosts.begin();

    ASSERT_EQUALS(markedDownHosts.size(), 1);
    ASSERT_EQUALS(markedDownHost, kHosts[0]);
}

/**
 * When onRemoteCommandError is called, the targeter updates its view of the underlying topology
 * correctly and the resolver receives those changes.
 */
TEST_F(AsyncRemoteCommandTargeterAdapterTest, OnRemoteErrorUpdatesTopologyAndResolver) {
    ReadPreferenceSetting readPref;
    AsyncRemoteCommandTargeterAdapter targeter =
        AsyncRemoteCommandTargeterAdapter(readPref, getTargeter());

    // Mark down a host and ensure that it has been noted as marked down.
    [[maybe_unused]] auto commandErrorResponse = targeter.onRemoteCommandError(
        kHosts[0], Status(ErrorCodes::NotPrimaryNoSecondaryOk, "mock"));
    auto markedDownHosts = getTargeterMock()->getAndClearMarkedDownHosts();
    auto markedDownHost = *markedDownHosts.begin();

    // Remove that host from the vector of targets and set that new vector as the return value of
    // findHosts.
    std::vector<HostAndPort> newTargets(1);
    remove_copy(kHosts.begin(), kHosts.end(), newTargets.begin(), markedDownHost);
    getTargeterMock()->setFindHostsReturnValue(newTargets);

    // Check that the resolve function has been updated accordingly.
    auto resolveFuture = targeter.resolve(CancellationToken::uncancelable(), TargetingMetadata{});
    ASSERT_EQUALS(resolveFuture.get(), kHosts[1]);
}

}  // namespace
}  // namespace async_rpc
}  // namespace mongo
