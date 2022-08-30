/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/client/async_remote_command_targeter.h"

#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/client/remote_command_targeter_rs.h"
#include "mongo/unittest/unittest.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace mongo {
namespace remote_command_runner {
namespace {

class AsyncRemoteCommandTargeterTest : public unittest::Test {
public:
    const std::vector<HostAndPort> kHosts{HostAndPort("FakeHost1", 12345),
                                          HostAndPort("FakeHost2", 12345)};

    void setUp() {
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
 * AsyncRemoteCommandTargeter resolves to the correct underlying HostAndPort.
 */
TEST_F(AsyncRemoteCommandTargeterTest, TargeterResolvesCorrectly) {
    ReadPreferenceSetting readPref;
    auto targeter = AsyncRemoteCommandTargeter(readPref, getTargeter());

    auto resolveFuture = targeter.resolve(CancellationToken::uncancelable());

    ASSERT_EQUALS(resolveFuture.get()[0], kHosts[0]);
}

/**
 * When onRemoteCommandError is called, the targeter updates its view of the underlying topology
 * correctly.
 */
TEST_F(AsyncRemoteCommandTargeterTest, OnRemoteErrorUpdatesTopology) {
    ReadPreferenceSetting readPref;
    AsyncRemoteCommandTargeter targeter = AsyncRemoteCommandTargeter(readPref, getTargeter());

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
TEST_F(AsyncRemoteCommandTargeterTest, OnRemoteErrorUpdatesTopologyAndResolver) {
    ReadPreferenceSetting readPref;
    AsyncRemoteCommandTargeter targeter = AsyncRemoteCommandTargeter(readPref, getTargeter());

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
    auto resolveFuture = targeter.resolve(CancellationToken::uncancelable());
    ASSERT_EQUALS(resolveFuture.get()[0], kHosts[1]);
}

}  // namespace
}  // namespace remote_command_runner
}  // namespace mongo
