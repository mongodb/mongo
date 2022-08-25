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

#include <memory>
#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/oplog_writer_impl.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/op_observer_sharding_impl.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_application.h"
#include "mongo/db/s/resharding/resharding_oplog_batch_applier.h"
#include "mongo/db/s/resharding/resharding_oplog_session_application.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/vector_clock_metadata_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class ReshardingOplogBatchApplierTest : public ServiceContextMongoDTest {
public:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        auto serviceContext = getServiceContext();

        // Initialize sharding components as a shard server.
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;

        {
            auto opCtx = makeOperationContext();
            auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(serviceContext);
            ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
            repl::ReplicationCoordinator::set(serviceContext, std::move(replCoord));

            repl::createOplog(opCtx.get());

            auto storageImpl = std::make_unique<repl::StorageInterfaceImpl>();
            repl::StorageInterface::set(serviceContext, std::move(storageImpl));

            MongoDSessionCatalog::set(
                serviceContext,
                std::make_unique<MongoDSessionCatalog>(
                    std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));
            auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx.get());
            mongoDSessionCatalog->onStepUp(opCtx.get());

            LogicalSessionCache::set(serviceContext, std::make_unique<LogicalSessionCacheNoop>());

            // OpObserverShardingImpl is required for timestamping the writes from
            // ReshardingOplogApplicationRules.
            auto opObserverRegistry =
                dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
            invariant(opObserverRegistry);

            opObserverRegistry->addObserver(
                std::make_unique<OpObserverShardingImpl>(std::make_unique<OplogWriterImpl>()));
        }

        {
            auto opCtx = makeOperationContext();

            for (const auto& nss : {_outputNss, _myStashNss, _otherStashNss}) {
                resharding::data_copy::ensureCollectionExists(
                    opCtx.get(), nss, CollectionOptions{});
            }

            _metrics =
                ReshardingMetrics::makeInstance(UUID::gen(),
                                                BSON("y" << 1),
                                                _outputNss,
                                                ShardingDataTransformMetrics::Role::kRecipient,
                                                serviceContext->getFastClockSource()->now(),
                                                serviceContext);
            _applierMetrics =
                std::make_unique<ReshardingOplogApplierMetrics>(_metrics.get(), boost::none);
            _crudApplication = std::make_unique<ReshardingOplogApplicationRules>(
                _outputNss,
                std::vector<NamespaceString>{_myStashNss, _otherStashNss},
                0U,
                _myDonorId,
                makeChunkManagerForSourceCollection(),
                _applierMetrics.get());

            _sessionApplication =
                std::make_unique<ReshardingOplogSessionApplication>(_myOplogBufferNss);

            _batchApplier = std::make_unique<ReshardingOplogBatchApplier>(*_crudApplication,
                                                                          *_sessionApplication);
        }
    }

    ReshardingOplogBatchApplier* applier() {
        return _batchApplier.get();
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> makeTaskExecutorForApplier() {
        // The ReshardingOplogBatchApplier expects there to already be a Client associated with the
        // thread from the thread pool. We set up the ThreadPoolTaskExecutor identically to how the
        // recipient's primary-only service is set up.
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.maxThreads = 1;
        threadPoolOptions.threadNamePrefix = "TestReshardOplogBatchApplier-";
        threadPoolOptions.poolName = "TestReshardOplogBatchApplierThreadPool";
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
                "TestReshardOplogBatchApplierNetwork", nullptr, std::move(hookList)));

        executor->startup();
        return executor;
    }

    CancelableOperationContextFactory makeCancelableOpCtxForApplier(CancellationToken cancelToken) {
        auto executor = std::make_shared<ThreadPool>([] {
            ThreadPool::Options options;
            options.poolName = "TestReshardOplogBatchApplierCancelableOpCtxPool";
            options.minThreads = 1;
            options.maxThreads = 1;
            return options;
        }());

        return CancelableOperationContextFactory(cancelToken, executor);
    }

    repl::OpTime makePreparedTxn(OperationContext* opCtx,
                                 LogicalSessionId lsid,
                                 TxnNumber txnNumber) {
        opCtx->setInMultiDocumentTransaction();
        opCtx->setLogicalSessionId(std::move(lsid));
        opCtx->setTxnNumber(txnNumber);

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(
            opCtx, {txnNumber}, false /* autocommit */, true /* startTransaction */);

        txnParticipant.unstashTransactionResources(opCtx, "prepareTransaction");

        // The transaction machinery cannot store an empty locker.
        { Lock::GlobalLock globalLock(opCtx, MODE_IX); }
        auto opTime = [opCtx] {
            TransactionParticipant::SideTransactionBlock sideTxn{opCtx};

            WriteUnitOfWork wuow{opCtx};
            auto opTime = repl::getNextOpTime(opCtx);
            wuow.release();

            opCtx->recoveryUnit()->abortUnitOfWork();
            opCtx->lockState()->endWriteUnitOfWork();

            return opTime;
        }();
        txnParticipant.prepareTransaction(opCtx, opTime);
        txnParticipant.stashTransactionResources(opCtx);

        return opTime;
    }

    void clearPreparedTxn(OperationContext* opCtx, LogicalSessionId lsid, TxnNumber txnNumber) {
        opCtx->setInMultiDocumentTransaction();
        opCtx->setLogicalSessionId(std::move(lsid));
        opCtx->setTxnNumber(txnNumber);

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(
            opCtx, {txnNumber}, false /* autocommit */, boost::none /* startTransaction */);

        txnParticipant.unstashTransactionResources(opCtx, "abortTransaction");
        txnParticipant.abortTransaction(opCtx);
        txnParticipant.stashTransactionResources(opCtx);
    }

    repl::OplogEntry makeFinishTxnOp(LogicalSessionId lsid, TxnNumber txnNumber) {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kCommand);
        // Use AbortTransactionOplogObject rather than CommitTransactionOplogObject to avoid needing
        // to deal with the commitTimestamp value. Both are treated the same way by
        // ReshardingOplogSessionApplication anyway.
        op.setObject(AbortTransactionOplogObject{}.toBSON());
        op.setSessionId(std::move(lsid));
        op.setTxnNumber(std::move(txnNumber));
        op.setOpTime({{}, {}});
        op.set_id(Value{
            Document{{ReshardingDonorOplogId::kClusterTimeFieldName, op.getOpTime().getTimestamp()},
                     {ReshardingDonorOplogId::kTsFieldName, op.getOpTime().getTimestamp()}}});

        // These are unused by ReshardingOplogSessionApplication but required by IDL parsing.
        op.setNss({});
        op.setWallClockTime({});

        return {op.toBSON()};
    }

    std::vector<repl::DurableOplogEntry> findOplogEntriesNewerThan(OperationContext* opCtx,
                                                                   Timestamp ts) {
        std::vector<repl::DurableOplogEntry> result;

        PersistentTaskStore<repl::OplogEntryBase> store(NamespaceString::kRsOplogNamespace);
        store.forEach(opCtx, BSON("ts" << BSON("$gt" << ts)), [&](const auto& oplogEntry) {
            result.emplace_back(
                unittest::assertGet(repl::DurableOplogEntry::parse(oplogEntry.toBSON())));
            return true;
        });

        return result;
    }

    boost::optional<SessionTxnRecord> findSessionRecord(OperationContext* opCtx,
                                                        const LogicalSessionId& lsid) {
        boost::optional<SessionTxnRecord> result;

        PersistentTaskStore<SessionTxnRecord> store(
            NamespaceString::kSessionTransactionsTableNamespace);
        store.forEach(opCtx,
                      BSON(SessionTxnRecord::kSessionIdFieldName << lsid.toBSON()),
                      [&](const auto& sessionTxnRecord) {
                          result.emplace(sessionTxnRecord);
                          return false;
                      });

        return result;
    }

    void checkGeneratedNoop(const repl::DurableOplogEntry& foundOp,
                            const LogicalSessionId& lsid,
                            TxnNumber txnNumber,
                            const std::vector<StmtId>& stmtIds) {
        ASSERT_EQ(OpType_serializer(foundOp.getOpType()),
                  OpType_serializer(repl::OpTypeEnum::kNoop))
            << foundOp;

        ASSERT_EQ(foundOp.getSessionId(), lsid) << foundOp;
        ASSERT_EQ(foundOp.getTxnNumber(), txnNumber) << foundOp;
        ASSERT(foundOp.getStatementIds() == stmtIds) << foundOp;

        // The oplog entry must have o2 and fromMigrate set or SessionUpdateTracker will ignore it.
        ASSERT_TRUE(foundOp.getObject2());
        ASSERT_TRUE(foundOp.getFromMigrate());
    }

    void checkSessionTxnRecord(const SessionTxnRecord& sessionTxnRecord,
                               const repl::DurableOplogEntry& foundOp) {
        ASSERT_EQ(sessionTxnRecord.getSessionId(), foundOp.getSessionId())
            << sessionTxnRecord.toBSON() << ", " << foundOp;
        ASSERT_EQ(sessionTxnRecord.getTxnNum(), foundOp.getTxnNumber())
            << sessionTxnRecord.toBSON() << ", " << foundOp;
        ASSERT_EQ(sessionTxnRecord.getLastWriteOpTime(), foundOp.getOpTime())
            << sessionTxnRecord.toBSON() << ", " << foundOp;
        ASSERT_EQ(sessionTxnRecord.getLastWriteDate(), foundOp.getWallClockTime())
            << sessionTxnRecord.toBSON() << ", " << foundOp;
    }

