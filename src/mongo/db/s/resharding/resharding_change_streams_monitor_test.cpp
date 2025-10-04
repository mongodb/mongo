/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/resharding_change_streams_monitor.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/create_collection.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_test_util.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const StringData kDefaultExecutorDescriptionSuffix = "Default";

class ReshardingChangeStreamsMonitorTest : public ShardServerTestFixtureWithCatalogCacheMock {
public:
    ReshardingChangeStreamsMonitorTest()
        : ShardServerTestFixtureWithCatalogCacheMock(Options{}.useReplSettings(true)) {}

    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheMock::setUp();

        opCtx = operationContext();
        executor = makeTaskExecutor();
        cleanupExecutor = makeCleanupTaskExecutor();
        markKilledExecutor = makeCleanupTaskExecutor();
        factory.emplace(cancelSource.token(), markKilledExecutor);

        DBDirectClient client(opCtx);
        ASSERT(client.createCollection(NamespaceString::kSessionTransactionsTableNamespace));
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        // This is required to be able to run prepared transactions.
        setGlobalReplSettings(replicationCoordinator()->getSettings());
    }

    void tearDown() override {
        tearDownExecutors({executor, cleanupExecutor, markKilledExecutor});

        ShardServerTestFixtureWithCatalogCacheMock::tearDown();
    }

    /**
     * Shuts down and joins the given task executors.
     */
    void tearDownExecutors(
        std::vector<std::shared_ptr<executor::ThreadPoolTaskExecutor>> executors) {
        for (auto& executor : executors) {
            executor->shutdown();
            executor->join();
        }
    }

    /**
     * Create a collection on 'nss' and inserts document with '_id' and 'x' ranging from minDocValue
     * to maxDocValue (inclusive).
     */
    void createCollectionAndInsertDocuments(const NamespaceString& nss,
                                            int minDocValue,
                                            int maxDocValue) {
        AutoGetDb autoDb(opCtx, nss.dbName(), LockMode::MODE_X);
        autoDb.ensureDbExists(opCtx);

        createTestCollection(opCtx, nss);

        DBDirectClient client(opCtx);
        for (int i = minDocValue; i <= maxDocValue; i++) {
            client.insert(nss, BSON("_id" << i << "x" << i));
        }

        // Perform an update so change stream start time doesn't include the inserts above.
        client.update(nss,
                      BSON("x" << minDocValue),
                      BSON("$set" << BSON("y" << minDocValue)),
                      false /*upsert*/,
                      false /*multi*/);
    }

    /**
     * Create a timeseris collection on 'nss' and inserts document with '_id' and 'x' ranging from
     * minDocValue to maxDocValue (inclusive) and with a 'timestamp' set to the current time.
     */
    void createTimeseriesCollectionAndInsertDocuments(const NamespaceString& nss,
                                                      int minDocValue,
                                                      int maxDocValue) {
        AutoGetDb autoDb(opCtx, nss.dbName(), LockMode::MODE_X);
        autoDb.ensureDbExists(opCtx);

        BSONObj timeseriesOptions = BSON("timeField" << "timestamp");

        createTestCollection(
            opCtx, nss, BSON("create" << nss.coll() << "timeseries" << timeseriesOptions));

        DBDirectClient client(opCtx);
        for (int i = minDocValue; i <= maxDocValue; i++) {
            client.insert(nss, BSON("_id" << i << "x" << i << "timestamp" << Date_t::now()));
        }

        // Perform an update so change stream start time doesn't include the inserts above.
        client.update(nss,
                      BSON("x" << minDocValue),
                      BSON("$set" << BSON("y" << minDocValue)),
                      false /*upsert*/,
                      false /*multi*/);
    }

    /**
     * Starts a transaction with the given session id and transaction number, and runs the given
     * callback function.
     */
    template <typename Callable>
    void beginTxn(OperationContext* opCtx,
                  LogicalSessionId sessionId,
                  TxnNumber txnNumber,
                  Callable&& func) {
        opCtx->setLogicalSessionId(sessionId);
        opCtx->setTxnNumber(txnNumber);
        opCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(opCtx,
                                       {txnNumber},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);

        txnParticipant.unstashTransactionResources(opCtx, "ReshardingChangeStreamsMonitor");
        func();
        txnParticipant.stashTransactionResources(opCtx);
    }

    /**
     * Makes the transaction with the given session id, transaction number enter the "prepared"
     * state, and leaves it uncommitted. Returns the prepare op time.
     */
    repl::OpTime prepareTxn(OperationContext* opCtx,
                            LogicalSessionId sessionId,
                            TxnNumber txnNumber) {

        opCtx->setLogicalSessionId(sessionId);
        opCtx->setTxnNumber(txnNumber);
        opCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        txnParticipant.beginOrContinue(opCtx,
                                       {txnNumber},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kContinue);

        txnParticipant.unstashTransactionResources(opCtx, "preparedTransaction");
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

    /**
     * Commits the transaction with the given session id and transaction number. If it is a prepared
     * transaction, the commit timestamp must be provided.
     */
    void commitTxn(OperationContext* opCtx,
                   LogicalSessionId sessionId,
                   TxnNumber txnNumber,
                   boost::optional<Timestamp> commitTimestamp = boost::none) {
        opCtx->setLogicalSessionId(sessionId);
        opCtx->setTxnNumber(txnNumber);
        opCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);

        txnParticipant.beginOrContinue(opCtx,
                                       {txnNumber},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kContinue);

        txnParticipant.unstashTransactionResources(opCtx, "commitTransaction");

        if (commitTimestamp) {
            // Committing a prepared transaction involves asserting that the corresponding prepare
            // timestamp has been majority committed. We exempt the unitests from this expectation
            // since this fixture doesn't set up the majority committing machinery.
            FailPointEnableBlock failPointBlock("skipCommitTxnCheckPrepareMajorityCommitted");

            txnParticipant.commitPreparedTransaction(
                opCtx, *commitTimestamp, boost::none /* commitOplogEntryOpTime */);
        } else {
            txnParticipant.commitUnpreparedTransaction(opCtx);
        }

        txnParticipant.stashTransactionResources(opCtx);
    }

    void abortTxn(OperationContext* opCtx, LogicalSessionId sessionId, TxnNumber txnNumber) {
        opCtx->setInMultiDocumentTransaction();
        opCtx->setLogicalSessionId(sessionId);
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

    void insertNoopOplogEntry(NamespaceString nss, BSONObj msg, BSONObj o2Field) {
        const auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        opCtx->getServiceContext()->getOpObserver()->onInternalOpMessage(opCtx,
                                                                         coll.nss(),
                                                                         coll.uuid(),
                                                                         msg,
                                                                         o2Field,
                                                                         boost::none,
                                                                         boost::none,
                                                                         boost::none,
                                                                         boost::none);
        wuow.commit();
    }

    /**
     * Inserts the 'reshardingBlockingWrites' noop oplog entry.
     */
    void insertDonorFinalEventNoopOplogEntry(const NamespaceString& sourceNss) {
        auto msg = BSON("msg" << "Writes to {} are temporarily blocked for resharding");
        ReshardBlockingWritesChangeEventO2Field o2Field{
            sourceNss, UUID::gen(), std::string{resharding::kReshardFinalOpLogType}};
        insertNoopOplogEntry(sourceNss, msg, o2Field.toBSON());
    }

    /**
     * Inserting the 'reshardingDoneCatchUp' noop oplog entry.
     */
    void insertRecipientFinalEventNoopOplogEntry(const NamespaceString& tempNss) {
        auto msg = BSON("msg" << "The temporary resharding collection now has a "
                                 "strictly consistent view of the data");
        ReshardDoneCatchUpChangeEventO2Field o2Field{tempNss, reshardingUUID};
        insertNoopOplogEntry(tempNss, msg, o2Field.toBSON());
    }

    /**
     * Returns true if there is an open cursor with the given namespace.
     */
    bool hasOpenCursor(const NamespaceString& nss, const BSONObj& aggComment) {
        // Create an alternative client and opCtx since the original opCtx may have been used to
        // run a transaction and $currentOp is not supported in a transaction.
        auto client = opCtx->getServiceContext()->getService()->makeClient("AlternativeClient");
        AlternativeClientRegion acr(client);
        auto opCtx = cc().makeOperationContext();

        std::vector<BSONObj> pipeline;
        pipeline.push_back(BSON("$currentOp" << BSON("allUsers" << true << "idleCursors" << true)));
        // Filter by aggregation comment to identify reshardingChangeStreamsMonitor cursors and ops.
        pipeline.push_back(
            BSON("$match" << BSON(
                     "$or" << BSON_ARRAY(BSON("cursor.originatingCommand.comment" << aggComment)
                                         << BSON("command.comment" << aggComment)))));

        DBDirectClient dbclient(opCtx.get());
        AggregateCommandRequest aggRequest(
            NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin), pipeline);
        auto cursor = uassertStatusOK(DBClientCursor::fromAggregationRequest(
            &dbclient, aggRequest, false /* secondaryOk */, false /* useExhaust*/));

        bool openCursorExists = false;
        while (cursor->more()) {
            auto doc = cursor->next();
            LOGV2(10066810,
                  "Found currentOp with reshardingChangeStreamsMonitor comment",
                  "doc"_attr = doc);

            if (doc.getBoolField("killPending")) {
                // Cursors with {killPending: true} will eventually get killed. They should not
                // be consider as open cursors.
                continue;
            }

            auto typeStr = doc.getStringField("type");
            if (typeStr == "idleCursor") {
                // For idle cursors, top-level ns is correct.
                if (doc.getStringField("ns") == nss.ns_forTest()) {
                    LOGV2(10764600, "Found idle cursor", "ns"_attr = nss, "doc"_attr = doc);
                    openCursorExists = true;
                }
            } else if (typeStr == "op") {
                auto opStr = doc.getStringField("op");
                auto command = doc["command"].Obj();

                StringData targetedNS;
                if (opStr == "getmore") {
                    targetedNS = command.getStringField("collection");
                } else if (opStr == "command") {
                    targetedNS = command.getStringField("aggregate");
                }

                if (targetedNS == nss.coll()) {
                    LOGV2(
                        10764601, "Found active cursor command", "ns"_attr = nss, "doc"_attr = doc);
                    openCursorExists = true;
                }
            }
        }

        return openCursorExists;
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> makeTaskExecutor(
        const StringData descSuffix = kDefaultExecutorDescriptionSuffix) {
        return _makeTaskExecutor("ReshardingChangeStreamsMonitorTestExecutor" + descSuffix);
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> makeCleanupTaskExecutor(
        const StringData descSuffix = kDefaultExecutorDescriptionSuffix) {
        return _makeTaskExecutor("ReshardingChangeStreamsMonitorTestCleanupExecutor" + descSuffix);
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> makeMarkKilledTaskExecutor(
        const StringData descSuffix = kDefaultExecutorDescriptionSuffix) {
        return _makeTaskExecutor("ReshardingChangeStreamsMonitorTestMarkKilledExecutor" +
                                 descSuffix);
    }

private:
    std::shared_ptr<executor::ThreadPoolTaskExecutor> _makeTaskExecutor(auto desc) {
        executor::ThreadPoolMock::Options threadPoolOptions;
        threadPoolOptions.onCreateThread = [this, desc] {
            Client::initThread(desc, getServiceContext()->getService());
        };

        auto executor = executor::makeThreadPoolTestExecutor(
            std::make_unique<executor::NetworkInterfaceMock>(), std::move(threadPoolOptions));

        executor->startup();
        return executor;
    }

protected:
    const UUID collUUID = UUID::gen();
    const NamespaceString sourceNss = NamespaceString::createNamespaceString_forTest("db", "coll");
    const NamespaceString tempNss =
        resharding::constructTemporaryReshardingNss(sourceNss, collUUID);
    const UUID reshardingUUID = UUID::gen();

    OperationContext* opCtx;

    std::shared_ptr<executor::ThreadPoolTaskExecutor> executor;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> cleanupExecutor;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> markKilledExecutor;

    CancellationSource cancelSource;
    boost::optional<CancelableOperationContextFactory> factory;

    // Set the batch size 1 to test multi-batch processing in unit tests with multiple events.
    RAIIServerParameterControllerForTest batchSize{
        "reshardingVerificationChangeStreamsEventsBatchSizeLimit", 1};

    int delta = 0;
    BSONObj resumeToken;
    bool completed;
    ReshardingChangeStreamsMonitor::BatchProcessedCallback callback = [&](const auto& batch) {
        delta += batch.getDocumentsDelta();
        resumeToken = batch.getResumeToken().getOwned();
        completed = batch.containsFinalEvent();
    };
};

class TestReshardingChangeStreamsMonitorNoKill : public ReshardingChangeStreamsMonitor {
public:
    using ReshardingChangeStreamsMonitor::ReshardingChangeStreamsMonitor;

protected:
    Status killCursors(OperationContext* opCtx) override {
        LOGV2(10960500, "Skipping killCursors for test");
        return Status::OK();
    }
};

TEST_F(ReshardingChangeStreamsMonitorTest, SuccessfullyInitializeMonitorWithStartAtTime) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, 0);
    ASSERT_FALSE(resumeToken.isEmpty());
    ASSERT(completed);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

DEATH_TEST_REGEX_F(ReshardingChangeStreamsMonitorTest,
                   FailIfAwaitFinalEventBeforeStartMonitoring,
                   "Tripwire assertion.*1009073") {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);

    monitor->awaitFinalChangeEvent().get();
}

