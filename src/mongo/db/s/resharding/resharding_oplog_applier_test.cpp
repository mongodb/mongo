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

#include "mongo/db/s/resharding/resharding_oplog_applier.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader_mock.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/s/resharding/resharding_donor_oplog_iterator.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_noop_o2_field_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/sharding_mongod_test_fixture.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/update_oplog_entry_serialization.h"
#include "mongo/db/vector_clock/vector_clock_metadata_hook.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

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

class ReshardingOplogApplierTest : service_context_test::WithSetupTransportLayer,
                                   public ShardingMongoDTestFixture {
public:
    const HostAndPort kConfigHostAndPort{"DummyConfig", 12345};
    const std::string kOriginalShardKey = "sk";
    const BSONObj kOriginalShardKeyPattern{BSON(kOriginalShardKey << 1)};

    ReshardingOplogApplierTest()
        : ShardingMongoDTestFixture(
              Options{}.useMockClock(true).useMockTickSource<Milliseconds>(true)) {}

    void setUp() override {
        ShardingMongoDTestFixture::setUp();

        ShardingState::get(getServiceContext())
            ->setRecoveryCompleted({OID::gen(),
                                    ClusterRole::ShardServer,
                                    ConnectionString(kConfigHostAndPort),
                                    _sourceId.getShardId()});

        _mockConfigServerCacheLoader = std::make_shared<ConfigServerCatalogCacheLoaderMock>();
        _mockShardServerCacheLoader = std::make_shared<ShardServerCatalogCacheLoaderMock>();
        auto catalogCache =
            std::make_unique<CatalogCache>(getServiceContext(),
                                           _mockConfigServerCacheLoader,
                                           _mockShardServerCacheLoader,
                                           true /* cascadeDatabaseCacheLoaderShutdown */,
                                           false /* cascadeCollectionCacheLoaderShutdown */);
        uassertStatusOK(
            initializeGlobalShardingStateForMongodForTest(ConnectionString(kConfigHostAndPort),
                                                          std::move(catalogCache),
                                                          _mockShardServerCacheLoader));

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());

        uassertStatusOK(createCollection(
            operationContext(),
            NamespaceString::kSessionTransactionsTableNamespace.dbName(),
            BSON("create" << NamespaceString::kSessionTransactionsTableNamespace.coll())));
        DBDirectClient client(operationContext());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        createTestCollection(operationContext(), kAppliedToNs);
        createTestCollection(operationContext(), kStashNs);
        createTestCollection(operationContext(), kOtherDonorStashNs);

        _cm = createChunkManagerForOriginalColl();

        _metrics = ReshardingMetrics::makeInstance_forTest(
            kCrudUUID,
            BSON("y" << 1),
            kCrudNs,
            ReshardingMetrics::Role::kRecipient,
            getServiceContext()->getFastClockSource()->now(),
            getServiceContext());
        _metrics->registerDonors({_sourceId.getShardId()});

        _applierMetrics = std::make_unique<ReshardingOplogApplierMetrics>(
            _sourceId.getShardId(), _metrics.get(), boost::none);

        _executor = makeTaskExecutorForApplier();
        _executor->startup();

        _cancelableOpCtxExecutor = makeExecutorForCancelableOpCtx();
        _cancelableOpCtxExecutor->startup();
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();

        _cancelableOpCtxExecutor->shutdown();
        _cancelableOpCtxExecutor->join();

        ShardingMongoDTestFixture::tearDown();
    }

    class StaticCatalogClient final : public ShardingCatalogClientMock {
    public:
        StaticCatalogClient(std::vector<ShardType> shards) : _shards(std::move(shards)) {}

        repl::OpTimeWith<std::vector<ShardType>> getAllShards(OperationContext* opCtx,
                                                              repl::ReadConcernLevel readConcern,
                                                              BSONObj filter) override {
            return repl::OpTimeWith<std::vector<ShardType>>(_shards);
        }

        std::vector<CollectionType> getShardedCollections(OperationContext* opCtx,
                                                          const DatabaseName& dbName,
                                                          repl::ReadConcernLevel readConcernLevel,
                                                          const BSONObj& sort) override {
            return {};
        }

        std::vector<CollectionType> getCollections(OperationContext* opCtx,
                                                   const DatabaseName& dbName,
                                                   repl::ReadConcernLevel readConcernLevel,
                                                   const BSONObj& sort) override {
            return _colls;
        }

        void setCollections(std::vector<CollectionType> colls) {
            _colls = std::move(colls);
        }

    private:
        const std::vector<ShardType> _shards;
        std::vector<CollectionType> _colls;
    };

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        return std::make_unique<StaticCatalogClient>(kShardList);
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
                                               false, /* unsplittable */
                                               nullptr,
                                               false,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,

                                               false,
                                               chunks);

        return ChunkManager(makeStandaloneRoutingTableHistory(std::move(rt)), boost::none);
    }

    void loadCatalogCacheValues() {
        _mockConfigServerCacheLoader->setDatabaseRefreshReturnValue(
            DatabaseType(kAppliedToNs.dbName(),
                         _sourceId.getShardId(),
                         DatabaseVersion(UUID::gen(), Timestamp(1, 1))));
        std::vector<ChunkType> chunks;
        _cm->forEachChunk([&](const auto& chunk) {
            chunks.emplace_back(
                _cm->getUUID(), chunk.getRange(), chunk.getLastmod(), chunk.getShardId());
            return true;
        });
        _mockShardServerCacheLoader->setCollectionRefreshValues(
            kAppliedToNs,
            CollectionType(kAppliedToNs,
                           _cm->getVersion().epoch(),
                           _cm->getVersion().getTimestamp(),
                           Date_t::now(),
                           kCrudUUID,
                           kOriginalShardKeyPattern),
            chunks,
            boost::none);
    }

    repl::OplogEntry makeOplog(const repl::OpTime& opTime,
                               repl::OpTypeEnum opType,
                               const BSONObj& obj1,
                               const boost::optional<BSONObj> obj2,
                               Date_t wallClockTime = {}) {
        return makeOplog(opTime, opType, obj1, obj2, {}, {}, wallClockTime);
    }

    repl::OplogEntry makeOplog(const repl::OpTime& opTime,
                               repl::OpTypeEnum opType,
                               const BSONObj& obj1,
                               const boost::optional<BSONObj> obj2,
                               const OperationSessionInfo& sessionInfo,
                               const std::vector<StmtId>& statementIds,
                               Date_t wallClockTime = {}) {
        ReshardingDonorOplogId id(opTime.getTimestamp(), opTime.getTimestamp());
        return {repl::DurableOplogEntry(opTime,
                                        opType,
                                        kCrudNs,
                                        kCrudUUID,
                                        false /* fromMigrate */,
                                        boost::none,  // checkExistenceForDiffInsert
                                        boost::none,  // versionContext
                                        0 /* version */,
                                        obj1,
                                        obj2,
                                        sessionInfo,
                                        boost::none /* upsert */,
                                        wallClockTime,
                                        statementIds,
                                        boost::none /* prevWrite */,
                                        boost::none /* preImage */,
                                        boost::none /* postImage */,
                                        kMyShardId,
                                        Value(id.toBSON()),
                                        boost::none /* needsRetryImage) */)};
    }

    repl::OplogEntry makeProgressMarkNoopOplog(repl::OpTime opTime,
                                               Date_t wallClockTime,
                                               bool createdAfterOplogApplicationStarted) {
        ReshardingDonorOplogId id(opTime.getTimestamp(), opTime.getTimestamp());

        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kNoop);
        op.set_id(Value(id.toBSON()));
        op.setObject({});

        ReshardProgressMarkO2Field o2Field;
        o2Field.setType(resharding::kReshardProgressMarkOpLogType);
        if (createdAfterOplogApplicationStarted) {
            o2Field.setCreatedAfterOplogApplicationStarted(true);
        }
        op.setObject2(o2Field.toBSON());

        op.setNss({});
        op.setOpTime(opTime);
        op.setWallClockTime(wallClockTime);

        return {op.toBSON()};
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
        return _cm.value();
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

    ReshardingMetrics* metrics() {
        return _metrics.get();
    }

    ClockSourceMock* clockSource() {
        return dynamic_cast<ClockSourceMock*>(getServiceContext()->getFastClockSource());
    }

    TickSourceMock<Milliseconds>* tickSource() {
        return dynamic_cast<TickSourceMock<Milliseconds>*>(getServiceContext()->getTickSource());
    }

    Date_t now() {
        return clockSource()->now();
    }

    void advanceTime(Milliseconds millis) {
        clockSource()->advance(millis);
        tickSource()->advance(millis);
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
            Client::initThread(threadName, getGlobalServiceContext()->getService());
            auto* client = Client::getCurrent();
            AuthorizationSession::get(*client)->grantInternalAuthorization();
        };

        auto hookList = std::make_unique<rpc::EgressMetadataHookList>();
        hookList->addHook(std::make_unique<rpc::VectorClockMetadataHook>(getServiceContext()));

        auto executor = executor::ThreadPoolTaskExecutor::create(
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

    static constexpr size_t kWriterPoolSize = 4;
    static constexpr size_t kApplierBatchTaskCount = kWriterPoolSize;

    const NamespaceString kOplogNs =
        NamespaceString::createNamespaceString_forTest("config.localReshardingOplogBuffer.xxx.yyy");
    const NamespaceString kCrudNs = NamespaceString::createNamespaceString_forTest("foo.bar");
    const UUID kCrudUUID = UUID::gen();
    const NamespaceString kAppliedToNs = NamespaceString::createNamespaceString_forTest(
        "foo", fmt::format("system.resharding.{}", kCrudUUID.toString()));
    const NamespaceString kStashNs =
        NamespaceString::makeReshardingLocalConflictStashNSS(UUID::gen(), "1");
    const NamespaceString kOtherDonorStashNs =
        NamespaceString::makeReshardingLocalConflictStashNSS(UUID::gen(), "2");
    const std::vector<NamespaceString> kStashCollections{kStashNs, kOtherDonorStashNs};
    const ShardId kMyShardId{"shard1"};
    const ShardId kOtherShardId{"shard2"};
    const std::vector<ShardType> kShardList = {ShardType(kMyShardId.toString(), "Host0:12345"),
                                               ShardType(kOtherShardId.toString(), "Host1:12345")};
    const ReshardingSourceId _sourceId{UUID::gen(), kMyShardId};

    service_context_test::ShardRoleOverride _shardRole;

    boost::optional<ChunkManager> _cm;

    std::shared_ptr<ConfigServerCatalogCacheLoaderMock> _mockConfigServerCacheLoader;
    std::shared_ptr<ShardServerCatalogCacheLoaderMock> _mockShardServerCacheLoader;

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
                    kApplierBatchTaskCount,
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
    loadCatalogCacheValues();
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
                    kApplierBatchTaskCount,
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
                    kApplierBatchTaskCount,
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
    loadCatalogCacheValues();

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
                    kApplierBatchTaskCount,
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
    loadCatalogCacheValues();
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
                    kApplierBatchTaskCount,
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
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::FailedToParse);

    DBDirectClient client(operationContext());
    auto doc = client.findOne(appliedToNs(), BSON("_id" << 1));
    ASSERT_BSONOBJ_EQ(BSON("_id" << 1), doc);

    auto progressDoc = ReshardingOplogApplier::checkStoredProgress(operationContext(), sourceId());
    ASSERT_FALSE(progressDoc);
}

