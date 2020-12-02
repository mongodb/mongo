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

#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/logical_time_metadata_hook.h"
#include "mongo/db/pipeline/process_interface/shardsvr_process_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_txn_cloner.h"
#include "mongo/db/s/resharding/resharding_txn_cloner_progress_gen.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/catalog_cache_loader_mock.h"
#include "mongo/s/database_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace {

class ScopedServerParameterChange {
public:
    ScopedServerParameterChange(int* param, int newValue) : _param(param), _originalValue(*_param) {
        *param = newValue;
    }

    ~ScopedServerParameterChange() {
        *_param = _originalValue;
    }

private:
    int* const _param;
    const int _originalValue;
};

class ReshardingTxnClonerTest : public ShardServerTestFixture {
    void setUp() {
        // Don't call ShardServerTestFixture::setUp so we can install a mock catalog cache loader.
        ShardingMongodTestFixture::setUp();

        replicationCoordinator()->alwaysAllowWrites(true);
        serverGlobalParams.clusterRole = ClusterRole::ShardServer;

        _clusterId = OID::gen();
        ShardingState::get(getServiceContext())->setInitialized(kTwoShardIdList[0], _clusterId);

        auto mockLoader = std::make_unique<CatalogCacheLoaderMock>();

        // The config database's primary shard is always config, and it is always sharded.
        mockLoader->setDatabaseRefreshReturnValue(
            DatabaseType{NamespaceString::kConfigDb.toString(),
                         ShardRegistry::kConfigServerShardId,
                         true,
                         DatabaseVersion::makeFixed()});

        // The config.transactions collection is always unsharded.
        mockLoader->setCollectionRefreshReturnValue(
            {ErrorCodes::NamespaceNotFound, "collection not found"});

        CatalogCacheLoader::set(getServiceContext(), std::move(mockLoader));

        uassertStatusOK(
            initializeGlobalShardingStateForMongodForTest(ConnectionString(kConfigHostAndPort)));

        configTargeterMock()->setFindHostReturnValue(kConfigHostAndPort);

        for (const auto& shardId : kTwoShardIdList) {
            auto shardTargeter = RemoteCommandTargeterMock::get(
                uassertStatusOK(shardRegistry()->getShard(operationContext(), shardId))
                    ->getTargeter());
            shardTargeter->setFindHostReturnValue(makeHostAndPort(shardId));
        }

        WaitForMajorityService::get(getServiceContext()).setUp(getServiceContext());

        // onStepUp() relies on the storage interface to create the config.transactions table.
        repl::StorageInterface::set(getServiceContext(),
                                    std::make_unique<repl::StorageInterfaceImpl>());
        MongoDSessionCatalog::onStepUp(operationContext());
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
    }

    void tearDown() {
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ShardServerTestFixture::tearDown();
    }

