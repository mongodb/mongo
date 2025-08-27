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


#include "mongo/db/s/resharding/resharding_txn_cloner.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_loader.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_loader_mock.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/optime_with.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding/resharding_txn_cloner_progress_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_mongod_test_fixture.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/vector_clock/vector_clock_metadata_hook.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

class ReshardingTxnClonerTest : service_context_test::WithSetupTransportLayer,
                                public ShardServerTestFixtureWithCatalogCacheLoaderMock {
    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheLoaderMock::setUp();

        // The config database's primary shard is always config, and it is always sharded.
        getCatalogCacheLoaderMock()->setDatabaseRefreshReturnValue(DatabaseType{
            DatabaseName::kConfig, ShardId::kConfigServerId, DatabaseVersion::makeFixed()});

        // The config.transactions collection is always unsharded.
        getCatalogCacheLoaderMock()->setCollectionRefreshReturnValue(
            {ErrorCodes::NamespaceNotFound, "collection not found"});

        for (const auto& shardId : kTwoShardIdList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
        }

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        // onStepUp() relies on the storage interface to create the config.transactions table.
        repl::StorageInterface::set(getServiceContext(),
                                    std::make_unique<repl::StorageInterfaceImpl>());
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(operationContext());
        mongoDSessionCatalog->onStepUp(operationContext());
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());

        _executor = makeTaskExecutorForCloner();
        _executor->startup();

        _threadPool = std::make_shared<ThreadPool>([] {
            ThreadPool::Options options;
            options.poolName = "TestReshardCloneConfigTransactionsCancelableOpCtxPool";
            options.minThreads = 1;
            options.maxThreads = 1;
            return options;
        }());
        _threadPool->startup();
    }

    void tearDown() override {
        _executor->shutdown();
        _executor->join();

        _threadPool->shutdown();
        _threadPool->join();

        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardServerTestFixtureWithCatalogCacheLoaderMock::tearDown();
    }

    /**
     * Override the CatalogClient to make CatalogClient::getAllShards automatically return the
     * expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
     * ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
     * DBClientMock analogous to the NetworkInterfaceMock.
     */
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardId> shardIds) : _shardIds(std::move(shardIds)) {}

            repl::OpTimeWith<std::vector<ShardType>> getAllShards(
                OperationContext* opCtx,
                repl::ReadConcernLevel readConcern,
                BSONObj filter) override {
                std::vector<ShardType> shardTypes;
                for (const auto& shardId : _shardIds) {
                    const ConnectionString cs = ConnectionString::forReplicaSet(
                        shardId.toString(), {makeHostAndPort(shardId)});
                    ShardType sType;
                    sType.setName(cs.getSetName());
                    sType.setHost(cs.toString());
                    shardTypes.push_back(std::move(sType));
                };
                return repl::OpTimeWith<std::vector<ShardType>>(shardTypes);
            }

        private:
            const std::vector<ShardId> _shardIds;
        };

        return std::make_unique<StaticCatalogClient>(kTwoShardIdList);
    }

