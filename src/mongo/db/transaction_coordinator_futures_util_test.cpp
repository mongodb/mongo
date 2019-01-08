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

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/transaction_coordinator_futures_util.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace txn {
namespace {

TEST(TransactionCoordinatorFuturesUtilTest, CollectReturnsInitValueWhenInputIsEmptyVector) {
    std::vector<Future<int>> futures;
    auto resultFuture = txn::collect(std::move(futures), 0, [](int& result, const int& next) {
        result = 20;
        return txn::ShouldStopIteration::kNo;
    });

    ASSERT_EQ(resultFuture.get(), 0);
}

TEST(TransactionCoordinatorFuturesUtilTest, CollectReturnsOnlyResultWhenOnlyOneFuture) {
    std::vector<Future<int>> futures;
    auto pf = makePromiseFuture<int>();
    futures.push_back(std::move(pf.future));
    auto resultFuture = txn::collect(std::move(futures), 0, [](int& result, const int& next) {
        result = next;
        return txn::ShouldStopIteration::kNo;
    });
    pf.promise.emplaceValue(3);

    ASSERT_EQ(resultFuture.get(), 3);
}

TEST(TransactionCoordinatorFuturesUtilTest, CollectReturnsCombinedResultWithSeveralInputFutures) {
    std::vector<Future<int>> futures;
    std::vector<Promise<int>> promises;
    std::vector<int> futureValues;
    for (int i = 0; i < 5; ++i) {
        auto pf = makePromiseFuture<int>();
        futures.push_back(std::move(pf.future));
        promises.push_back(std::move(pf.promise));
        futureValues.push_back(i);
    }

    // Sum all of the inputs.
    auto resultFuture = txn::collect(std::move(futures), 0, [](int& result, const int& next) {
        result += next;
        return txn::ShouldStopIteration::kNo;
    });

    for (size_t i = 0; i < promises.size(); ++i) {
        promises[i].emplaceValue(futureValues[i]);
    }

    // Result should be the sum of all the values emplaced into the promises.
    ASSERT_EQ(resultFuture.get(), std::accumulate(futureValues.begin(), futureValues.end(), 0));
}


class AsyncWorkSchedulerTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();

        for (const auto& shardId : kShardIds) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(HostAndPort(str::stream() << shardId << ":123"));
        }
    }

    void assertCommandSentAndRespondWith(const StringData& commandName,
                                         const StatusWith<BSONObj>& response,
                                         boost::optional<BSONObj> expectedWriteConcern) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.cmdObj.firstElement().fieldNameStringData(), commandName);
            if (expectedWriteConcern) {
                ASSERT_BSONOBJ_EQ(
                    *expectedWriteConcern,
                    request.cmdObj.getObjectField(WriteConcernOptions::kWriteConcernField));
            }
            return response;
        });
    }

    // Override the CatalogClient to make CatalogClient::getAllShards automatically return the
    // expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
    // ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
    // DBClientMock analogous to the NetworkInterfaceMock.
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) override {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient() : ShardingCatalogClientMock(nullptr) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
                std::vector<ShardType> shardTypes;
                for (const auto& shardId : makeThreeShardIdsList()) {
                    const ConnectionString cs = ConnectionString::forReplicaSet(
                        shardId.toString(), {HostAndPort(str::stream() << shardId << ":123")});
                    ShardType sType;
                    sType.setName(cs.getSetName());
                    sType.setHost(cs.toString());
                    shardTypes.push_back(std::move(sType));
                };
                return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
            }
        };

        return stdx::make_unique<StaticCatalogClient>();
    }

    static std::vector<ShardId> makeThreeShardIdsList() {
        return std::vector<ShardId>{{"s1"}, {"s2"}, {"s3"}};
    }
    const std::vector<ShardId> kShardIds = makeThreeShardIdsList();
};

TEST_F(AsyncWorkSchedulerTest, ScheduledBlockingWorkSucceeds) {
    AsyncWorkScheduler async(getServiceContext());

    unittest::Barrier barrier(2);
    auto pf = makePromiseFuture<int>();
    auto future =
        async.scheduleWork([&barrier, future = std::move(pf.future) ](OperationContext * opCtx) {
            barrier.countDownAndWait();
            return future.get(opCtx);
        });

    barrier.countDownAndWait();
    ASSERT(!future.isReady());

    pf.promise.emplaceValue(5);
    ASSERT_EQ(5, future.get(operationContext()));
}

TEST_F(AsyncWorkSchedulerTest, ScheduledBlockingWorkThrowsException) {
    AsyncWorkScheduler async(getServiceContext());

    unittest::Barrier barrier(2);
    auto pf = makePromiseFuture<int>();
    auto future =
        async.scheduleWork([&barrier, future = std::move(pf.future) ](OperationContext * opCtx) {
            barrier.countDownAndWait();
            future.get(opCtx);
            uasserted(ErrorCodes::InternalError, "Test error");
        });

    barrier.countDownAndWait();
    ASSERT(!future.isReady());

    pf.promise.emplaceValue(5);
    ASSERT_THROWS_CODE(
        future.get(operationContext()), AssertionException, ErrorCodes::InternalError);
}

