/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <fmt/format.h>

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/session_update_tracker.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding/resharding_oplog_applier.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding_util.h"
#include "mongo/db/s/sharding_mongod_test_fixture.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

using namespace fmt::literals;

class OplogIteratorMock : public ReshardingDonorOplogIteratorInterface {
public:
    OplogIteratorMock(std::deque<repl::OplogEntry> oplogToReturn, size_t batchSize)
        : _oplogToReturn(std::move(oplogToReturn)), _batchSize(batchSize) {
        invariant(batchSize > 0);
    }

    ExecutorFuture<std::vector<repl::OplogEntry>> getNextBatch(
        std::shared_ptr<executor::TaskExecutor> executor) override {
        // This operation context is unused by the function but confirms that the Client calling
        // getNextBatch() doesn't already have an operation context.
        auto opCtx = cc().makeOperationContext();

        return ExecutorFuture(std::move(executor)).then([this] {
            std::vector<repl::OplogEntry> ret;

            auto end = _oplogToReturn.begin() + std::min(_batchSize, _oplogToReturn.size());
            std::copy(_oplogToReturn.begin(), end, std::back_inserter(ret));
            _oplogToReturn.erase(_oplogToReturn.begin(), end);

            if (_oplogToReturn.empty() && _doThrow) {
                uasserted(ErrorCodes::InternalError, "OplogIteratorMock simulating error");
            }

            return ret;
        });
    }

    /**
     * Makes this iterator throw an error when calling getNextBatch with only a single item left in
     * the buffer. This allows simulating an exception being thrown at different points in time.
     */
    void setThrowWhenSingleItem() {
        _doThrow = true;
    }

private:
    std::deque<repl::OplogEntry> _oplogToReturn;
    const size_t _batchSize;
    bool _doThrow{false};
};

/**
 * RAII type for temporarily changing the value for gReshardingWriterThreadCount.
 */
class ReshardingWriterVectorSizeBlock {
public:
    using ValType = decltype(resharding::gReshardingWriterThreadCount);

    ReshardingWriterVectorSizeBlock(ValType newValue)
        : _origValue(resharding::gReshardingWriterThreadCount) {
        resharding::gReshardingWriterThreadCount = newValue;
    }

    ~ReshardingWriterVectorSizeBlock() {
        resharding::gReshardingWriterThreadCount = _origValue;
    }

private:
    ValType _origValue;
};

class ReshardingOplogApplierTest : public ShardingMongodTestFixture {
public:
    const HostAndPort kConfigHostAndPort{"DummyConfig", 12345};
    const std::string kOriginalShardKey = "sk";
    const BSONObj kOriginalShardKeyPattern{BSON(kOriginalShardKey << 1)};

    void setUp() override {
        ShardingMongodTestFixture::setUp();

        serverGlobalParams.clusterRole = ClusterRole::ShardServer;

        auto clusterId = OID::gen();
        ShardingState::get(getServiceContext())
            ->setInitialized(_sourceId.getShardId().toString(), clusterId);

        auto mockLoader = std::make_unique<CatalogCacheLoaderMock>();
        CatalogCacheLoader::set(getServiceContext(), std::move(mockLoader));

        uassertStatusOK(
            initializeGlobalShardingStateForMongodForTest(ConnectionString(kConfigHostAndPort)));

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());

        uassertStatusOK(createCollection(
            operationContext(),
            NamespaceString::kSessionTransactionsTableNamespace.db().toString(),
            BSON("create" << NamespaceString::kSessionTransactionsTableNamespace.coll())));
        uassertStatusOK(createCollection(operationContext(),
                                         kAppliedToNs.db().toString(),
                                         BSON("create" << kAppliedToNs.coll())));
        uassertStatusOK(createCollection(
            operationContext(), kStashNs.db().toString(), BSON("create" << kStashNs.coll())));
        uassertStatusOK(createCollection(operationContext(),
                                         kOtherDonorStashNs.db().toString(),
                                         BSON("create" << kOtherDonorStashNs.coll())));

        _cm = createChunkManagerForOriginalColl();
    }

    ChunkManager createChunkManagerForOriginalColl() {
        // Create three chunks, two that are owned by this donor shard and one owned by some other
        // shard. The chunk for {sk: null} is owned by this donor shard to allow test cases to omit
        // the shard key field when it isn't relevant.
        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks = {
            ChunkType{
                kCrudNs,
                ChunkRange{BSON(kOriginalShardKey << MINKEY),
                           BSON(kOriginalShardKey << -std::numeric_limits<double>::infinity())},
                ChunkVersion(1, 0, epoch, boost::none /* timestamp */),
                _sourceId.getShardId()},
            ChunkType{
                kCrudNs,
                ChunkRange{BSON(kOriginalShardKey << -std::numeric_limits<double>::infinity()),
                           BSON(kOriginalShardKey << 0)},
                ChunkVersion(1, 0, epoch, boost::none /* timestamp */),
                kOtherShardId},
            ChunkType{kCrudNs,
                      ChunkRange{BSON(kOriginalShardKey << 0), BSON(kOriginalShardKey << MAXKEY)},
                      ChunkVersion(1, 0, epoch, boost::none /* timestamp */),
                      _sourceId.getShardId()}};

        auto rt = RoutingTableHistory::makeNew(kCrudNs,
                                               kCrudUUID,
                                               kOriginalShardKeyPattern,
                                               nullptr,
                                               false,
                                               epoch,
                                               boost::none /* timestamp */,
                                               boost::none,
                                               false,
                                               chunks);

        return ChunkManager(_sourceId.getShardId(),
                            DatabaseVersion(UUID::gen()),
                            makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none);
    }

    repl::OplogEntry makeOplog(const repl::OpTime& opTime,
                               repl::OpTypeEnum opType,
                               const BSONObj& obj1,
                               const boost::optional<BSONObj> obj2) {
        return makeOplog(opTime, opType, obj1, obj2, {}, boost::none);
    }

    repl::OplogEntry makeOplog(const repl::OpTime& opTime,
                               repl::OpTypeEnum opType,
                               const BSONObj& obj1,
                               const boost::optional<BSONObj> obj2,
                               const OperationSessionInfo& sessionInfo,
                               const boost::optional<StmtId>& statementId) {
        ReshardingDonorOplogId id(opTime.getTimestamp(), opTime.getTimestamp());
        return {repl::DurableOplogEntry(opTime,
                                        boost::none /* hash */,
                                        opType,
                                        kCrudNs,
                                        kCrudUUID,
                                        false /* fromMigrate */,
                                        0 /* version */,
                                        obj1,
                                        obj2,
                                        sessionInfo,
                                        boost::none /* upsert */,
                                        {} /* date */,
                                        statementId,
                                        boost::none /* prevWrite */,
                                        boost::none /* preImage */,
                                        boost::none /* postImage */,
                                        kMyShardId,
                                        Value(id.toBSON()))};
    }

    const NamespaceString& oplogNs() {
        return kOplogNs;
    }

    const NamespaceString& crudNs() {
        return kCrudNs;
    }

    const UUID& crudUUID() {
        return kCrudUUID;
    }

    const NamespaceString& appliedToNs() {
        return kAppliedToNs;
    }

    const NamespaceString& stashNs() {
        return kStashNs;
    }

    const ReshardingSourceId& sourceId() {
        return _sourceId;
    }

    const ChunkManager& chunkManager() {
        return _cm.get();
    }

    const std::vector<NamespaceString>& stashCollections() {
        return kStashCollections;
    }

