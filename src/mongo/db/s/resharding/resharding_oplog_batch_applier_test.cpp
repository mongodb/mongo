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

#include "mongo/db/s/resharding/resharding_oplog_batch_applier.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/local_catalog/clustered_collection_options_gen.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/s/migration_chunk_cloner_source_op_observer.h"
#include "mongo/db/s/resharding/donor_oplog_id_gen.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_metrics.h"
#include "mongo/db/s/resharding/resharding_noop_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_oplog_application.h"
#include "mongo/db/s/resharding/resharding_oplog_applier_metrics.h"
#include "mongo/db/s/resharding/resharding_oplog_session_application.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/vector_clock/vector_clock_metadata_hook.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

class ReshardingOplogBatchApplierTest : service_context_test::WithSetupTransportLayer,
                                        public ServiceContextMongoDTest {
public:
    ReshardingOplogBatchApplierTest()
        : ServiceContextMongoDTest(
              Options{}.useMockClock(true).useMockTickSource<Milliseconds>(true)) {}

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        advanceTime(Seconds(100));

        auto serviceContext = getServiceContext();
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

            // OpObserverImpl is required for timestamping the writes from
            // ReshardingOplogApplicationRules.
            auto opObserverRegistry =
                dynamic_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
            invariant(opObserverRegistry);

            opObserverRegistry->addObserver(
                std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
            opObserverRegistry->addObserver(
                std::make_unique<MigrationChunkClonerSourceOpObserver>());
        }

        {
            auto opCtx = makeOperationContext();

            for (const auto& nss : {_outputNss, _myStashNss, _otherStashNss}) {
                resharding::data_copy::ensureCollectionExists(
                    opCtx.get(), nss, CollectionOptions{});
            }

            _metrics =
                ReshardingMetrics::makeInstance_forTest(UUID::gen(),
                                                        BSON("y" << 1),
                                                        _outputNss,
                                                        ReshardingMetricsCommon::Role::kRecipient,
                                                        serviceContext->getFastClockSource()->now(),
                                                        serviceContext);
            _metrics->registerDonors({_myDonorId});

            _applierMetrics = std::make_unique<ReshardingOplogApplierMetrics>(
                _myDonorId, _metrics.get(), boost::none);

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

    ShardId getMyDonorShardId() {
        return _myDonorId;
    }

    ReshardingMetrics* metrics() {
        return _metrics.get();
    }

    ReshardingOplogBatchApplier* applier() {
        return _batchApplier.get();
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

    std::shared_ptr<executor::ThreadPoolTaskExecutor> makeTaskExecutorForApplier() {
        // The ReshardingOplogBatchApplier expects there to already be a Client associated with the
        // thread from the thread pool. We set up the ThreadPoolTaskExecutor identically to how the
        // recipient's primary-only service is set up.
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.maxThreads = 1;
        threadPoolOptions.threadNamePrefix = "TestReshardOplogBatchApplier-";
        threadPoolOptions.poolName = "TestReshardOplogBatchApplierThreadPool";
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
        txnParticipant.beginOrContinue(opCtx,
                                       {txnNumber},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);

        txnParticipant.unstashTransactionResources(opCtx, "prepareTransaction");

        // The transaction machinery cannot store an empty locker.
        {
            Lock::GlobalLock globalLock(opCtx, MODE_IX);
        }
        auto opTime = [opCtx] {
            TransactionParticipant::SideTransactionBlock sideTxn{opCtx};

            WriteUnitOfWork wuow{opCtx};
            auto opTime = repl::getNextOpTime(opCtx);
            wuow.release();

            shard_role_details::getRecoveryUnit(opCtx)->abortUnitOfWork();
            shard_role_details::getLocker(opCtx)->endWriteUnitOfWork();

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
        txnParticipant.beginOrContinue(opCtx,
                                       {txnNumber},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kContinue);

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

    repl::OplogEntry makeProgressMarkNoopOplogEntry(Date_t wallClockTime,
                                                    bool createdAfterOplogApplicationStarted) {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kNoop);
        op.setObject({});

        ReshardProgressMarkO2Field o2Field;
        o2Field.setType(resharding::kReshardProgressMarkOpLogType);
        if (createdAfterOplogApplicationStarted) {
            o2Field.setCreatedAfterOplogApplicationStarted(true);
        }
        op.setObject2(o2Field.toBSON());
        op.setNss({});
        op.setOpTime({{}, {}});
        op.setWallClockTime(wallClockTime);

        return {op.toBSON()};
    }

    repl::OplogEntry makeFinalNoopOplogEntry() {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kNoop);
        op.setObject({});

        ReshardBlockingWritesChangeEventO2Field o2Field;
        o2Field.setType(resharding::kReshardFinalOpLogType);
        o2Field.setReshardBlockingWrites({});
        o2Field.setReshardingUUID(UUID::gen());
        op.setObject2(o2Field.toBSON());

        op.setNss({});
        op.setOpTime({{}, {}});
        op.setWallClockTime({});

        return {op.toBSON()};
    }

    repl::OplogEntry makeGenericNoopOplogEntry() {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kNoop);
        op.setObject({});
        op.setNss({});
        op.setOpTime({{}, {}});
        op.setWallClockTime({});
        return {op.toBSON()};
    }

    repl::OplogEntry makeInsertOplogEntry() {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kInsert);
        op.setNss(_outputNss);
        op.setObject(BSON("_id" << 1));
        op.setTimestamp({});
        op.setWallClockTime({});
        return {op.toBSON()};
    }

    repl::OplogEntry makeUpdateOplogEntry() {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kUpdate);
        op.setNss(_outputNss);
        op.setObject(BSON("_id" << 1 << "x" << 1));
        op.setObject2(BSON("_id" << 1));
        op.setTimestamp({});
        op.setWallClockTime({});
        return {op.toBSON()};
    }

    repl::OplogEntry makeDeleteOplogEntry() {
        repl::MutableOplogEntry op;
        op.setOpType(repl::OpTypeEnum::kDelete);
        op.setNss(_outputNss);
        op.setObject(BSON("_id" << 1));
        op.setTimestamp({});
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
                                               false, /* unsplittable */
                                               nullptr /* defaultCollator */,
                                               false /* unique */,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               true /* allowMigrations */,
                                               chunks);

        return ChunkManager(makeStandaloneRoutingTableHistory(std::move(rt)),
                            boost::none /* clusterTime */);
    }

    RoutingTableHistoryValueHandle makeStandaloneRoutingTableHistory(RoutingTableHistory rt) {
        const auto version = rt.getVersion();
        return RoutingTableHistoryValueHandle(
            std::make_shared<RoutingTableHistory>(std::move(rt)),
            ComparableChunkVersion::makeComparableChunkVersion(version));
    }

    const StringData _currentShardKey = "sk";

    const NamespaceString _sourceNss =
        NamespaceString::createNamespaceString_forTest("test_crud", "collection_being_resharded");
    const UUID _sourceUUID = UUID::gen();

    const ShardId _myDonorId{"myDonorId"};
    const ShardId _otherDonorId{"otherDonorId"};

    const NamespaceString _outputNss =
        resharding::constructTemporaryReshardingNss(_sourceNss, _sourceUUID);
    const NamespaceString _myStashNss =
        resharding::getLocalConflictStashNamespace(_sourceUUID, _myDonorId);
    const NamespaceString _otherStashNss =
        resharding::getLocalConflictStashNamespace(_sourceUUID, _otherDonorId);
    const NamespaceString _myOplogBufferNss =
        resharding::getLocalOplogBufferNamespace(_sourceUUID, _myDonorId);

    service_context_test::ShardRoleOverride _shardRole;

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

