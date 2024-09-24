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

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <system_error>

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/s/async_requests_sender.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("testdb.testcoll");
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
            _targeters.push_back(targeter.get());

            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHosts[i]));
            targeter->setFindHostReturnValue(kTestShardHosts[i]);

            targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHosts[i]),
                                                   std::move(targeter));
        }

        setupShards(shards);
    }

protected:
    std::vector<RemoteCommandTargeterMock*> _targeters;  // Targeters are owned by the factory.
};

TEST_F(AsyncRequestsSenderTest, HandlesExceptionWhenYielding) {
    class ThrowyResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) override {
            if (_count++) {
                uasserted(ErrorCodes::BadValue, "Simulated error");
            }
        }

        void unyield(OperationContext*) override {}

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
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   std::make_unique<ThrowyResourceYielder>(),
                                   {} /* designatedHostsMap */);

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
        void yield(OperationContext*) override {}

        void unyield(OperationContext*) override {
            if (_count++ == 1) {
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
    std::set<ShardId> pendingShardIds{kTestShardIds[0], kTestShardIds[1], kTestShardIds[2]};

    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   std::make_unique<ThrowyResourceYielder>(),
                                   {} /* designatedHostsMap */);

    auto firstResponseProcessed = unittest::Barrier(2);

    // Issue blocking waits on a different thread.
    auto future = launchAsync([&]() {
        // Unyield doesn't throw the first time.
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT(response.shardId.isValid());
        pendingShardIds.erase(response.shardId);

        firstResponseProcessed.countDownAndWait();

        // Unyield throws this time. Even if the next response was already ready and successful,
        // the returned response should have the unyield error.
        response = ars.next();
        ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::BadValue);
        ASSERT(response.shardId.isValid());
        pendingShardIds.erase(response.shardId);

        // Unyield doesn't throw this time but this next() call should not even try to yield and
        // unyield and the returned response should have the unyield error.
        response = ars.next();
        ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::BadValue);
        ASSERT(response.shardId.isValid());
        pendingShardIds.erase(response.shardId);
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
    ASSERT(pendingShardIds.empty());
}

TEST_F(AsyncRequestsSenderTest, ExceptionWhileWaitingDoesNotSkipUnyield) {
    class CountingResourceYielder : public ResourceYielder {
    public:
        void yield(OperationContext*) override {
            ++timesYielded;
        }

        void unyield(OperationContext*) override {
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
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   std::move(yielder),
                                   {} /* designatedHostsMap */);

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

TEST_F(AsyncRequestsSenderTest, DesignatedHostChosen) {
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

    AsyncRequestsSender::ShardHostMap designatedHosts;

    auto shard1Secondary = HostAndPort("SecondaryHostShard1", 12345);
    _targeters[1]->setConnectionStringReturnValue(
        ConnectionString::forReplicaSet("shard1_rs"_sd, {kTestShardHosts[1], shard1Secondary}));
    designatedHosts[kTestShardIds[1]] = shard1Secondary;
    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   nullptr /* no yielder */,
                                   designatedHosts);

    auto future = launchAsync([&]() {
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, kTestShardIds[0]);
        ASSERT_EQ(response.shardHostAndPort, kTestShardHosts[0]);

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, kTestShardIds[1]);
        ASSERT_EQ(response.shardHostAndPort, shard1Secondary);

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, kTestShardIds[2]);
        ASSERT_EQ(response.shardHostAndPort, kTestShardHosts[2]);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[0]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, shard1Secondary);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 2)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[2]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 3)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });
    future.default_timed_get();
}

TEST_F(AsyncRequestsSenderTest, DesignatedHostMustBeInShard) {
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

    AsyncRequestsSender::ShardHostMap designatedHosts;
    designatedHosts[kTestShardIds[1]] = HostAndPort("HostNotInShard", 12345);
    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kNoRetry,
                                   nullptr /* no yielder */,
                                   designatedHosts);

    // We see the error immediately, because it happens in construction.
    auto response = ars.next();
    ASSERT_EQ(response.swResponse.getStatus(), ErrorCodes::HostNotFound);
    ASSERT_EQ(response.shardId, kTestShardIds[1]);
}

TEST_F(AsyncRequestsSenderTest, PreLoadedShardIsUsedForInitialRequest) {
    auto shardRegistry = Grid::get(operationContext())->shardRegistry();
    auto shard0 = uassertStatusOK(shardRegistry->getShard(operationContext(), kTestShardIds[0]));
    auto shard1 = uassertStatusOK(shardRegistry->getShard(operationContext(), kTestShardIds[1]));
    auto shard2 = uassertStatusOK(shardRegistry->getShard(operationContext(), kTestShardIds[2]));

    // Intentionally provide ShardIds mismatched with Shard types to prove the Shard given in the
    // request is used for the initial attempt.
    std::vector<AsyncRequestsSender::Request> requests;
    requests.emplace_back(kTestShardIds[0],
                          BSON("find"
                               << "bar"),
                          shard1);
    requests.emplace_back(kTestShardIds[1],
                          BSON("find"
                               << "bar"),
                          shard2);
    requests.emplace_back(kTestShardIds[2],
                          BSON("find"
                               << "bar"),
                          shard0);

    auto ars = AsyncRequestsSender(operationContext(),
                                   executor(),
                                   kTestNss.dbName(),
                                   requests,
                                   ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                   Shard::RetryPolicy::kIdempotent,
                                   nullptr /* no yielder */,
                                   {} /* designatedHostsMap */);

    auto future = launchAsync([&]() {
        auto response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, kTestShardIds[0]);
        ASSERT_EQ(response.shardHostAndPort, kTestShardHosts[1]);

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, kTestShardIds[1]);
        ASSERT_EQ(response.shardHostAndPort, kTestShardHosts[2]);

        response = ars.next();
        ASSERT(response.swResponse.getStatus().isOK());
        ASSERT_EQ(response.shardId, kTestShardIds[2]);
        // The ARS initially targets the host in Shard0 because that is the provided Shard but it
        // retries on a network error and "refreshes," targeting the host in Shard2.
        ASSERT_EQ(response.shardHostAndPort, kTestShardHosts[2]);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[1]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[2]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 2)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // The initial attempt targets the host in shard0 because that was the provided shard. Return a
    // retriable error to verify the provided shard is only used for the initial attempt.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[0]);
        return Status(ErrorCodes::HostUnreachable, "mock network error");
    });

    // Retry targets the "right" host in Shard2.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[2]);
        return Status(ErrorCodes::HostUnreachable, "mock network error");
    });

    // Further retries also use the reloaded Shard.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT_EQ(request.target, kTestShardHosts[2]);
        return CursorResponse(kTestNss, 0LL, {BSON("x" << 3)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
