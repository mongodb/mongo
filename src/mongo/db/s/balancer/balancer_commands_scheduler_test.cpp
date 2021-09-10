/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/s/balancer/balancer_commands_scheduler.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler_impl.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"

namespace mongo {
namespace {

using unittest::assertGet;

class BalancerCommandsSchedulerTest : public ShardServerTestFixture {
public:
    const std::vector<ShardType> kShardList{ShardType("shard0", "Host0:12345"),
                                            ShardType("shard1", "Host1:12345")};
    const NamespaceString kNss{"testDb.testColl"};

    void setUp() override {
        ShardServerTestFixture::setUp();
        for (auto shardType : kShardList) {
            auto shard = assertGet(
                shardRegistry()->getShard(operationContext(), ShardId(shardType.getName())));
            RemoteCommandTargeterMock::get(shard->getTargeter())
                ->setFindHostReturnValue(HostAndPort::parse(shardType.getHost()));
        }
    }

    void tearDown() override {
        _scheduler.stop();
        ShardServerTestFixture::tearDown();
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(const std::vector<ShardType> kShardList) : _shards(kShardList) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
                return repl::OpTimeWith<std::vector<ShardType>>(_shards);
            }

        private:
            const std::vector<ShardType> _shards;
        };
        return std::make_unique<StaticCatalogClient>(kShardList);
    }

    ChunkType makeChunk(long long min, std::string shardName) {
        ChunkType chunk;
        chunk.setMin(BSON("x" << min));
        chunk.setMax(BSON("x" << min + 10));
        chunk.setJumbo(false);
        chunk.setShard(ShardId(shardName));
        chunk.setVersion(ChunkVersion(1, 1, OID::gen(), Timestamp(10)));
        return chunk;
    }

    MoveChunkSettings getDefaultMoveChunkSettings() {
        return MoveChunkSettings(
            128,
            MigrationSecondaryThrottleOptions::create(
                MigrationSecondaryThrottleOptions::SecondaryThrottleOption::kDefault),
            false,
            MoveChunkRequest::ForceJumbo::kDoNotForce);
    }

protected:
    BalancerCommandsSchedulerImpl _scheduler;
};