protected:
    const UUID kDefaultReshardingId = UUID::gen();
    const std::vector<ShardId> kTwoShardIdList{kMyShardName, {"otherShardName"}};
    const std::vector<ReshardingSourceId> kTwoSourceIdList = {
        {kDefaultReshardingId, kMyShardName}, {kDefaultReshardingId, ShardId("otherShardName")}};

    const std::vector<DurableTxnStateEnum> kDurableTxnStateEnumValues = {
        DurableTxnStateEnum::kPrepared,
        DurableTxnStateEnum::kCommitted,
        DurableTxnStateEnum::kAborted,
        DurableTxnStateEnum::kInProgress};

    std::vector<boost::optional<DurableTxnStateEnum>> getDurableTxnStatesAndBoostNone() {
        std::vector<boost::optional<DurableTxnStateEnum>> statesAndBoostNone = {boost::none};
        for (auto state : kDurableTxnStateEnumValues) {
            statesAndBoostNone.push_back(state);
        }
        return statesAndBoostNone;
    }

    BSONObj makeTxn(boost::optional<DurableTxnStateEnum> multiDocTxnState = boost::none) {
        auto txn = SessionTxnRecord(
            makeLogicalSessionIdForTest(), 0, repl::OpTime(Timestamp::min(), 0), Date_t());
        txn.setState(multiDocTxnState);
        return txn.toBSON();
    }

    LogicalSessionId getTxnRecordLsid(BSONObj txnRecord) {
        return SessionTxnRecord::parse(txnRecord, IDLParserContext("ReshardingTxnClonerTest"))
            .getSessionId();
    }

    std::vector<BSONObj> makeSortedTxns(int numTxns) {
        std::vector<BSONObj> txns;
        for (int i = 0; i < numTxns; i++) {
            txns.emplace_back(makeTxn());
        }
        std::sort(txns.begin(), txns.end(), [this](BSONObj a, BSONObj b) {
            return getTxnRecordLsid(a).toBSON().woCompare(getTxnRecordLsid(b).toBSON()) < 0;
        });
        return txns;
    }

    void onCommandReturnTxnBatch(std::vector<BSONObj> firstBatch,
                                 CursorId cursorId,
                                 bool isFirstBatch) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            return CursorResponse(
                       NamespaceString::kSessionTransactionsTableNamespace, cursorId, firstBatch)
                .toBSON(isFirstBatch ? CursorResponse::ResponseType::InitialResponse
                                     : CursorResponse::ResponseType::SubsequentResponse);
        });
    }

    void onCommandReturnTxns(std::vector<BSONObj> firstBatch, std::vector<BSONObj> secondBatch) {
        CursorId cursorId(0);
        if (secondBatch.size() > 0) {
            cursorId = 123;
        }
        onCommand([&](const executor::RemoteCommandRequest& request) {
            return CursorResponse(
                       NamespaceString::kSessionTransactionsTableNamespace, cursorId, firstBatch)
                .toBSON(CursorResponse::ResponseType::InitialResponse);
        });

        if (secondBatch.size() == 0) {
            return;
        }

        onCommand([&](const executor::RemoteCommandRequest& request) {
            return CursorResponse(NamespaceString::kSessionTransactionsTableNamespace,
                                  CursorId{0},
                                  secondBatch)
                .toBSON(CursorResponse::ResponseType::SubsequentResponse);
        });
    }

    void onKillCursorsRequest(CursorId cursorId) {
        // Handle the 'killCursors' command.
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT(request.cmdObj["killCursors"]);
            auto cursors = request.cmdObj["cursors"];
            ASSERT_EQ(cursors.type(), BSONType::array);
            auto cursorsArray = cursors.Array();
            ASSERT_FALSE(cursorsArray.empty());
            ASSERT_EQ(cursorsArray[0].Long(), cursorId);
            // The AsyncResultsMerger doesn't actually inspect the response of the killCursors, so
            // we don't have to put anything except {ok: true}.
            return BSON("ok" << true);
        });
    }

    void seedTransactionOnRecipient(LogicalSessionId sessionId,
                                    TxnNumber txnNum,
                                    bool multiDocTxn) {
        auto opCtx = operationContext();
        opCtx->setLogicalSessionId(sessionId);
        opCtx->setTxnNumber(txnNum);

        if (multiDocTxn) {
            opCtx->setInMultiDocumentTransaction();
        }

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT(txnParticipant);
        if (multiDocTxn) {
            txnParticipant.beginOrContinue(opCtx,
                                           {txnNum},
                                           false /* autocommit */,
                                           TransactionParticipant::TransactionActions::kStart);
        } else {
            txnParticipant.beginOrContinue(opCtx,
                                           {txnNum},
                                           boost::none /* autocommit */,
                                           TransactionParticipant::TransactionActions::kNone);
        }
    }

    void checkTxnHasBeenUpdated(LogicalSessionId sessionId, TxnNumber txnNum) {
        DBDirectClient client(operationContext());
        // The same logical session entry may be inserted more than once by a test case, so use a
        // $natural sort to find the most recently inserted entry.
        FindCommandRequest findCmd{NamespaceString::kRsOplogNamespace};
        findCmd.setFilter(BSON(repl::OplogEntryBase::kSessionIdFieldName << sessionId.toBSON()));
        findCmd.setSort(BSON("$natural" << -1));
        auto bsonOplog = client.findOne(std::move(findCmd));
        ASSERT(!bsonOplog.isEmpty());
        auto oplogEntry = repl::MutableOplogEntry::parse(bsonOplog).getValue();
        ASSERT_EQ(oplogEntry.getTxnNumber().value(), txnNum);
        ASSERT_BSONOBJ_EQ(oplogEntry.getObject(), BSON("$sessionMigrateInfo" << 1));
        ASSERT_BSONOBJ_EQ(oplogEntry.getObject2().value(), BSON("$incompleteOplogHistory" << 1));
        ASSERT(oplogEntry.getOpType() == repl::OpTypeEnum::kNoop);

        auto bsonTxn =
            client.findOne(NamespaceString::kSessionTransactionsTableNamespace,
                           BSON(SessionTxnRecord::kSessionIdFieldName << sessionId.toBSON()));
        ASSERT(!bsonTxn.isEmpty());
        auto txn = SessionTxnRecord::parse(
            bsonTxn, IDLParserContext("resharding config transactions cloning test"));
        ASSERT_EQ(txn.getTxnNum(), txnNum);
        ASSERT_EQ(txn.getLastWriteOpTime(), oplogEntry.getOpTime());
    }

    void checkTxnHasNotBeenUpdated(LogicalSessionId sessionId, TxnNumber txnNum) {
        auto opCtx = operationContext();
        opCtx->setLogicalSessionId(sessionId);
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);

        DBDirectClient client(operationContext());
        auto bsonOplog =
            client.findOne(NamespaceString::kRsOplogNamespace,
                           BSON(repl::OplogEntryBase::kSessionIdFieldName << sessionId.toBSON()));

        ASSERT_BSONOBJ_EQ(bsonOplog, {});
        ASSERT_EQ(txnParticipant.getActiveTxnNumberAndRetryCounter().getTxnNumber(), txnNum);
    }

    boost::optional<ReshardingTxnClonerProgress> getTxnCloningProgress(
        const ReshardingSourceId& sourceId) {
        DBDirectClient client(operationContext());
        auto progressDoc = client.findOne(
            NamespaceString::kReshardingTxnClonerProgressNamespace,
            BSON(ReshardingTxnClonerProgress::kSourceIdFieldName << sourceId.toBSON()));

        if (progressDoc.isEmpty()) {
            return boost::none;
        }

        return ReshardingTxnClonerProgress::parse(progressDoc,
                                                  IDLParserContext("ReshardingTxnClonerProgress"));
    }

    boost::optional<LogicalSessionId> getProgressLsid(const ReshardingSourceId& sourceId) {
        auto progressDoc = getTxnCloningProgress(sourceId);
        return progressDoc ? boost::optional<LogicalSessionId>(progressDoc->getProgress())
                           : boost::none;
    }

    std::shared_ptr<MongoProcessInterface> makeMongoProcessInterface() {
        return std::make_shared<ShardServerProcessInterface>(
            Grid::get(getServiceContext())->getExecutorPool()->getFixedExecutor());
    }

    ExecutorFuture<void> runCloner(
        ReshardingTxnCloner& cloner,
        boost::optional<CancellationToken> customCancelToken = boost::none) {
        // Allows callers to control the cancellation of the cloner's run() function when specified.
        auto cancelToken = customCancelToken.has_value()
            ? customCancelToken.value()
            : operationContext()->getCancellationToken();

        CancelableOperationContextFactory opCtxFactory(cancelToken, _threadPool);

        // There isn't a guarantee that the reference count to `executor` has been decremented after
        // .run() returns. We schedule a trivial task on the task executor to ensure the callback's
        // destructor has run. Otherwise `executor` could end up outliving the ServiceContext and
        // triggering an invariant due to the task executor's thread having a Client still.
        return cloner
            .run(_executor,
                 _executor,
                 cancelToken,
                 std::move(opCtxFactory),
                 makeMongoProcessInterface())
            .thenRunOn(_executor)
            .onCompletion([](auto x) { return x; });
    }

    void makeInProgressTxn(OperationContext* opCtx, LogicalSessionId lsid, TxnNumber txnNumber) {
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

        txnParticipant.unstashTransactionResources(opCtx, "insert");
        txnParticipant.stashTransactionResources(opCtx);
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

    void abortTxn(OperationContext* opCtx, LogicalSessionId lsid, TxnNumber txnNumber) {
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

    Timestamp getLatestOplogTimestamp(OperationContext* opCtx) {
        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        LocalOplogInfo* oplogInfo = LocalOplogInfo::get(opCtx);

        // Oplog should be available in this test.
        invariant(oplogInfo);
        storageEngine->waitForAllEarlierOplogWritesToBeVisible(opCtx, oplogInfo->getRecordStore());
        DBDirectClient client(opCtx);

        FindCommandRequest findRequest{NamespaceString::kRsOplogNamespace};
        findRequest.setSort(BSON("ts" << -1));
        findRequest.setLimit(1);
        auto cursor = client.find(findRequest);
        ASSERT_TRUE(cursor->more());
        const auto oplogEntry = cursor->next();

        return oplogEntry.getField("ts").timestamp();
    }

    std::vector<repl::DurableOplogEntry> findOplogEntriesNewerThan(OperationContext* opCtx,
                                                                   Timestamp ts) {
        std::vector<repl::DurableOplogEntry> result;

        auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
        LocalOplogInfo* oplogInfo = LocalOplogInfo::get(opCtx);

        // Oplog should be available in this test.
        invariant(oplogInfo);
        storageEngine->waitForAllEarlierOplogWritesToBeVisible(opCtx, oplogInfo->getRecordStore());
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

    std::shared_ptr<executor::ThreadPoolTaskExecutor> getExecutor() {
        return _executor;
    }

private:
    std::shared_ptr<executor::ThreadPoolTaskExecutor> makeTaskExecutorForCloner() {
        // The ReshardingTxnCloner expects there to already be a Client associated with the thread
        // from the thread pool. We set up the ThreadPoolTaskExecutor identically to how the
        // recipient's primary-only service is set up.
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.maxThreads = 1;
        threadPoolOptions.threadNamePrefix = "TestReshardCloneConfigTransactions-";
        threadPoolOptions.poolName = "TestReshardCloneConfigTransactionsThreadPool";
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
                "TestReshardCloneConfigTransactionsNetwork", nullptr, std::move(hookList)));

        return executor;
    }

    static HostAndPort makeHostAndPort(const ShardId& shardId) {
        return HostAndPort(str::stream() << shardId << ":123");
    }

    RAIIServerParameterControllerForTest controller{"reshardingTxnClonerProgressBatchSize", 1};

    std::shared_ptr<executor::ThreadPoolTaskExecutor> _executor;
    std::shared_ptr<ThreadPool> _threadPool;
};