protected:
    std::shared_ptr<executor::ThreadPoolTaskExecutor> makeTaskExecutorForApplier() {
        auto executor = executor::makeThreadPoolTestExecutor(
            std::make_unique<executor::NetworkInterfaceMock>());

        executor->startup();
        return executor;
    }

    static constexpr int kWriterPoolSize = 4;
    const NamespaceString kOplogNs{"config.localReshardingOplogBuffer.xxx.yyy"};
    const NamespaceString kCrudNs{"foo.bar"};
    const UUID kCrudUUID = UUID::gen();
    const NamespaceString kAppliedToNs{"foo", "system.resharding.{}"_format(kCrudUUID.toString())};
    const NamespaceString kStashNs{"foo", "{}.{}"_format(kCrudNs.coll(), kOplogNs.coll())};
    const NamespaceString kOtherDonorStashNs{"foo", "{}.{}"_format("otherstash", "otheroplog")};
    const std::vector<NamespaceString> kStashCollections{kStashNs, kOtherDonorStashNs};
    const ShardId kMyShardId{"shard1"};
    const ShardId kOtherShardId{"shard2"};
    UUID _crudNsUuid = UUID::gen();
    boost::optional<ChunkManager> _cm;

    const ReshardingSourceId _sourceId{UUID::gen(), kMyShardId};
};

TEST_F(ReshardingOplogApplierTest, NothingToIterate) {
    std::deque<repl::OplogEntry> crudOps;
    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);

    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();
}

TEST_F(ReshardingOplogApplierTest, ApplyBasicCrud) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kUpdate,
                                BSON("$set" << BSON("x" << 1)),
                                BSON("_id" << 2)));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                repl::OpTypeEnum::kDelete,
                                BSON("_id" << 1),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    future = applier->applyUntilDone();
    future.get();

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "x" << 1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, InsertTypeOplogAppliedInMultipleBatches) {
    std::deque<repl::OplogEntry> crudOps;

    for (int x = 0; x < 20; x++) {
        crudOps.push_back(makeOplog(repl::OpTime(Timestamp(x, 3), 1),
                                    repl::OpTypeEnum::kInsert,
                                    BSON("_id" << x),
                                    boost::none));
    }

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 3 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(8, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    DBDirectClient client(operationContext());

    for (int x = 0; x < 9; x++) {
        auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << x));
        ASSERT_BSONOBJ_EQ(BSON("_id" << x), doc);
    }

    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 9));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());

    future = applier->applyUntilDone();
    future.get();

    for (int x = 0; x < 19; x++) {
        doc = client.findOne(appliedToNs().ns(), BSON("_id" << x));
        ASSERT_BSONOBJ_EQ(BSON("_id" << x), doc);
    }

    progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(19, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(19, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ErrorDuringBatchApplyCloningPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kUpdate,
                                BSON("$invalidOperator" << BSON("x" << 1)),
                                BSON("_id" << 1)));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 4 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(7, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::FailedToParse);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ErrorDuringBatchApplyCatchUpPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 3),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                repl::OpTypeEnum::kUpdate,
                                BSON("$invalidOperator" << BSON("x" << 1)),
                                BSON("_id" << 1)));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::FailedToParse);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingFirstOplogCloningPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    iterator->setThrowWhenSingleItem();

    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingFirstOplogCatchUpPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 3),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    iterator->setThrowWhenSingleItem();

    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingFirstBatchCloningPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 4 /* batchSize */);
    iterator->setThrowWhenSingleItem();

    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(8, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingFirstBatchCatchUpPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none));

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 3),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none));
    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    iterator->setThrowWhenSingleItem();

    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingSecondBatchCloningPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 3),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    iterator->setThrowWhenSingleItem();

    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(7, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingSecondBatchCatchUpPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 3),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 4),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(9, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 5),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    iterator->setThrowWhenSingleItem();

    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 4));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 4), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 5));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, ExecutorIsShutDownCloningPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 4 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    executor->shutdown();

    auto future = applier->applyUntilCloneFinishedTs();
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::ShutdownInProgress);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ExecutorIsShutDownCatchUpPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 3),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    executor->shutdown();
    future = applier->applyUntilDone();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::ShutdownInProgress);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, WriterPoolIsShutDownCloningPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 4 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    writerPool->shutdown();

    auto future = applier->applyUntilCloneFinishedTs();
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::ShutdownInProgress);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, WriterPoolIsShutDownCatchUpPhase) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 3),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    writerPool->shutdown();
    future = applier->applyUntilDone();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::ShutdownInProgress);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, InsertOpIntoOuputCollectionUseReshardingApplicationRules) {
    // This case tests applying rule #2 described in
    // ReshardingOplogApplicationRules::_applyInsert_inlock.
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 3),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 4),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    future = applier->applyUntilDone();
    future.get();

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 4));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 4), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest,
       InsertOpShouldTurnIntoReplacementUpdateOnOutputCollectionUseReshardingApplicationRules) {
    // This case tests applying rule #3 described in
    // ReshardingOplogApplicationRules::_applyInsert_inlock.
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1 << "sk" << 2),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    // Make sure a doc with {_id: 1} exists in the output collection before applying an insert with
    // the same _id. This donor shard owns these docs under the original shard key (it owns the
    // range {sk: 0} -> {sk: maxKey}).
    DBDirectClient client(operationContext());
    client.insert(appliedToNs().toString(), BSON("_id" << 1 << "sk" << 1));

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    // We should have replaced the existing doc in the output collection.
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << 2), doc);

    future = applier->applyUntilDone();
    future.get();

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(5, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(5, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest,
       InsertOpShouldWriteToStashCollectionUseReshardingApplicationRules) {
    // This case tests applying rules #1 and #4 described in
    // ReshardingOplogApplicationRules::_applyInsert_inlock.
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1 << "sk" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1 << "sk" << 3),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    // Make sure a doc with {_id: 1} exists in the output collection before applying inserts with
    // the same _id. This donor shard does not own the doc {_id: 1, sk: -1} under the original shard
    // key, so we should apply rule #4 and insert the doc into the stash collection.
    DBDirectClient client(operationContext());
    client.insert(appliedToNs().toString(), BSON("_id" << 1 << "sk" << -1));

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    // The output collection should still hold the doc {_id: 1, sk: -1}, and the doc with {_id: 1,
    // sk: 2} should have been inserted into the stash collection.
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -1), doc);

    doc = client.findOne(stashNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << 2), doc);

    future = applier->applyUntilDone();
    future.get();

    // The output collection should still hold the doc {_id: 1, x: 1}. We should have applied rule
    // #1 and turned the last insert op into a replacement update on the stash collection, so the
    // doc {_id: 1, x: 3} should now exist in the stash collection.
    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -1), doc);

    doc = client.findOne(stashNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << 3), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest,
       DeleteFromOutputCollNonEmptyStashCollForThisDonorUseReshardingApplicationRules) {
    // This case tests applying rule #1 described in
    // ReshardingOplogApplicationRules::_applyDelete_inlock.
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1 << "sk" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kDelete,
                                BSON("_id" << 1),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    // Insert a doc {_id: 1} in the output collection before applying the insert of doc with
    // {_id: 1}. This will force the doc {_id: 1, sk: 1} to be inserted to the stash collection for
    // this donor shard.
    DBDirectClient client(operationContext());
    client.insert(appliedToNs().toString(), BSON("_id" << 1 << "sk" << -1));

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    // The output collection should still hold the doc {_id: 1, sk: -1}, and the doc with {_id: 1,
    // sk: 2} should have been inserted into the stash collection.
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -1), doc);

    doc = client.findOne(stashNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << 2), doc);

    future = applier->applyUntilDone();
    future.get();

    // We should have applied rule #1 and deleted the doc with {_id : 1} from the stash collection
    // for this donor.
    doc = client.findOne(stashNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    // The output collection should remain unchanged.
    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest,
       DeleteFromOutputCollShouldDoNothingUseReshardingApplicationRules) {
    // This case tests applying rules #2 and #3 described in
    // ReshardingOplogApplicationRules::_applyDelete_inlock.
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kDelete,
                                BSON("_id" << 1 << "sk" << -1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kDelete,
                                BSON("_id" << 2),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    // Make sure a doc with {_id: 1} exists in the output collection that does not belong to this
    // donor shard before applying the deletes.
    DBDirectClient client(operationContext());
    client.insert(appliedToNs().ns(), BSON("_id" << 1 << "sk" << -1));

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    // The doc {_id: 1, sk: -1} that exists in the output collection does not belong to this donor
    // shard, so we should have applied rule #3 and done nothing and the doc should still be in the
    // output collection.
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -1), doc);

    future = applier->applyUntilDone();
    future.get();

    // There does not exist a doc with {_id : 2} in the output collection, so we should have applied
    // rule #2 and done nothing.
    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    // The doc with {_id: 1, sk: -1} should still exist.
    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest,
       DeleteFromOutputCollAllStashCollsEmptyUseReshardingApplicationRules) {
    // This case tests applying rule #4 described in
    // ReshardingOplogApplicationRules::_applyDelete_inlock.
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1 << "sk" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2 << "sk" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kDelete,
                                BSON("_id" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                repl::OpTypeEnum::kDelete,
                                BSON("_id" << 2),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    // Apply the inserts first so there exists docs in the output collection
    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "sk" << 2), doc);

    future = applier->applyUntilDone();
    future.get();

    // None of the stash collections have docs with _id == [op _id], so we should not have found any
    // docs to insert into the output collection with either {_id : 1} or {_id : 2}.
    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());

    // Assert that the delete on the output collection was run in a transaction by looking in the
    // oplog for an applyOps entry with a "d" op on 'appliedToNs'.
    doc = client.findOne(NamespaceString::kRsOplogNamespace.ns(),
                         BSON("o.applyOps.0.op"
                              << "d"
                              << "o.applyOps.0.ns" << appliedToNs().ns()));
    ASSERT(!doc.isEmpty());
}

