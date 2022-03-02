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

#include "mongo/platform/basic.h"

#include "mongo/client/connection_string.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/vector_clock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_factory.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/sharding_router_test_fixture.h"

namespace mongo {
namespace {

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

const std::vector<ShardId> kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> kTestShardHosts = {HostAndPort("FakeShard1Host", 12345),
                                                  HostAndPort("FakeShard2Host", 12345),
                                                  HostAndPort("FakeShard3Host", 12345)};

class ShardRemoteTest : public ShardingTestFixture {
protected:
    void setUp() {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        std::vector<ShardType> shards;

        for (size_t i = 0; i < kTestShardIds.size(); i++) {
            ShardType shardType;
            shardType.setName(kTestShardIds[i].toString());
            shardType.setHost(kTestShardHosts[i].toString());
            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHosts[i]));
            targeter->setFindHostReturnValue(kTestShardHosts[i]);

            targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHosts[i]),
                                                   std::move(targeter));
        }

        setupShards(shards);
    }

    void runDummyCommandOnShard(ShardId shardId) {
        auto shard = shardRegistry()->getShardNoReload(shardId);
        uassertStatusOK(shard->runCommand(operationContext(),
                                          ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                          "unusedDb",
                                          BSON("unused"
                                               << "cmd"),
                                          Shard::RetryPolicy::kNoRetry));
    }
};

BSONObj makeLastCommittedOpTimeMetadata(LogicalTime time) {
    return BSON("lastCommittedOpTime" << time.asTimestamp());
}

TEST_F(ShardRemoteTest, GetAndSetLatestLastCommittedOpTime) {
    auto shard = shardRegistry()->getShardNoReload(kTestShardIds[0]);

    // Shards can store last committed opTimes.
    LogicalTime time(Timestamp(10, 2));
    shard->updateLastCommittedOpTime(time);
    ASSERT_EQ(time, shard->getLastCommittedOpTime());

    // Later times overwrite earlier times.
    LogicalTime laterTime(Timestamp(20, 2));
    shard->updateLastCommittedOpTime(laterTime);
    ASSERT_EQ(laterTime, shard->getLastCommittedOpTime());

    // Earlier times are ignored.
    LogicalTime earlierTime(Timestamp(5, 1));
    shard->updateLastCommittedOpTime(earlierTime);
    ASSERT_EQ(laterTime, shard->getLastCommittedOpTime());
}

TEST_F(ShardRemoteTest, NetworkReplyWithLastCommittedOpTime) {
    // Send a request to one shard.
    auto targetedShard = kTestShardIds[0];
    auto future = launchAsync([&] { runDummyCommandOnShard(targetedShard); });

    // Mock a find response with a returned lastCommittedOpTime.
    LogicalTime expectedTime(Timestamp(100, 2));
    onFindWithMetadataCommand([&](const executor::RemoteCommandRequest& request) {
        auto result = std::vector<BSONObj>{BSON("_id" << 1)};
        auto metadata = makeLastCommittedOpTimeMetadata(expectedTime);
        return std::make_tuple(result, metadata);
    });

    future.default_timed_get();

    // Verify the targeted shard has updated its lastCommittedOpTime.
    ASSERT_EQ(expectedTime,
              shardRegistry()->getShardNoReload(targetedShard)->getLastCommittedOpTime());

    // Verify shards that were not targeted were not affected.
    for (auto shardId : kTestShardIds) {
        if (shardId != targetedShard) {
            ASSERT(!VectorClock::isValidComponentTime(
                shardRegistry()->getShardNoReload(shardId)->getLastCommittedOpTime()));
        }
    }
}

TEST_F(ShardRemoteTest, NetworkReplyWithoutLastCommittedOpTime) {
    // Send a request to one shard.
    auto targetedShard = kTestShardIds[0];
    auto future = launchAsync([&] { runDummyCommandOnShard(targetedShard); });

    // Mock a find response without a returned lastCommittedOpTime.
    onFindWithMetadataCommand([&](const executor::RemoteCommandRequest& request) {
        auto result = std::vector<BSONObj>{BSON("_id" << 1)};
        auto metadata = BSONObj();
        return std::make_tuple(result, metadata);
    });

    future.default_timed_get();

    // Verify the targeted shard has not updated its lastCommittedOpTime.
    ASSERT_EQ(LogicalTime::kUninitialized,
              shardRegistry()->getShardNoReload(targetedShard)->getLastCommittedOpTime());
}

TEST_F(ShardRemoteTest, ScatterGatherRepliesWithLastCommittedOpTime) {
    // Send requests to several shards.
    auto nss = NamespaceString("test.foo");
    auto cmdObj = BSON("find" << nss.coll());
    std::vector<std::pair<ShardId, BSONObj>> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        establishCursors(operationContext(),
                         executor(),
                         nss,
                         ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                         remotes,
                         false);  // allowPartialResults
    });

    // All remotes respond with a lastCommittedOpTime.
    LogicalTime expectedTime(Timestamp(50, 1));
    for (auto remote : remotes) {
        onCommandWithMetadata([&](const executor::RemoteCommandRequest& request) {
            std::vector<BSONObj> batch = {BSON("_id" << 1)};
            CursorResponse cursorResponse(nss, CursorId(123), batch);
            auto result = BSONObjBuilder(
                cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse));
            result.appendElements(makeLastCommittedOpTimeMetadata(expectedTime));

            return executor::RemoteCommandResponse(result.obj(), Milliseconds(1));
        });
    }

    future.default_timed_get();

    // Verify all shards updated their lastCommittedOpTime.
    for (auto shardId : kTestShardIds) {
        ASSERT_EQ(expectedTime,
                  shardRegistry()->getShardNoReload(shardId)->getLastCommittedOpTime());
    }
}

TEST_F(ShardRemoteTest, TargeterMarksHostAsDownWhenConfigStepdown) {
    auto targetedNode = ShardId("config");

    ASSERT_EQ(0UL, configTargeter()->getAndClearMarkedDownHosts().size());
    auto future = launchAsync([&] { runDummyCommandOnShard(targetedNode); });

    auto error = Status(ErrorCodes::PrimarySteppedDown, "Config stepped down");
    onCommand([&](const executor::RemoteCommandRequest& request) { return error; });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::PrimarySteppedDown);
    ASSERT_EQ(1UL, configTargeter()->getAndClearMarkedDownHosts().size());
}

TEST_F(ShardRemoteTest, TargeterMarksHostAsDownWhenConfigShuttingDown) {
    auto targetedNode = ShardId("config");

    ASSERT_EQ(0UL, configTargeter()->getAndClearMarkedDownHosts().size());
    auto future = launchAsync([&] { runDummyCommandOnShard(targetedNode); });

    auto error = Status(ErrorCodes::InterruptedAtShutdown, "Interrupted at shutdown");
    onCommand([&](const executor::RemoteCommandRequest& request) { return error; });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::InterruptedAtShutdown);
    ASSERT_EQ(1UL, configTargeter()->getAndClearMarkedDownHosts().size());
}

}  // namespace
}  // namespace mongo