DEATH_TEST_REGEX_F(ReshardingChangeStreamsMonitorTest,
                   FailIfAwaitCleanupBeforeStartMonitoring,
                   "Tripwire assertion.*1006686") {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);

    monitor->awaitCleanup().get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, FailIfStartMonitoringMoreThanOnce) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion0 =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);
    auto awaitCompletion1 =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    ASSERT_EQ(awaitCompletion1.getNoThrow().code(), 1006687);

    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();
    monitor->awaitCleanup().get();
    awaitCompletion0.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, KillCursorAfterCancellationAndExecutorShutDown) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    // Wait for the monitor to open a change stream cursor.
    resharding_test_util::assertSoon(opCtx, [&] {
        return hasOpenCursor(tempNss, monitor->makeAggregateComment(reshardingUUID));
    });

    cancelSource.cancel();
    executor->shutdown();

    auto status = monitor->awaitFinalChangeEvent().getNoThrow();
    ASSERT(status == ErrorCodes::ShutdownInProgress || status == ErrorCodes::CallbackCanceled)
        << "Found an unexpected error " << status;

    // The cleanup should still succeed.
    monitor->awaitCleanup().get();

    // Verify that the cursor got killed.
    ASSERT_FALSE(hasOpenCursor(tempNss, monitor->makeAggregateComment(reshardingUUID)));
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, KillCursorFromPreviousTry) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto hangKillCursorFp =
        globalFailPointRegistry().find("hangReshardingChangeStreamsMonitorBeforeKillingCursors");
    auto timesEntered = hangKillCursorFp->setMode(FailPoint::alwaysOn);

    // Start a monitor.
    auto monitor0 = std::make_shared<TestReshardingChangeStreamsMonitorNoKill>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion0 =
        monitor0->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    // Wait for the monitor to open a change stream cursor.
    resharding_test_util::assertSoon(opCtx, [&] {
        return hasOpenCursor(tempNss, monitor0->makeAggregateComment(reshardingUUID));
    });

    // Start another monitor and make it run to completion successfully.
    auto executor1 = makeTaskExecutor("New");
    auto cleanupExecutor1 = makeCleanupTaskExecutor("New");
    auto cancelSource1 = CancellationSource();
    auto factory1 = CancelableOperationContextFactory(cancelSource1.token(), markKilledExecutor);

    auto teardownGuard = ScopeGuard([&] { tearDownExecutors({executor1, cleanupExecutor1}); });

    auto monitor1 = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion1 =
        monitor1->startMonitoring(executor1, cleanupExecutor1, cancelSource1.token(), factory1);

    insertRecipientFinalEventNoopOplogEntry(tempNss);

    // Delay monitor1 cleanup until monitor0 sees the final event so monitor0's cursor is idle.
    hangKillCursorFp->waitForTimesEntered(timesEntered + 1);
    monitor0->awaitFinalChangeEvent().get();
    hangKillCursorFp->setMode(FailPoint::off);

    monitor1->awaitFinalChangeEvent().get();
    monitor1->awaitCleanup().get();
    awaitCompletion1.get();

    // Verify that monitor1 killed monitor0's cursor during cleanup.
    ASSERT_FALSE(hasOpenCursor(tempNss, monitor0->makeAggregateComment(reshardingUUID)));
}