TEST_F(ReshardingOplogApplierTest,
       DeleteFromOutputCollNonEmptyStashCollForOtherDonorUseReshardingApplicationRules) {
    // This case tests applying rule #4 described in
    // ReshardingOplogApplicationRules::_applyDelete_inlock.
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1 << "sk" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kDelete,
                                BSON("_id" << 1),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    // Make sure a doc with {_id: 1} exists in the stash collection for the other donor shard. The
    // stash collection for this donor shard is empty.
    DBDirectClient client(operationContext());
    client.insert(stashCollections()[1].toString(), BSON("_id" << 1 << "sk" << -3));

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    // The output collection should now hold the doc {_id: 1, sk: 1}.
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << 1), doc);

    // The stash collection for this donor shard still should be empty.
    doc = client.findOne(stashNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    // The stash collection for the other donor shard should still hold the doc {_id: 1, sk: -3}.
    doc = client.findOne(stashCollections()[1].toString(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -3), doc);

    future = applier->applyUntilDone();
    future.get();

    // We should have applied rule #4 and deleted the doc that was in the output collection {_id: 1,
    // sk: 1}, deleted the doc with the same _id {_id: 1, sk: -3} in the other donor shard's stash
    // collection and inserted this doc into the output collection.
    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -3), doc);

    // This donor shard's stash collection should remain empty.
    doc = client.findOne(stashNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    // The other donor shard's stash collection should now be empty.
    doc = client.findOne(stashCollections()[1].toString(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());

    // Assert that the delete on the output collection was run in a transaction by looking in the
    // oplog for an applyOps entry with the following ops: ["d" op on 'appliedToNs', "d" on
    // 'otherStashNs'. "i" on 'appliedToNs'].
    doc = client.findOne(NamespaceString::kRsOplogNamespace.ns(),
                         BSON("o.applyOps.0.op"
                              << "d"
                              << "o.applyOps.0.ns" << appliedToNs().ns() << "o.applyOps.1.op"
                              << "d"
                              << "o.applyOps.1.ns" << stashCollections()[1].toString()
                              << "o.applyOps.2.op"
                              << "i"
                              << "o.applyOps.2.ns" << appliedToNs().ns()));
    ASSERT(!doc.isEmpty());
}

TEST_F(ReshardingOplogApplierTest, UpdateShouldModifyStashCollectionUseReshardingApplicationRules) {
    // This case tests applying rule #1 described in
    // ReshardingOplogApplicationRules::_applyUpdate_inlock.
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1 << "sk" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kUpdate,
                                BSON("$set" << BSON("x" << 1)),
                                BSON("_id" << 1)));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    // Insert a doc {_id: 1} in the output collection before applying the insert of doc with
    // {_id: 1}. This will force the doc {_id: 1, sk: 2} to be inserted to the stash collection for
    // this donor shard.
    DBDirectClient client(operationContext());
    client.insert(appliedToNs().toString(), BSON("_id" << 1 << "sk" << -1));

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    // The output collection should still hold the doc {_id: 1, sk: -1}, and the doc with {_id: 1,
    // sk: 2} should have been inserted into the stash collection.
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -1), doc);

    doc = client.findOne(stashNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << 2), doc);

    future = applier->applyUntilDone();
    future.get();

    // We should have applied rule #1 and updated the doc with {_id : 1} in the stash collection
    // for this donor to now have the new field 'x'.
    doc = client.findOne(stashNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << 2 << "x" << 1), doc);

    // The output collection should remain unchanged.
    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, UpdateShouldDoNothingUseReshardingApplicationRules) {
    // This case tests applying rules #2 and #3 described in
    // ReshardingOplogApplicationRules::_applyUpdate_inlock.
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kUpdate,
                                BSON("$set" << BSON("x" << 1)),
                                BSON("_id" << 1)));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kUpdate,
                                BSON("$set" << BSON("x" << 2)),
                                BSON("_id" << 2)));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    // Make sure a doc with {_id: 1} exists in the output collection that does not belong to this
    // donor shard before applying the updates.
    DBDirectClient client(operationContext());
    client.insert(appliedToNs().ns(), BSON("_id" << 1 << "sk" << -1));

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    // The doc {_id: 1, sk: -1} that exists in the output collection does not belong to this donor
    // shard, so we should have applied rule #3 and done nothing and the doc should still be in the
    // output collection.
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -1), doc);

    future = applier->applyUntilDone();
    future.get();

    // There does not exist a doc with {_id : 2} in the output collection, so we should have applied
    // rule #2 and done nothing.
    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    // The doc with {_id: 1, sk: -1} should still exist.
    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << -1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, UpdateOutputCollUseReshardingApplicationRules) {
    // This case tests applying rule #4 described in
    // ReshardingOplogApplicationRules::_applyUpdate_inlock.
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1 << "sk" << 1),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2 << "sk" << 2),
                                boost::none));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kUpdate,
                                BSON("$set" << BSON("x" << 1)),
                                BSON("_id" << 1)));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                repl::OpTypeEnum::kUpdate,
                                BSON("$set" << BSON("x" << 2)),
                                BSON("_id" << 2)));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    // Apply the inserts first so there exists docs in the output collection.
    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "sk" << 2), doc);

    future = applier->applyUntilDone();
    future.get();

    // We should have updated both docs in the output collection to include the new field "x".
    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "sk" << 1 << "x" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "sk" << 2 << "x" << 2), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest, UnsupportedCommandOpsShouldErrorUseReshardingApplicationRules) {
    std::deque<repl::OplogEntry> ops;
    ops.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                            repl::OpTypeEnum::kInsert,
                            BSON("_id" << 1),
                            boost::none));
    ops.push_back(
        makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                  repl::OpTypeEnum::kCommand,
                  BSON("renameCollection" << appliedToNs().ns() << "to" << stashNs().ns()),
                  boost::none));
    ops.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                            repl::OpTypeEnum::kInsert,
                            BSON("_id" << 2),
                            boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(ops), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    future = applier->applyUntilDone();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::OplogOperationUnsupported);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(5, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(5, 3), progressDoc->getProgress().getTs());
}