TEST_F(ReshardingTxnClonerTest, MergeTxnNotOnRecipient) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = runCloner(cloner);

        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber txnNum = 3;

        auto txn = SessionTxnRecord(sessionId, txnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        auto status = future.getNoThrow();
        ASSERT_OK(status) << (state ? DurableTxnState_serializer(*state) : "retryable write");

        checkTxnHasBeenUpdated(sessionId, txnNum);
    }
}

TEST_F(ReshardingTxnClonerTest, MergeUnParsableTxn) {
    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    auto future = runCloner(cloner);

    const auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNum = 3;
    onCommandReturnTxns({SessionTxnRecord(sessionId, txnNum, repl::OpTime(), Date_t::now())
                             .toBSON()
                             .removeField(SessionTxnRecord::kSessionIdFieldName)},
                        {});

    auto status = future.getNoThrow();
    ASSERT_EQ(status, ErrorCodes::IDLFailedToParse);
}

TEST_F(ReshardingTxnClonerTest, MergeNewTxnOverMultiDocTxn) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber donorTxnNum = 3;
        TxnNumber recipientTxnNum = donorTxnNum - 1;

        seedTransactionOnRecipient(sessionId, recipientTxnNum, true);

        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = runCloner(cloner);

        auto txn = SessionTxnRecord(sessionId, donorTxnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        auto status = future.getNoThrow();
        ASSERT_OK(status) << (state ? DurableTxnState_serializer(*state) : "retryable write");

        checkTxnHasBeenUpdated(sessionId, donorTxnNum);
    }
}

