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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/fail_point.h"

namespace mongo::replicated_fast_count {
namespace {

/**
 * Tests replicated fast count across multidocument transactions.
 */
class ReplicatedFastCountTxnFixture : public MockReplCoordServerFixture {
public:
    explicit ReplicatedFastCountTxnFixture(Options options = {})
        : MockReplCoordServerFixture(options.useReplSettings(true)) {}

protected:
    void setUp() override {
        repl::ReplSettings replSettings;
        replSettings.setReplSetString("realReplicaSet");
        setGlobalReplSettings(replSettings);

        MockReplCoordServerFixture::setUp();

        auto* service = opCtx()->getServiceContext();

        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());

        MongoDSessionCatalog::set(
            service,
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

        auto* registry = dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
        ASSERT(registry);
        registry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx());
        mongoDSessionCatalog->onStepUp(opCtx());

        // Now your replicated fast count setup, using opCtx()
        _opCtx = opCtx();
        _fastCountManager = &ReplicatedFastCountManager::get(_opCtx->getServiceContext());
        _fastCountManager->disablePeriodicWrites_ForTest();

        setUpReplicatedFastCount(_opCtx);

        ASSERT_OK(createCollection(_opCtx, _nss1.dbName(), BSON("create" << _nss1.coll())));
        ASSERT_OK(createCollection(_opCtx, _nss2.dbName(), BSON("create" << _nss2.coll())));
        auto coll1 = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss1, AcquisitionPrerequisites::kRead),
            LockMode::MODE_IS);
        auto coll2 = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss2, AcquisitionPrerequisites::kRead),
            LockMode::MODE_IS);
        _uuid1 = coll1.uuid();
        _uuid2 = coll2.uuid();
    }

    void tearDown() override {
        if (_fastCountManager) {
            _fastCountManager = nullptr;
        }
        MockReplCoordServerFixture::tearDown();
    }

    /**
     * With a fresh OperationContext, starts a transaction with the given session id and transaction
     * number, and runs the given callback function.
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

        txnParticipant.unstashTransactionResources(opCtx, "ReplicatedFastCountTxns");
        func(opCtx);
        txnParticipant.stashTransactionResources(opCtx);
    }
    template <typename Callable>
    void beginTxn(LogicalSessionId sessionId, TxnNumber txnNumber, Callable&& func) {
        auto newClientOwned = getServiceContext()->getService()->makeClient("newClient");
        AlternativeClientRegion acr(newClientOwned);
        auto opCtxHolder = cc().makeOperationContext();
        beginTxn(opCtxHolder.get(), sessionId, txnNumber, func);
    }

    /**
     * With a fresh OperationContext, continues the transaction with the given session id and
     * transaction number, and runs 'func' before committing.
     */
    template <typename Callable>
    void continueAndCommitTxn(LogicalSessionId sessionId, TxnNumber txnNumber, Callable&& func) {
        auto newClientOwned = getServiceContext()->getService()->makeClient("newClient");
        AlternativeClientRegion acr(newClientOwned);
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();

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
        func(opCtx);
        txnParticipant.commitUnpreparedTransaction(opCtx);

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
    void abortTxn(LogicalSessionId sessionId, TxnNumber txnNumber) {
        auto newClientOwned = getServiceContext()->getService()->makeClient("newClient");
        AlternativeClientRegion acr(newClientOwned);
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();
        abortTxn(opCtx, sessionId, txnNumber);
    }

    OperationContext* _opCtx = nullptr;
    ReplicatedFastCountManager* _fastCountManager = nullptr;
    NamespaceString _nss1 =
        NamespaceString::createNamespaceString_forTest("replicated_fast_count_test", "coll1");
    NamespaceString _nss2 =
        NamespaceString::createNamespaceString_forTest("replicated_fast_count_test", "coll2");
    UUID _uuid1 = UUID::gen();
    UUID _uuid2 = UUID::gen();
};