TEST_F(ReshardingOplogApplierTest,
       DropSourceCollectionCmdShouldErrorUseReshardingApplicationRules) {
    std::deque<repl::OplogEntry> ops;
    ops.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                            repl::OpTypeEnum::kCommand,
                            BSON("drop" << appliedToNs().ns()),
                            boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(ops), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(5, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();

    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::OplogOperationUnsupported);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

class ReshardingOplogApplierRetryableTest : public ReshardingOplogApplierTest {
public:
    void setUp() override {
        ReshardingOplogApplierTest::setUp();

        repl::StorageInterface::set(operationContext()->getServiceContext(),
                                    std::make_unique<repl::StorageInterfaceImpl>());
        MongoDSessionCatalog::onStepUp(operationContext());

        // Normally, committing a transaction is supposed to uassert if the corresponding prepare
        // has not been majority committed. We exempt our unit tests from this expectation.
        setGlobalFailPoint("skipCommitTxnCheckPrepareMajorityCommitted",
                           BSON("mode"
                                << "alwaysOn"));
    }

    void tearDown() override {
        // Clear all sessions to free up any stashed resources.
        SessionCatalog::get(operationContext()->getServiceContext())->reset_forTest();

        setGlobalFailPoint("skipCommitTxnCheckPrepareMajorityCommitted",
                           BSON("mode"
                                << "off"));

        ReshardingOplogApplierTest::tearDown();
    }

    static repl::OpTime insertRetryableOplog(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             UUID uuid,
                                             const LogicalSessionId& lsid,
                                             TxnNumber txnNumber,
                                             StmtId stmtId,
                                             repl::OpTime prevOpTime) {
        repl::MutableOplogEntry oplogEntry;
        oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
        oplogEntry.setNss(nss);
        oplogEntry.setUuid(uuid);
        oplogEntry.setObject(BSON("TestValue" << 0));
        oplogEntry.setWallClockTime(Date_t::now());
        if (stmtId != kUninitializedStmtId) {
            oplogEntry.setSessionId(lsid);
            oplogEntry.setTxnNumber(txnNumber);
            oplogEntry.setStatementId(stmtId);
            oplogEntry.setPrevWriteOpTimeInTransaction(prevOpTime);
        }
        return repl::logOp(opCtx, &oplogEntry);
    }

    void writeTxnRecord(const LogicalSessionId& lsid,
                        const TxnNumber& txnNum,
                        StmtId stmtId,
                        repl::OpTime prevOpTime,
                        boost::optional<DurableTxnStateEnum> txnState) {
        auto newClient = operationContext()->getServiceContext()->makeClient("testWriteTxnRecord");
        AlternativeClientRegion acr(newClient);
        auto scopedOpCtx = cc().makeOperationContext();
        auto opCtx = scopedOpCtx.get();

        opCtx->setLogicalSessionId(lsid);
        opCtx->setTxnNumber(txnNum);
        OperationContextSession scopedSession(opCtx);

        const auto session = OperationContextSession::get(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.refreshFromStorageIfNeeded(opCtx);
        txnParticipant.beginOrContinue(opCtx, txnNum, boost::none, boost::none);

        AutoGetCollection autoColl(opCtx, kCrudNs, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        const auto opTime = insertRetryableOplog(
            opCtx, kCrudNs, kCrudUUID, session->getSessionId(), txnNum, stmtId, prevOpTime);

        SessionTxnRecord sessionTxnRecord;
        sessionTxnRecord.setSessionId(session->getSessionId());
        sessionTxnRecord.setTxnNum(txnNum);
        sessionTxnRecord.setLastWriteOpTime(opTime);
        sessionTxnRecord.setLastWriteDate(Date_t::now());
        sessionTxnRecord.setState(txnState);
        txnParticipant.onWriteOpCompletedOnPrimary(opCtx, {stmtId}, sessionTxnRecord);
        wuow.commit();
    }

    bool isWriteAlreadyExecuted(const OperationSessionInfo& session, StmtId stmtId) {
        return checkWriteAlreadyExecuted(session, stmtId).is_initialized();
    }

    boost::optional<repl::OplogEntry> checkWriteAlreadyExecuted(const OperationSessionInfo& session,
                                                                StmtId stmtId) {
        auto newClient =
            operationContext()->getServiceContext()->makeClient("testCheckStmtExecuted");
        AlternativeClientRegion acr(newClient);
        auto scopedOpCtx = cc().makeOperationContext();
        auto opCtx = scopedOpCtx.get();

        opCtx->setLogicalSessionId(*session.getSessionId());
        OperationContextSession scopedSession(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.refreshFromStorageIfNeeded(opCtx);
        txnParticipant.beginOrContinue(opCtx, *session.getTxnNumber(), boost::none, boost::none);

        return txnParticipant.checkStatementExecuted(opCtx, stmtId);
    }

    /**
     * Checkout the transaction participant for inspection.
     */
    void checkOutTxnParticipant(
        const OperationSessionInfo& session,
        std::function<void(const TransactionParticipant::Participant&)> checkFn) {
        auto newClient =
            operationContext()->getServiceContext()->makeClient("testCheckStmtExecuted");
        AlternativeClientRegion acr(newClient);
        auto scopedOpCtx = cc().makeOperationContext();
        auto opCtx = scopedOpCtx.get();

        opCtx->setLogicalSessionId(*session.getSessionId());
        OperationContextSession scopedSession(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.refreshFromStorageIfNeeded(opCtx);

        checkFn(txnParticipant);
    }

    /**
     * Extract the pre or post image document for the oplog operation if it exists.
     */
    BSONObj extractPreOrPostImage(const repl::OplogEntry& oplog) {
        repl::OpTime opTime;

        if (oplog.getPreImageOpTime()) {
            opTime = oplog.getPreImageOpTime().value();
        } else if (oplog.getPostImageOpTime()) {
            opTime = oplog.getPostImageOpTime().value();
        } else {
            return {};
        }

        DBDirectClient client(operationContext());
        auto oplogDoc =
            client.findOne(NamespaceString::kRsOplogNamespace.ns(), opTime.asQuery(), nullptr);

        if (oplogDoc.isEmpty()) {
            return {};
        }

        auto oplogEntry = uassertStatusOK(repl::OplogEntry::parse(oplogDoc));
        return oplogEntry.getObject().getOwned();
    }

    /**
     * Start a transaction with lsid, txnNum and stash the resources.
     */
    void makeUnpreparedTransaction(OperationContext* opCtx,
                                   LogicalSessionId lsid,
                                   TxnNumber txnNum) {
        // Note: marking opCtx in txn mode is permanent.
        auto newClient = opCtx->getServiceContext()->makeClient("testWriteTxnRecord");
        AlternativeClientRegion acr(newClient);
        auto scopedOpCtx = cc().makeOperationContext();
        auto innerOpCtx = scopedOpCtx.get();
        innerOpCtx->setLogicalSessionId(lsid);
        innerOpCtx->setTxnNumber(txnNum);
        innerOpCtx->setInMultiDocumentTransaction();

        OperationContextSession scopedSession(innerOpCtx);

        auto txnParticipant = TransactionParticipant::get(innerOpCtx);
        txnParticipant.refreshFromStorageIfNeeded(innerOpCtx);
        txnParticipant.beginOrContinue(innerOpCtx, txnNum, false, true);
        txnParticipant.unstashTransactionResources(innerOpCtx, "insert");

        WriteUnitOfWork wuow(innerOpCtx);

        // The transaction machinery cannot store an empty locker.
        {
            Lock::GlobalLock lk(
                innerOpCtx, MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
        }

        auto operation =
            repl::DurableOplogEntry::makeInsertOperation(crudNs(), crudUUID(), BSON("x" << 20));
        txnParticipant.addTransactionOperation(innerOpCtx, operation);
        wuow.commit();

        txnParticipant.stashTransactionResources(innerOpCtx);
    }

    /**
     * Make transaction participant with lsid, txnNum go into prepared state with no ops. Returns
     * the prepare timestamp.
     */
    Timestamp prepareWithEmptyTransaction(OperationContext* opCtx,
                                          LogicalSessionId lsid,
                                          TxnNumber txnNum) {
        // Note: marking opCtx in txn mode is permanent.
        auto newClient = opCtx->getServiceContext()->makeClient("testWriteTxnRecord");
        AlternativeClientRegion acr(newClient);
        auto scopedOpCtx = cc().makeOperationContext();
        auto innerOpCtx = scopedOpCtx.get();

        innerOpCtx->setLogicalSessionId(lsid);
        innerOpCtx->setTxnNumber(txnNum);
        innerOpCtx->setInMultiDocumentTransaction();

        OperationContextSession scopedSession(innerOpCtx);

        auto txnParticipant = TransactionParticipant::get(innerOpCtx);
        txnParticipant.refreshFromStorageIfNeeded(innerOpCtx);
        txnParticipant.beginOrContinue(innerOpCtx, txnNum, false, true);
        txnParticipant.unstashTransactionResources(innerOpCtx, "prepareTransaction");

        // The transaction machinery cannot store an empty locker.
        {
            Lock::GlobalLock lk(
                innerOpCtx, MODE_IX, Date_t::now(), Lock::InterruptBehavior::kThrow);
        }

        auto prepareTimestamp = txnParticipant.prepareTransaction(innerOpCtx, {});
        txnParticipant.stashTransactionResources(innerOpCtx);
        return Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);
    }

    void commitPreparedTxn(OperationContext* opCtx,
                           LogicalSessionId lsid,
                           TxnNumber txnNum,
                           Timestamp commitTs) {
        // Note: marking opCtx in txn mode is permanent.
        auto newClient = operationContext()->getServiceContext()->makeClient("commitTxn");
        AlternativeClientRegion acr(newClient);
        auto scopedOpCtx = cc().makeOperationContext();
        auto innerOpCtx = scopedOpCtx.get();

        innerOpCtx->setLogicalSessionId(lsid);
        innerOpCtx->setTxnNumber(txnNum);
        innerOpCtx->setInMultiDocumentTransaction();

        OperationContextSession scopedSession(innerOpCtx);

        auto txnParticipant = TransactionParticipant::get(innerOpCtx);
        txnParticipant.beginOrContinue(innerOpCtx, txnNum, false, boost::none);

        txnParticipant.unstashTransactionResources(innerOpCtx, "commitTransaction");
        txnParticipant.commitPreparedTransaction(innerOpCtx, commitTs, boost::none);
    }

    void abortTxn(OperationContext* opCtx, LogicalSessionId lsid, TxnNumber txnNum) {
        // Note: marking opCtx in txn mode is permanent.
        auto newClient = operationContext()->getServiceContext()->makeClient("abortTxn");
        AlternativeClientRegion acr(newClient);
        auto scopedOpCtx = cc().makeOperationContext();
        auto innerOpCtx = scopedOpCtx.get();

        innerOpCtx->setLogicalSessionId(lsid);
        innerOpCtx->setTxnNumber(txnNum);
        innerOpCtx->setInMultiDocumentTransaction();

        OperationContextSession scopedSession(innerOpCtx);

        auto txnParticipant = TransactionParticipant::get(innerOpCtx);
        txnParticipant.beginOrContinue(innerOpCtx, txnNum, false, boost::none);

        txnParticipant.unstashTransactionResources(innerOpCtx, "abortTransaction");
        txnParticipant.abortTransaction(innerOpCtx);
    }

    /**
     * Generate simple oplog entries and push them to crudOps.
     */
    void pushPrepareCommittedTxnInsertOp(std::deque<repl::OplogEntry>* crudOps,
                                         const OperationSessionInfo& session,
                                         const repl::OpTime prepareOptime,
                                         BSONObj doc) {
        auto applyOpsCmd = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                              << "i"
                                                              << "ns" << kCrudNs.ns() << "ui"
                                                              << kCrudUUID << "o" << doc))
                                           << "prepare" << true);

        crudOps->push_back(makeOplog(prepareOptime,
                                     repl::OpTypeEnum::kCommand,
                                     applyOpsCmd,
                                     boost::none,
                                     session,
                                     boost::none));

        repl::OpTime commitOptime(prepareOptime.getTimestamp() + 1, prepareOptime.getTerm());
        crudOps->push_back(makeOplog(commitOptime,
                                     repl::OpTypeEnum::kCommand,
                                     BSON("commitTransaction" << 1),
                                     boost::none,
                                     session,
                                     boost::none));
    }

    /**
     * Extract config.transaction documents the secondaries would have tried to replicate based from
     * the current oplog entries.
     *
     * Note: this will only capture secondary oplog application from derived ops and not direct
     * writes to config.transactions.
     */
    std::vector<BSONObj> extractProjectedTxnDocFromSecondary(OperationContext* opCtx) {
        SimpleBSONObjMap<BSONObj> lsidMap;
        repl::SessionUpdateTracker updateTracker;

        DBDirectClient client(opCtx);
        auto cursor = client.query(NamespaceString::kRsOplogNamespace, {});

        while (cursor->more()) {
            if (auto newUpdates = updateTracker.updateSession(cursor->next())) {
                for (const auto& updateOplog : *newUpdates) {
                    // Note: the updates are replacement style so the object field should contain
                    // the full doc.
                    auto txnReplacementDoc = updateOplog.getObject().getOwned();
                    auto _id = txnReplacementDoc["_id"].Obj().getOwned();

                    lsidMap[_id] = txnReplacementDoc;
                }
            }
        }

        auto remainingUpdates = updateTracker.flushAll();

        if (!remainingUpdates.empty()) {
            for (const auto& updateOplog : remainingUpdates) {
                auto txnReplacementDoc = updateOplog.getObject().getOwned();
                auto _id = txnReplacementDoc["_id"].Obj().getOwned();

                lsidMap[_id] = txnReplacementDoc;
            }
        }

        std::vector<BSONObj> txnDocs;
        for (const auto& mapEntry : lsidMap) {
            txnDocs.push_back(mapEntry.second);
        }

        return txnDocs;
    }

    /**
     * Checks to see if the secondary would replicate the config.transactions table correctly.
     *
     * See extractProjectedTxnDocFromSecondary for assumptions being made.
     */
    void checkSecondaryCanReplicateCorrectly() {
        auto opCtx = operationContext();
        auto secondaryTxnDocs = extractProjectedTxnDocFromSecondary(opCtx);

        DBDirectClient client(opCtx);
        const auto txnTableCount =
            client.count(NamespaceString::kSessionTransactionsTableNamespace);

        ASSERT_EQ(txnTableCount, secondaryTxnDocs.size())
            << dumpTxnTable() << ", " << toString(secondaryTxnDocs);

        for (auto&& secondaryTxnDoc : secondaryTxnDocs) {
            auto idField = secondaryTxnDoc["_id"].Obj();

            auto primaryTxnDoc = client.findOne(
                NamespaceString::kSessionTransactionsTableNamespace.ns(), BSON("_id" << idField));

            ASSERT_FALSE(primaryTxnDoc.isEmpty())
                << "secondary doc: " << secondaryTxnDoc << ", " << dumpTxnTable();

            ASSERT_BSONOBJ_EQ(primaryTxnDoc, secondaryTxnDoc);
        }
    }

    /**
     * Output the current contents of config.transactions into a string format.
     */
    std::string dumpTxnTable() {
        DBDirectClient client(operationContext());
        auto cursor = client.query(NamespaceString::kSessionTransactionsTableNamespace, {});

        if (!cursor->more()) {
            return "<no config.transaction entries>";
        }

        StringBuilder str;
        str << "config.transaction entries:";

        while (cursor->more()) {
            str << " txnDoc: " << cursor->next();
        }

        return str.str();
    }

    std::string toString(const std::vector<BSONObj>& txnDocs) {
        if (txnDocs.empty()) {
            return "<no txnDocs>";
        }

        StringBuilder str;
        str << "txnDocs:";

        for (const auto& doc : txnDocs) {
            str << " txnDocs: " << doc;
        }

        return str.str();
    }
};

TEST_F(ReshardingOplogApplierRetryableTest, GroupInserts) {
    std::deque<repl::OplogEntry> crudOps;

    OperationSessionInfo session;
    session.setSessionId(makeLogicalSessionIdForTest());
    session.setTxnNumber(1);

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                session,
                                1));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none,
                                session,
                                2));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 3),
                                boost::none,
                                session,
                                3));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 5 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();

    // Make the writer pool size 1 so all the inserts will be assigned to a single writer.
    auto writerPool = repl::makeReplWriterPool(1);
    ReshardingWriterVectorSizeBlock reshardingWriteVectorSizeForThisTest(1);

    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(1, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(7, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(7, 3), progressDoc->getProgress().getTs());

    ASSERT_TRUE(isWriteAlreadyExecuted(session, 1));
    ASSERT_TRUE(isWriteAlreadyExecuted(session, 2));
    ASSERT_TRUE(isWriteAlreadyExecuted(session, 3));

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, CrudWithEmptyConfigTransactions) {
    std::deque<repl::OplogEntry> crudOps;

    OperationSessionInfo session1;
    session1.setSessionId(makeLogicalSessionIdForTest());
    session1.setTxnNumber(1);

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                session1,
                                1));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none,
                                session1,
                                2));

    OperationSessionInfo session2;
    session2.setSessionId(makeLogicalSessionIdForTest());
    session2.setTxnNumber(1);

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kUpdate,
                                BSON("$set" << BSON("x" << 1)),
                                BSON("_id" << 2),
                                session2,
                                1));

    OperationSessionInfo session3;
    session3.setSessionId(makeLogicalSessionIdForTest());
    session3.setTxnNumber(1);

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                repl::OpTypeEnum::kDelete,
                                BSON("_id" << 1),
                                boost::none,
                                session3,
                                1));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "x" << 1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());

    ASSERT_TRUE(isWriteAlreadyExecuted(session1, 1));
    ASSERT_TRUE(isWriteAlreadyExecuted(session1, 2));
    ASSERT_TRUE(isWriteAlreadyExecuted(session2, 1));
    ASSERT_TRUE(isWriteAlreadyExecuted(session3, 1));

    ASSERT_FALSE(isWriteAlreadyExecuted(session2, 2));
    ASSERT_FALSE(isWriteAlreadyExecuted(session3, 2));

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, MultipleTxnSameLsidInOneBatch) {
    std::deque<repl::OplogEntry> crudOps;

    OperationSessionInfo session1;
    session1.setSessionId(makeLogicalSessionIdForTest());
    session1.setTxnNumber(1);

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                session1,
                                1));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 2),
                                boost::none,
                                session1,
                                2));

    OperationSessionInfo session2;
    session2.setSessionId(makeLogicalSessionIdForTest());
    session2.setTxnNumber(1);

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 3),
                                boost::none,
                                session2,
                                1));

    session1.setTxnNumber(2);

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 4),
                                boost::none,
                                session1,
                                21));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), doc);

    doc = client.findOne(appliedToNs().ns(), BSON("_id" << 4));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 4), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session1, 21));
    ASSERT_TRUE(isWriteAlreadyExecuted(session2, 1));

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithLowerExistingTxn) {
    auto lsid = makeLogicalSessionIdForTest();

    writeTxnRecord(lsid, 2, 1, {}, boost::none);

    std::deque<repl::OplogEntry> crudOps;

    OperationSessionInfo session;
    session.setSessionId(lsid);
    session.setTxnNumber(5);

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                session,
                                21));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session, 21));

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithHigherExistingTxnNum) {
    auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber existingTxnNum = 20;
    const StmtId existingStmtId = 1;
    writeTxnRecord(lsid, existingTxnNum, existingStmtId, {}, boost::none);

    OperationSessionInfo session;
    const TxnNumber incomingTxnNum = 15;
    const StmtId incomingStmtId = 21;
    session.setSessionId(lsid);
    session.setTxnNumber(incomingTxnNum);

    std::deque<repl::OplogEntry> crudOps;

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                session,
                                incomingStmtId));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    // Op should always be applied, even if session info was not compatible.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_THROWS_CODE(isWriteAlreadyExecuted(session, incomingStmtId),
                       DBException,
                       ErrorCodes::TransactionTooOld);

    // Check that original txn info is intact.
    OperationSessionInfo origSession;
    origSession.setSessionId(lsid);
    origSession.setTxnNumber(existingTxnNum);

    ASSERT_TRUE(isWriteAlreadyExecuted(origSession, existingStmtId));

    // Don't call checkSecondaryCanReplicateCorrectly since this test directly updates
    // config.transactions and won't generate derivedOps for secondaries.
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithEqualExistingTxnNum) {
    auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber existingTxnNum = 20;
    const StmtId existingStmtId = 1;
    writeTxnRecord(lsid, existingTxnNum, existingStmtId, {}, boost::none);

    OperationSessionInfo session;
    const TxnNumber incomingTxnNum = existingTxnNum;
    const StmtId incomingStmtId = 21;
    session.setSessionId(lsid);
    session.setTxnNumber(incomingTxnNum);

    std::deque<repl::OplogEntry> crudOps;

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                session,
                                incomingStmtId));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session, incomingStmtId));
    ASSERT_TRUE(isWriteAlreadyExecuted(session, existingStmtId));

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithStmtIdAlreadyExecuted) {
    auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber existingTxnNum = 20;
    const StmtId existingStmtId = 1;
    writeTxnRecord(lsid, existingTxnNum, existingStmtId, {}, boost::none);

    OperationSessionInfo session;
    const TxnNumber incomingTxnNum = existingTxnNum;
    const StmtId incomingStmtId = existingStmtId;
    session.setSessionId(lsid);
    session.setTxnNumber(incomingTxnNum);

    std::deque<repl::OplogEntry> crudOps;

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                session,
                                incomingStmtId));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session, incomingStmtId));

    // Don't call checkSecondaryCanReplicateCorrectly since this test directly updates
    // config.transactions and won't generate derivedOps for secondaries.
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithActiveUnpreparedTxnSameTxn) {
    const auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 1;

    makeUnpreparedTransaction(operationContext(), lsid, txnNum);

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    OperationSessionInfo session;
    session.setSessionId(lsid);
    session.setTxnNumber(txnNum);

    std::deque<repl::OplogEntry> crudOps;

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                session,
                                1));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    // Op should always be applied, even if session info was not compatible.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_THROWS_CODE(
        isWriteAlreadyExecuted(session, 1), DBException, ErrorCodes::IncompleteTransactionHistory);

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithActiveUnpreparedTxnWithLowerTxnNum) {
    const auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 10;

    makeUnpreparedTransaction(operationContext(), lsid, txnNum - 1);

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    OperationSessionInfo session;
    session.setSessionId(lsid);
    session.setTxnNumber(txnNum);

    std::deque<repl::OplogEntry> crudOps;

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                session,
                                1));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    // Op should always be applied, even if session info was not compatible.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session, 1));

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithPreparedTxnThatWillCommit) {
    const auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 10;

    auto commitTs = prepareWithEmptyTransaction(operationContext(), lsid, txnNum - 1);

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    OperationSessionInfo session;
    session.setSessionId(lsid);
    session.setTxnNumber(txnNum);

    std::deque<repl::OplogEntry> crudOps;

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                session,
                                1));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();

    // Sleep a little bit to make the applier block on the prepared transaction.
    sleepmillis(200);

    ASSERT_FALSE(future.isReady());

    commitPreparedTxn(operationContext(), lsid, txnNum - 1, commitTs);

    future.get();

    future = applier->applyUntilDone();
    future.get();

    // Op should always be applied, even if session info was not compatible.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session, 1));

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWithPreparedTxnThatWillAbort) {
    const auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 10;

    static_cast<void>(prepareWithEmptyTransaction(operationContext(), lsid, txnNum - 1));

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    OperationSessionInfo session;
    session.setSessionId(lsid);
    session.setTxnNumber(txnNum);

    std::deque<repl::OplogEntry> crudOps;

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                session,
                                1));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();

    // Sleep a little bit to make the applier block on the prepared transaction.
    sleepmillis(200);

    ASSERT_FALSE(future.isReady());

    abortTxn(operationContext(), lsid, txnNum - 1);

    future.get();

    future = applier->applyUntilDone();
    future.get();

    // Op should always be applied, even if session info was not compatible.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session, 1));

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWriteWithPreImage) {
    std::deque<repl::OplogEntry> crudOps;

    OperationSessionInfo session1;
    session1.setSessionId(makeLogicalSessionIdForTest());
    session1.setTxnNumber(1);

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                {},
                                boost::none));

    auto preImageOplog = makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                   repl::OpTypeEnum::kNoop,
                                   BSON("_id" << 1),
                                   boost::none,
                                   session1,
                                   boost::none);

    auto updateOplog = makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                 repl::OpTypeEnum::kUpdate,
                                 BSON("$set" << BSON("x" << 1)),
                                 BSON("_id" << 1),
                                 session1,
                                 1);
    updateOplog.setPreImageOp(std::make_shared<repl::DurableOplogEntry>(preImageOplog.getEntry()));
    crudOps.push_back(updateOplog);

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "x" << 1), doc);

    auto oplogEntry = checkWriteAlreadyExecuted(session1, 1);
    ASSERT_TRUE(oplogEntry);

    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), extractPreOrPostImage(*oplogEntry));

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, RetryableWriteWithPostImage) {
    std::deque<repl::OplogEntry> crudOps;

    OperationSessionInfo session1;
    session1.setSessionId(makeLogicalSessionIdForTest());
    session1.setTxnNumber(1);

    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none,
                                {},
                                boost::none));

    auto postImageOplog = makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                    repl::OpTypeEnum::kNoop,
                                    BSON("_id" << 1 << "x" << 1),
                                    boost::none,
                                    session1,
                                    boost::none);

    auto updateOplog = makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                 repl::OpTypeEnum::kUpdate,
                                 BSON("$set" << BSON("x" << 1)),
                                 BSON("_id" << 1),
                                 session1,
                                 1);
    updateOplog.setPostImageOp(
        std::make_shared<repl::DurableOplogEntry>(postImageOplog.getEntry()));
    crudOps.push_back(updateOplog);

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "x" << 1), doc);

    auto oplogEntry = checkWriteAlreadyExecuted(session1, 1);
    ASSERT_TRUE(oplogEntry);

    ASSERT_BSONOBJ_EQ(BSON("_id" << 1 << "x" << 1), extractPreOrPostImage(*oplogEntry));

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, ApplyTxnWithLowerExistingTxn) {
    auto lsid = makeLogicalSessionIdForTest();

    writeTxnRecord(lsid, 2, 1, {}, boost::none);

    std::deque<repl::OplogEntry> crudOps;

    OperationSessionInfo session;
    session.setSessionId(lsid);
    session.setTxnNumber(5);

    pushPrepareCommittedTxnInsertOp(
        &crudOps, session, repl::OpTime(Timestamp(5, 3), 1), BSON("_id" << 1));
    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);

    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_THROWS_CODE(
        isWriteAlreadyExecuted(session, 21), DBException, ErrorCodes::IncompleteTransactionHistory);

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, ApplyTxnWithHigherExistingTxnNum) {
    auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber existingTxnNum = 20;
    const StmtId existingStmtId = 1;
    writeTxnRecord(lsid, existingTxnNum, existingStmtId, {}, boost::none);

    OperationSessionInfo session;
    const TxnNumber incomingTxnNum = 15;
    const StmtId incomingStmtId = 21;
    session.setSessionId(lsid);
    session.setTxnNumber(incomingTxnNum);

    std::deque<repl::OplogEntry> crudOps;

    pushPrepareCommittedTxnInsertOp(
        &crudOps, session, repl::OpTime(Timestamp(5, 3), 1), BSON("_id" << 1));
    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);

    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    // Op should always be applied, even if txn info was not compatible.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_THROWS_CODE(isWriteAlreadyExecuted(session, incomingStmtId),
                       DBException,
                       ErrorCodes::TransactionTooOld);

    // Check that original txn info is intact.
    OperationSessionInfo origSession;
    origSession.setSessionId(lsid);
    origSession.setTxnNumber(existingTxnNum);

    ASSERT_TRUE(isWriteAlreadyExecuted(origSession, existingStmtId));

    // Don't call checkSecondaryCanReplicateCorrectly since this test directly updates
    // config.transactions and won't generate derivedOps for secondaries.
}

