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

#include <fmt/format.h>

#include "mongo/db/cancelable_operation_context.h"
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
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/sharding_mongod_test_fixture.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/vector_clock_metadata_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

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
        std::shared_ptr<executor::TaskExecutor> executor,
        CancellationToken cancelToken,
        CancelableOperationContextFactory factory) override {
        // This operation context is unused by the function but confirms that the Client calling
        // getNextBatch() doesn't already have an operation context.
        auto opCtx = factory.makeOperationContext(&cc());

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
        DBDirectClient client(operationContext());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            operationContext());
        uassertStatusOK(createCollection(operationContext(),
                                         kAppliedToNs.db().toString(),
                                         BSON("create" << kAppliedToNs.coll())));
        uassertStatusOK(createCollection(
            operationContext(), kStashNs.db().toString(), BSON("create" << kStashNs.coll())));
        uassertStatusOK(createCollection(operationContext(),
                                         kOtherDonorStashNs.db().toString(),
                                         BSON("create" << kOtherDonorStashNs.coll())));

        _cm = createChunkManagerForOriginalColl();

        _metrics = ReshardingMetrics::makeInstance(kCrudUUID,
                                                   BSON("y" << 1),
                                                   kCrudNs,
                                                   ReshardingMetrics::Role::kRecipient,
                                                   getServiceContext()->getFastClockSource()->now(),
                                                   getServiceContext());
        _applierMetrics =
            std::make_unique<ReshardingOplogApplierMetrics>(_metrics.get(), boost::none);

        _executor = makeTaskExecutorForApplier();
        _executor->startup();

        _cancelableOpCtxExecutor = makeExecutorForCancelableOpCtx();
        _cancelableOpCtxExecutor->startup();
    }

    void tearDown() {
        _executor->shutdown();
        _executor->join();

        _cancelableOpCtxExecutor->shutdown();
        _cancelableOpCtxExecutor->join();

        ShardingMongodTestFixture::tearDown();
    }

    ChunkManager createChunkManagerForOriginalColl() {
        // Create three chunks, two that are owned by this donor shard and one owned by some other
        // shard. The chunk for {sk: null} is owned by this donor shard to allow test cases to omit
        // the shard key field when it isn't relevant.
        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks = {
            ChunkType{
                kCrudUUID,
                ChunkRange{BSON(kOriginalShardKey << MINKEY),
                           BSON(kOriginalShardKey << -std::numeric_limits<double>::infinity())},
                ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                _sourceId.getShardId()},
            ChunkType{
                kCrudUUID,
                ChunkRange{BSON(kOriginalShardKey << -std::numeric_limits<double>::infinity()),
                           BSON(kOriginalShardKey << 0)},
                ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                kOtherShardId},
            ChunkType{kCrudUUID,
                      ChunkRange{BSON(kOriginalShardKey << 0), BSON(kOriginalShardKey << MAXKEY)},
                      ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                      _sourceId.getShardId()}};

        auto rt = RoutingTableHistory::makeNew(kCrudNs,
                                               kCrudUUID,
                                               kOriginalShardKeyPattern,
                                               nullptr,
                                               false,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none,
                                               boost::none /* chunkSizeBytes */,
                                               false,
                                               chunks);

        return ChunkManager(_sourceId.getShardId(),
                            DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                            makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none);
    }

    repl::OplogEntry makeOplog(const repl::OpTime& opTime,
                               repl::OpTypeEnum opType,
                               const BSONObj& obj1,
                               const boost::optional<BSONObj> obj2) {
        return makeOplog(opTime, opType, obj1, obj2, {}, {});
    }

    repl::OplogEntry makeOplog(const repl::OpTime& opTime,
                               repl::OpTypeEnum opType,
                               const BSONObj& obj1,
                               const boost::optional<BSONObj> obj2,
                               const OperationSessionInfo& sessionInfo,
                               const std::vector<StmtId>& statementIds) {
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
                                        statementIds,
                                        boost::none /* prevWrite */,
                                        boost::none /* preImage */,
                                        boost::none /* postImage */,
                                        kMyShardId,
                                        Value(id.toBSON()),
                                        boost::none /* needsRetryImage) */)};
    }

    const NamespaceString& oplogBufferNs() {
        return kOplogNs;
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

    BSONObj getMetricsOpCounters() {
        return _metrics->reportForCurrentOp();
    }

    long long metricsAppliedCount() const {
        auto fullCurOp = _metrics->reportForCurrentOp();
        return fullCurOp["oplogEntriesApplied"_sd].Long();
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> getExecutor() {
        return _executor;
    }


    std::shared_ptr<ThreadPool> getCancelableOpCtxExecutor() {
        return _cancelableOpCtxExecutor;
    }

protected:
    auto makeApplierEnv() {
        return std::make_unique<ReshardingOplogApplier::Env>(getServiceContext(),
                                                             _applierMetrics.get());
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> makeTaskExecutorForApplier() {
        // The ReshardingOplogApplier expects there to already be a Client associated with the
        // thread from the thread pool. We set up the ThreadPoolTaskExecutor identically to how the
        // recipient's primary-only service is set up.
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.maxThreads = kWriterPoolSize;
        threadPoolOptions.threadNamePrefix = "TestReshardOplogApplication-";
        threadPoolOptions.poolName = "TestReshardOplogApplicationThreadPool";
        threadPoolOptions.onCreateThread = [](const std::string& threadName) {
            Client::initThread(threadName.c_str());
            auto* client = Client::getCurrent();
            AuthorizationSession::get(*client)->grantInternalAuthorization(client);

            {
                stdx::lock_guard<Client> lk(*client);
                client->setSystemOperationKillableByStepdown(lk);
            }
        };

        auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
        hookList->addHook(std::make_unique<rpc::VectorClockMetadataHook>(getServiceContext()));

        auto executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
            std::make_unique<ThreadPool>(std::move(threadPoolOptions)),
            executor::makeNetworkInterface(
                "TestReshardOplogApplicationNetwork", nullptr, std::move(hookList)));

        return executor;
    }

    std::shared_ptr<ThreadPool> makeExecutorForCancelableOpCtx() {
        return std::make_shared<ThreadPool>([] {
            ThreadPool::Options options;
            options.poolName = "TestReshardOplogApplierCancelableOpCtxPool";
            options.minThreads = 1;
            options.maxThreads = 1;
            return options;
        }());
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
    boost::optional<ChunkManager> _cm;

    const ReshardingSourceId _sourceId{UUID::gen(), kMyShardId};
    std::unique_ptr<ReshardingMetrics> _metrics;
    std::unique_ptr<ReshardingOplogApplierMetrics> _applierMetrics;

    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    std::shared_ptr<ThreadPool> _cancelableOpCtxExecutor;
};

TEST_F(ReshardingOplogApplierTest, NothingToIterate) {
    std::deque<repl::OplogEntry> crudOps;
    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);

    boost::optional<ReshardingOplogApplier> applier;

    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_OK(future.getNoThrow());
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
                                update_oplog_entry::makeDeltaOplogEntry(
                                    BSON(doc_diff::kUpdateSectionFieldName << BSON("x" << 1))),
                                BSON("_id" << 2)));
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                repl::OpTypeEnum::kDelete,
                                BSON("_id" << 1),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_OK(future.getNoThrow());

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    doc = client.findOne(appliedToNs(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2 << "x" << 1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(8, 3), progressDoc->getProgress().getTs());
    ASSERT_EQ(4, progressDoc->getNumEntriesApplied());
}