class ReplicatedFastCountTxnTest : public ReplicatedFastCountTxnFixture {
public:
    ReplicatedFastCountTxnTest()
        : ReplicatedFastCountTxnFixture(Options().setPersistenceProvider(
              std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>())) {}
};

TEST_F(ReplicatedFastCountTxnTest,
       UncommittedChangesPreservedAcrossResumedMultiDocumentTransactionCommit) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    auto doc1 = BSON("_id" << 0 << "x" << 1);
    auto doc2 = BSON("_id" << 1 << "x" << 2);
    const int64_t expectedCount = 2;
    const int64_t expectedSize = doc1.objsize() + doc2.objsize();

    boost::optional<UUID> uuid;
    auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNumber(0);

    // Start the transaction and perform the insert on a fresh OperationContext.
    beginTxn(sessionId, txnNumber, [&](OperationContext* opCtx1) {
        auto coll = acquireCollection(opCtx1,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          opCtx1, _nss1, AcquisitionPrerequisites::kWrite),
                                      LockMode::MODE_IX);
        uuid = coll.uuid();

        {
            WriteUnitOfWork wuow{opCtx1};
            ASSERT_OK(Helpers::insert(opCtx1, coll.getCollectionPtr(), doc1));
            wuow.commit();
        }

        // Since the transaction as a whole hasn't been committed, expect doc1 to only count toward
        // uncommitted changes.
        test_helpers::checkCommittedFastCountChanges(*uuid, _fastCountManager, 0, 0);
        test_helpers::checkUncommittedFastCountChanges(opCtx1, *uuid, 1, doc1.objsize());
    });

    // The insert shouldn't be visible outside the transaction.
    ASSERT(uuid.has_value());
    test_helpers::checkCommittedFastCountChanges(*uuid, _fastCountManager, 0, 0);
    test_helpers::checkUncommittedFastCountChanges(_opCtx, *uuid, 0, 0);

    // Continue and commit the transaction.
    continueAndCommitTxn(sessionId, txnNumber, [&](OperationContext* opCtx2) {
        auto coll = acquireCollection(opCtx2,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          opCtx2, _nss1, AcquisitionPrerequisites::kWrite),
                                      LockMode::MODE_IX);
        ASSERT_EQ(coll.uuid(), *uuid);

        {
            WriteUnitOfWork wuow{opCtx2};
            ASSERT_OK(Helpers::insert(opCtx2, coll.getCollectionPtr(), doc2));
            wuow.commit();
        }

        // Uncommitted fast count changes should include both inserts, even though they were
        // executed on different OperationContexts.
        test_helpers::checkCommittedFastCountChanges(*uuid, _fastCountManager, 0, 0);
        test_helpers::checkUncommittedFastCountChanges(opCtx2, *uuid, expectedCount, expectedSize);
    });

    test_helpers::checkCommittedFastCountChanges(
        *uuid, _fastCountManager, expectedCount, expectedSize);
    test_helpers::checkUncommittedFastCountChanges(_opCtx, *uuid, 0, 0);
}

TEST_F(ReplicatedFastCountTxnTest, UncommittedChangesDiscardedAfterMultiDocumentTxnAbort) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    auto doc1 = BSON("_id" << 0 << "x" << 1);

    boost::optional<UUID> uuid;
    auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNumber(0);

    // Start the transaction and perform the insert on a fresh OperationContext.
    beginTxn(sessionId, txnNumber, [&](OperationContext* opCtx1) {
        auto coll = acquireCollection(opCtx1,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          opCtx1, _nss1, AcquisitionPrerequisites::kWrite),
                                      LockMode::MODE_IX);
        uuid = coll.uuid();

        {
            WriteUnitOfWork wuow{opCtx1};
            ASSERT_OK(Helpers::insert(opCtx1, coll.getCollectionPtr(), doc1));
            wuow.commit();
        }

        // Since the transaction as a whole hasn't been committed, expect doc1 to only count toward
        // uncommitted changes.
        test_helpers::checkCommittedFastCountChanges(*uuid, _fastCountManager, 0, 0);
        test_helpers::checkUncommittedFastCountChanges(opCtx1, *uuid, 1, doc1.objsize());
    });

    // The insert shouldn't be visible outside the transaction.
    ASSERT(uuid.has_value());
    test_helpers::checkCommittedFastCountChanges(*uuid, _fastCountManager, 0, 0);
    test_helpers::checkUncommittedFastCountChanges(_opCtx, *uuid, 0, 0);

    abortTxn(sessionId, txnNumber);

    // Confirm the uncommitted changes were discarded.
    test_helpers::checkCommittedFastCountChanges(*uuid, _fastCountManager, 0, 0);
    test_helpers::checkUncommittedFastCountChanges(_opCtx, *uuid, 0, 0);
}