TEST_F(ReshardingOplogApplierRetryableTest, ApplyTxnWithEqualExistingTxnNum) {
    auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber existingTxnNum = 20;
    const StmtId existingStmtId = 1;
    writeTxnRecord(lsid, existingTxnNum, existingStmtId, {}, boost::none);

    OperationSessionInfo session;
    const TxnNumber incomingTxnNum = existingTxnNum;
    session.setSessionId(lsid);
    session.setTxnNumber(incomingTxnNum);

    std::deque<repl::OplogEntry> crudOps;

    pushPrepareCommittedTxnInsertOp(
        &crudOps, session, repl::OpTime(Timestamp(5, 3), 1), BSON("_id" << 1));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_TRUE(isWriteAlreadyExecuted(session, existingStmtId));
    ASSERT_THROWS_CODE(
        isWriteAlreadyExecuted(session, 21), DBException, ErrorCodes::IncompleteTransactionHistory);

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, ApplyTxnWithActiveUnpreparedTxnSameTxn) {
    const auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 1;

    makeUnpreparedTransaction(operationContext(), lsid, txnNum);

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    OperationSessionInfo session;
    session.setSessionId(lsid);
    session.setTxnNumber(txnNum);

    std::deque<repl::OplogEntry> crudOps;

    pushPrepareCommittedTxnInsertOp(
        &crudOps, session, repl::OpTime(Timestamp(5, 3), 1), BSON("_id" << 1));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    // Ops should always be applied regardless of conflict with existing txn.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_THROWS_CODE(
        isWriteAlreadyExecuted(session, 1), DBException, ErrorCodes::IncompleteTransactionHistory);

    checkOutTxnParticipant(session, [](const TransactionParticipant::Participant& participant) {
        ASSERT_TRUE(participant.transactionIsInProgress());
    });

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, ApplyTxnActiveUnpreparedTxnWithLowerTxnNum) {
    const auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 10;

    makeUnpreparedTransaction(operationContext(), lsid, txnNum - 1);

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    OperationSessionInfo session;
    session.setSessionId(lsid);
    session.setTxnNumber(txnNum);

    std::deque<repl::OplogEntry> crudOps;

    pushPrepareCommittedTxnInsertOp(
        &crudOps, session, repl::OpTime(Timestamp(5, 3), 1), BSON("_id" << 1));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();
    future.get();

    future = applier->applyUntilDone();
    future.get();

    // Op should always be applied, even if txn info was not compatible.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_THROWS_CODE(
        isWriteAlreadyExecuted(session, 1), DBException, ErrorCodes::IncompleteTransactionHistory);

    checkOutTxnParticipant(session, [](const TransactionParticipant::Participant& participant) {
        ASSERT_FALSE(participant.transactionIsInProgress());
    });

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, ApplyTxnWithPreparedTxnThatWillCommit) {
    const auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 10;

    auto commitTs = prepareWithEmptyTransaction(operationContext(), lsid, txnNum - 1);

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    OperationSessionInfo session;
    session.setSessionId(lsid);
    session.setTxnNumber(txnNum);

    std::deque<repl::OplogEntry> crudOps;
    pushPrepareCommittedTxnInsertOp(
        &crudOps, session, repl::OpTime(Timestamp(5, 3), 1), BSON("_id" << 1));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();

    // Sleep a little bit to make the applier block on the prepared transaction.
    sleepmillis(200);

    ASSERT_FALSE(future.isReady());

    commitPreparedTxn(operationContext(), lsid, txnNum - 1, commitTs);

    future.get();

    future = applier->applyUntilDone();
    future.get();

    // Op should always be applied, even if txn info was not compatible.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_THROWS_CODE(
        isWriteAlreadyExecuted(session, 1), DBException, ErrorCodes::IncompleteTransactionHistory);

    checkSecondaryCanReplicateCorrectly();
}