TEST_F(ReshardingChangeStreamsMonitorTest, DoNotKillCursorOpenedByOtherMonitor) {
    createCollectionAndInsertDocuments(sourceNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto donorExecutor = makeTaskExecutor("Donor");
    auto donorCleanupExecutor = makeCleanupTaskExecutor("Donor");
    auto donorMarkKilledExecutor = makeCleanupTaskExecutor("Donor");
    auto donorCancelSource = CancellationSource();
    auto donorFactory =
        CancelableOperationContextFactory(cancelSource.token(), donorMarkKilledExecutor);
    auto donorGuard = ScopeGuard(
        [&] { tearDownExecutors({donorExecutor, donorCleanupExecutor, donorMarkKilledExecutor}); });

    int donorDelta = 0;
    auto donorCallback = [&](const auto& batch) {
        donorDelta += batch.getDocumentsDelta();
    };
    auto donorMonitor =
        std::make_shared<ReshardingChangeStreamsMonitor>(reshardingUUID,
                                                         sourceNss,
                                                         startAtTime,
                                                         boost::none /* startAfterResumeToken */,
                                                         donorCallback);
    auto awaitDonorCompletion = donorMonitor->startMonitoring(
        donorExecutor, donorCleanupExecutor, donorCancelSource.token(), donorFactory);

    auto recipientExecutor = makeTaskExecutor("Recipient");
    auto recipientCleanupExecutor = makeCleanupTaskExecutor("Recipient");
    auto recipientMarkKilledExecutor = makeCleanupTaskExecutor("Recipient");
    auto recipientCancelSource = CancellationSource();
    auto recipientFactory =
        CancelableOperationContextFactory(cancelSource.token(), recipientMarkKilledExecutor);
    auto recipientGuard = ScopeGuard([&] {
        tearDownExecutors(
            {recipientExecutor, recipientCleanupExecutor, recipientMarkKilledExecutor});
    });

    int recipientDelta = 0;
    auto recipientCallback = [&](const auto& batch) {
        recipientDelta += batch.getDocumentsDelta();
    };
    auto recipientMonitor =
        std::make_shared<ReshardingChangeStreamsMonitor>(reshardingUUID,
                                                         tempNss,
                                                         startAtTime,
                                                         boost::none /* startAfterResumeToken */,
                                                         recipientCallback);
    auto awaitRecipientCompletion = recipientMonitor->startMonitoring(recipientExecutor,
                                                                      recipientCleanupExecutor,
                                                                      recipientCancelSource.token(),
                                                                      recipientFactory);

    // Wait for both donor and recipient monitors to open a change stream cursor.
    resharding_test_util::assertSoon(opCtx, [&] {
        return hasOpenCursor(sourceNss, donorMonitor->makeAggregateComment(reshardingUUID));
    });
    resharding_test_util::assertSoon(opCtx, [&] {
        return hasOpenCursor(tempNss, recipientMonitor->makeAggregateComment(reshardingUUID));
    });

    DBDirectClient client(opCtx);
    client.insert(sourceNss, BSON("_id" << 10));
    client.insert(tempNss, BSON("_id" << 10));

    // Make the donor monitor run to completion first.
    insertDonorFinalEventNoopOplogEntry(sourceNss);
    donorMonitor->awaitFinalChangeEvent().get();
    ASSERT_EQ(donorDelta, 1);
    donorMonitor->awaitCleanup().get();
    awaitDonorCompletion.get();

    // Verify that the donor monitor's cursor got killed but the recipient monitor's cursor did not.
    ASSERT(!hasOpenCursor(sourceNss, donorMonitor->makeAggregateComment(reshardingUUID)));
    resharding_test_util::assertSoon(opCtx, [&] {
        return hasOpenCursor(tempNss, recipientMonitor->makeAggregateComment(reshardingUUID));
    });

    client.insert(tempNss, BSON("_id" << 11));

    // Make the recipient monitor run to completion.
    insertRecipientFinalEventNoopOplogEntry(tempNss);
    recipientMonitor->awaitFinalChangeEvent().get();
    ASSERT_EQ(recipientDelta, 2);
    recipientMonitor->awaitCleanup().get();
    awaitRecipientCompletion.get();

    ASSERT(!hasOpenCursor(sourceNss, donorMonitor->makeAggregateComment(reshardingUUID)));
    ASSERT(!hasOpenCursor(tempNss, recipientMonitor->makeAggregateComment(reshardingUUID)));
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessSingleInsert) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    client.insert(tempNss, BSON("_id" << 10));

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, 1);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessSingleInsertWithMultiDocs) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    write_ops::InsertCommandRequest insertOp(tempNss);
    insertOp.setDocuments({BSON("_id" << 10), BSON("_id" << 11)});
    client.insert(insertOp);

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, 2);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessMultipleInserts) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    for (int i = 10; i < 13; i++) {
        client.insert(tempNss, BSON("_id" << i));
    }

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, 3);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessSingleDelete) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    client.remove(tempNss, BSON("_id" << 0), false);

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, -1);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessSingleDeleteMany) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    // Update doc {x:0} to {x:1} to ensure there are two documents matching the delete query.
    client.update(
        tempNss, BSON("x" << 0), BSON("$set" << BSON("x" << 1)), false /*upsert*/, false /*multi*/);
    client.remove(tempNss, BSON("x" << 1), true /*multi*/);

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, -2);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessMultipleDeletes) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    for (int i = 0; i < 3; i++) {
        client.remove(tempNss, BSON("_id" << i), false);
    }

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, -3);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessMultipleInsertsDeletes) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    for (int i = 10; i < 15; i++) {
        client.insert(tempNss, BSON("_id" << i));
        if (i == 10) {
            client.remove(tempNss, BSON("_id" << i), false);
        }
    }

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, 4);

    monitor->awaitCleanup().get();
    ASSERT_FALSE(hasOpenCursor(tempNss, monitor->makeAggregateComment(reshardingUUID)));
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, DisregardUpdates) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    client.update(tempNss,
                  BSON("_id" << 0),
                  BSON("$set" << BSON("x" << 5)),
                  false /*upsert*/,
                  true /*multi*/);

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, 0);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, EnsurePromiseFulfilledOnReachingRecipientFinalEvent) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    insertRecipientFinalEventNoopOplogEntry(tempNss);

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);
    monitor->awaitFinalChangeEvent().get();
    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, EnsurePromiseFulfilledOnReachingDonorFinalEvent) {
    AutoGetDb autoDb(opCtx, sourceNss.dbName(), LockMode::MODE_X);
    autoDb.ensureDbExists(opCtx);
    createTestCollection(opCtx, sourceNss);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    insertDonorFinalEventNoopOplogEntry(sourceNss);

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, sourceNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    monitor->awaitFinalChangeEvent().get();
    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, TxnCommittedAfterStartTime_Unprepared) {
    createCollectionAndInsertDocuments(sourceNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto sessionId = makeLogicalSessionIdForTest();
    auto txnNumber = 0;
    beginTxn(opCtx, sessionId, txnNumber, [&] {
        DBDirectClient client(opCtx);
        write_ops::InsertCommandRequest insertOp(sourceNss);
        insertOp.setDocuments({BSON("_id" << 10), BSON("_id" << 11), BSON("_id" << 12)});
        client.insert(insertOp);

        client.insert(sourceNss, BSON("_id" << 13));
        client.remove(sourceNss, BSON("_id" << 0), false);
        client.update(sourceNss,
                      BSON("x" << 13),
                      BSON("$set" << BSON("y" << 13)),
                      false /*upsert*/,
                      false /*multi*/);
    });
    commitTxn(opCtx, sessionId, txnNumber);

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, sourceNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertDonorFinalEventNoopOplogEntry(sourceNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, 3);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, TxnCommittedAfterStartTime_PreparedAfterStartTime) {
    createCollectionAndInsertDocuments(sourceNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);

    auto sessionId = makeLogicalSessionIdForTest();
    auto txnNumber = 0;
    beginTxn(opCtx, sessionId, txnNumber, [&] {
        DBDirectClient client(opCtx);
        write_ops::InsertCommandRequest insertOp(sourceNss);
        insertOp.setDocuments({BSON("_id" << 10), BSON("_id" << 11), BSON("_id" << 12)});
        client.insert(insertOp);

        client.insert(sourceNss, BSON("_id" << 13));
        client.remove(sourceNss, BSON("_id" << 0), false);
        client.update(sourceNss,
                      BSON("x" << 13),
                      BSON("$set" << BSON("y" << 13)),
                      false /*upsert*/,
                      false /*multi*/);
    });

    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto prepareOpTime = prepareTxn(opCtx, sessionId, txnNumber);
    commitTxn(opCtx, sessionId, txnNumber, prepareOpTime.getTimestamp());

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, sourceNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertDonorFinalEventNoopOplogEntry(sourceNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, 3);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, TxnCommittedAfterStartTime_PreparedBeforeStartTime) {
    createCollectionAndInsertDocuments(sourceNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);

    auto sessionId = makeLogicalSessionIdForTest();
    auto txnNumber = 0;
    beginTxn(opCtx, sessionId, txnNumber, [&] {
        DBDirectClient client(opCtx);
        write_ops::InsertCommandRequest insertOp(sourceNss);
        insertOp.setDocuments({BSON("_id" << 10), BSON("_id" << 11), BSON("_id" << 12)});
        client.insert(insertOp);

        client.insert(sourceNss, BSON("_id" << 13));
        client.remove(sourceNss, BSON("_id" << 0), false);
        client.update(sourceNss,
                      BSON("x" << 13),
                      BSON("$set" << BSON("y" << 13)),
                      false /*upsert*/,
                      false /*multi*/);
    });
    auto prepareOpTime = prepareTxn(opCtx, sessionId, txnNumber);

    insertNoopOplogEntry(sourceNss, BSON("msg" << "mock noop"), BSONObj() /* o2Field */);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    commitTxn(opCtx, sessionId, txnNumber, prepareOpTime.getTimestamp());

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, sourceNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertDonorFinalEventNoopOplogEntry(sourceNss);
    monitor->awaitFinalChangeEvent().get();

    // The events in the transaction should be discarded.
    ASSERT_EQ(delta, 0);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, TxnAbortedAfterStartTime) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto sessionId = makeLogicalSessionIdForTest();
    auto txnNumber = 0;
    beginTxn(opCtx, sessionId, txnNumber, [&] {
        DBDirectClient client(opCtx);
        client.insert(tempNss, BSON("_id" << 10));
        client.insert(tempNss, BSON("_id" << 11));
        client.remove(tempNss, BSON("_id" << 0), false);
    });
    abortTxn(opCtx, sessionId, txnNumber);

    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, 0);

    monitor->awaitCleanup().get();
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, ResumeWithLastTokenAfterFailure) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto monitorFp =
        globalFailPointRegistry().find("failReshardingChangeStreamsMonitorAfterProcessingBatch");
    auto timesEntered = monitorFp->setMode(FailPoint::alwaysOn);

    DBDirectClient client(opCtx);
    for (int i = 10; i < 15; i++) {
        client.insert(tempNss, BSON("_id" << i));
    }

    // Run the monitor in another thread.
    auto monitorThread = stdx::thread([&] {
        Client::initThread("monitorThread", getGlobalServiceContext()->getService());

        auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
            reshardingUUID,
            tempNss,
            startAtTime,
            boost::none /* startAfterResumeToken */,
            callback);
        auto awaitCompletion =
            monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);

        ASSERT_EQ(monitor->awaitFinalChangeEvent().getNoThrow().code(), ErrorCodes::InternalError);
        // The cleanup should still succeed.
        monitor->awaitCleanup().get();
        ASSERT_FALSE(hasOpenCursor(tempNss, monitor->makeAggregateComment(reshardingUUID)));
        awaitCompletion.get();
    });

    monitorFp->waitForTimesEntered(timesEntered + 1);
    monitorFp->setMode(FailPoint::off);
    monitorThread.join();

    ASSERT_EQ(delta, 1);
    ASSERT_FALSE(resumeToken.isEmpty());
    ASSERT_FALSE(completed);

    // Resume monitor with the last resume token recorded.
    auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, resumeToken, callback);
    auto awaitCompletion =
        monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);
    insertRecipientFinalEventNoopOplogEntry(tempNss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_EQ(delta, 5);
    ASSERT_FALSE(resumeToken.isEmpty());
    ASSERT(completed);

    monitor->awaitCleanup().get();
    ASSERT_FALSE(hasOpenCursor(tempNss, monitor->makeAggregateComment(reshardingUUID)));
    awaitCompletion.get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, ChangeBatchSizeWhileChangeStreamOpen) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    int numInserts = 20;
    int numEventsTotal = numInserts + 1;  // Add 1 for the 'reshardingDoneCatchup' event.
    auto numEventsBatch0 = 3;
    auto numEventsBatch1 = 100;
    // The monitor should process the insert events in two batches.
    auto numBatches = 2;

    DBDirectClient client(opCtx);
    for (int i = 10; i < 10 + numInserts; i++) {
        client.insert(tempNss, BSON("_id" << i));
    }
    // Perform an update to make sure that the change stream does not output update events for the
    // monitor to process.
    client.update(tempNss,
                  BSON("_id" << 0),
                  BSON("$set" << BSON("x" << 1)),
                  false /*upsert*/,
                  false /*multi*/);

    auto monitortHangFp = globalFailPointRegistry().find(
        "hangReshardingChangeStreamsMonitorBeforeReceivingNextBatch");
    auto timesEntered = monitortHangFp->setMode(FailPoint::alwaysOn);

    // Update the batch size.
    RAIIServerParameterControllerForTest batchSizeServerParameter0{
        "reshardingVerificationChangeStreamsEventsBatchSizeLimit", numEventsBatch0};

    auto monitorThread = stdx::thread([&] {
        Client::initThread("monitorThread", getGlobalServiceContext()->getService());

        auto monitor = std::make_shared<ReshardingChangeStreamsMonitor>(
            reshardingUUID,
            tempNss,
            startAtTime,
            boost::none /* startAfterResumeToken */,
            callback);

        auto awaitCompletion =
            monitor->startMonitoring(executor, cleanupExecutor, cancelSource.token(), *factory);
        insertRecipientFinalEventNoopOplogEntry(tempNss);
        monitor->awaitFinalChangeEvent().get();
        monitor->awaitCleanup().get();
        awaitCompletion.get();

        ASSERT_EQ(monitor->numEventsTotalForTest(), numEventsTotal);
        ASSERT_EQ(monitor->numBatchesForTest(), numBatches);
        ASSERT_FALSE(hasOpenCursor(tempNss, monitor->makeAggregateComment(reshardingUUID)));
    });

    monitortHangFp->waitForTimesEntered(timesEntered + 1);

    // Update the batch size.
    RAIIServerParameterControllerForTest batchSizeServerParameter1{
        "reshardingVerificationChangeStreamsEventsBatchSizeLimit", numEventsBatch1};

    // Turn on failpoint during processing to ensure the monitor will fail if the new batchSize is
    // not respect.
    auto monitorInternalErrorFp =
        globalFailPointRegistry().find("failReshardingChangeStreamsMonitorAfterProcessingBatch");
    monitorInternalErrorFp->setMode(FailPoint::skip, 1);

    monitortHangFp->setMode(FailPoint::off);
    monitorThread.join();
    monitorInternalErrorFp->setMode(FailPoint::off);

    ASSERT_EQ(delta, numInserts);
}