TEST_F(ReplicatedFastCountTxnTest, FastCountResetForSessionBetweenTransactions) {
    // Tests that the 'UncommittedFastCountChange' is reset when there is a new
    // 'RecoveryUnit::Snapshot', even across a single OperationContext.
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const auto doc1 = BSON("_id" << 0 << "x" << 1);

    boost::optional<UUID> uuid;
    auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNumber(0);

    beginTxn(_opCtx, sessionId, txnNumber, [&](OperationContext* opCtx) {
        auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, _nss1, AcquisitionPrerequisites::kWrite),
            LockMode::MODE_IX);
        uuid = coll.uuid();

        {
            WriteUnitOfWork wuow{opCtx};
            ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), doc1));
            wuow.commit();
        }

        test_helpers::checkCommittedFastCountChanges(*uuid, _fastCountManager, 0, 0);
        test_helpers::checkUncommittedFastCountChanges(opCtx, *uuid, 1, doc1.objsize());
    });
    abortTxn(_opCtx, sessionId, txnNumber);

    txnNumber = TxnNumber(2);
    beginTxn(_opCtx, sessionId, txnNumber, [&](OperationContext* opCtx) {
        // Nothing leaked over from the previous transaction on the session.
        test_helpers::checkCommittedFastCountChanges(*uuid, _fastCountManager, 0, 0);
        test_helpers::checkUncommittedFastCountChanges(opCtx, *uuid, 0, 0);
    });
    abortTxn(_opCtx, sessionId, txnNumber);
}

TEST_F(ReplicatedFastCountTxnTest, ApplyOpsOplogEntryContainsSizeDeltaMetadataSingleInsert) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    auto doc = BSON("_id" << 0 << "x" << 1);
    auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNumber(0);
    UUID uuid = UUID::gen();
    beginTxn(sessionId, txnNumber, [&](OperationContext* opCtx1) {
        auto coll = acquireCollection(opCtx1,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          opCtx1, _nss1, AcquisitionPrerequisites::kWrite),
                                      LockMode::MODE_IX);
        {
            WriteUnitOfWork wuow{opCtx1};
            ASSERT_OK(Helpers::insert(opCtx1, coll.getCollectionPtr(), doc));
            wuow.commit();
        }
        uuid = coll.uuid();
    });
    continueAndCommitTxn(sessionId, txnNumber, [&](OperationContext*) {});

    const auto applyOpsOplogEntry = test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1);
    std::vector<repl::OplogEntry> innerEntries;
    repl::ApplyOps::extractOperationsTo(
        applyOpsOplogEntry, applyOpsOplogEntry.getEntry().toBSON(), &innerEntries);

    // Confirm the applyOps entry generated includes replicated size count metadata in the inner
    // insert oplog entry.
    ASSERT_EQ(1, innerEntries.size());
    const auto insertOp = innerEntries[0];
    test_helpers::assertOpMatchesSpec(
        insertOp,
        {.uuid = uuid, .opType = repl::OpTypeEnum::kInsert, .expectedSizeDelta = doc.objsize()});
}