TEST_F(ReshardingTxnClonerTest, MergeNewTxnOverRetryableWriteTxn) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber donorTxnNum = 3;
        TxnNumber recipientTxnNum = donorTxnNum - 1;

        seedTransactionOnRecipient(sessionId, recipientTxnNum, false);

        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = runCloner(cloner);

        auto txn = SessionTxnRecord(sessionId, donorTxnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        auto status = future.getNoThrow();
        ASSERT_OK(status) << (state ? DurableTxnState_serializer(*state) : "retryable write");

        checkTxnHasBeenUpdated(sessionId, donorTxnNum);
    }
}

TEST_F(ReshardingTxnClonerTest, MergeCurrentTxnOverRetryableWriteTxn) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber txnNum = 3;

        seedTransactionOnRecipient(sessionId, txnNum, false);

        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = runCloner(cloner);

        auto txn = SessionTxnRecord(sessionId, txnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        auto status = future.getNoThrow();
        ASSERT_OK(status) << (state ? DurableTxnState_serializer(*state) : "retryable write");

        checkTxnHasBeenUpdated(sessionId, txnNum);
    }
}

TEST_F(ReshardingTxnClonerTest, MergeCurrentTxnOverMultiDocTxn) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber txnNum = 3;

        seedTransactionOnRecipient(sessionId, txnNum, true);

        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = runCloner(cloner);

        auto txn = SessionTxnRecord(sessionId, txnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        auto status = future.getNoThrow();
        ASSERT_OK(status) << (state ? DurableTxnState_serializer(*state) : "retryable write");

        checkTxnHasNotBeenUpdated(sessionId, txnNum);
    }
}


TEST_F(ReshardingTxnClonerTest, MergeOldTxnOverTxn) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber recipientTxnNum = 3;
        TxnNumber donorTxnNum = recipientTxnNum - 1;

        seedTransactionOnRecipient(sessionId, recipientTxnNum, false);

        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = runCloner(cloner);

        auto txn = SessionTxnRecord(sessionId, donorTxnNum, repl::OpTime(), Date_t::now());
        txn.setState(state);
        onCommandReturnTxns({txn.toBSON()}, {});

        auto status = future.getNoThrow();
        ASSERT_OK(status) << (state ? DurableTxnState_serializer(*state) : "retryable write");

        checkTxnHasNotBeenUpdated(sessionId, recipientTxnNum);
    }
}