TEST_F(ReshardingChangeStreamsMonitorTest, TestChangeStreamMonitorSettingsForDonor) {
    createCollectionAndInsertDocuments(sourceNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();
    auto donorMonitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, sourceNss, startAtTime, boost::none /* startAfterResumeToken */, callback);

    AggregateCommandRequest donorRequest = donorMonitor->makeAggregateCommandRequest();

    ASSERT_EQ(donorRequest.getNamespace(), sourceNss);

    ASSERT_TRUE(!donorRequest.getPipeline().empty());
    BSONObj donorFirstStage = donorRequest.getPipeline().front();

    auto donorChangeStreamSpec = DocumentSourceChangeStreamSpec::parse(
        donorFirstStage.getObjectField(DocumentSourceChangeStream::kStageName),
        IDLParserContext("TestChangeStreamMonitorSettingsForDonor"));

    ASSERT_FALSE(donorChangeStreamSpec.getShowMigrationEvents());
    ASSERT_TRUE(donorChangeStreamSpec.getShowSystemEvents());
    ASSERT_FALSE(donorChangeStreamSpec.getAllowToRunOnSystemNS());
    ASSERT_FALSE(donorChangeStreamSpec.getShowExpandedEvents());
    ASSERT_TRUE(donorChangeStreamSpec.getShowCommitTimestamp());
}

