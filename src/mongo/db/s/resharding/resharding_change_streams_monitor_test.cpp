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

#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/s/resharding/resharding_change_event_o2_field_gen.h"
#include "mongo/db/s/resharding/resharding_change_streams_monitor.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/s/catalog/sharding_catalog_client_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

class ReshardingChangeStreamsMonitorTest : public ShardServerTestFixtureWithCatalogCacheMock {
public:
    void setUp() override {
        ShardServerTestFixtureWithCatalogCacheMock::setUp();

        opCtx = operationContext();
        executor = makeTaskExecutor();
    }

    void tearDown() override {
        executor->shutdown();
        executor->join();

        ShardServerTestFixtureWithCatalogCacheMock::tearDown();
    }

    // Create a collection on nss and inserts document with '_id' and 'x' ranging from minDocValue
    // to maxDocValue(inclusive).
    void createCollectionAndInsertDocuments(int minDocValue, int maxDocValue) {
        AutoGetDb autoDb(opCtx, nss.dbName(), LockMode::MODE_X);
        autoDb.ensureDbExists(opCtx);

        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx);
        uassertStatusOK(createCollection(opCtx, nss.dbName(), BSON("create" << nss.coll())));

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

    void createInstanceAndStartMonitoring(Timestamp startAt) {
        monitor = std::make_unique<ReshardingChangeStreamsMonitor>(
            ReshardingChangeStreamsMonitor(nss, startAt, true /*isRecipient*/, callback));

        auto factory = makeCancelableOpCtx();
        monitor->startMonitoring(executor, factory);
    }

    void createInstanceAndStartMonitoring(BSONObj startAfter) {
        monitor = std::make_unique<ReshardingChangeStreamsMonitor>(
            ReshardingChangeStreamsMonitor(nss, startAfter, true /*isRecipient*/, callback));

        auto factory = makeCancelableOpCtx();
        monitor->startMonitoring(executor, factory);
    }

    template <typename Callable>
    void runInTransaction(bool abortTxn, Callable&& func) {
        DBDirectClient client(opCtx);
        ASSERT(client.createCollection(NamespaceString::kSessionTransactionsTableNamespace));
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        auto sessionId = makeLogicalSessionIdForTest();
        const TxnNumber txnNum = 0;

        opCtx->setLogicalSessionId(sessionId);
        opCtx->setTxnNumber(txnNum);
        opCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx);

        auto txnParticipant = TransactionParticipant::get(opCtx);
        ASSERT(txnParticipant);
        txnParticipant.beginOrContinue(opCtx,
                                       {txnNum},
                                       false /* autocommit */,
                                       TransactionParticipant::TransactionActions::kStart);
        txnParticipant.unstashTransactionResources(opCtx, "SetDestinedRecipient");

        func();

        if (abortTxn) {
            txnParticipant.abortTransaction(opCtx);
        } else {
            txnParticipant.commitUnpreparedTransaction(opCtx);
        }
        txnParticipant.stashTransactionResources(opCtx);
    }

    void insertNoopFinalEvent(NamespaceString ns, BSONObj msg, BSONObj o2Field) {
        AutoGetCollection coll(opCtx, ns, LockMode::MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        opCtx->getServiceContext()->getOpObserver()->onInternalOpMessage(
            opCtx,
            coll.getCollection()->ns(),
            coll.getCollection()->uuid(),
            msg,
            o2Field,
            boost::none,
            boost::none,
            boost::none,
            boost::none);
        wuow.commit();
    }

    void fulfillRecipientFinalEventPromise(NamespaceString ns, UUID reshardingUuid = UUID::gen()) {
        auto msg = BSON("msg"
                        << "The temporary resharding collection now has a "
                           "strictly consistent view of the data");
        ReshardDoneCatchUpChangeEventO2Field o2Field{ns, reshardingUuid};
        insertNoopFinalEvent(ns, msg, o2Field.toBSON());
    }

    std::shared_ptr<executor::ThreadPoolTaskExecutor> makeTaskExecutor() {
        executor::ThreadPoolMock::Options threadPoolOptions;
        threadPoolOptions.onCreateThread = [this] {
            Client::initThread("ReshardingChangeStreamsMonitorThreadPool",
                               getServiceContext()->getService());
        };

        auto executor = executor::makeThreadPoolTestExecutor(
            std::make_unique<executor::NetworkInterfaceMock>(), std::move(threadPoolOptions));

        executor->startup();
        return executor;
    }

    CancelableOperationContextFactory makeCancelableOpCtx() {
        auto cancelableOpCtxExecutor = std::make_shared<ThreadPool>([] {
            ThreadPool::Options options;
            options.poolName = "ReshardingChangeStreamsMonitorCancelableOpCtxPool";
            options.minThreads = 1;
            options.maxThreads = 1;
            return options;
        }());

        return CancelableOperationContextFactory(opCtx->getCancellationToken(),
                                                 cancelableOpCtxExecutor);
    }

protected:
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("db", "coll");
    OperationContext* opCtx;
    std::shared_ptr<executor::ThreadPoolTaskExecutor> executor;
    std::unique_ptr<ReshardingChangeStreamsMonitor> monitor;

    // Set the batch size 1 to test multi-batch processing in unit tests with multiple events.
    RAIIServerParameterControllerForTest batchSize{
        "reshardingVerificationChangeStreamsEventsBatchSize", 1};

    int delta = 0;
    BSONObj lastResume;
    ReshardingChangeStreamsMonitor::BatchProcessedCallback callback = [&](int documentDelta,
                                                                          BSONObj resumeToken) {
        delta += documentDelta;
        lastResume = resumeToken.getOwned();
    };
};