TEST_F(BalancerCommandsSchedulerTest, StartAndStopScheduler) {
    _scheduler.start();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulMoveChunkCommand) {
    _scheduler.start();
    ChunkType moveChunk = makeChunk(0, "shard0");
    auto networkResponseFuture = launchAsync([&]() {
        onCommand(
            [&](const executor::RemoteCommandRequest& request) { return BSON("ok" << true); });
    });
    auto resp = _scheduler.requestMoveChunk(
        kNss, moveChunk, ShardId("shard1"), getDefaultMoveChunkSettings());
    ASSERT_OK(resp->getOutcome());
    networkResponseFuture.default_timed_get();
    // Ensure DistLock is released correctly
    {
        auto opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
    }
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulMergeChunkCommand) {
    _scheduler.start();
    ChunkType chunk1 = makeChunk(0, "shard0");
    ChunkType chunk2 = makeChunk(10, "shard0");
    auto networkResponseFuture = launchAsync([&]() {
        onCommand(
            [&](const executor::RemoteCommandRequest& request) { return BSON("ok" << true); });
    });
    auto resp = _scheduler.requestMergeChunks(kNss, chunk1, chunk2);
    ASSERT_OK(resp->getOutcome());
    networkResponseFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, MergeChunkNonexistentShard) {
    _scheduler.start();
    ChunkType brokenChunk1 = makeChunk(0, "shard0");
    brokenChunk1.setShard(ShardId("nonexistent"));
    ChunkType brokenChunk2 = makeChunk(10, "shard0");
    brokenChunk2.setShard(ShardId("nonexistent"));
    auto resp = _scheduler.requestMergeChunks(kNss, brokenChunk1, brokenChunk2);
    auto shardNotFoundError = Status{ErrorCodes::ShardNotFound, "Shard nonexistent not found"};
    ASSERT_EQ(resp->getOutcome(), shardNotFoundError);
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulSplitVectorCommand) {
    _scheduler.start();
    ChunkType splitChunk = makeChunk(0, "shard0");
    BSONObjBuilder splitChunkResponse;
    splitChunkResponse.append("ok", "1");
    BSONArrayBuilder splitKeys(splitChunkResponse.subarrayStart("splitKeys"));
    splitKeys.append(BSON("x" << 5));
    splitKeys.done();
    auto networkResponseFuture = launchAsync([&]() {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            return splitChunkResponse.obj();
        });
    });
    auto resp = _scheduler.requestSplitVector(
        kNss, splitChunk, ShardKeyPattern(BSON("x" << 1)), SplitVectorSettings());
    ASSERT_OK(resp->getOutcome());
    ASSERT_OK(resp->getSplitKeys().getStatus());
    ASSERT_EQ(resp->getSplitKeys().getValue().size(), 1);
    ASSERT_BSONOBJ_EQ(resp->getSplitKeys().getValue()[0], BSON("x" << 5));
    networkResponseFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulSplitChunkCommand) {
    _scheduler.start();
    ChunkType splitChunk = makeChunk(0, "shard0");
    auto networkResponseFuture = launchAsync([&]() {
        onCommand(
            [&](const executor::RemoteCommandRequest& request) { return BSON("ok" << true); });
    });
    auto resp = _scheduler.requestSplitChunk(
        kNss, splitChunk, ShardKeyPattern(BSON("x" << 1)), std::vector<BSONObj>{BSON("x" << 5)});
    ASSERT_OK(resp->getOutcome());
    networkResponseFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulRequestChunkDataSizeCommand) {
    _scheduler.start();
    ChunkType chunk = makeChunk(0, "shard0");
    BSONObjBuilder chunkSizeResponse;
    chunkSizeResponse.append("ok", "1");
    chunkSizeResponse.append("size", 156);
    chunkSizeResponse.append("numObjects", 25);
    auto networkResponseFuture = launchAsync([&]() {
        onCommand(
            [&](const executor::RemoteCommandRequest& request) { return chunkSizeResponse.obj(); });
    });
    auto resp =
        _scheduler.requestChunkDataSize(kNss, chunk, ShardKeyPattern(BSON("x" << 1)), false);
    ASSERT_OK(resp->getOutcome());
    ASSERT_OK(resp->getSize().getStatus());
    ASSERT_EQ(resp->getSize().getValue(), 156);
    ASSERT_OK(resp->getNumObjects().getStatus());
    ASSERT_EQ(resp->getNumObjects().getValue(), 25);
    networkResponseFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, CommandFailsWhenNetworkReturnsError) {
    _scheduler.start();
    ChunkType moveChunk = makeChunk(0, "shard0");
    auto timeoutError = Status{ErrorCodes::NetworkTimeout, "Mock error: network timed out"};
    auto networkResponseFuture = launchAsync([&]() {
        onCommand([&](const executor::RemoteCommandRequest& request) { return timeoutError; });
    });
    auto resp = _scheduler.requestMoveChunk(
        kNss, moveChunk, ShardId("shard1"), getDefaultMoveChunkSettings());
    ASSERT_EQUALS(resp->getOutcome(), timeoutError);
    networkResponseFuture.default_timed_get();
    // Ensure DistLock is released correctly
    {
        auto opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
    }
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, CommandFailsWhenSchedulerIsStopped) {
    ChunkType moveChunk = makeChunk(0, "shard0");
    auto resp = _scheduler.requestMoveChunk(
        kNss, moveChunk, ShardId("shard1"), getDefaultMoveChunkSettings());
    ASSERT_EQUALS(
        resp->getOutcome(),
        Status(ErrorCodes::CallbackCanceled, "Request rejected - balancer scheduler is stopped"));
    // Ensure DistLock is not taken
    {
        auto opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
    }
}

TEST_F(BalancerCommandsSchedulerTest, CommandCanceledIfBalancerStops) {
    std::unique_ptr<MoveChunkResponse> resp;
    {
        FailPointEnableBlock failPoint("pauseBalancerWorkerThread");
        _scheduler.start();
        ChunkType moveChunk = makeChunk(0, "shard0");
        resp = _scheduler.requestMoveChunk(
            kNss, moveChunk, ShardId("shard1"), getDefaultMoveChunkSettings());
        _scheduler.stop();
    }
    ASSERT_EQUALS(
        resp->getOutcome(),
        Status(ErrorCodes::CallbackCanceled, "Request cancelled - balancer scheduler is stopping"));
    // Ensure DistLock is released correctly
    {
        auto opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
    }
}

TEST_F(BalancerCommandsSchedulerTest, DistLockPreventsMoveChunkWithConcurrentDDL) {
    OperationContext* opCtx;
    FailPoint* failpoint = globalFailPointRegistry().find("pauseBalancerWorkerThread");
    failpoint->setMode(FailPoint::Mode::alwaysOn);
    {
        _scheduler.start();
        opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
        failpoint->setMode(FailPoint::Mode::off);
        ChunkType moveChunk = makeChunk(0, "shard0");
        auto resp = _scheduler.requestMoveChunk(
            kNss, moveChunk, ShardId("shard1"), getDefaultMoveChunkSettings());
        ASSERT_EQ(
            resp->getOutcome(),
            Status(ErrorCodes::LockBusy, "Failed to acquire dist lock testDb.testColl locally"));
    }
    _scheduler.stop();
}

}  // namespace
}  // namespace mongo