TEST_F(ReshardingOplogApplierTest, CanceledApplyingBatch) {
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
                                BSON("_id" << 2),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;

    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    auto abortSource = CancellationSource();
    abortSource.cancel();
    auto cancelToken = abortSource.token();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());

    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::CallbackCanceled);
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
    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_OK(future.getNoThrow());

    DBDirectClient client(operationContext());

    for (int x = 0; x < 19; x++) {
        auto doc = client.findOne(appliedToNs(), BSON("_id" << x));
        ASSERT_BSONOBJ_EQ(BSON("_id" << x), doc);
    }

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(19, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(19, 3), progressDoc->getProgress().getTs());
    ASSERT_EQ(20, progressDoc->getNumEntriesApplied());
}

TEST_F(ReshardingOplogApplierTest, ErrorDuringFirstBatchApply) {
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
    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::duplicateCodeForTest(4772600));

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ErrorDuringSecondBatchApply) {
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
    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::duplicateCodeForTest(4772600));

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    doc = client.findOne(appliedToNs(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 3), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
    ASSERT_EQ(2, progressDoc->getNumEntriesApplied());
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingFirstOplog) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 2 /* batchSize */);
    iterator->setThrowWhenSingleItem();

    boost::optional<ReshardingOplogApplier> applier;
    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingFirstBatch) {
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
    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ErrorWhileIteratingSecondBatch) {
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
    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::InternalError);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 2), doc);

    doc = client.findOne(appliedToNs(), BSON("_id" << 3));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(6, 3), progressDoc->getProgress().getTs());
    ASSERT_EQ(2, progressDoc->getNumEntriesApplied());
}