TEST_F(ReshardingChangeStreamsMonitorTest, SuccessfullyInitializeMonitorWithStartAtTime) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    createInstanceAndStartMonitoring(startAtTime);

    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == 0);
    ASSERT_FALSE(lastResume.isEmpty());
}

DEATH_TEST_REGEX_F(ReshardingChangeStreamsMonitorTest,
                   FailIfAwaitEventCalledBeforeStartMonitoring,
                   "Tripwire assertion.*1009073") {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    monitor = std::make_unique<ReshardingChangeStreamsMonitor>(
        ReshardingChangeStreamsMonitor(nss, startAtTime, true /*isRecipient*/, callback));

    monitor->awaitFinalChangeEvent().get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, StartMonitoringCalledTwiceSuccessful) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    monitor = std::make_unique<ReshardingChangeStreamsMonitor>(
        ReshardingChangeStreamsMonitor(nss, startAtTime, true /*isRecipient*/, callback));

    auto factory = makeCancelableOpCtx();
    monitor->startMonitoring(executor, factory);
    monitor->startMonitoring(executor, factory);
}


TEST_F(ReshardingChangeStreamsMonitorTest, ProcessSingleInsert) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    client.insert(nss, BSON("_id" << 10));

    createInstanceAndStartMonitoring(startAtTime);

    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == 1);
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessSingleInsertWithMultiDocs) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    write_ops::InsertCommandRequest insertOp(nss);
    insertOp.setDocuments({BSON("_id" << 10), BSON("_id" << 11)});
    client.insert(insertOp);

    createInstanceAndStartMonitoring(startAtTime);

    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == 2);
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessMultipleInserts) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    for (int i = 10; i < 13; i++) {
        client.insert(nss, BSON("_id" << i));
    }

    createInstanceAndStartMonitoring(startAtTime);

    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == 3);
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessSingleDelete) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    client.remove(nss, BSON("_id" << 0), false);

    createInstanceAndStartMonitoring(startAtTime);

    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == -1);
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessSingleDeleteMany) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    // Update doc {x:0} to {x:1} to ensure there are two documents matching the delete query.
    client.update(
        nss, BSON("x" << 0), BSON("$set" << BSON("x" << 1)), false /*upsert*/, false /*multi*/);
    client.remove(nss, BSON("x" << 1), true /*multi*/);

    createInstanceAndStartMonitoring(startAtTime);

    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == -2);
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessMultipleDeletes) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    for (int i = 0; i < 3; i++) {
        client.remove(nss, BSON("_id" << i), false);
    }

    createInstanceAndStartMonitoring(startAtTime);

    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == -3);
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessMultipleInsertsDeletes) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    for (int i = 10; i < 15; i++) {
        client.insert(nss, BSON("_id" << i));
        client.remove(nss, BSON("_id" << i), false);
    }

    createInstanceAndStartMonitoring(startAtTime);

    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == 0);
}

TEST_F(ReshardingChangeStreamsMonitorTest, DisregardUpdates) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    client.update(
        nss, BSON("_id" << 0), BSON("$set" << BSON("x" << 5)), false /*upsert*/, true /*multi*/);

    createInstanceAndStartMonitoring(startAtTime);

    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == 0);
}

TEST_F(ReshardingChangeStreamsMonitorTest, EnsurePromiseFulfilledOnReachingRecipientFinalEvent) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    fulfillRecipientFinalEventPromise(nss);

    createInstanceAndStartMonitoring(startAtTime);
    monitor->awaitFinalChangeEvent().get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, EnsurePromiseFulfilledOnReachingDonorFinalEvent) {
    AutoGetDb autoDb(opCtx, nss.dbName(), LockMode::MODE_X);
    autoDb.ensureDbExists(opCtx);
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
        opCtx);
    uassertStatusOK(createCollection(opCtx, nss.dbName(), BSON("create" << nss.coll())));
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto msg = BSON("msg"
                    << "Writes to {} are temporarily blocked for resharding");
    ReshardBlockingWritesChangeEventO2Field o2Field{
        nss, UUID::gen(), resharding::kReshardFinalOpLogType.toString()};

    insertNoopFinalEvent(nss, msg, o2Field.toBSON());

    monitor = std::make_unique<ReshardingChangeStreamsMonitor>(
        ReshardingChangeStreamsMonitor(nss, startAtTime, false /*isRecipient*/, callback));

    auto factory = makeCancelableOpCtx();
    monitor->startMonitoring(executor, factory);

    monitor->awaitFinalChangeEvent().get();
}

