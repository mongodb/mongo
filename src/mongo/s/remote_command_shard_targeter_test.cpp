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

#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/remote_command_shard_targeter.h"

#include "mongo/base/error_codes.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/shard_id.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/sharding_test_fixture_common.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include <cstddef>
#include <memory>

namespace mongo {
namespace remote_command_runner {
namespace {

using mongo::ShardingTestFixture;

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardId> kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> kTestShardHosts = {HostAndPort("FakeShard1Host", 12345),
                                                  HostAndPort("FakeShard2Host", 12345),
                                                  HostAndPort("FakeShard3Host", 12345)};


class RemoteCommandShardingTestFixture : public ShardingTestFixture {
public:
    RemoteCommandShardingTestFixture() {}

    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        for (size_t i = 0; i < kTestShardIds.size(); i++) {
            ShardType shardType;
            shardType.setName(kTestShardIds[i].toString());
            shardType.setHost(kTestShardHosts[i].toString());

            _shards.push_back(shardType);
        }

        setupShards(_shards);

        for (size_t i = 0; i < kTestShardIds.size(); i++) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), kTestShardIds[i]))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(kTestShardHosts[i]);
        }
    }

    void removeShard(ShardId shardId) {
        std::vector<ShardType> newShards;
        for (size_t i = 0; i < _shards.size(); i++) {
            if (_shards[i].getName() != shardId) {
                newShards.emplace_back(_shards[i]);
            }
        }

        setupShards(newShards);
    }

private:
    std::vector<ShardType> _shards;
};

/**
 * Shard targeter resolves to the correct underlying HostAndPort.
 */
TEST_F(RemoteCommandShardingTestFixture, ShardTargeter) {
    ReadPreferenceSetting readPref;
    auto targeter =
        RemoteCommandShardIdTargeter(kTestShardIds[0], operationContext(), readPref, executor());

    auto resolveFuture = targeter.resolve(CancellationToken::uncancelable());

    ASSERT_EQUALS(resolveFuture.get()[0], kTestShardHosts[0]);
}

/**
 * Shard targeter correctly throws ShardNotFound when provided with an invalid ShardId.
 */
TEST_F(RemoteCommandShardingTestFixture, ShardDoesNotExist) {
    ReadPreferenceSetting readPref;
    auto targeter = RemoteCommandShardIdTargeter(
        ShardId("MissingShard"), operationContext(), readPref, executor());

    auto resolveFuture = targeter.resolve(CancellationToken::uncancelable());

    // Mock the response to the cache refresh request to the config shard.
    onFindCommand([](const executor::RemoteCommandRequest& request) {
        ASSERT_EQUALS(request.cmdObj["find"].str(), "shards");
        ASSERT_EQUALS(request.target, kTestConfigShardHost);

        BSONObj responseForCacheRefresh(fromjson(R"({
            _id: "FakeShard1",
            host: "FakeShard1Host:12345"
        })"));

        std::vector<BSONObj> result;
        result.push_back(responseForCacheRefresh);

        return result;
    });

    ASSERT_THROWS_CODE(resolveFuture.get(), DBException, ErrorCodes::ShardNotFound);
}

/**
 * getShard correctly returns the Shard when provided with ShardId that is not in the intial cache.
 */
TEST_F(RemoteCommandShardingTestFixture, ShardNotInCache) {
    ReadPreferenceSetting readPref;
    auto targeter = RemoteCommandShardIdTargeter(
        ShardId("MissingShard"), operationContext(), readPref, executor());

    auto getShardFuture = targeter.getShard();

    // Mock the response to the cache refresh request to the config shard.
    onFindCommand([](const executor::RemoteCommandRequest& request) {
        ASSERT_EQUALS(request.cmdObj["find"].str(), "shards");
        ASSERT_EQUALS(request.target, kTestConfigShardHost);

        BSONObj responseForCacheRefresh(fromjson(R"({
            _id: "MissingShard",
            host: "MissingShardHost:12345"
        })"));

        std::vector<BSONObj> result;
        result.push_back(responseForCacheRefresh);

        return result;
    });

    ASSERT_EQUALS(getShardFuture.get()->getId(), ShardId("MissingShard"));
}

/**
 * When onRemoteCommandError is called, the shard targeter updates its view of the underlying
 * topology correctly.
 */
TEST_F(RemoteCommandShardingTestFixture, OnRemoteErrorUpdatesTopology) {
    ReadPreferenceSetting readPref;
    RemoteCommandShardIdTargeter targeter{
        kTestShardIds[0], operationContext(), readPref, executor()};

    // We must call resolve before calling onRemoteCommandError
    auto initialResolve = targeter.resolve(CancellationToken::uncancelable()).get();

    [[maybe_unused]] auto commandErrorResult = targeter.onRemoteCommandError(
        kTestShardHosts[0], Status(ErrorCodes::NotPrimaryNoSecondaryOk, "mock"));

    SemiFuture<std::shared_ptr<Shard>> shardFuture = targeter.getShard();
    std::shared_ptr<RemoteCommandTargeterMock> targeterMock =
        RemoteCommandTargeterMock::get(shardFuture.get()->getTargeter());

    std::set<HostAndPort> markedDownHosts = targeterMock->getAndClearMarkedDownHosts();
    HostAndPort markedDownHost = *markedDownHosts.begin();

    ASSERT_EQUALS(markedDownHosts.size(), 1);
    ASSERT_EQUALS(markedDownHost, kTestShardHosts[0]);
}

