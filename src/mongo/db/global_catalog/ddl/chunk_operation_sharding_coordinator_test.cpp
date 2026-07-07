/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/chunk_operation_sharding_coordinator.h"

#include "mongo/db/global_catalog/ddl/merge_all_chunks_coordinator.h"
#include "mongo/db/global_catalog/ddl/merge_chunks_coordinator.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_external_state_for_test.h"
#include "mongo/db/global_catalog/ddl/split_chunk_coordinator.h"
#include "mongo/db/global_catalog/ddl/test_chunk_operation_sharding_coordinator_document_gen.h"
#include "mongo/db/repl/primary_only_service_test_fixture.h"
#include "mongo/db/s/move_range_coordinator.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"

#include <memory>

namespace mongo {

class ClientObserver : public ServiceContext::ClientObserver {
public:
    void onCreateClient(Client*) override {}

    void onDestroyClient(Client*) override {}

    void onCreateOperationContext(OperationContext* opCtx) override {
        opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
    }

    void onDestroyOperationContext(OperationContext*) override {}
};

class ChunkOperationShardingCoordinatorTest : public repl::PrimaryOnlyServiceMongoDTest {
public:
    static inline const auto kTestNs = NamespaceString::createNamespaceString_forTest("test.test");
    static inline const ShardHandle kTestShardHandle{ShardId("test-shard"), UUID::gen()};
    static inline const KeyPattern kTestKeyPattern{BSON("x" << 1)};

    ChunkOperationShardingCoordinatorTest()
        : repl::PrimaryOnlyServiceMongoDTest(
              Options{}.addClientObserver(std::make_unique<ClientObserver>())),
          _externalState(std::make_shared<ShardingCoordinatorExternalStateForTest>()) {}

    std::unique_ptr<repl::PrimaryOnlyService> makeService(ServiceContext* serviceContext) override {
        auto externalStateFactory =
            std::make_unique<ShardingCoordinatorExternalStateFactoryForTest>(_externalState);
        return std::make_unique<ShardingCoordinatorService>(serviceContext,
                                                            std::move(externalStateFactory));
    }

    void setUp() override {
        repl::PrimaryOnlyServiceMongoDTest::setUp();

        ShardingState::get(getServiceContext())
            ->setRecoveryCompleted({OID::gen(),
                                    ClusterRole::ShardServer,
                                    ConnectionString(HostAndPort("localhost", 27017)),
                                    kTestShardHandle.name()},
                                   kTestShardHandle.uuid().value());

        _opCtx = cc().getOperationContext();
        if (!_opCtx) {
            _opCtxHolder = cc().makeOperationContext();
            _opCtx = _opCtxHolder.get();
        }

        setupCollectionMetadata();

        auto network = std::make_unique<executor::NetworkInterfaceMock>();
        _network = network.get();
        executor::ThreadPoolMock::Options thread_pool_options;
        thread_pool_options.onCreateThread = [] {
            Client::initThread("ChunkOperationShardingCoordinatorTest",
                               getGlobalServiceContext()->getService());
        };

        _executor = makeThreadPoolTestExecutor(std::move(network), thread_pool_options);
        _executor->startup();

        _scopedExecutor = std::make_shared<executor::ScopedTaskExecutor>(_executor);
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();
        _executor.reset();

        repl::PrimaryOnlyServiceMongoDTest::tearDown();
    }

protected:
    executor::NetworkInterfaceMock* _network;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    std::shared_ptr<executor::ScopedTaskExecutor> _scopedExecutor;
    std::shared_ptr<ShardingCoordinatorExternalStateForTest> _externalState;
    ServiceContext::UniqueOperationContext _opCtxHolder;
    OperationContext* _opCtx;

    ChunkType generateChunk(const UUID& collUuid) {
        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collUuid);
        chunk.setVersion(ChunkVersion({OID::gen(), Timestamp(1, 1)}, {1, 0}));
        chunk.setShard(kTestShardHandle.name());
        chunk.setRange({kTestKeyPattern.globalMin(), kTestKeyPattern.globalMax()});
        chunk.setOnCurrentShardSince(Timestamp(1, 0));
        chunk.setHistory({});
        return chunk;
    }

