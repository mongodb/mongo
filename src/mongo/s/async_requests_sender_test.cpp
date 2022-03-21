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

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

const NamespaceString kTestNss("testdb.testcoll");
const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardId> kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> kTestShardHosts = {HostAndPort("FakeShard1Host", 12345),
                                                  HostAndPort("FakeShard2Host", 12345),
                                                  HostAndPort("FakeShard3Host", 12345)};

class AsyncRequestsSenderTest : public ShardingTestFixture {
public:
    AsyncRequestsSenderTest() {}

    void setUp() override {
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
};

TEST_F(AsyncRequestsSenderTest, HandlesExceptionWhenYielding) {
    class ThrowyResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) {
            if (_count++) {
                uasserted(ErrorCodes::BadValue, "Simulated error");
            }
        }

        void unyield(OperationContext*) {}

    private:
        int _count = 0;
    };

    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(kTestShardIds[0],
                          BSON("find"
                               << "bar"));
    requests.emplace_back(kTestShardIds[1],
                          BSON("find"
                               << "bar"));
    requests.emplace_back(kTestShardIds[2],
                          BSON("find"
                               << "bar"));

    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.db(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   std::make_unique<ThrowyResourceYielder>());

    // Issue blocking waits on a different thread.
    auto future = launchAsync([&]() {
        // Yield doesn't throw the first time.
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, kTestShardIds[0]);

        // Yield throws here and all outstanding responses, including the one currently being waited
        // on, are cancelled with the error yield threw.
        response = ars.next();
        ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::BadValue);
        ASSERT_EQ(response.shardId, kTestShardIds[1]);

        response = ars.next();
        ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::BadValue);
        ASSERT_EQ(response.shardId, kTestShardIds[2]);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(AsyncRequestsSenderTest, HandlesExceptionWhenUnyielding) {
    class ThrowyResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) {}

        void unyield(OperationContext*) {
            if (_count++) {
                uasserted(ErrorCodes::BadValue, "Simulated error");
            }
        }

    private:
        int _count = 0;
    };

    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(kTestShardIds[0],
                          BSON("find"
                               << "bar"));
    requests.emplace_back(kTestShardIds[1],
                          BSON("find"
                               << "bar"));
    requests.emplace_back(kTestShardIds[2],
                          BSON("find"
                               << "bar"));

    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.db(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   std::make_unique<ThrowyResourceYielder>());

    auto firstResponseProcessed = unittest::Barrier(2);

    // Issue blocking waits on a different thread.
    auto future = launchAsync([&]() {
        // Unyield doesn't throw the first time.
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, kTestShardIds[0]);

        firstResponseProcessed.countDownAndWait();

        // Unyield throws here, but the next response was already ready so it's returned. The
        // outstanding requests are cancelled with the error unyield threw.
        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, kTestShardIds[1]);

        response = ars.next();
        ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::BadValue);
        ASSERT_EQ(response.shardId, kTestShardIds[2]);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    firstResponseProcessed.countDownAndWait();

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(AsyncRequestsSenderTest, ExceptionWhileWaitingDoesNotSkipUnyield) {
    class CountingResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) {
            ++timesYielded;
        }

        void unyield(OperationContext*) {
            ++timesUnyielded;
        }

        int timesYielded = 0;
        int timesUnyielded = 0;
    };

    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(kTestShardIds[0],
                          BSON("find"
                               << "bar"));

    auto yielder = std::make_unique<CountingResourceYielder>();
    auto yielderPointer = yielder.get();
    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.db(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   std::move(yielder));

    // Issue blocking wait on a different thread.
    auto future = launchAsync([&]() {
        // Unyield doesn't throw the first time.
        auto response = ars.next();
        ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::Interrupted);
        ASSERT_EQ(response.shardId, kTestShardIds[0]);
    });

    // Interrupt the waiting opCtx and verify unyield wasn't called.
    operationContext()->markKilled();

    future.default_timed_get();

    ASSERT_EQ(yielderPointer->timesYielded, 1);
    ASSERT_EQ(yielderPointer->timesUnyielded, 1);
}
}  // namespace
}  // namespace mongo