TEST_F(ReshardingOplogBatchApplierTest,
       NotThrowUponSeeingProgressMarkOplogCreatedAfterOplogApplicationStarted) {
    auto executor = makeTaskExecutorForApplier();
    auto factory = makeCancelableOpCtxForApplier(CancellationToken::uncancelable());

    auto oplogEntry =
        makeProgressMarkNoopOplogEntry(now(), true /* createdAfterOplogApplicationStarted */);

    auto future = applier()->applyBatch<false>(
        {&oplogEntry}, executor, CancellationToken::uncancelable(), factory);
    future.get();
}

DEATH_TEST_F(ReshardingOplogBatchApplierTest,
             ThrowUponSeeingProgressMarkOplogCreatedBeforeOplogApplicationStarted,
             "invariant") {
    auto executor = makeTaskExecutorForApplier();
    auto factory = makeCancelableOpCtxForApplier(CancellationToken::uncancelable());

    auto oplogEntry =
        makeProgressMarkNoopOplogEntry(now(), false /* createdAfterOplogApplicationStarted */);

    auto future = applier()->applyBatch<false>(
        {&oplogEntry}, executor, CancellationToken::uncancelable(), factory);
    future.get();
}

DEATH_TEST_F(ReshardingOplogBatchApplierTest, ThrowUponSeeingFinalOplog, "invariant") {
    auto executor = makeTaskExecutorForApplier();
    auto factory = makeCancelableOpCtxForApplier(CancellationToken::uncancelable());

    auto oplogEntry = makeFinalNoopOplogEntry();

    auto future = applier()->applyBatch<false>(
        {&oplogEntry}, executor, CancellationToken::uncancelable(), factory);
    future.get();
}

DEATH_TEST_F(ReshardingOplogBatchApplierTest, ThrowUponSeeingGenericNoop, "invariant") {
    auto executor = makeTaskExecutorForApplier();
    auto factory = makeCancelableOpCtxForApplier(CancellationToken::uncancelable());

    auto oplogEntry = makeGenericNoopOplogEntry();

    auto future = applier()->applyBatch<false>(
        {&oplogEntry}, executor, CancellationToken::uncancelable(), factory);
    future.get();
}

}  // namespace
}  // namespace mongo