    void setupCollectionMetadata() {
        const auto uuid = UUID::gen();
        const std::vector chunks{generateChunk(uuid)};
        auto rt = RoutingTableHistory::makeNewAllowingGaps(kTestNs,
                                                           uuid,
                                                           kTestKeyPattern,
                                                           false,
                                                           nullptr,
                                                           false,
                                                           chunks[0].getVersion().epoch(),
                                                           chunks[0].getVersion().getTimestamp(),
                                                           boost::none,
                                                           boost::none,
                                                           true,
                                                           chunks);
        const auto version = rt.getVersion();
        const auto rtHandle = RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
        const auto collectionMetadata =
            CollectionMetadata(CurrentChunkManager(rtHandle), kTestShardHandle.name());
        auto scopedCSR = CollectionShardingRuntime::acquireExclusive(_opCtx, kTestNs);
        scopedCSR->setCollectionMetadata(
            _opCtx, collectionMetadata, CollectionShardingRuntime::NoRoutingTableAs::kUntracked);
    }

    MergeChunksCoordinatorDocument makeMergeChunksCoordinatorDoc(std::vector<BSONObj> bounds,
                                                                 OID epoch) {
        MergeChunksCoordinatorDocument doc;
        ShardsvrMergeChunksRequest req;
        req.setBounds(bounds);
        req.setEpoch(epoch);
        ShardingCoordinatorMetadata metadata{{kTestNs, CoordinatorTypeEnum::kMergeChunks}};
        ForwardableOperationMetadata forwardableOpMetadata(_opCtx);
        metadata.setForwardableOpMetadata(forwardableOpMetadata);
        doc.setShardingCoordinatorMetadata(std::move(metadata));
        doc.setShardsvrMergeChunksRequest(req);
        return doc;
    }

    MergeAllChunksCoordinatorDocument makeMergeAllChunksCoordinatorDoc(
        const ShardId& shard,
        int maxNumberOfChunksToMerge = std::numeric_limits<int>::max(),
        int maxTimeProcessingChunksMS = std::numeric_limits<int>::max()) {
        MergeAllChunksCoordinatorDocument doc;
        ShardsvrMergeAllChunksOnShardRequest req;
        req.setShard(shard);
        req.setMaxNumberOfChunksToMerge(maxNumberOfChunksToMerge);
        req.setMaxTimeProcessingChunksMS(maxTimeProcessingChunksMS);
        ShardingCoordinatorMetadata metadata{
            {NamespaceString::createNamespaceString_forTest("test.coll"),
             CoordinatorTypeEnum::kMergeAllChunks}};
        ForwardableOperationMetadata forwardableOpMetadata(_opCtx);
        metadata.setForwardableOpMetadata(forwardableOpMetadata);
        doc.setShardingCoordinatorMetadata(std::move(metadata));
        doc.setShardsvrMergeAllChunksOnShardRequest(req);
        return doc;
    }

    SplitChunkCoordinatorDocument makeSplitChunkCoordinatorDoc(std::vector<BSONObj> splitKeys,
                                                               OID epoch) {
        SplitChunkCoordinatorDocument doc;
        ShardsvrSplitChunkRequest req;
        req.setKeyPattern(BSON("x" << 1));
        req.setMin(BSON("x" << 0));
        req.setMax(BSON("x" << 100));
        req.setSplitKeys(std::move(splitKeys));
        req.setFrom("shard0000");
        req.setEpoch(epoch);
        ShardingCoordinatorMetadata metadata{{kTestNs, CoordinatorTypeEnum::kSplitChunk}};
        ForwardableOperationMetadata forwardableOpMetadata(_opCtx);
        metadata.setForwardableOpMetadata(forwardableOpMetadata);
        doc.setShardingCoordinatorMetadata(std::move(metadata));
        doc.setShardsvrSplitChunkRequest(req);
        return doc;
    }

    MoveRangeCoordinatorDocument makeMoveRangeCoordinatorDoc(BSONObj min,
                                                             BSONObj max,
                                                             ShardId fromShard,
                                                             ShardId toShard) {
        MoveRangeCoordinatorDocument doc;
        ShardsvrMoveRangeRequest req;
        req.setToShard(toShard);
        req.setMin(min);
        req.setMax(max);
        req.setFromShard(fromShard);
        req.setCollectionTimestamp(Timestamp(1, 1));
        req.setMaxChunkSizeBytes(64 * 1024 * 1024);
        ShardingCoordinatorMetadata metadata{
            {NamespaceString::createNamespaceString_forTest("test.move"),
             CoordinatorTypeEnum::kMoveRange}};
        ForwardableOperationMetadata forwardableOpMetadata(_opCtx);
        metadata.setForwardableOpMetadata(forwardableOpMetadata);
        doc.setShardingCoordinatorMetadata(std::move(metadata));
        doc.setShardsvrMoveRangeRequest(req);
        doc.setMigrationId(UUID::gen());
        return doc;
    }