TEST_F(ReshardingOplogApplierRetryableTest, ApplyTxnWithPreparedTxnThatWillAbort) {
    const auto lsid = makeLogicalSessionIdForTest();
    const TxnNumber txnNum = 10;

    static_cast<void>(prepareWithEmptyTransaction(operationContext(), lsid, txnNum - 1));

    auto opCtx = operationContext();
    opCtx->setLogicalSessionId(lsid);
    opCtx->setTxnNumber(txnNum);

    OperationSessionInfo session;
    session.setSessionId(lsid);
    session.setTxnNumber(txnNum);

    std::deque<repl::OplogEntry> crudOps;
    pushPrepareCommittedTxnInsertOp(
        &crudOps, session, repl::OpTime(Timestamp(5, 3), 1), BSON("_id" << 1));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    auto executor = makeTaskExecutorForApplier();
    auto writerPool = repl::makeReplWriterPool(kWriterPoolSize);
    applier.emplace(getServiceContext(),
                    sourceId(),
                    oplogNs(),
                    crudNs(),
                    crudUUID(),
                    stashCollections(),
                    0U, /* myStashIdx */
                    Timestamp(6, 3),
                    std::move(iterator),
                    chunkManager(),
                    executor,
                    writerPool.get());

    auto future = applier->applyUntilCloneFinishedTs();

    // Sleep a little bit to make the applier block on the prepared transaction.
    sleepmillis(200);

    ASSERT_FALSE(future.isReady());

    abortTxn(operationContext(), lsid, txnNum - 1);

    future.get();

    future = applier->applyUntilDone();
    future.get();

    // Op should always be applied, even if txn info was not compatible.
    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs().ns(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    ASSERT_THROWS_CODE(
        isWriteAlreadyExecuted(session, 1), DBException, ErrorCodes::IncompleteTransactionHistory);

    checkSecondaryCanReplicateCorrectly();
}

}  // unnamed namespace
}  // namespace mongo
