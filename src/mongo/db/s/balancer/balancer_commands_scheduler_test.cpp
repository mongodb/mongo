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

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler.h"
#include "mongo/db/s/balancer/balancer_commands_scheduler_impl.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/request_types/move_range_request_gen.h"


namespace mongo {
namespace {

using unittest::assertGet;

class BalancerCommandsSchedulerTest : public ConfigServerTestFixture {
public:
    const ShardId kShardId0 = ShardId("shard0");
    const ShardId kShardId1 = ShardId("shard1");
    const HostAndPort kShardHost0 = HostAndPort("TestHost0", 12345);
    const HostAndPort kShardHost1 = HostAndPort("TestHost1", 12346);

    const std::vector<ShardType> kShardList{
        ShardType(kShardId0.toString(), kShardHost0.toString()),
        ShardType(kShardId1.toString(), kShardHost1.toString())};

    const NamespaceString kNss{"testDb.testColl"};
    const NamespaceString kNssWithCustomizedSize{"testDb.testCollCustomized"};

    const UUID kUuid = UUID::gen();

    static constexpr int64_t kDefaultMaxChunkSizeBytes = 128;
    static constexpr int64_t kCustomizedMaxChunkSizeBytes = 256;

    ChunkType makeChunk(long long min, const ShardId& shardId) {
        ChunkType chunk;
        chunk.setMin(BSON("x" << min));
        chunk.setMax(BSON("x" << min + 10));
        chunk.setJumbo(false);
        chunk.setShard(shardId);
        chunk.setVersion(ChunkVersion({OID::gen(), Timestamp(10)}, {1, 1}));
        return chunk;
    }

    MigrateInfo makeMigrationInfo(long long min, const ShardId& to, const ShardId& from) {
        return MigrateInfo(to,
                           from,
                           kNss,
                           kUuid,
                           BSON("x" << min),
                           BSON("x" << min + 10),
                           ChunkVersion({OID::gen(), Timestamp(10)}, {1, 1}),
                           ForceJumbo::kDoNotForce);
    }

    MoveChunkSettings getMoveChunkSettings(int64_t maxChunkSize = kDefaultMaxChunkSizeBytes) {
        return MoveChunkSettings(
            maxChunkSize,
            MigrationSecondaryThrottleOptions::create(
                MigrationSecondaryThrottleOptions::SecondaryThrottleOption::kDefault),
            false);
    }

    MigrationsRecoveryDefaultValues getMigrationRecoveryDefaultValues() {
        return MigrationsRecoveryDefaultValues(
            kDefaultMaxChunkSizeBytes,
            MigrationSecondaryThrottleOptions::create(
                MigrationSecondaryThrottleOptions::SecondaryThrottleOption::kDefault));
    }

    std::vector<BSONObj> getPersistedCommandDocuments(OperationContext* opCtx) {
        auto statusWithPersistedCommandDocs =
            Grid::get(opCtx)->shardRegistry()->getConfigShard()->exhaustiveFindOnConfig(
                opCtx,
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kLocalReadConcern,
                MigrationType::ConfigNS,
                BSONObj(),
                BSONObj(),
                boost::none);

        ASSERT_OK(statusWithPersistedCommandDocs.getStatus());
        return statusWithPersistedCommandDocs.getValue().docs;
    }


protected:
    void setUp() override {
        setUpAndInitializeConfigDb();
        setupShards(kShardList);
        // Scheduler commands target shards that need to be retrieved.
        auto opCtx = operationContext();
        configureTargeter(opCtx, kShardId0, kShardHost0);
        configureTargeter(opCtx, kShardId1, kShardHost1);
    }

    void tearDown() override {
        _scheduler.stop();
        ConfigServerTestFixture::tearDown();
    }

    /*
     * Extra setup function to define the whole sequence of (mocked) remote command responses that a
     * test is expected to receive during its execution.
     * - Must be invoked before running _scheduler.start()
     * - The returned future must be stored in a local variable and kept in scope to ensure that the
     * sequence gets generated
     */
    executor::NetworkTestEnv::FutureHandle<void> setRemoteResponses(
        std::vector<executor::NetworkTestEnv::OnCommandFunction> remoteResponseGenerators = {}) {
        std::vector<executor::NetworkTestEnv::OnCommandFunction> generatorsWithStartSequence;
        // Set an OK response for every shardSvrJoinMigration command send out by the start() method
        // of the commands scheduler
        for (size_t i = 0; i < kShardList.size(); ++i) {
            generatorsWithStartSequence.push_back(
                [&](const executor::RemoteCommandRequest& request) { return OkReply().toBSON(); });
        }
        generatorsWithStartSequence.insert(generatorsWithStartSequence.end(),
                                           remoteResponseGenerators.begin(),
                                           remoteResponseGenerators.end());
        return launchAsync([this, g = std::move(generatorsWithStartSequence)] { onCommands(g); });
    }