TEST_F(ReshardingChangeStreamsMonitorTest, TestChangeStreamMonitorSettingsForDonorTimeseries) {
    createTimeseriesCollectionAndInsertDocuments(sourceNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();
    auto donorMonitor =
        std::make_shared<ReshardingChangeStreamsMonitor>(reshardingUUID,
                                                         sourceNss.makeTimeseriesBucketsNamespace(),
                                                         startAtTime,
                                                         boost::none /* startAfterResumeToken */,
                                                         callback);

    AggregateCommandRequest donorRequest = donorMonitor->makeAggregateCommandRequest();

    ASSERT_EQ(donorRequest.getNamespace(), sourceNss.makeTimeseriesBucketsNamespace());

    ASSERT_TRUE(!donorRequest.getPipeline().empty());
    BSONObj donorFirstStage = donorRequest.getPipeline().front();

    auto donorChangeStreamSpec = DocumentSourceChangeStreamSpec::parse(
        donorFirstStage.getObjectField(DocumentSourceChangeStream::kStageName),
        IDLParserContext("TestChangeStreamMonitorSettingsForDonorTimeseries"));

    ASSERT_FALSE(donorChangeStreamSpec.getShowMigrationEvents());
    ASSERT_TRUE(donorChangeStreamSpec.getShowSystemEvents());
    ASSERT_TRUE(donorChangeStreamSpec.getAllowToRunOnSystemNS());
    ASSERT_FALSE(donorChangeStreamSpec.getShowExpandedEvents());
    ASSERT_TRUE(donorChangeStreamSpec.getShowCommitTimestamp());
}

TEST_F(ReshardingChangeStreamsMonitorTest, TestChangeStreamMonitorSettingsForRecipient) {
    createCollectionAndInsertDocuments(tempNss, 0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();
    auto recipientMonitor = std::make_shared<ReshardingChangeStreamsMonitor>(
        reshardingUUID, tempNss, startAtTime, boost::none /* startAfterResumeToken */, callback);

    AggregateCommandRequest recipientRequest = recipientMonitor->makeAggregateCommandRequest();

    ASSERT_EQ(recipientRequest.getNamespace(), tempNss);

    ASSERT_TRUE(!recipientRequest.getPipeline().empty());
    BSONObj recipientFirstStage = recipientRequest.getPipeline().front();

    auto recipientChangeStreamSpec = DocumentSourceChangeStreamSpec::parse(
        recipientFirstStage.getObjectField(DocumentSourceChangeStream::kStageName),
        IDLParserContext("TestChangeStreamMonitorSettingsForRecipient"));

    ASSERT_TRUE(recipientChangeStreamSpec.getShowMigrationEvents());
    ASSERT_FALSE(recipientChangeStreamSpec.getShowSystemEvents());
    ASSERT_TRUE(recipientChangeStreamSpec.getAllowToRunOnSystemNS());
    ASSERT_FALSE(recipientChangeStreamSpec.getShowExpandedEvents());
    ASSERT_TRUE(recipientChangeStreamSpec.getShowCommitTimestamp());
}

}  // namespace
}  // namespace mongo