/**
 * When onRemoteCommandError is called, the targeter updates its view of the underlying topology
 * correctly and the resolver receives those changes.
 */
TEST_F(RemoteCommandShardingTestFixture, OnRemoteErrorUpdatesTopologyAndResolver) {
    ReadPreferenceSetting readPref;
    RemoteCommandShardIdTargeter targeter{
        kTestShardIds[0], operationContext(), readPref, executor()};

    // We must call resolve before calling onRemoteCommandError
    auto initialResolve = targeter.resolve(CancellationToken::uncancelable()).get();

    // Mark down a host and ensure that it has been noted as marked down.
    [[maybe_unused]] auto commandErrorResult = targeter.onRemoteCommandError(
        kTestShardHosts[0], Status(ErrorCodes::NotPrimaryNoSecondaryOk, "mock"));

    SemiFuture<std::shared_ptr<Shard>> shardFuture = targeter.getShard();
    std::shared_ptr<RemoteCommandTargeterMock> targeterMock =
        RemoteCommandTargeterMock::get(shardFuture.get()->getTargeter());

    auto markedDownHosts = targeterMock->getAndClearMarkedDownHosts();
    auto markedDownHost = *markedDownHosts.begin();

    // Remove that host from the vector of targets and set that new vector as the return value of
    // findHosts.
    std::vector<HostAndPort> newTargets(2);
    remove_copy(kTestShardHosts.begin(), kTestShardHosts.end(), newTargets.begin(), markedDownHost);
    targeterMock->setFindHostsReturnValue(newTargets);

    // Check that the resolve function has been updated accordingly.
    auto resolveFuture = targeter.resolve(CancellationToken::uncancelable());
    ASSERT_EQUALS(resolveFuture.get()[0], kTestShardHosts[1]);
}

/**
 * ShardId is removed from the shard registry in between call to resolve and onRemoteCommandError.
 * No error is thrown from this scenario.
 */
TEST_F(RemoteCommandShardingTestFixture, TestingIfShardRemoved) {
    ReadPreferenceSetting readPref;
    RemoteCommandShardIdTargeter targeter{
        kTestShardIds[0], operationContext(), readPref, executor()};

    // Pretend we are inside the doRequest() function.

    // We resolve for the first time and get a host.
    SemiFuture<std::vector<HostAndPort>> resolveFuture =
        targeter.resolve(CancellationToken::uncancelable());
    ASSERT_EQUALS(resolveFuture.get()[0], kTestShardHosts[0]);

    // We will send the command through scheduleRemoteCommand.

    // In the meantime, the shard we were targeting is removed.
    removeShard(kTestShardIds[0]);
    ASSERT_EQUALS(shardRegistry()->getNumShards(operationContext()), kTestShardIds.size() - 1);

    // We get an error response e from the scheduleRemoteCommandFunction, so we are going to call
    // onRemoteCommandError, which now uses the cached shard.
    Status e = Status(ErrorCodes::NetworkTimeout, "mock");
    auto commandErrorResult = targeter.onRemoteCommandError(kTestShardHosts[0], e);

    // onRemoteCommandError does not throw-- now doRequest() will be able to propagate e or
    // re-resolve
    ASSERT_DOES_NOT_THROW(commandErrorResult.get());

    SemiFuture<std::vector<HostAndPort>> secondResolveFuture =
        targeter.resolve(CancellationToken::uncancelable());

    // Mock the response to the cache refresh request to the config shard.
    onFindCommand([](const executor::RemoteCommandRequest& request) {
        ASSERT_EQUALS(request.cmdObj["find"].str(), "shards");
        ASSERT_EQUALS(request.target, kTestConfigShardHost);

        BSONObj responseForCacheRefresh(fromjson(R"({
            _id: "MissingShard",
            host: "MissingShardHost:12345"
        })"));

        std::vector<BSONObj> result;
        result.push_back(responseForCacheRefresh);

        return result;
    });

    ASSERT_THROWS_CODE(secondResolveFuture.get(), DBException, ErrorCodes::ShardNotFound);
}

/**
 * Make sure that we cannot call onRemoteCommandError before calling resolve.
 */
DEATH_TEST_F(RemoteCommandShardingTestFixture, CannotCallOnRemoteErrorBeforeResolve, "invariant") {
    ReadPreferenceSetting readPref;
    RemoteCommandShardIdTargeter targeter{
        kTestShardIds[0], operationContext(), readPref, executor()};

    Status e = Status(ErrorCodes::NetworkTimeout, "mock");
    auto commandErrorResult = targeter.onRemoteCommandError(kTestShardHosts[0], e);
}

}  // namespace
}  // namespace remote_command_runner
}  // namespace mongo