TEST_F(ReshardingOplogApplierTest, ExecutorIsShutDown) {
    std::deque<repl::OplogEntry> crudOps;
    crudOps.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                repl::OpTypeEnum::kInsert,
                                BSON("_id" << 1),
                                boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(crudOps), 4 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    getExecutor()->shutdown();

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::ShutdownInProgress);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, UnsupportedCommandOpsShouldError) {
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
    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::OplogOperationUnsupported);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    doc = client.findOne(appliedToNs(), BSON("_id" << 2));
    ASSERT_BSONOBJ_EQ(BSONObj(), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(Timestamp(5, 3), progressDoc->getProgress().getClusterTime());
    ASSERT_EQ(Timestamp(5, 3), progressDoc->getProgress().getTs());
    ASSERT_EQ(1, progressDoc->getNumEntriesApplied());
}

TEST_F(ReshardingOplogApplierTest, DropSourceCollectionCmdShouldError) {
    std::deque<repl::OplogEntry> ops;
    ops.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                            repl::OpTypeEnum::kCommand,
                            BSON("drop" << appliedToNs().ns()),
                            boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(ops), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    applier.emplace(makeApplierEnv(),
                    sourceId(),
                    oplogBufferNs(),
                    appliedToNs(),
                    stashCollections(),
                    0U /* myStashIdx */,
                    chunkManager(),
                    std::move(iterator));

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier->run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::OplogOperationUnsupported);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, MetricsAreReported) {
    // Compress the makeOplog syntax a little further for this special case.
    using OpT = repl::OpTypeEnum;
    auto easyOp = [this](auto ts, OpT opType, BSONObj obj1, boost::optional<BSONObj> obj2 = {}) {
        return makeOplog(repl::OpTime(Timestamp(ts, 3), 1), opType, obj1, obj2);
    };
    auto iterator = std::make_unique<OplogIteratorMock>(
        std::deque<repl::OplogEntry>{
            easyOp(5, OpT::kDelete, BSON("_id" << 1)),
            easyOp(6, OpT::kInsert, BSON("_id" << 2)),
            easyOp(7,
                   OpT::kUpdate,
                   update_oplog_entry::makeDeltaOplogEntry(
                       BSON(doc_diff::kUpdateSectionFieldName << BSON("x" << 1))),
                   BSON("_id" << 2)),
            easyOp(8, OpT::kDelete, BSON("_id" << 1)),
            easyOp(9, OpT::kInsert, BSON("_id" << 3))},
        2);
    ReshardingOplogApplier applier(makeApplierEnv(),
                                   sourceId(),
                                   oplogBufferNs(),
                                   appliedToNs(),
                                   stashCollections(),
                                   0U /* myStashIdx */,
                                   chunkManager(),
                                   std::move(iterator));

    ASSERT_EQ(metricsAppliedCount(), 0);

    auto cancelToken = operationContext()->getCancellationToken();
    CancelableOperationContextFactory factory(cancelToken, getCancelableOpCtxExecutor());
    auto future = applier.run(getExecutor(), getExecutor(), cancelToken, factory);
    ASSERT_OK(future.getNoThrow());

    auto opCountersObj = getMetricsOpCounters();
    ASSERT_EQ(opCountersObj.getIntField("insertsApplied"), 2);
    ASSERT_EQ(opCountersObj.getIntField("updatesApplied"), 1);
    ASSERT_EQ(opCountersObj.getIntField("deletesApplied"), 2);

    // The in-memory metrics should show the 5 ops above + the final oplog entry, but on disk should
    // not include the final entry in its count.
    ASSERT_EQ(metricsAppliedCount(), 6);
    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_TRUE(progressDoc);
    ASSERT_EQ(5, progressDoc->getNumEntriesApplied());
}

}  // unnamed namespace
}  // namespace mongo