TEST_F(ReplicatedFastCountTxnTest, ApplyOpsOplogEntryContainsSizeDeltaMetadata) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    // Both collections begin empty.
    EXPECT_EQ(CollectionSizeCount{}, test_helpers::scanForAccurateSizeCount(_opCtx, _nss1));
    EXPECT_EQ(CollectionSizeCount{}, test_helpers::scanForAccurateSizeCount(_opCtx, _nss2));

    std::vector<test_helpers::OpValidationSpec> expectedOps{};
    const auto doc = BSON("_id" << 0 << "x" << 1);
    const auto sessionId = makeLogicalSessionIdForTest();
    const TxnNumber txnNumber(0);

    // Perform ops on 2 separate collections as a part of the same multi-doc transaction.
    beginTxn(sessionId, txnNumber, [&](OperationContext* opCtx1) {
        // '_nss1': Insert op and update op.
        {
            auto coll = acquireCollection(opCtx1,
                                          CollectionAcquisitionRequest::fromOpCtx(
                                              opCtx1, _nss1, AcquisitionPrerequisites::kWrite),
                                          MODE_IX);
            const auto uuid = coll.uuid();
            {
                WriteUnitOfWork wuow{opCtx1};
                ASSERT_OK(Helpers::insert(opCtx1, coll.getCollectionPtr(), doc));
                wuow.commit();
                expectedOps.push_back({.uuid = uuid,
                                       .opType = repl::OpTypeEnum::kInsert,
                                       .expectedSizeDelta = doc.objsize()});
            }

            {
                WriteUnitOfWork wuow{opCtx1};
                Helpers::update(opCtx1,
                                coll,
                                BSON("_id" << 0),
                                BSON("$set" << BSON("note" << "Make Doc Larger")));
                wuow.commit();
                const auto docSizeAfterUpdate =
                    Helpers::findOneForTesting(opCtx1, coll, BSON("_id" << 0)).objsize();
                const auto updateDelta = docSizeAfterUpdate - doc.objsize();
                expectedOps.push_back({.uuid = uuid,
                                       .opType = repl::OpTypeEnum::kUpdate,
                                       .expectedSizeDelta = updateDelta});
            }
        }

        // '_nss2': Insert op and delete op.
        {
            auto coll = acquireCollection(opCtx1,
                                          CollectionAcquisitionRequest::fromOpCtx(
                                              opCtx1, _nss2, AcquisitionPrerequisites::kWrite),
                                          MODE_IX);
            const auto uuid = coll.uuid();
            {
                WriteUnitOfWork wuow{opCtx1};
                ASSERT_OK(Helpers::insert(opCtx1, coll.getCollectionPtr(), doc));
                wuow.commit();
                expectedOps.push_back({.uuid = uuid,
                                       .opType = repl::OpTypeEnum::kInsert,
                                       .expectedSizeDelta = doc.objsize()});
            }
            const auto rid = Helpers::findOne(opCtx1, coll, BSON("_id" << 0));
            ASSERT_FALSE(rid.isNull());

            {
                WriteUnitOfWork wuow{opCtx1};
                Helpers::deleteByRid(opCtx1, coll, rid);
                wuow.commit();
                expectedOps.push_back({.uuid = uuid,
                                       .opType = repl::OpTypeEnum::kDelete,
                                       .expectedSizeDelta = -doc.objsize()});
            }
        }
    });
    continueAndCommitTxn(sessionId, txnNumber, [&](OperationContext*) {});

    // The applyOps should cover both namespaces, so searching by _nss1 should be sufficient.
    const auto applyOpsOplogEntry = test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1);

    // Validate the logging of the sizeMetadata.
    std::vector<repl::OplogEntry> innerEntries;
    repl::ApplyOps::extractOperationsTo(
        applyOpsOplogEntry, applyOpsOplogEntry.getEntry().toBSON(), &innerEntries);
    test_helpers::assertOpsMatchSpecs(innerEntries, expectedOps);

    // Validate the logged sizeMetadata can be parsed back into to accurate size and count.
    //
    // The total count and size for each collection should be equal to aggregated deltas given the
    // collection began empty before the transaction.
    const auto deltas = test_helpers::extractSizeCountDeltasForApplyOps(applyOpsOplogEntry);
    // 2 UUIDs had replicated size count information updated from the transaction.
    EXPECT_EQ(2u, deltas.size());

    const auto expectedDeltasColl1 = test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    ASSERT_TRUE(deltas.contains(_uuid1));
    EXPECT_EQ(expectedDeltasColl1, deltas.at(_uuid1));

    const auto expectedDeltasColl2 = test_helpers::scanForAccurateSizeCount(_opCtx, _nss2);
    ASSERT_TRUE(deltas.contains(_uuid2));
    EXPECT_EQ(expectedDeltasColl2, deltas.at(_uuid2));
}