    /**
     * Override the CatalogClient to make CatalogClient::getAllShards automatically return the
     * expected shards. We cannot mock the network responses for the ShardRegistry reload, since the
     * ShardRegistry reload is done over DBClient, not the NetworkInterface, and there is no
     * DBClientMock analogous to the NetworkInterfaceMock.
     */
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient(
        std::unique_ptr<DistLockManager> distLockManager) {

        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(std::vector<ShardId> shardIds)
                : ShardingCatalogClientMock(nullptr), _shardIds(std::move(shardIds)) {}

            StatusWith<repl::OpTimeWith<std::vector<ShardType>>> getAllShards(
                OperationContext* opCtx, repl::ReadConcernLevel readConcern) override {
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
    const std::vector<ShardId> kTwoShardIdList{_myShardName, {"otherShardName"}};
    const std::vector<ReshardingSourceId> kTwoSourceIdList = {
        {kDefaultReshardingId, _myShardName}, {kDefaultReshardingId, ShardId("otherShardName")}};

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
        return SessionTxnRecord::parse(IDLParserErrorContext("ReshardingTxnClonerTest"), txnRecord)
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

    void seedTransactionOnRecipient(LogicalSessionId sessionId,
                                    TxnNumber txnNum,
                                    bool multiDocTxn) {
        auto opCtx = operationContext();
        opCtx->setLogicalSessionId(sessionId);
        opCtx->setTxnNumber(txnNum);

        if (multiDocTxn) {
            opCtx->setInMultiDocumentTransaction();
        }

        MongoDOperationContextSession ocs(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT(txnParticipant);
        if (multiDocTxn) {
            txnParticipant.beginOrContinue(opCtx, txnNum, false, true);
        } else {
            txnParticipant.beginOrContinue(opCtx, txnNum, boost::none, boost::none);
        }
    }

    void checkTxnHasBeenUpdated(LogicalSessionId sessionId, TxnNumber txnNum) {
        DBDirectClient client(operationContext());
        // The same logical session entry may be inserted more than once by a test case, so use a
        // $natural sort to find the most recently inserted entry.
        Query oplogQuery(BSON(repl::OplogEntryBase::kSessionIdFieldName << sessionId.toBSON()));
        auto bsonOplog = client.findOne(NamespaceString::kRsOplogNamespace.ns(),
                                        oplogQuery.sort(BSON("$natural" << -1)));
        ASSERT(!bsonOplog.isEmpty());
        auto oplogEntry = repl::MutableOplogEntry::parse(bsonOplog).getValue();
        ASSERT_EQ(oplogEntry.getTxnNumber().get(), txnNum);
        ASSERT_BSONOBJ_EQ(oplogEntry.getObject(), BSON("$sessionMigrateInfo" << 1));
        ASSERT_BSONOBJ_EQ(oplogEntry.getObject2().get(), BSON("$incompleteOplogHistory" << 1));
        ASSERT(oplogEntry.getOpType() == repl::OpTypeEnum::kNoop);

        auto bsonTxn =
            client.findOne(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                           {BSON(SessionTxnRecord::kSessionIdFieldName << sessionId.toBSON())});
        ASSERT(!bsonTxn.isEmpty());
        auto txn = SessionTxnRecord::parse(
            IDLParserErrorContext("resharding config transactions cloning test"), bsonTxn);
        ASSERT_EQ(txn.getTxnNum(), txnNum);
        ASSERT_EQ(txn.getLastWriteOpTime(), oplogEntry.getOpTime());
    }

    void checkTxnHasNotBeenUpdated(LogicalSessionId sessionId, TxnNumber txnNum) {
        auto opCtx = operationContext();
        opCtx->setLogicalSessionId(sessionId);
        MongoDOperationContextSession ocs(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);

        DBDirectClient client(operationContext());
        auto bsonOplog =
            client.findOne(NamespaceString::kRsOplogNamespace.ns(),
                           BSON(repl::OplogEntryBase::kSessionIdFieldName << sessionId.toBSON()));

        ASSERT_BSONOBJ_EQ(bsonOplog, {});
        ASSERT_EQ(txnParticipant.getActiveTxnNumber(), txnNum);
    }

    boost::optional<ReshardingTxnClonerProgress> getTxnCloningProgress(
        const ReshardingSourceId& sourceId) {
        DBDirectClient client(operationContext());
        auto progressDoc = client.findOne(
            NamespaceString::kReshardingTxnClonerProgressNamespace.ns(),
            BSON(ReshardingTxnClonerProgress::kSourceIdFieldName << sourceId.toBSON()));

        if (progressDoc.isEmpty()) {
            return boost::none;
        }

        return ReshardingTxnClonerProgress::parse(
            IDLParserErrorContext("ReshardingTxnClonerProgress"), progressDoc);
    }

    boost::optional<LogicalSessionId> getProgressLsid(const ReshardingSourceId& sourceId) {
        auto progressDoc = getTxnCloningProgress(sourceId);
        return progressDoc ? boost::optional<LogicalSessionId>(progressDoc->getProgress())
                           : boost::none;
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> makeTaskExecutorForCloner() {
        // The ReshardingTxnCloner expects there to already be a Client associated with the thread
        // from the thread pool. We set up the ThreadPoolTaskExecutor identically to how the
        // recipient's primary-only service is set up.
        ThreadPool::Options threadPoolOptions;
        threadPoolOptions.maxThreads = 1;
        threadPoolOptions.threadNamePrefix = "TestReshardCloneConfigTransactions-";
        threadPoolOptions.poolName = "TestReshardCloneConfigTransactionsThreadPool";
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
        hookList->addHook(std::make_unique<rpc::LogicalTimeMetadataHook>(getServiceContext()));

        auto executor = std::make_shared<executor::ThreadPoolTaskExecutor>(
            std::make_unique<ThreadPool>(std::move(threadPoolOptions)),
            executor::makeNetworkInterface(
                "TestReshardCloneConfigTransactionsNetwork", nullptr, std::move(hookList)));
        executor->startup();

        return executor;
    }

    std::shared_ptr<MongoProcessInterface> makeMongoProcessInterface() {
        return std::make_shared<ShardServerProcessInterface>(
            operationContext(),
            Grid::get(getServiceContext())->getExecutorPool()->getFixedExecutor());
    }

private:
    static HostAndPort makeHostAndPort(const ShardId& shardId) {
        return HostAndPort(str::stream() << shardId << ":123");
    }

    ScopedServerParameterChange _txnClonerBatchSize{
        &resharding::gReshardingTxnClonerProgressBatchSize, 1};
};

TEST_F(ReshardingTxnClonerTest, MergeTxnNotOnRecipient) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        auto executor = makeTaskExecutorForCloner();
        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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
    auto executor = makeTaskExecutorForCloner();
    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

    const auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNum = 3;
    onCommandReturnTxns({SessionTxnRecord(sessionId, txnNum, repl::OpTime(), Date_t::now())
                             .toBSON()
                             .removeField(SessionTxnRecord::kSessionIdFieldName)},
                        {});

    auto status = future.getNoThrow();
    ASSERT_EQ(status, static_cast<ErrorCodes::Error>(40414));
}

TEST_F(ReshardingTxnClonerTest, MergeNewTxnOverMultiDocTxn) {
    for (auto state : getDurableTxnStatesAndBoostNone()) {
        const auto sessionId = makeLogicalSessionIdForTest();
        TxnNumber donorTxnNum = 3;
        TxnNumber recipientTxnNum = donorTxnNum - 1;

        seedTransactionOnRecipient(sessionId, recipientTxnNum, true);

        auto executor = makeTaskExecutorForCloner();
        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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

        auto executor = makeTaskExecutorForCloner();
        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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

        auto executor = makeTaskExecutorForCloner();
        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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

        auto executor = makeTaskExecutorForCloner();
        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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

        auto executor = makeTaskExecutorForCloner();
        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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

    auto executor = makeTaskExecutorForCloner();
    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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

TEST_F(ReshardingTxnClonerTest, ClonerStoresProgressSingleBatch) {
    const auto txns = makeSortedTxns(2);
    const auto lastLsid = getTxnRecordLsid(txns.back());

    {
        ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
        ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[1]));

        auto executor = makeTaskExecutorForCloner();
        ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
        auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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

        auto executor = makeTaskExecutorForCloner();
        ReshardingTxnCloner cloner(kTwoSourceIdList[0], Timestamp::max());
        auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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

    auto executor = makeTaskExecutorForCloner();
    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

    // The progress document is updated asynchronously after the session record is updated. We fake
    // the cursor having been killed to inspect the progress document after the first batch has been
    // processed.
    onCommandReturnTxnBatch(std::vector<BSONObj>(txns.begin(), txns.begin() + 2),
                            CursorId{123},
                            true /* isFirstBatch */);

    onCommand([&](const executor::RemoteCommandRequest& request) {
        return Status{ErrorCodes::CursorNotFound, "Simulate cursor not found error"};
    });

    auto status = future.getNoThrow();
    ASSERT_EQ(status, ErrorCodes::CursorNotFound);

    // After the first batch, the progress document should contain the lsid of the last document in
    // that batch.
    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
    ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), firstLsid);

    // Now we run the cloner again and give it the remaining documents.
    future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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

    auto executor = makeTaskExecutorForCloner();
    ReshardingTxnCloner cloner(kTwoSourceIdList[1], Timestamp::max());
    auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

    onCommandReturnTxnBatch({txns.front()}, CursorId{123}, true /* isFirstBatch */);

    // Simulate the cursor being killed as the batch was being filled and interrupting the cloner.
    // The ARS automatically retries on ErrorCodes::isRetriableError() codes, so we intentionally
    // use a non-retryable error code.
    onCommand([&](const executor::RemoteCommandRequest& request) {
        // The progress document is updated asynchronously after the session record is updated but
        // is guaranteed to have been updated before the next getMore request is sent.
        ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
        ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), firstLsid);

        return Status{ErrorCodes::CursorKilled, "Simulate cursor killed error"};
    });

    auto status = future.getNoThrow();
    ASSERT_EQ(status, ErrorCodes::CursorKilled);

    // The stored progress should be unchanged.
    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
    ASSERT_EQ(*getProgressLsid(kTwoSourceIdList[1]), firstLsid);

    // Simulate a resume on the new primary by creating a new fetcher that resumes after the lsid in
    // the progress document.
    future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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
    future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

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

    auto executor = makeTaskExecutorForCloner();
    ReshardingTxnCloner cloner(kTwoSourceIdList[0], Timestamp::max());
    auto future = cloner.run(getServiceContext(), executor, makeMongoProcessInterface());

    onCommandReturnTxnBatch({}, CursorId{0}, true /* isFirstBatch */);

    auto status = future.getNoThrow();
    ASSERT_OK(status);

    ASSERT_FALSE(getTxnCloningProgress(kTwoSourceIdList[0]));
}

}  // namespace
}  // namespace mongo