private:
    ChunkManager makeChunkManagerForSourceCollection() {
        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks = {ChunkType{
            _sourceUUID,
            ChunkRange{BSON(_currentShardKey << MINKEY), BSON(_currentShardKey << MAXKEY)},
            ChunkVersion({epoch, Timestamp(1, 1)}, {100, 0}),
            _myDonorId}};

        auto rt = RoutingTableHistory::makeNew(_sourceNss,
                                               _sourceUUID,
                                               BSON(_currentShardKey << 1),
                                               nullptr /* defaultCollator */,
                                               false /* unique */,
                                               std::move(epoch),
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               boost::none /* chunkSizeBytes */,
                                               true /* allowMigrations */,
                                               chunks);

        return ChunkManager(_myDonorId,
                            DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                            makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none /* clusterTime */);
    }

    RoutingTableHistoryValueHandle makeStandaloneRoutingTableHistory(RoutingTableHistory rt) {
        const auto version = rt.getVersion();
        return RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
    }

    const StringData _currentShardKey = "sk";

    const NamespaceString _sourceNss{"test_crud", "collection_being_resharded"};
    const UUID _sourceUUID = UUID::gen();

    const ShardId _myDonorId{"myDonorId"};
    const ShardId _otherDonorId{"otherDonorId"};

    const NamespaceString _outputNss =
        resharding::constructTemporaryReshardingNss(_sourceNss.db(), _sourceUUID);
    const NamespaceString _myStashNss =
        resharding::getLocalConflictStashNamespace(_sourceUUID, _myDonorId);
    const NamespaceString _otherStashNss =
        resharding::getLocalConflictStashNamespace(_sourceUUID, _otherDonorId);
    const NamespaceString _myOplogBufferNss =
        resharding::getLocalOplogBufferNamespace(_sourceUUID, _myDonorId);

    std::unique_ptr<ReshardingMetrics> _metrics;
    std::unique_ptr<ReshardingOplogApplierMetrics> _applierMetrics;

    std::unique_ptr<ReshardingOplogApplicationRules> _crudApplication;
    std::unique_ptr<ReshardingOplogSessionApplication> _sessionApplication;
    std::unique_ptr<ReshardingOplogBatchApplier> _batchApplier;
};