    void configureTargeter(OperationContext* opCtx, ShardId shardId, const HostAndPort& host) {
        auto targeter = RemoteCommandTargeterMock::get(
            uassertStatusOK(shardRegistry()->getShard(opCtx, shardId))->getTargeter());
        targeter->setFindHostReturnValue(kShardHost0);
    }

    BalancerCommandsSchedulerImpl _scheduler;
};

TEST_F(BalancerCommandsSchedulerTest, StartAndStopScheduler) {
    auto remoteResponsesFuture = setRemoteResponses();
    _scheduler.start(operationContext(), getMigrationRecoveryDefaultValues());
    _scheduler.stop();
    remoteResponsesFuture.default_timed_get();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulMoveChunkCommand) {
    auto deferredCleanupCompletedCheckpoint =
        globalFailPointRegistry().find("deferredCleanupCompletedCheckpoint");
    auto timesEnteredFailPoint =
        deferredCleanupCompletedCheckpoint->setMode(FailPoint::alwaysOn, 0);
    auto remoteResponsesFuture = setRemoteResponses(
        {[&](const executor::RemoteCommandRequest& request) { return OkReply().toBSON(); }});

    _scheduler.start(operationContext(), getMigrationRecoveryDefaultValues());
    MigrateInfo migrateInfo = makeMigrationInfo(0, kShardId1, kShardId0);

    auto futureResponse = _scheduler.requestMoveChunk(
        operationContext(), migrateInfo, getMoveChunkSettings(), false /* issuedByRemoteUser */);
    ASSERT_OK(futureResponse.getNoThrow());
    remoteResponsesFuture.default_timed_get();
    deferredCleanupCompletedCheckpoint->waitForTimesEntered(timesEnteredFailPoint + 1);
    // Ensure DistLock is released correctly
    {
        auto opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
    }
    deferredCleanupCompletedCheckpoint->setMode(FailPoint::off, 0);
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulMoveRangeCommand) {
    auto deferredCleanupCompletedCheckpoint =
        globalFailPointRegistry().find("deferredCleanupCompletedCheckpoint");
    auto timesEnteredFailPoint =
        deferredCleanupCompletedCheckpoint->setMode(FailPoint::alwaysOn, 0);
    auto remoteResponsesFuture = setRemoteResponses(
        {[&](const executor::RemoteCommandRequest& request) { return OkReply().toBSON(); }});
    _scheduler.start(operationContext(), getMigrationRecoveryDefaultValues());
    ShardsvrMoveRange shardsvrRequest(kNss);
    shardsvrRequest.setDbName(NamespaceString::kAdminDb);
    shardsvrRequest.setFromShard(kShardId0);
    shardsvrRequest.setMaxChunkSizeBytes(1024);
    auto& moveRangeRequestBase = shardsvrRequest.getMoveRangeRequestBase();
    moveRangeRequestBase.setToShard(kShardId1);
    moveRangeRequestBase.setMin({});
    moveRangeRequestBase.setMax({});

    auto futureResponse = _scheduler.requestMoveRange(
        operationContext(), shardsvrRequest, WriteConcernOptions(), false /* issuedByRemoteUser */);
    ASSERT_OK(futureResponse.getNoThrow());
    remoteResponsesFuture.default_timed_get();
    deferredCleanupCompletedCheckpoint->waitForTimesEntered(timesEnteredFailPoint + 1);
    // Ensure DistLock is released correctly
    {
        auto opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
    }
    deferredCleanupCompletedCheckpoint->setMode(FailPoint::off, 0);
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulMergeChunkCommand) {
    auto remoteResponsesFuture = setRemoteResponses(
        {[&](const executor::RemoteCommandRequest& request) { return OkReply().toBSON(); }});
    _scheduler.start(operationContext(), getMigrationRecoveryDefaultValues());

    ChunkRange range(BSON("x" << 0), BSON("x" << 20));
    ChunkVersion version({OID::gen(), Timestamp(10)}, {1, 1});
    auto futureResponse =
        _scheduler.requestMergeChunks(operationContext(), kNss, kShardId0, range, version);
    ASSERT_OK(futureResponse.getNoThrow());
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, MergeChunkNonexistentShard) {
    auto remoteResponsesFuture = setRemoteResponses();
    _scheduler.start(operationContext(), getMigrationRecoveryDefaultValues());
    ChunkRange range(BSON("x" << 0), BSON("x" << 20));
    ChunkVersion version({OID::gen(), Timestamp(10)}, {1, 1});
    auto futureResponse = _scheduler.requestMergeChunks(
        operationContext(), kNss, ShardId("nonexistent"), range, version);
    auto shardNotFoundError = Status{ErrorCodes::ShardNotFound, "Shard nonexistent not found"};
    ASSERT_EQ(futureResponse.getNoThrow(), shardNotFoundError);
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulAutoSplitVectorCommand) {
    BSONObjBuilder autoSplitVectorResponse;
    autoSplitVectorResponse.append("ok", "1");
    BSONArrayBuilder splitKeys(autoSplitVectorResponse.subarrayStart("splitKeys"));
    splitKeys.append(BSON("x" << 7));
    splitKeys.append(BSON("x" << 9));
    splitKeys.done();
    autoSplitVectorResponse.append("continuation", false);

    auto remoteResponsesFuture =
        setRemoteResponses({[&](const executor::RemoteCommandRequest& request) {
            return autoSplitVectorResponse.obj();
        }});
    _scheduler.start(operationContext(), getMigrationRecoveryDefaultValues());

    ChunkType splitChunk = makeChunk(0, kShardId0);
    auto futureResponse = _scheduler.requestAutoSplitVector(operationContext(),
                                                            kNss,
                                                            splitChunk.getShard(),
                                                            BSON("x" << 1),
                                                            splitChunk.getMin(),
                                                            splitChunk.getMax(),
                                                            4);
    auto swAutoSplitVectorResponse = futureResponse.getNoThrow();
    ASSERT_OK(swAutoSplitVectorResponse.getStatus());
    auto receivedSplitKeys = swAutoSplitVectorResponse.getValue().getSplitKeys();
    auto continuation = swAutoSplitVectorResponse.getValue().getContinuation();
    ASSERT_EQ(receivedSplitKeys.size(), 2);
    ASSERT_BSONOBJ_EQ(receivedSplitKeys[0], BSON("x" << 7));
    ASSERT_BSONOBJ_EQ(receivedSplitKeys[1], BSON("x" << 9));
    ASSERT_FALSE(continuation);
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulSplitChunkCommand) {
    auto remoteResponsesFuture = setRemoteResponses(
        {[&](const executor::RemoteCommandRequest& request) { return OkReply().toBSON(); }});
    _scheduler.start(operationContext(), getMigrationRecoveryDefaultValues());
    ChunkType splitChunk = makeChunk(0, kShardId0);
    auto futureResponse = _scheduler.requestSplitChunk(operationContext(),
                                                       kNss,
                                                       splitChunk.getShard(),
                                                       splitChunk.getVersion(),
                                                       KeyPattern(BSON("x" << 1)),
                                                       splitChunk.getMin(),
                                                       splitChunk.getMax(),
                                                       std::vector<BSONObj>{BSON("x" << 5)});
    ASSERT_OK(futureResponse.getNoThrow());
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, SuccessfulRequestChunkDataSizeCommand) {
    BSONObjBuilder chunkSizeResponse;
    chunkSizeResponse.append("ok", "1");
    chunkSizeResponse.append("size", 156);
    chunkSizeResponse.append("numObjects", 25);
    auto remoteResponsesFuture = setRemoteResponses(
        {[&](const executor::RemoteCommandRequest& request) { return chunkSizeResponse.obj(); }});

    _scheduler.start(operationContext(), getMigrationRecoveryDefaultValues());
    ChunkType chunk = makeChunk(0, kShardId0);

    auto futureResponse = _scheduler.requestDataSize(operationContext(),
                                                     kNss,
                                                     chunk.getShard(),
                                                     chunk.getRange(),
                                                     chunk.getVersion(),
                                                     KeyPattern(BSON("x" << 1)),
                                                     false /* issuedByRemoteUser */);
    auto swReceivedDataSize = futureResponse.getNoThrow();
    ASSERT_OK(swReceivedDataSize.getStatus());
    auto receivedDataSize = swReceivedDataSize.getValue();
    ASSERT_EQ(receivedDataSize.sizeBytes, 156);
    ASSERT_EQ(receivedDataSize.numObjects, 25);
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, CommandFailsWhenNetworkReturnsError) {
    auto deferredCleanupCompletedCheckpoint =
        globalFailPointRegistry().find("deferredCleanupCompletedCheckpoint");
    auto timesEnteredFailPoint =
        deferredCleanupCompletedCheckpoint->setMode(FailPoint::alwaysOn, 0);
    auto timeoutError = Status{ErrorCodes::NetworkTimeout, "Mock error: network timed out"};
    auto remoteResponsesFuture = setRemoteResponses(
        {[&](const executor::RemoteCommandRequest& request) { return timeoutError; }});
    _scheduler.start(operationContext(), getMigrationRecoveryDefaultValues());
    MigrateInfo migrateInfo = makeMigrationInfo(0, kShardId1, kShardId0);

    auto futureResponse = _scheduler.requestMoveChunk(
        operationContext(), migrateInfo, getMoveChunkSettings(), false /* issuedByRemoteUser */);
    ASSERT_EQUALS(futureResponse.getNoThrow(), timeoutError);
    remoteResponsesFuture.default_timed_get();
    deferredCleanupCompletedCheckpoint->waitForTimesEntered(timesEnteredFailPoint + 1);

    // Ensure DistLock is released correctly
    {
        auto opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
    }
    deferredCleanupCompletedCheckpoint->setMode(FailPoint::off, 0);
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, CommandFailsWhenSchedulerIsStopped) {
    MigrateInfo migrateInfo = makeMigrationInfo(0, kShardId1, kShardId0);
    auto futureResponse = _scheduler.requestMoveChunk(
        operationContext(), migrateInfo, getMoveChunkSettings(), false /* issuedByRemoteUser */);
    ASSERT_EQUALS(futureResponse.getNoThrow(),
                  Status(ErrorCodes::BalancerInterrupted,
                         "Request rejected - balancer scheduler is stopped"));
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

TEST_F(BalancerCommandsSchedulerTest, CommandCanceledIfUnsubmittedBeforeBalancerStops) {
    SemiFuture<void> futureResponse;
    {
        auto remoteResponsesFuture = setRemoteResponses();
        FailPointEnableBlock failPoint("pauseSubmissionsFailPoint");
        _scheduler.start(operationContext(), getMigrationRecoveryDefaultValues());
        MigrateInfo migrateInfo = makeMigrationInfo(0, kShardId1, kShardId0);
        futureResponse = _scheduler.requestMoveChunk(operationContext(),
                                                     migrateInfo,
                                                     getMoveChunkSettings(),
                                                     false /* issuedByRemoteUser */);
        _scheduler.stop();
        remoteResponsesFuture.default_timed_get();
    }
    ASSERT_EQUALS(futureResponse.getNoThrow(),
                  Status(ErrorCodes::BalancerInterrupted,
                         "Request cancelled - balancer scheduler is stopping"));
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

TEST_F(BalancerCommandsSchedulerTest, MoveChunkCommandGetsPersistedOnDiskWhenRequestIsSubmitted) {
    auto opCtx = operationContext();
    auto defaultValues = getMigrationRecoveryDefaultValues();
    MigrateInfo migrateInfo = makeMigrationInfo(0, kShardId1, kShardId0);
    auto requestSettings = getMoveChunkSettings(kCustomizedMaxChunkSizeBytes);
    auto const serviceContext = getServiceContext();
    auto remoteResponsesFuture =
        setRemoteResponses({[&, serviceContext](const executor::RemoteCommandRequest& request) {
            ThreadClient tc("Test", getGlobalServiceContext());
            auto opCtxHolder = Client::getCurrent()->makeOperationContext();
            // As long as the request is not completed, a persisted recovery document should
            // exist...
            auto persistedCommandDocs = getPersistedCommandDocuments(opCtxHolder.get());
            ASSERT_EQUALS(1, persistedCommandDocs.size());
            auto swPersistedCommand = MigrationType::fromBSON(persistedCommandDocs[0]);

            // ... with the needed info to reconstruct an equivalent command.
            ASSERT_OK(swPersistedCommand.getStatus());
            auto recoveredCommand =
                MoveChunkCommandInfo::recoverFrom(swPersistedCommand.getValue(), defaultValues);
            ASSERT_EQ(kNss, recoveredCommand->getNameSpace());
            ASSERT_EQ(migrateInfo.from, recoveredCommand->getTarget());
            ASSERT_TRUE(recoveredCommand->requiresDistributedLock());

            MoveChunkCommandInfo originalCommandInfo(migrateInfo.nss,
                                                     migrateInfo.from,
                                                     migrateInfo.to,
                                                     migrateInfo.minKey,
                                                     *migrateInfo.maxKey,
                                                     requestSettings.maxChunkSizeBytes,
                                                     requestSettings.secondaryThrottle,
                                                     requestSettings.waitForDelete,
                                                     migrateInfo.forceJumbo,
                                                     migrateInfo.version,
                                                     boost::none);
            ASSERT_BSONOBJ_EQ(originalCommandInfo.serialise(), recoveredCommand->serialise());

            return OkReply().toBSON();
        }});

    _scheduler.start(opCtx, getMigrationRecoveryDefaultValues());
    auto deferredResponse = _scheduler.requestMoveChunk(
        operationContext(), migrateInfo, requestSettings, false /* issuedByRemoteUser */);
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
}

TEST_F(BalancerCommandsSchedulerTest, PersistedCommandsAreReissuedWhenRecoveringFromCrash) {
    auto opCtx = operationContext();
    MigrateInfo migrateInfo = makeMigrationInfo(0, kShardId1, kShardId0);
    // 1. Insert a recovery document on an outstanding migration.
    auto requestSettings = getMoveChunkSettings(kCustomizedMaxChunkSizeBytes);
    MigrationType recoveryInfo(migrateInfo.nss,
                               migrateInfo.minKey,
                               *migrateInfo.maxKey,
                               migrateInfo.from,
                               migrateInfo.to,
                               migrateInfo.version,
                               requestSettings.waitForDelete,
                               migrateInfo.forceJumbo,
                               kCustomizedMaxChunkSizeBytes,
                               boost::none /* secondaryTrottle */);
    ASSERT_OK(Grid::get(opCtx)->catalogClient()->insertConfigDocument(
        opCtx,
        MigrationType::ConfigNS,
        recoveryInfo.toBSON(),
        ShardingCatalogClient::kMajorityWriteConcern));

    // 2. Once started, the persisted document should trigger the remote execution of a request...
    auto defaultValues = getMigrationRecoveryDefaultValues();
    auto moveChunkRemoteResponseGenerator = [&](const executor::RemoteCommandRequest& request) {
        auto expectedCommandInfo = MoveChunkCommandInfo::recoverFrom(recoveryInfo, defaultValues);
        // 3. ... Which content should match the recovery doc & configuration.
        ASSERT_BSONOBJ_EQ(expectedCommandInfo->serialise(), request.cmdObj);
        return OkReply().toBSON();
    };
    auto remoteResponsesFuture = setRemoteResponses({moveChunkRemoteResponseGenerator});
    _scheduler.start(opCtx, defaultValues);

    // 4. Once the recovery phase is complete, no persisted documents should remain
    //    (stop() is invoked to ensure that the observed state is stable).
    remoteResponsesFuture.default_timed_get();
    _scheduler.stop();
    auto persistedCommandDocs = getPersistedCommandDocuments(operationContext());
    ASSERT_EQUALS(0, persistedCommandDocs.size());
}

TEST_F(BalancerCommandsSchedulerTest, DistLockPreventsMoveChunkWithConcurrentDDL) {
    OperationContext* opCtx;
    FailPoint* failpoint = globalFailPointRegistry().find("pauseSubmissionsFailPoint");
    failpoint->setMode(FailPoint::Mode::alwaysOn);
    {
        auto remoteResponsesFuture = setRemoteResponses();
        _scheduler.start(operationContext(), getMigrationRecoveryDefaultValues());
        opCtx = Client::getCurrent()->getOperationContext();
        const std::string whyMessage(str::stream()
                                     << "Test acquisition of distLock for " << kNss.ns());
        auto scopedDistLock = DistLockManager::get(opCtx)->lock(
            opCtx, kNss.ns(), whyMessage, DistLockManager::kSingleLockAttemptTimeout);
        ASSERT_OK(scopedDistLock.getStatus());
        failpoint->setMode(FailPoint::Mode::off);
        MigrateInfo migrateInfo = makeMigrationInfo(0, kShardId1, kShardId0);
        auto futureResponse = _scheduler.requestMoveChunk(operationContext(),
                                                          migrateInfo,
                                                          getMoveChunkSettings(),
                                                          false /* issuedByRemoteUser */);
        remoteResponsesFuture.default_timed_get();
        ASSERT_EQ(
            futureResponse.getNoThrow(),
            Status(ErrorCodes::LockBusy, "Failed to acquire dist lock testDb.testColl locally"));
    }
    _scheduler.stop();
}

}  // namespace
}  // namespace mongo