TEST_F(ReshardingOplogApplierTest, ErrorDuringSecondBatchApply) {
    loadCatalogCacheValues();
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
                    kApplierBatchTaskCount,
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
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::FailedToParse);

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
                    kApplierBatchTaskCount,
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
                    kApplierBatchTaskCount,
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
    loadCatalogCacheValues();

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
                    kApplierBatchTaskCount,
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
                    kApplierBatchTaskCount,
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
    loadCatalogCacheValues();
    std::deque<repl::OplogEntry> ops;
    ops.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                            repl::OpTypeEnum::kInsert,
                            BSON("_id" << 1),
                            boost::none));
    ops.push_back(makeOplog(
        repl::OpTime(Timestamp(6, 3), 1),
        repl::OpTypeEnum::kCommand,
        BSON("renameCollection" << appliedToNs().ns_forTest() << "to" << stashNs().ns_forTest()),
        boost::none));
    ops.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                            repl::OpTypeEnum::kInsert,
                            BSON("_id" << 2),
                            boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(ops), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    applier.emplace(makeApplierEnv(),
                    kApplierBatchTaskCount,
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
                            BSON("drop" << appliedToNs().ns_forTest()),
                            boost::none));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(ops), 1 /* batchSize */);
    boost::optional<ReshardingOplogApplier> applier;
    applier.emplace(makeApplierEnv(),
                    kApplierBatchTaskCount,
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
    loadCatalogCacheValues();

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
                                   4,
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

TEST_F(ReshardingOplogApplierTest, UpdateAverageTimeToApplyBasic) {
    auto batchSize = 2;
    auto smoothingFactor = 0.3;

    const RAIIServerParameterControllerForTest smoothingFactorServerParameter{
        "reshardingExponentialMovingAverageTimeToFetchAndApplySmoothingFactor", smoothingFactor};

    loadCatalogCacheValues();
    for (bool movingAvgFeatureFlag : {false, true}) {
        for (bool movingAvgServerParameter : {false, true}) {
            LOGV2(10655503,
                  "Running case",
                  "test"_attr = unittest::getTestName(),
                  "movingAvgFeatureFlag"_attr = movingAvgFeatureFlag,
                  "movingAvgServerParameter"_attr = movingAvgServerParameter);

            const RAIIServerParameterControllerForTest movingAvgFeatureFlagRAII{
                "featureFlagReshardingRemainingTimeEstimateBasedOnMovingAverage",
                movingAvgFeatureFlag};
            const RAIIServerParameterControllerForTest movingAvgServerParameterRAII{
                "reshardingRemainingTimeEstimateBasedOnMovingAverage", movingAvgServerParameter};

            // Verify that the average started out uninitialized.
            ASSERT_FALSE(metrics()->getAverageTimeToApplyOplogEntries(sourceId().getShardId()));

            // Advance the clock before making each oplog entry to make them have distinct wall
            // clock times.
            std::deque<repl::OplogEntry> ops;

            // The oplog batch has size greater than 1. The last oplog entry is a CRUD oplog entry.
            advanceTime(Milliseconds(1250));
            ops.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                                    repl::OpTypeEnum::kInsert,
                                    BSON("_id" << 1),
                                    boost::none,
                                    now()));
            advanceTime(Milliseconds(150));
            auto oplogWallTime1 = now();
            ops.push_back(makeOplog(repl::OpTime(Timestamp(6, 3), 1),
                                    repl::OpTypeEnum::kInsert,
                                    BSON("_id" << 2),
                                    boost::none,
                                    oplogWallTime1));

            // The oplog batch has batch greater than 1. The last oplog entry is a session oplog
            // entry.
            advanceTime(Milliseconds(750));
            ops.push_back(makeOplog(repl::OpTime(Timestamp(7, 3), 1),
                                    repl::OpTypeEnum::kUpdate,
                                    update_oplog_entry::makeDeltaOplogEntry(
                                        BSON(doc_diff::kUpdateSectionFieldName << BSON("x" << 1))),
                                    BSON("_id" << 2),
                                    now()));
            advanceTime(Milliseconds(50));
            OperationSessionInfo sessionInfo3;
            sessionInfo3.setSessionId(makeLogicalSessionIdForTest());
            sessionInfo3.setTxnNumber(1);
            auto statementIds3 = std::vector<int>{0};
            auto oplogWallTime3 = now();
            ops.push_back(makeOplog(repl::OpTime(Timestamp(8, 3), 1),
                                    repl::OpTypeEnum::kDelete,
                                    BSON("_id" << 1),
                                    boost::none,
                                    sessionInfo3,
                                    statementIds3,
                                    oplogWallTime3));

            // The oplog batch has equal to 1. The last oplog entry is a 'reshardProgressMark' oplog
            // entry.
            advanceTime(Milliseconds(500));
            auto oplogWallTime4 = now();
            ops.push_back(
                makeProgressMarkNoopOplog(repl::OpTime(Timestamp(9, 3), 1),
                                          oplogWallTime4,
                                          true /* createdAfterOplogApplicationStarted */));

            advanceTime(Milliseconds(100));
            auto iterator = std::make_unique<OplogIteratorMock>(std::move(ops), batchSize);
            boost::optional<ReshardingOplogApplier> applier;
            applier.emplace(makeApplierEnv(),
                            kApplierBatchTaskCount,
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

            auto avgTimeToApplyAfter =
                metrics()->getAverageTimeToApplyOplogEntries(sourceId().getShardId());

            if (movingAvgFeatureFlag && movingAvgServerParameter) {
                // Upon applying the first batch, the average should get based on the difference
                // between the current timestamp and oplogEntry1 wall time.
                auto timeToApply1 = now() - oplogWallTime1;
                auto avgTimeToApply1 = timeToApply1;

                // Upon applying the second batch, the average should get based on the difference
                // between the current timestamp and oplogEntry3 wall time.
                auto timeToApply3 = now() - oplogWallTime3;
                auto avgTimeToApply3 =
                    Milliseconds((int)resharding::calculateExponentialMovingAverage(
                        avgTimeToApply1.count(), timeToApply3.count(), smoothingFactor));

                // Upon applying the third batch, the average should get based on the difference
                // between the current timestamp and oplogEntry4 wall time.
                auto timeToApply4 = now() - oplogWallTime4;
                auto avgTimeToApply4 =
                    Milliseconds((int)resharding::calculateExponentialMovingAverage(
                        avgTimeToApply3.count(), timeToApply4.count(), smoothingFactor));

                ASSERT_EQ(avgTimeToApplyAfter, avgTimeToApply4);
            } else {
                // Verify that the average did not get initialized.
                ASSERT_FALSE(avgTimeToApplyAfter);
            }
        }
    }
}