/**
 * Tests that prepared transactions write sizeMetadata to the session txn record and commit oplog
 * entry.
 */
class PreparedSizeMetadataTest : public ReplicatedFastCountTxnFixture {
protected:
    PreparedSizeMetadataTest(Options options = {})
        : ReplicatedFastCountTxnFixture(options.useReplSettings(true)) {}
    void setUp() override {
        ReplicatedFastCountTxnFixture::setUp();
        setGlobalFailPoint("skipCommitTxnCheckPrepareMajorityCommitted",
                           BSON("mode" << "alwaysOn"));
    }

    void tearDown() override {
        setGlobalFailPoint("skipCommitTxnCheckPrepareMajorityCommitted", BSON("mode" << "off"));
        ReplicatedFastCountTxnFixture::tearDown();
    }

    const BSONObj docA = BSON("_id" << 0 << "data" << "x");
    const BSONObj docB = BSON("_id" << 1 << "data" << "y");
    const BSONObj docC = BSON("_id" << 0 << "data" << "z");

    struct ExpectedSizeEntry {
        UUID uuid;
        int64_t sz;
        int64_t ct;
    };

    /**
     * Appends an in-memory 'insert' operation for each doc to the transaction's operation list.
     * Automatically generates the size delta and includes it in the transaction operation. No
     * writes are persisted to the `nss` collection.
     */
    void addTransactionInsertOps(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 const std::vector<BSONObj>& docs) {
        auto coll = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, nss, AcquisitionPrerequisites::kWrite),
            MODE_IX);
        auto txnParticipant = TransactionParticipant::get(opCtx);
        for (const auto& doc : docs) {
            auto operation = repl::DurableOplogEntry::makeInsertOperation(
                nss, coll.uuid(), doc, doc["_id"].wrap());
            operation.setSizeMetadata(repl::OplogEntrySizeMetadata{
                SingleOpSizeMetadata(static_cast<int32_t>(doc.objsize()))});
            txnParticipant.addTransactionOperation(opCtx, operation);
        }
    }

    Timestamp prepareTxn(LogicalSessionId sessionId, TxnNumber txnNumber) {
        auto newClientOwned = getServiceContext()->getService()->makeClient("prepareTxnClient");
        AlternativeClientRegion acr(newClientOwned);
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();

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
        txnParticipant.unstashTransactionResources(opCtx, "prepareTransaction");
        auto prepareTs = txnParticipant.prepareTransaction(opCtx, {}).first;
        txnParticipant.stashTransactionResources(opCtx);
        return prepareTs;
    }

    void commitPreparedTxn(LogicalSessionId sessionId, TxnNumber txnNumber, Timestamp prepareTs) {
        auto newClientOwned =
            getServiceContext()->getService()->makeClient("commitPreparedTxnClient");
        AlternativeClientRegion acr(newClientOwned);
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();

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
        const auto commitTs = Timestamp(prepareTs.getSecs(), prepareTs.getInc() + 1);
        txnParticipant.commitPreparedTransaction(opCtx, commitTs, {});
        txnParticipant.stashTransactionResources(opCtx);
    }

    SessionTxnRecord getTxnRecord(const LogicalSessionId& sessionId) {
        DBDirectClient client(_opCtx);
        FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
        findRequest.setFilter(BSON("_id" << sessionId.toBSON()));
        auto cursor = client.find(std::move(findRequest));
        ASSERT(cursor && cursor->more());
        auto record = SessionTxnRecord::parse(cursor->next(), IDLParserContext("getTxnRecord"));
        ASSERT(!cursor->more());
        return record;
    }

    void assertSizeMetadataEqual(const std::vector<MultiOpSizeMetadata>& actual,
                                 const std::vector<MultiOpSizeMetadata>& expected) {
        std::vector<ExpectedSizeEntry> entries;
        for (const auto& e : expected) {
            entries.push_back({e.getUuid(), e.getSz(), e.getCt()});
        }
        assertSizeMetadata(actual, entries);
    }

    void assertSizeMetadata(const std::vector<MultiOpSizeMetadata>& actual,
                            std::vector<ExpectedSizeEntry> expected) {
        EXPECT_EQ(actual.size(), expected.size());
        std::unordered_map<UUID, const MultiOpSizeMetadata*, UUID::Hash> byUuid;
        for (const auto& entry : actual) {
            byUuid[entry.getUuid()] = &entry;
        }
        for (const auto& [uuid, sz, ct] : expected) {
            ASSERT_TRUE(byUuid.count(uuid));
            EXPECT_EQ(byUuid[uuid]->getSz(), sz);
            EXPECT_EQ(byUuid[uuid]->getCt(), ct);
        }
    }

    /**
     * Prepares the given transaction and returns a SessionTxnRecordForPrepareRecovery capturing
     * its state, including any computed sizeMetadata. The session is stashed before returning.
     */
    SessionTxnRecordForPrepareRecovery prepareTxnAndCaptureRecoveryRecord(
        LogicalSessionId sessionId, TxnNumber txnNumber) {
        auto newClientOwned = getServiceContext()->getService()->makeClient("prepareTxnClient");
        AlternativeClientRegion acr(newClientOwned);
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();

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
        txnParticipant.unstashTransactionResources(opCtx, "prepareTransaction");
        txnParticipant.prepareTransaction(opCtx, {});

        SessionTxnRecord txnRecord;
        txnRecord.setState(DurableTxnStateEnum::kPrepared);
        txnRecord.setSessionId(sessionId);
        txnRecord.setTxnNum(txnNumber);
        txnRecord.setLastWriteOpTime(txnParticipant.getLastWriteOpTime());
        txnRecord.setLastWriteDate(Date_t::now());
        txnParticipant.addPreparedTransactionPreciseCheckpointRecoveryFields(txnRecord);
        if (const auto& sizeMetadata = txnParticipant.getPreparedSizeMetadata()) {
            txnRecord.setSizeMetadata(*sizeMetadata);
        }

        txnParticipant.stashTransactionResources(opCtx);
        return SessionTxnRecordForPrepareRecovery(std::move(txnRecord));
    }

    /**
     * Restores a prepared transaction from a precise checkpoint record on a fresh client and
     * returns the participant's preparedSizeMetadata.
     */
    boost::optional<std::vector<MultiOpSizeMetadata>> restoreFromCheckpointAndGetSizeMetadata(
        LogicalSessionId sessionId,
        TxnNumber txnNumber,
        SessionTxnRecordForPrepareRecovery recoveryRecord) {
        auto newClientOwned = getServiceContext()->getService()->makeClient("recoverClient");
        AlternativeClientRegion acr(newClientOwned);
        auto opCtxHolder = cc().makeOperationContext();
        auto opCtx = opCtxHolder.get();

        opCtx->setLogicalSessionId(sessionId);
        opCtx->setTxnNumber(txnNumber);
        opCtx->setInMultiDocumentTransaction();

        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
        auto ocs = mongoDSessionCatalog->checkOutSessionWithoutRefresh(opCtx);
        auto txnParticipant = TransactionParticipant::get(opCtx);

        txnParticipant.unstashTransactionResources(opCtx, "prepareTransaction");
        for (const auto& ns : recoveryRecord.getAffectedNamespaces()) {
            (void)acquireCollection(opCtx,
                                    CollectionAcquisitionRequest::fromOpCtx(
                                        opCtx, ns, AcquisitionPrerequisites::kWrite),
                                    MODE_IX);
        }

        txnParticipant.restorePreparedTxnFromPreciseCheckpoint(opCtx, std::move(recoveryRecord));
        return txnParticipant.getPreparedSizeMetadata();
    }
};