TEST_F(ReshardingOplogBatchApplierTest, WaitsOnPreparedTxnAndAutomaticallyRetries) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber existingTxnNumber = 100;

    auto opTime = [&] {
        auto opCtx = makeOperationContext();
        return makePreparedTxn(opCtx.get(), lsid, existingTxnNumber);
    }();

    TxnNumber incomingTxnNumber = existingTxnNumber + 1;
    auto oplogEntry = makeFinishTxnOp(lsid, incomingTxnNumber);

    auto executor = makeTaskExecutorForApplier();
    auto factory = makeCancelableOpCtxForApplier(CancellationToken::uncancelable());
    auto future = applier()->applyBatch<true>(
        {&oplogEntry}, executor, CancellationToken::uncancelable(), factory);

    ASSERT_FALSE(future.isReady());
    // Wait a little bit to increase the likelihood that the applier has blocked on the prepared
    // transaction before the transaction is aborted.
    ASSERT_OK(
        executor->sleepFor(Milliseconds{200}, CancellationToken::uncancelable()).getNoThrow());
    ASSERT_FALSE(future.isReady());

    {
        auto opCtx = makeOperationContext();
        clearPreparedTxn(opCtx.get(), lsid, existingTxnNumber);
    }

    ASSERT_OK(future.getNoThrow());

    {
        auto opCtx = makeOperationContext();
        auto foundOps = findOplogEntriesNewerThan(opCtx.get(), opTime.getTimestamp());
        ASSERT_EQ(foundOps.size(), 2U);
        // The first oplog entry is from aborting the prepared transaction via clearPreparedTxn().
        // The second oplog entry is from session application via ReshardingOplogSessionApplication.
        checkGeneratedNoop(foundOps[1], lsid, incomingTxnNumber, {kIncompleteHistoryStmtId});

        auto sessionTxnRecord = findSessionRecord(opCtx.get(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[1]);
    }
}

TEST_F(ReshardingOplogBatchApplierTest, CancelableWhileWaitingOnPreparedTxn) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber existingTxnNumber = 100;

    {
        auto opCtx = makeOperationContext();
        makePreparedTxn(opCtx.get(), lsid, existingTxnNumber);
    }

    ON_BLOCK_EXIT([&] {
        auto opCtx = makeOperationContext();
        clearPreparedTxn(opCtx.get(), lsid, existingTxnNumber);
    });

    TxnNumber incomingTxnNumber = existingTxnNumber + 1;
    auto oplogEntry = makeFinishTxnOp(lsid, incomingTxnNumber);

    CancellationSource cancelSource;
    auto executor = makeTaskExecutorForApplier();
    auto factory = makeCancelableOpCtxForApplier(CancellationToken::uncancelable());
    auto future =
        applier()->applyBatch<true>({&oplogEntry}, executor, cancelSource.token(), factory);

    ASSERT_FALSE(future.isReady());
    // Wait a little bit to increase the likelihood that the applier has blocked on the prepared
    // transaction before the cancellation source is canceled.
    ASSERT_OK(
        executor->sleepFor(Milliseconds{200}, CancellationToken::uncancelable()).getNoThrow());
    ASSERT_FALSE(future.isReady());

    cancelSource.cancel();
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::CallbackCanceled);
}

}  // namespace
}  // namespace mongo