    class TestChunkOperationShardingCoordinator
        : public ChunkOperationShardingCoordinator<TestChunkOperationShardingCoordinatorDocument> {
    public:
        TestChunkOperationShardingCoordinator(
            ShardingCoordinatorService* service,
            TestChunkOperationShardingCoordinatorDocument coordinatorMetadata)
            : ChunkOperationShardingCoordinator<TestChunkOperationShardingCoordinatorDocument>(
                  service, "TestChunkOperationShardingCoordinator", coordinatorMetadata.toBSON()) {}

        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override {
            return boost::none;
        }

        void checkIfOptionsConflict(const BSONObj& doc) const final {}

        ExecutorFuture<void> _acquireLocksAsync(
            OperationContext* opCtx,
            std::shared_ptr<executor::ScopedTaskExecutor> executor,
            const CancellationToken& token) override {
            return ExecutorFuture<void>{**executor};
        }

        ExecutorFuture<void> _runImpl(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                      const CancellationToken& token) noexcept override {
            return ExecutorFuture<void>(**executor);
        }

        bool isInCriticalSection(Phase phase) const override {
            return false;
        }
    };
};

TEST_F(ChunkOperationShardingCoordinatorTest, SmokeTest) {
    CancellationSource cancellationSource;

    TestChunkOperationShardingCoordinatorDocument doc;
    ShardingCoordinatorMetadata coorMetadata{{kTestNs, CoordinatorTypeEnum::kTestCoordinator}};

    ForwardableOperationMetadata forwardableOpMetadata(_opCtx);
    coorMetadata.setForwardableOpMetadata(forwardableOpMetadata);

    doc.setShardingCoordinatorMetadata(std::move(coorMetadata));

    auto coordinator = std::make_shared<TestChunkOperationShardingCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), std::move(doc));
    auto future = (static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get()))
                      ->run(_scopedExecutor, cancellationSource.token());
    future.get();
}