TEST_F(PreparedSizeMetadataTest, PrepareTransactionWritesSizeMetadataToSessionTxnRecord) {
    RAIIServerParameterControllerForTest flagReplicatedFastCount("featureFlagReplicatedFastCount",
                                                                 true);
    RAIIServerParameterControllerForTest flagDurability("featureFlagReplicatedFastCountDurability",
                                                        true);

    auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNumber(0);

    beginTxn(sessionId, txnNumber, [&](OperationContext* opCtx) {
        addTransactionInsertOps(opCtx, _nss1, {docA, docB});
        addTransactionInsertOps(opCtx, _nss2, {docC});
    });
    prepareTxn(sessionId, txnNumber);

    auto txnRecord = getTxnRecord(sessionId);
    ASSERT_TRUE(txnRecord.getSizeMetadata().has_value());
    assertSizeMetadata(*txnRecord.getSizeMetadata(),
                       {{_uuid1, docA.objsize() + docB.objsize(), 2}, {_uuid2, docC.objsize(), 1}});

    abortTxn(sessionId, txnNumber);
}

TEST_F(PreparedSizeMetadataTest, PrepareTransactionWritesSizeMetadataForSplitLinkedApplyOps) {
    RAIIServerParameterControllerForTest flagBase("featureFlagReplicatedFastCount", true);
    RAIIServerParameterControllerForTest flagDurability("featureFlagReplicatedFastCountDurability",
                                                        true);
    // Force a split after every 2 ops, so 3 total ops yields 2 linked applyOps entries.
    RAIIServerParameterControllerForTest maxOps(
        "maxNumberOfTransactionOperationsInSingleOplogEntry", 2);

    auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNumber(0);

    beginTxn(sessionId, txnNumber, [&](OperationContext* opCtx) {
        addTransactionInsertOps(opCtx, _nss1, {docA, docB});
        addTransactionInsertOps(opCtx, _nss2, {docC});
    });
    prepareTxn(sessionId, txnNumber);

    auto txnRecord = getTxnRecord(sessionId);
    ASSERT_TRUE(txnRecord.getSizeMetadata().has_value());
    assertSizeMetadata(*txnRecord.getSizeMetadata(),
                       {{_uuid1, docA.objsize() + docB.objsize(), 2}, {_uuid2, docC.objsize(), 1}});

    abortTxn(sessionId, txnNumber);
}