TEST_F(ReshardingTxnClonerTest, MergeMultiDocTransactionAndRetryableWrite) {
    const auto sessionIdRetryableWrite = makeLogicalSessionIdForTest();
    const auto sessionIdMultiDocTxn = makeLogicalSessionIdForTest();
    TxnNumber txnNum = 3;

    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    auto future = runCloner(cloner);

    auto sessionRecordRetryableWrite =
        SessionTxnRecord(sessionIdRetryableWrite, txnNum, repl::OpTime(), Date_t::now());
    auto sessionRecordMultiDocTxn =
        SessionTxnRecord(sessionIdMultiDocTxn, txnNum, repl::OpTime(), Date_t::now());
    sessionRecordMultiDocTxn.setState(DurableTxnStateEnum::kAborted);

    onCommandReturnTxns({sessionRecordRetryableWrite.toBSON(), sessionRecordMultiDocTxn.toBSON()},
                        {});

    auto status = future.getNoThrow();
    ASSERT_OK(status);

    checkTxnHasBeenUpdated(sessionIdRetryableWrite, txnNum);
    checkTxnHasBeenUpdated(sessionIdMultiDocTxn, txnNum);
}

/**
 * Test that the ReshardingTxnCloner stops processing batches when canceled via cancelToken.
 */
TEST_F(ReshardingTxnClonerTest, ClonerOneBatchThenCanceled) {
    const auto txns = makeSortedTxns(4);
    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    auto opCtxToken = operationContext()->getCancellationToken();
    auto cancelSource = CancellationSource(opCtxToken);
    auto future = runCloner(cloner, cancelSource.token());

    onCommand([&](const executor::RemoteCommandRequest& request) {
        cancelSource.cancel();

        return CursorResponse(NamespaceString::kSessionTransactionsTableNamespace,
                              CursorId{123},
                              std::vector<BSONObj>(txns.begin(), txns.begin() + 2))
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    auto status = future.getNoThrow();
    ASSERT_EQ(status.code(), ErrorCodes::CallbackCanceled);
}

TEST_F(ReshardingTxnClonerTest, ClonerStoresProgressSingleBatch) {
    const auto txns = makeSortedTxns(2);
    const auto lastLsid = getTxnRecordLsid(txns.back());

    {
        ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
        ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[1]));

        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = runCloner(cloner);

        onCommandReturnTxnBatch(txns, CursorId{0}, true /* isFirstBatch */);

        auto status = future.getNoThrow();
        ASSERT_OK(status);

        ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
        ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), lastLsid);
    }

    // Repeat with a different shard to verify multiple progress docs can exist at once for a
    // resharding operation.
    {
        ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
        ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), lastLsid);

        ReshardingTxnCloner cloner(kTwoSourceIdList[0], Timestamp::max());
        auto future = runCloner(cloner);

        onCommandReturnTxnBatch(txns, CursorId{0}, true /* isFirstBatch */);

        auto status = future.getNoThrow();
        ASSERT_OK(status);

        ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[0]), lastLsid);
        ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), lastLsid);
    }

    // Verify each progress document is scoped to a single resharding operation.
    ASSERT_FALSE(getTxnCloningProgress(ReshardingSourceId(UUID::gen(), kTwoShardIdList[0])));
    ASSERT_FALSE(getTxnCloningProgress(ReshardingSourceId(UUID::gen(), kTwoShardIdList[1])));
}

TEST_F(ReshardingTxnClonerTest, ClonerStoresProgressMultipleBatches) {
    const auto txns = makeSortedTxns(4);
    const auto firstLsid = getTxnRecordLsid(txns[1]);
    const auto lastLsid = getTxnRecordLsid(txns.back());

    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[1]));

    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    auto cancelSource = CancellationSource(operationContext()->getCancellationToken());
    auto future = runCloner(cloner, cancelSource.token());

    // The progress document is updated asynchronously after the session record is updated. We fake
    // the cloning operation being canceled to inspect the progress document after the first batch
    // has been processed.
    onCommandReturnTxnBatch(std::vector<BSONObj>(txns.begin(), txns.begin() + 2),
                            CursorId{123},
                            true /* isFirstBatch */);
    onCommand([&](const executor::RemoteCommandRequest& request) {
        // Simulate a stepdown.
        cancelSource.cancel();

        // With a non-mock network, disposing of the pipeline upon cancellation would also cancel
        // the original request.
        return Status{ErrorCodes::CallbackCanceled, "Simulate cancellation"};
    });
    auto status = future.getNoThrow();
    ASSERT_EQ(status, ErrorCodes::CallbackCanceled);

    onKillCursorsRequest(CursorId{123});

    // After the first batch, the progress document should contain the lsid of the last document in
    // that batch.
    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
    ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), firstLsid);

    // Now we run the cloner again and give it the remaining documents.
    future = runCloner(cloner);

    onCommandReturnTxnBatch(
        std::vector<BSONObj>(txns.begin() + 1, txns.end()), CursorId{0}, false /* isFirstBatch */);

    status = future.getNoThrow();
    ASSERT_OK(status);

    // After the second and final batch, the progress document should have been updated to the final
    // overall lsid.
    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
    ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), lastLsid);
}