TEST_F(ReshardingChangeStreamsMonitorTest, ProcessInsertsAndDeletesInTransaction) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    runInTransaction(false /*abort*/, [&]() {
        DBDirectClient client(opCtx);
        write_ops::InsertCommandRequest insertOp(nss);
        insertOp.setDocuments({BSON("_id" << 10), BSON("_id" << 11), BSON("_id" << 12)});
        client.insert(insertOp);

        client.insert(nss, BSON("_id" << 13));
        client.remove(nss, BSON("_id" << 0), false);
        client.update(nss,
                      BSON("x" << 13),
                      BSON("$set" << BSON("y" << 13)),
                      false /*upsert*/,
                      false /*multi*/);
    });

    createInstanceAndStartMonitoring(startAtTime);

    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == 3);
}

TEST_F(ReshardingChangeStreamsMonitorTest, AbortedTxnShouldNotIncrementDelta) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    runInTransaction(true /*abort*/, [&]() {
        DBDirectClient client(opCtx);
        client.insert(nss, BSON("_id" << 10));
        client.insert(nss, BSON("_id" << 11));
        client.remove(nss, BSON("_id" << 0), false);
    });

    createInstanceAndStartMonitoring(startAtTime);

    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == 0);
}

TEST_F(ReshardingChangeStreamsMonitorTest, ResumeWithLastTokenAfterFailure) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    auto monitorFp =
        globalFailPointRegistry().find("failReshardingChangeStreamsMonitorAfterProcessingBatch");
    auto timesEntered = monitorFp->setMode(FailPoint::alwaysOn);

    DBDirectClient client(opCtx);
    for (int i = 10; i < 15; i++) {
        client.insert(nss, BSON("_id" << i));
    }

    // Run the monitor in another thread.
    auto monitorThread = stdx::thread([&] {
        Client::initThread("monitorThread", getGlobalServiceContext()->getService());

        createInstanceAndStartMonitoring(startAtTime);
        ASSERT_EQ(monitor->awaitFinalChangeEvent().getNoThrow().code(), ErrorCodes::InternalError);
    });

    monitorFp->waitForTimesEntered(timesEntered + 1);
    monitorFp->setMode(FailPoint::off);
    monitorThread.join();

    ASSERT_TRUE(delta == 1);
    ASSERT_FALSE(lastResume.isEmpty());

    // Resume monitor with the last resume token recorded.
    createInstanceAndStartMonitoring(lastResume);
    fulfillRecipientFinalEventPromise(nss);
    monitor->awaitFinalChangeEvent().get();

    ASSERT_TRUE(delta == 5);
    ASSERT_FALSE(lastResume.isEmpty());
}

TEST_F(ReshardingChangeStreamsMonitorTest, ChangeBatchSizeWhileChangeStreamOpen) {
    createCollectionAndInsertDocuments(0 /*minDocValue*/, 9 /*maxDocValue*/);
    Timestamp startAtTime = replicationCoordinator()->getMyLastAppliedOpTime().getTimestamp();

    DBDirectClient client(opCtx);
    for (int i = 10; i < 30; i++) {
        client.insert(nss, BSON("_id" << i));
    }

    auto monitortHangFp = globalFailPointRegistry().find(
        "hangReshardingChangeStreamsMonitorBeforeRecievingNextBatch");
    auto timesEntered = monitortHangFp->setMode(FailPoint::alwaysOn);

    auto monitorThread = stdx::thread([&] {
        Client::initThread("monitorThread", getGlobalServiceContext()->getService());

        createInstanceAndStartMonitoring(startAtTime);
        fulfillRecipientFinalEventPromise(nss);
        monitor->awaitFinalChangeEvent().get();
    });

    monitortHangFp->waitForTimesEntered(timesEntered + 1);

    // Update the batchSize to 100. Previoulsy, the batchSize was 1.
    RAIIServerParameterControllerForTest newbatchSize{
        "reshardingVerificationChangeStreamsEventsBatchSize", 100};

    // Turn on failpoint during processing to ensure the monitor will fail if the new batchSize is
    // not respect.
    auto monitorInternalErrorFp =
        globalFailPointRegistry().find("failReshardingChangeStreamsMonitorAfterProcessingBatch");
    monitorInternalErrorFp->setMode(FailPoint::skip, 1);

    monitortHangFp->setMode(FailPoint::off);
    monitorThread.join();
    monitorInternalErrorFp->setMode(FailPoint::off);

    ASSERT_TRUE(delta == 20);
}

}  // namespace
}  // namespace mongo