TEST_F(ReshardingOplogApplierTest, UpdateAverageTimeToApply_EmptyBatch) {
    auto batchSize = 2;

    // Not set the 'reshardingRemainingTimeEstimateBasedOnMovingAverage' server parameter to verify
    // that it defaults to true.
    const RAIIServerParameterControllerForTest movingAvgFeatureFlagRAII{
        "featureFlagReshardingRemainingTimeEstimateBasedOnMovingAverage", true};

    // Verify that the average started out uninitialized.
    ASSERT_FALSE(metrics()->getAverageTimeToApplyOplogEntries(sourceId().getShardId()));

    std::deque<repl::OplogEntry> ops;

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(ops), batchSize);
    boost::optional<ReshardingOplogApplier> applier;
    applier.emplace(makeApplierEnv(),
                    kApplierBatchTaskCount,
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

    // Verify that the average is still uninitialized.
    ASSERT_FALSE(metrics()->getAverageTimeToApplyOplogEntries(sourceId().getShardId()));
}

TEST_F(ReshardingOplogApplierTest, UpdateAverageTimeToApply_ClockSkew) {
    auto batchSize = 2;

    // Not set the 'reshardingRemainingTimeEstimateBasedOnMovingAverage' server parameter to verify
    // that it defaults to true.
    const RAIIServerParameterControllerForTest movingAvgFeatureFlagRAII{
        "featureFlagReshardingRemainingTimeEstimateBasedOnMovingAverage", true};

    // Verify that the average started out uninitialized.
    ASSERT_FALSE(metrics()->getAverageTimeToApplyOplogEntries(sourceId().getShardId()));

    std::deque<repl::OplogEntry> ops;
    // Make the oplog wall time greater than the current time on the recipient.
    ops.push_back(makeOplog(repl::OpTime(Timestamp(5, 3), 1),
                            repl::OpTypeEnum::kInsert,
                            BSON("_id" << 1),
                            boost::none,
                            now() + Milliseconds(100)));

    auto iterator = std::make_unique<OplogIteratorMock>(std::move(ops), batchSize);
    boost::optional<ReshardingOplogApplier> applier;
    applier.emplace(makeApplierEnv(),
                    kApplierBatchTaskCount,
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

    ASSERT_EQ(metrics()->getAverageTimeToApplyOplogEntries(sourceId().getShardId()),
              Milliseconds(0));
}

}  // unnamed namespace
}  // namespace mongo