TEST_F(ReshardingTxnClonerTest, ClonerStoresProgressResume) {
    const auto txns = makeSortedTxns(4);
    const auto firstLsid = getTxnRecordLsid(txns.front());
    const auto lastLsid = getTxnRecordLsid(txns.back());

    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[1]));

    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    auto cancelSource = CancellationSource(operationContext()->getCancellationToken());
    auto future = runCloner(cloner, cancelSource.token());

    onCommandReturnTxnBatch({txns.front()}, CursorId{123}, true /* isFirstBatch */);

    // Simulate the cloning operation being canceled as the batch was being filled.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        // The progress document is updated asynchronously after the session record is updated but
        // is guaranteed to have been updated before the next getMore request is sent.
        ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
        ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), firstLsid);

        // Simulate a stepdown.
        cancelSource.cancel();

        // With a non-mock network, disposing of the pipeline upon cancellation would also cancel
        // the original request.
        return Status{ErrorCodes::CallbackCanceled, "Simulate cancellation"};
    });

    auto status = future.getNoThrow();
    ASSERT_EQ(status, ErrorCodes::CallbackCanceled);

    onKillCursorsRequest(CursorId{123});

    // The stored progress should be unchanged.
    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
    ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), firstLsid);

    // Simulate a resume on the new primary by creating a new fetcher that resumes after the lsid in
    // the progress document.
    future = runCloner(cloner);

    BSONObj cmdObj;
    onCommand([&](const executor::RemoteCommandRequest& request) {
        cmdObj = request.cmdObj.getOwned();
        return CursorResponse(NamespaceString::kSessionTransactionsTableNamespace,
                              CursorId{0},
                              std::vector<BSONObj>(txns.begin() + 1, txns.end()))
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    status = future.getNoThrow();
    ASSERT_OK(status);

    // The aggregation request should include the progress lsid to resume from.
    ASSERT_STRING_CONTAINS(cmdObj.toString(), firstLsid.toBSON().toString());

    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
    ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), lastLsid);

    for (const auto& txn : txns) {
        // Note 0 is the default txnNumber used for txn records made by makeSortedTxns()
        checkTxnHasBeenUpdated(getTxnRecordLsid(txn), 0);
    }

    // Simulate a resume after a rollback of an update to the progress document by creating a new
    // fetcher that resumes at the lsid from the first progress document. This verifies cloning is
    // idempotent on the cloning shard.
    cloner.updateProgressDocument_forTest(operationContext(), firstLsid);
    future = runCloner(cloner);

    onCommand([&](const executor::RemoteCommandRequest& request) {
        cmdObj = request.cmdObj.getOwned();
        return CursorResponse(NamespaceString::kSessionTransactionsTableNamespace,
                              CursorId{0},
                              std::vector<BSONObj>(txns.begin() + 1, txns.end()))
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    status = future.getNoThrow();
    ASSERT_OK(status);

    // The aggregation request should have included the previous progress lsid to resume from.
    ASSERT_STRING_CONTAINS(cmdObj.toString(), firstLsid.toBSON().toString());

    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
    ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), lastLsid);

    for (const auto& txn : txns) {
        // Note 0 is the default txnNumber used for txn records made by makeSortedTxns()
        checkTxnHasBeenUpdated(getTxnRecordLsid(txn), 0);
    }
}

TEST_F(ReshardingTxnClonerTest, ClonerDoesNotUpdateProgressOnEmptyBatch) {
    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));

    ReshardingTxnCloner cloner(kTwoSourceIdList[0], Timestamp::max());
    auto future = runCloner(cloner);

    onCommandReturnTxnBatch({}, CursorId{0}, true /* isFirstBatch */);

    auto status = future.getNoThrow();
    ASSERT_OK(status);

    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
}

TEST_F(ReshardingTxnClonerTest, WaitsOnPreparedTxnAndAutomaticallyRetries) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber existingTxnNumber = 100;
    auto opTime = makePreparedTxn(operationContext(), lsid, existingTxnNumber);

    TxnNumber incomingTxnNumber = existingTxnNumber + 1;
    auto sessionTxnRecord = SessionTxnRecord{lsid, incomingTxnNumber, repl::OpTime{}, Date_t{}};

    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    CancellationSource cancelSource;
    auto future = runCloner(cloner, cancelSource.token());

    onCommandReturnTxns({sessionTxnRecord.toBSON()}, {});

    ASSERT_FALSE(future.isReady());
    // Wait a little bit to increase the likelihood that the cloner has blocked on the prepared
    // transaction before the transaction is aborted.
    ASSERT_OK(
        getExecutor()->sleepFor(Milliseconds{200}, CancellationToken::uncancelable()).getNoThrow());
    ASSERT_FALSE(future.isReady());

    abortTxn(operationContext(), lsid, existingTxnNumber);

    ASSERT_OK(future.getNoThrow());

    {
        auto foundOps = findOplogEntriesNewerThan(operationContext(), opTime.getTimestamp());
        ASSERT_GTE(foundOps.size(), 2U);
        // The first oplog entry is from aborting the prepared transaction via abortTxn().
        // The second oplog entry is from the session txn record being updated by
        // ReshardingTxnCloner.
        checkGeneratedNoop(foundOps[1], lsid, incomingTxnNumber, {kIncompleteHistoryStmtId});

        auto sessionTxnRecord = findSessionRecord(operationContext(), lsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[1]);
    }
}