TEST_F(PreparedSizeMetadataTest, CommitTransactionWritesSizeMetadataToOplogEntry) {
    RAIIServerParameterControllerForTest flagReplicatedFastCount("featureFlagReplicatedFastCount",
                                                                 true);
    RAIIServerParameterControllerForTest flagDurability("featureFlagReplicatedFastCountDurability",
                                                        true);

    auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNumber(0);

    beginTxn(sessionId, txnNumber, [&](OperationContext* opCtx) {
        addTransactionInsertOps(opCtx, _nss1, {docA, docB});
        addTransactionInsertOps(opCtx, _nss2, {docC});
    });
    auto prepareTs = prepareTxn(sessionId, txnNumber);
    commitPreparedTxn(sessionId, txnNumber, prepareTs);

    repl::OplogInterfaceLocal oplogInterface(_opCtx);
    auto oplogIter = oplogInterface.makeIterator();
    auto commitOplogObj = unittest::assertGet(oplogIter->next()).first;
    auto commitEntry = unittest::assertGet(repl::OplogEntry::parse(commitOplogObj));

    ASSERT_TRUE(commitEntry.getSizeMetadata().has_value());
    const auto* multiOpMeta =
        std::get_if<std::vector<MultiOpSizeMetadata>>(&commitEntry.getSizeMetadata().value());
    ASSERT_NE(multiOpMeta, nullptr);
    assertSizeMetadata(*multiOpMeta,
                       {{_uuid1, docA.objsize() + docB.objsize(), 2}, {_uuid2, docC.objsize(), 1}});
}