TEST_F(AsyncWorkSchedulerTest, ScheduledBlockingWorkInSucceeds) {
    AsyncWorkScheduler async(getServiceContext());

    auto pf = makePromiseFuture<int>();
    auto future = async.scheduleWorkIn(
        Milliseconds{10},
        [future = std::move(pf.future)](OperationContext * opCtx) { return future.get(opCtx); });

    pf.promise.emplaceValue(5);
    ASSERT(!future.isReady());

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->runUntil(network()->now() + Milliseconds{5});
        ASSERT(!future.isReady());
    }

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->runUntil(network()->now() + Milliseconds{5});
        ASSERT(future.isReady());
    }

    ASSERT_EQ(5, future.get(operationContext()));
}

TEST_F(AsyncWorkSchedulerTest, ScheduledRemoteCommandRespondsOK) {
    AsyncWorkScheduler async(getServiceContext());

    auto future = async.scheduleRemoteCommand(
        kShardIds[1], ReadPreferenceSetting{ReadPreference::PrimaryOnly}, BSON("TestCommand" << 1));
    ASSERT(!future.isReady());

    const auto objResponse = BSON("ok" << 1 << "responseData" << 2);
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_BSONOBJ_EQ(BSON("TestCommand" << 1), request.cmdObj);
        return objResponse;
    });

    const auto& response = future.get(operationContext());
    ASSERT(response.isOK());
    ASSERT_BSONOBJ_EQ(objResponse, response.data);
}

TEST_F(AsyncWorkSchedulerTest, ScheduledRemoteCommandRespondsNotOK) {
    AsyncWorkScheduler async(getServiceContext());

    auto future = async.scheduleRemoteCommand(
        kShardIds[1], ReadPreferenceSetting{ReadPreference::PrimaryOnly}, BSON("TestCommand" << 2));
    ASSERT(!future.isReady());

    const auto objResponse = BSON("ok" << 0 << "responseData" << 3);
    onCommand([&](const executor::RemoteCommandRequest& request) {
        ASSERT_BSONOBJ_EQ(BSON("TestCommand" << 2), request.cmdObj);
        return objResponse;
    });

    const auto& response = future.get(operationContext());
    ASSERT(response.isOK());
    ASSERT_BSONOBJ_EQ(objResponse, response.data);
}

TEST_F(AsyncWorkSchedulerTest, ScheduledRemoteCommandsOneOKAndOneError) {
    AsyncWorkScheduler async(getServiceContext());

    auto future1 = async.scheduleRemoteCommand(
        kShardIds[1], ReadPreferenceSetting{ReadPreference::PrimaryOnly}, BSON("TestCommand" << 2));
    auto future2 = async.scheduleRemoteCommand(
        kShardIds[2], ReadPreferenceSetting{ReadPreference::PrimaryOnly}, BSON("TestCommand" << 3));

    ASSERT(!future1.isReady());
    ASSERT(!future2.isReady());

    onCommand([](const executor::RemoteCommandRequest& request) {
        return BSON("ok" << 1 << "responseData" << 3);
    });
    onCommand([](const executor::RemoteCommandRequest& request) {
        return BSON("ok" << 0 << "responseData" << 3);
    });

    const auto& response2 = future2.get(operationContext());
    ASSERT(response2.isOK());

    const auto& response1 = future1.get(operationContext());
    ASSERT(response1.isOK());
}


using DoWhileTest = AsyncWorkSchedulerTest;

TEST_F(DoWhileTest, LoopBodyExecutesAtLeastOnceWithBackoff) {
    AsyncWorkScheduler async(getServiceContext());

    int numLoops = 0;
    auto future = doWhile(async,
                          Backoff(Seconds(1), Milliseconds::max()),
                          [](const StatusWith<int>& status) {
                              uassertStatusOK(status);
                              return false;
                          },
                          [&numLoops] { return Future<int>::makeReady(++numLoops); });

    ASSERT(future.isReady());
    ASSERT_EQ(1, numLoops);
    ASSERT_EQ(1, future.get(operationContext()));
}

TEST_F(DoWhileTest, LoopBodyExecutesManyIterationsWithoutBackoff) {
    AsyncWorkScheduler async(getServiceContext());

    int remainingLoops = 100'000;
    auto future = doWhile(async,
                          boost::none,
                          [&remainingLoops](const StatusWith<int>& status) {
                              uassertStatusOK(status);
                              return remainingLoops > 0;
                          },
                          [&remainingLoops] { return Future<int>::makeReady(--remainingLoops); });

    ASSERT_EQ(0, future.get(operationContext()));
    ASSERT_EQ(0, remainingLoops);
}

TEST_F(DoWhileTest, LoopObeysBackoff) {
    AsyncWorkScheduler async(getServiceContext());

    int numLoops = 0;
    auto future = doWhile(async,
                          Backoff(Seconds(1), Milliseconds::max()),
                          [](const StatusWith<int>& status) { return uassertStatusOK(status) < 3; },
                          [&numLoops] { return Future<int>::makeReady(++numLoops); });

    // The loop body needs to execute at least once
    ASSERT(!future.isReady());
    ASSERT_EQ(1, numLoops);

    // Back-off is 1 millisecond now
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->runUntil(network()->now() + Milliseconds{1});
        ASSERT(!future.isReady());
        ASSERT_EQ(2, numLoops);
    }

    // Back-off is 2 milliseconds now, so advancing the time by 1 millisecond will not cause the
    // loop body to run
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->runUntil(network()->now() + Milliseconds{1});
        ASSERT(!future.isReady());
        ASSERT_EQ(2, numLoops);
    }

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->runUntil(network()->now() + Seconds{1});
        ASSERT(future.isReady());
        ASSERT_EQ(3, numLoops);
    }

    ASSERT_EQ(3, future.get(operationContext()));
}

}  // namespace
}  // namespace txn
}  // namespace mongo