TEST_F(ReshardingTxnClonerTest, CancelableWhileWaitingOnPreparedTxn) {
    auto lsid = makeLogicalSessionIdForTest();

    TxnNumber existingTxnNumber = 100;
    makePreparedTxn(operationContext(), lsid, existingTxnNumber);
    ON_BLOCK_EXIT([&] { abortTxn(operationContext(), lsid, existingTxnNumber); });

    TxnNumber incomingTxnNumber = existingTxnNumber + 1;
    auto sessionTxnRecord = SessionTxnRecord{lsid, incomingTxnNumber, repl::OpTime{}, Date_t{}};

    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    CancellationSource cancelSource;
    auto future = runCloner(cloner, cancelSource.token());

    onCommandReturnTxns({sessionTxnRecord.toBSON()}, {});

    ASSERT_FALSE(future.isReady());
    // Wait a little bit to increase the likelihood that the applier has blocked on the prepared
    // transaction before the cancellation source is canceled.
    ASSERT_OK(
        getExecutor()->sleepFor(Milliseconds{200}, CancellationToken::uncancelable()).getNoThrow());
    ASSERT_FALSE(future.isReady());

    cancelSource.cancel();
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::CallbackCanceled);
}

TEST_F(ReshardingTxnClonerTest,
       WaitsOnInProgressInternalTxnForRetryableWriteAndAutomaticallyRetries) {
    auto retryableWriteLsid = makeLogicalSessionIdForTest();
    TxnNumber retryableWriteTxnNumber = 100;
    auto internalTxnLsid = makeLogicalSessionIdWithTxnNumberAndUUIDForTest(retryableWriteLsid,
                                                                           retryableWriteTxnNumber);
    TxnNumber internalTxnTxnNumber = 1;

    // Make two in progress transactions so the one started by resharding must block.
    {
        auto newClientOwned = getServiceContext()->getService()->makeClient("newClient");
        AlternativeClientRegion acr(newClientOwned);
        auto newOpCtx = cc().makeOperationContext();
        makeInProgressTxn(newOpCtx.get(),
                          makeLogicalSessionIdWithTxnNumberAndUUIDForTest(retryableWriteLsid,
                                                                          retryableWriteTxnNumber),
                          internalTxnTxnNumber);
    }
    makeInProgressTxn(operationContext(), internalTxnLsid, internalTxnTxnNumber);
    auto lastOplogTs = getLatestOplogTimestamp(operationContext());

    auto sessionTxnRecord =
        SessionTxnRecord{retryableWriteLsid, retryableWriteTxnNumber, repl::OpTime{}, Date_t{}};

    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    CancellationSource cancelSource;
    auto future = runCloner(cloner, cancelSource.token());

    onCommandReturnTxns({sessionTxnRecord.toBSON()}, {});

    ASSERT_FALSE(future.isReady());
    // Wait a little bit to increase the likelihood that the cloner has blocked on the in progress
    // internal transaction before the transaction is aborted.
    ASSERT_OK(
        getExecutor()->sleepFor(Milliseconds{200}, CancellationToken::uncancelable()).getNoThrow());
    ASSERT_FALSE(future.isReady());

    abortTxn(operationContext(), internalTxnLsid, internalTxnTxnNumber);

    ASSERT_OK(future.getNoThrow());

    {
        auto foundOps = findOplogEntriesNewerThan(operationContext(), lastOplogTs);
        ASSERT_GTE(foundOps.size(), 1U);
        // The first oplog entry is from the session txn record being updated by
        // ReshardingTxnCloner.
        checkGeneratedNoop(
            foundOps[0], retryableWriteLsid, retryableWriteTxnNumber, {kIncompleteHistoryStmtId});

        auto sessionTxnRecord = findSessionRecord(operationContext(), retryableWriteLsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[0]);
    }
}