TEST_F(ChunkOperationShardingCoordinatorTest, MergeChunksCheckIfOptionsConflictSameParams) {
    auto epoch = OID::gen();
    std::vector<BSONObj> bounds = {BSON("a" << 1), BSON("a" << 10)};
    auto coordinatorDoc = makeMergeChunksCoordinatorDoc(bounds, epoch);

    auto coordinator = std::make_shared<MergeChunksCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Same parameters — should not throw.
    ASSERT_DOES_NOT_THROW(coordinator->checkIfOptionsConflict(coordinatorDoc.toBSON()));

    // Satisfy destructor invariants by resolving internal promises.
    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, MergeAllChunksCheckIfOptionsConflictSameParams) {
    const ShardId shard{"shard0"};
    auto coordinatorDoc = makeMergeAllChunksCoordinatorDoc(shard);

    auto coordinator = std::make_shared<MergeAllChunksCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Same parameters — should not throw.
    ASSERT_DOES_NOT_THROW(coordinator->checkIfOptionsConflict(coordinatorDoc.toBSON()));

    // Satisfy destructor invariants by resolving internal promises.
    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, SplitChunkCheckIfOptionsConflictSameParams) {
    auto epoch = OID::gen();
    std::vector<BSONObj> splitKeys = {BSON("x" << 50)};
    auto coordinatorDoc = makeSplitChunkCoordinatorDoc(splitKeys, epoch);

    auto coordinator = std::make_shared<SplitChunkCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Same parameters — should not throw.
    ASSERT_DOES_NOT_THROW(coordinator->checkIfOptionsConflict(coordinatorDoc.toBSON()));

    // Satisfy destructor invariants by resolving internal promises.
    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, MergeChunksCheckIfOptionsConflictDifferentBounds) {
    auto epoch = OID::gen();
    std::vector<BSONObj> bounds = {BSON("a" << 1), BSON("a" << 10)};
    auto coordinatorDoc = makeMergeChunksCoordinatorDoc(bounds, epoch);

    auto coordinator = std::make_shared<MergeChunksCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Different bounds — should throw.
    std::vector<BSONObj> differentBounds = {BSON("a" << 5), BSON("a" << 20)};
    auto otherDoc = makeMergeChunksCoordinatorDoc(differentBounds, epoch);

    ASSERT_THROWS_CODE(coordinator->checkIfOptionsConflict(otherDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, MergeAllChunksCheckIfOptionsConflictDifferentShard) {
    const ShardId shard{"shard0"};
    auto coordinatorDoc = makeMergeAllChunksCoordinatorDoc(shard);

    auto coordinator = std::make_shared<MergeAllChunksCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Different shard — should throw.
    const ShardId differentShard{"shard1"};
    auto otherDoc = makeMergeAllChunksCoordinatorDoc(differentShard);

    ASSERT_THROWS_CODE(coordinator->checkIfOptionsConflict(otherDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, SplitChunkCheckIfOptionsConflictDifferentSplitKeys) {
    auto epoch = OID::gen();
    std::vector<BSONObj> splitKeys = {BSON("x" << 50)};
    auto coordinatorDoc = makeSplitChunkCoordinatorDoc(splitKeys, epoch);

    auto coordinator = std::make_shared<SplitChunkCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Different split keys — should throw.
    std::vector<BSONObj> differentSplitKeys = {BSON("x" << 60)};
    auto otherDoc = makeSplitChunkCoordinatorDoc(differentSplitKeys, epoch);

    ASSERT_THROWS_CODE(coordinator->checkIfOptionsConflict(otherDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, MergeChunksCheckIfOptionsConflictDifferentEpoch) {
    auto epoch = OID::gen();
    std::vector<BSONObj> bounds = {BSON("a" << 1), BSON("a" << 10)};
    auto coordinatorDoc = makeMergeChunksCoordinatorDoc(bounds, epoch);

    auto coordinator = std::make_shared<MergeChunksCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Different epoch — should throw.
    auto differentEpoch = OID::gen();
    auto otherDoc = makeMergeChunksCoordinatorDoc(bounds, differentEpoch);

    ASSERT_THROWS_CODE(coordinator->checkIfOptionsConflict(otherDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest,
       MergeAllChunksCheckIfOptionsConflictDifferentMaxNumberOfChunks) {
    const ShardId shard{"shard0"};
    auto coordinatorDoc = makeMergeAllChunksCoordinatorDoc(shard, /*maxNumberOfChunksToMerge=*/100);

    auto coordinator = std::make_shared<MergeAllChunksCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Different maxNumberOfChunksToMerge — should throw.
    auto otherDoc = makeMergeAllChunksCoordinatorDoc(shard, /*maxNumberOfChunksToMerge=*/50);

    ASSERT_THROWS_CODE(coordinator->checkIfOptionsConflict(otherDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, SplitChunkAppendCommandInfoIncludesRequestFields) {
    auto epoch = OID::gen();
    std::vector<BSONObj> splitKeys = {BSON("x" << 50), BSON("x" << 75)};
    auto coordinatorDoc = makeSplitChunkCoordinatorDoc(splitKeys, epoch);

    auto coordinator = std::make_shared<SplitChunkCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    BSONObjBuilder cmdInfoBuilder;
    coordinator->appendCommandInfo(&cmdInfoBuilder);
    auto cmdInfo = cmdInfoBuilder.obj();
    ASSERT_BSONOBJ_EQ(cmdInfo.getObjectField("min"), BSON("x" << 0));
    ASSERT_BSONOBJ_EQ(cmdInfo.getObjectField("max"), BSON("x" << 100));
    ASSERT_EQ(cmdInfo.getStringField("from"), "shard0000");
    ASSERT_EQ(cmdInfo.getField("splitKeys").Array().size(), 2u);

    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, MoveRangeCheckIfOptionsConflictSameParams) {
    auto coordinatorDoc = makeMoveRangeCoordinatorDoc(
        BSON("a" << 0), BSON("a" << 100), ShardId{"shard0000"}, ShardId{"shard0001"});

    auto coordinator = std::make_shared<MoveRangeCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Same parameters — should not throw.
    ASSERT_DOES_NOT_THROW(coordinator->checkIfOptionsConflict(coordinatorDoc.toBSON()));

    // Satisfy destructor invariants by resolving internal promises.
    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, MoveRangeCheckIfOptionsConflictDifferentBounds) {
    auto coordinatorDoc = makeMoveRangeCoordinatorDoc(
        BSON("a" << 0), BSON("a" << 100), ShardId{"shard0000"}, ShardId{"shard0001"});

    auto coordinator = std::make_shared<MoveRangeCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Different bounds — should throw.
    auto otherDoc = makeMoveRangeCoordinatorDoc(
        BSON("a" << 50), BSON("a" << 150), ShardId{"shard0000"}, ShardId{"shard0001"});

    ASSERT_THROWS_CODE(coordinator->checkIfOptionsConflict(otherDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, MoveRangeCheckIfOptionsConflictDifferentToShard) {
    auto coordinatorDoc = makeMoveRangeCoordinatorDoc(
        BSON("a" << 0), BSON("a" << 100), ShardId{"shard0000"}, ShardId{"shard0001"});

    auto coordinator = std::make_shared<MoveRangeCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Different recipient shard — should throw.
    auto otherDoc = makeMoveRangeCoordinatorDoc(
        BSON("a" << 0), BSON("a" << 100), ShardId{"shard0000"}, ShardId{"shard0002"});

    ASSERT_THROWS_CODE(coordinator->checkIfOptionsConflict(otherDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, MoveRangeCheckIfOptionsConflictDifferentFromShard) {
    auto coordinatorDoc = makeMoveRangeCoordinatorDoc(
        BSON("a" << 0), BSON("a" << 100), ShardId{"shard0000"}, ShardId{"shard0001"});

    auto coordinator = std::make_shared<MoveRangeCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Different donor shard — should throw.
    auto otherDoc = makeMoveRangeCoordinatorDoc(
        BSON("a" << 0), BSON("a" << 100), ShardId{"shard0003"}, ShardId{"shard0001"});

    ASSERT_THROWS_CODE(coordinator->checkIfOptionsConflict(otherDoc.toBSON()),
                       DBException,
                       ErrorCodes::ConflictingOperationInProgress);

    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest, MoveRangeAppendCommandInfoIncludesRequestFields) {
    auto coordinatorDoc = makeMoveRangeCoordinatorDoc(
        BSON("a" << 0), BSON("a" << 100), ShardId{"shard0000"}, ShardId{"shard0001"});

    auto coordinator = std::make_shared<MoveRangeCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    BSONObjBuilder cmdInfoBuilder;
    coordinator->appendCommandInfo(&cmdInfoBuilder);
    auto cmdInfo = cmdInfoBuilder.obj();
    ASSERT_BSONOBJ_EQ(cmdInfo.getObjectField("min"), BSON("a" << 0));
    ASSERT_BSONOBJ_EQ(cmdInfo.getObjectField("max"), BSON("a" << 100));
    ASSERT_EQ(cmdInfo.getStringField("toShard"), "shard0001");
    ASSERT_EQ(cmdInfo.getStringField("fromShard"), "shard0000");

    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

TEST_F(ChunkOperationShardingCoordinatorTest,
       MoveRangeCheckIfOptionsConflictIgnoresWriteConcernDifference) {
    auto coordinatorDoc = makeMoveRangeCoordinatorDoc(
        BSON("a" << 0), BSON("a" << 100), ShardId{"shard0000"}, ShardId{"shard0001"});

    auto coordinator = std::make_shared<MoveRangeCoordinator>(
        static_cast<ShardingCoordinatorService*>(_service), coordinatorDoc.toBSON());

    // Same logical request, different writeConcern — should NOT throw, since WC is intentionally
    // not part of ShardsvrMoveRangeRequest (and therefore not part of the conflict-compare).
    auto otherDoc = coordinatorDoc;
    otherDoc.setWriteConcern(WriteConcernOptions{
        "majority", WriteConcernOptions::SyncMode::UNSET, WriteConcernOptions::kNoTimeout});

    ASSERT_DOES_NOT_THROW(coordinator->checkIfOptionsConflict(otherDoc.toBSON()));

    static_cast<repl::PrimaryOnlyService::Instance*>(coordinator.get())
        ->interrupt({ErrorCodes::Interrupted, "Test cleanup"});
}

}  // namespace mongo