TEST_F(PreparedSizeMetadataTest,
       RestorePreciseCheckpointPopulatesPreparedSizeMetadataOnParticipant) {
    RAIIServerParameterControllerForTest flagReplicatedFastCount("featureFlagReplicatedFastCount",
                                                                 true);
    RAIIServerParameterControllerForTest flagDurability("featureFlagReplicatedFastCountDurability",
                                                        true);

    auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNumber(0);

    beginTxn(sessionId, txnNumber, [&](OperationContext* opCtx) {
        addTransactionInsertOps(opCtx, _nss1, {docA, docB});
        addTransactionInsertOps(opCtx, _nss2, {docC});
    });
    auto recoveryRecord = prepareTxnAndCaptureRecoveryRecord(sessionId, txnNumber);

    // Capture the sizeMetadata from the participant state before simulating a restart.
    ASSERT_TRUE(recoveryRecord.getSizeMetadata().has_value());
    const auto sizeMetadataBeforeRestart = *recoveryRecord.getSizeMetadata();

    SessionCatalog::get(getServiceContext())->reset_forTest();

    // After restoring from the checkpoint record, the participant's sizeMetadata should be
    // identical to what was present before the restart.
    const auto restoredSizeMetadata =
        restoreFromCheckpointAndGetSizeMetadata(sessionId, txnNumber, std::move(recoveryRecord));

    ASSERT_TRUE(restoredSizeMetadata.has_value());
    assertSizeMetadataEqual(*restoredSizeMetadata, sizeMetadataBeforeRestart);
}


struct PreparedSizeMetadataFlagParams {
    // Whether `featureFlagReplicatedFastCount` is enabled.
    bool baseFlagEnabled;
    // Whether `featureFlagReplicatedFastCountDurability` is enabled.
    bool durabilityFlagEnabled;
};

class PreparedSizeMetadataFlagTest
    : public PreparedSizeMetadataTest,
      public testing::WithParamInterface<PreparedSizeMetadataFlagParams> {};

TEST_P(PreparedSizeMetadataFlagTest,
       OmitsSizeMetadataFromConfigTransactionsUnlessBothFlagsEnabled) {
    const auto [baseFlagEnabled, durabilityFlagEnabled] = GetParam();

    RAIIServerParameterControllerForTest flagBase("featureFlagReplicatedFastCount",
                                                  baseFlagEnabled);
    RAIIServerParameterControllerForTest flagDurability("featureFlagReplicatedFastCountDurability",
                                                        durabilityFlagEnabled);
    auto sessionId = makeLogicalSessionIdForTest();
    TxnNumber txnNumber(0);

    beginTxn(sessionId, txnNumber, [&](OperationContext* opCtx) {
        addTransactionInsertOps(opCtx, _nss1, {BSON("_id" << 0 << "data" << "x")});
    });
    prepareTxn(sessionId, txnNumber);

    EXPECT_FALSE(getTxnRecord(sessionId).getSizeMetadata().has_value());

    abortTxn(sessionId, txnNumber);
}

INSTANTIATE_TEST_SUITE_P(SizeMetadataFlagCombinations,
                         PreparedSizeMetadataFlagTest,
                         testing::Values(PreparedSizeMetadataFlagParams{false, false},
                                         PreparedSizeMetadataFlagParams{true, false},
                                         PreparedSizeMetadataFlagParams{false, true}));

}  // namespace
}  // namespace mongo::replicated_fast_count