TEST_F(ReshardingTxnClonerTest,
       WaitsOnPreparedInternalTxnForRetryableWriteAndAutomaticallyRetries) {
    auto retryableWriteLsid = makeLogicalSessionIdForTest();
    TxnNumber retryableWriteTxnNumber = 100;
    auto internalTxnLsid = makeLogicalSessionIdWithTxnNumberAndUUIDForTest(retryableWriteLsid,
                                                                           retryableWriteTxnNumber);
    TxnNumber internalTxnTxnNumber = 1;

    auto opTime = makePreparedTxn(operationContext(), internalTxnLsid, internalTxnTxnNumber);

    auto sessionTxnRecord =
        SessionTxnRecord{retryableWriteLsid, retryableWriteTxnNumber, repl::OpTime{}, Date_t{}};

    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    CancellationSource cancelSource;
    auto future = runCloner(cloner, cancelSource.token());

    onCommandReturnTxns({sessionTxnRecord.toBSON()}, {});

    ASSERT_FALSE(future.isReady());
    // Wait a little bit to increase the likelihood that the cloner has blocked on the in progress
    // internal transaction before the transaction is aborted.
    ASSERT_OK(
        getExecutor()->sleepFor(Milliseconds{200}, CancellationToken::uncancelable()).getNoThrow());
    ASSERT_FALSE(future.isReady());

    abortTxn(operationContext(), internalTxnLsid, internalTxnTxnNumber);

    ASSERT_OK(future.getNoThrow());

    {
        auto foundOps = findOplogEntriesNewerThan(operationContext(), opTime.getTimestamp());
        ASSERT_GTE(foundOps.size(), 2U);
        // The first oplog entry is from aborting the prepared internal transaction via abortTxn().
        // The second oplog entry is from the session txn record being updated by
        // ReshardingTxnCloner.
        checkGeneratedNoop(
            foundOps[1], retryableWriteLsid, retryableWriteTxnNumber, {kIncompleteHistoryStmtId});

        auto sessionTxnRecord = findSessionRecord(operationContext(), retryableWriteLsid);
        ASSERT_TRUE(bool(sessionTxnRecord));
        checkSessionTxnRecord(*sessionTxnRecord, foundOps[1]);
    }
}

TEST_F(ReshardingTxnClonerTest, CancelableWhileWaitingOnInProgressInternalTxnForRetryableWrite) {
    auto retryableWriteLsid = makeLogicalSessionIdForTest();
    TxnNumber retryableWriteTxnNumber = 100;

    auto internalTxnLsid = makeLogicalSessionIdWithTxnNumberAndUUIDForTest(retryableWriteLsid,
                                                                           retryableWriteTxnNumber);
    TxnNumber internalTxnTxnNumber = 1;

    // Make two in progress transactions so the one started by resharding must block.
    {
        auto newClientOwned = getServiceContext()->getService()->makeClient("newClient");
        AlternativeClientRegion acr(newClientOwned);
        auto newOpCtx = cc().makeOperationContext();
        makeInProgressTxn(newOpCtx.get(),
                          makeLogicalSessionIdWithTxnNumberAndUUIDForTest(retryableWriteLsid,
                                                                          retryableWriteTxnNumber),
                          internalTxnTxnNumber);
    }
    makeInProgressTxn(operationContext(), internalTxnLsid, internalTxnTxnNumber);
    ON_BLOCK_EXIT([&] { abortTxn(operationContext(), internalTxnLsid, internalTxnTxnNumber); });

    auto sessionTxnRecord =
        SessionTxnRecord{retryableWriteLsid, retryableWriteTxnNumber, repl::OpTime{}, Date_t{}};

    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    CancellationSource cancelSource;
    auto future = runCloner(cloner, cancelSource.token());

    onCommandReturnTxns({sessionTxnRecord.toBSON()}, {});

    ASSERT_FALSE(future.isReady());
    // Wait a little bit to increase the likelihood that the applier has blocked on the prepared
    // transaction before the cancellation source is canceled.
    ASSERT_OK(
        getExecutor()->sleepFor(Milliseconds{200}, CancellationToken::uncancelable()).getNoThrow());
    ASSERT_FALSE(future.isReady());

    cancelSource.cancel();
    ASSERT_EQ(future.getNoThrow(), ErrorCodes::CallbackCanceled);
}

TEST_F(ReshardingTxnClonerTest, DoNotAddDeadEndSentinelTwice) {
    auto opCtx = operationContext();
    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    auto txnRecord = SessionTxnRecord::parse(
        makeTxn(), IDLParserContext{"ReshardingTxnClonerTest::DoNotAddDeadEndSentinelTwice"});

    DBDirectClient client(opCtx);
    auto filter = fromjson(fmt::format(
        R"(
            {{
                lsid: {{ $eq: {} }},
                o2: {{ $eq: {} }}
            }}
        )",
        tojson(txnRecord.getSessionId().toBSON()),
        tojson(TransactionParticipant::kDeadEndSentinel)));

    auto getSentinelCount = [&] {
        return client.count(NamespaceString::kRsOplogNamespace, filter);
    };

    ASSERT_EQ(getSentinelCount(), 0);
    cloner.doOneRecord(opCtx, txnRecord);
    ASSERT_EQ(getSentinelCount(), 1);
    cloner.doOneRecord(opCtx, txnRecord);
    ASSERT_EQ(getSentinelCount(), 1);
}

}  // namespace
}  // namespace mongo
