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

#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_uncommitted_changes.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
class ReplicatedFastCountTest : public CatalogTestFixture {
public:
    ReplicatedFastCountTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<replicated_fast_count_test_helpers::
                                   ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();

        auto* registry = dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        ASSERT(registry);
        registry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

        _fastCountManager = &ReplicatedFastCountManager::get(_opCtx->getServiceContext());
        // Allow for control over when we write to our internal collection for testing. We only
        // write to the internal collection when we explicitly call
        // ReplicatedFastCountManager::flushSync().
        _fastCountManager->disablePeriodicWrites_ForTest();

        setUpReplicatedFastCount(_opCtx);

        ASSERT_OK(createCollection(_opCtx, _nss1.dbName(), BSON("create" << _nss1.coll())));
        ASSERT_OK(createCollection(_opCtx, _nss2.dbName(), BSON("create" << _nss2.coll())));
        {
            AutoGetCollection coll1(_opCtx, _nss1, LockMode::MODE_IS);
            AutoGetCollection coll2(_opCtx, _nss2, LockMode::MODE_IS);
            _uuid1 = coll1->uuid();
            _uuid2 = coll2->uuid();
        }
    }

    void tearDown() override {
        shutdownFastCountManager();
        CatalogTestFixture::tearDown();
    }

    /**
     * Signals the fast count manager to shut down if it has not already done so.
     */
    void shutdownFastCountManager() {
        if (_fastCountManager != nullptr) {
            _fastCountManager = nullptr;
        }
    }

    OperationContext* _opCtx;
    ReplicatedFastCountManager* _fastCountManager;

    NamespaceString _nss1 =
        NamespaceString::createNamespaceString_forTest("replicated_fast_count_test", "coll1");
    NamespaceString _nss2 =
        NamespaceString::createNamespaceString_forTest("replicated_fast_count_test", "coll2");

    UUID _uuid1 = UUID::gen();
    UUID _uuid2 = UUID::gen();

    BSONObj sampleDocForInsert = BSON("_id" << 0 << "x" << 0);
    BSONObj sampleDocForUpdate = BSON("_id" << 0 << "x" << 0 << "y" << 0);
};

const std::function<BSONObj(int)> docGeneratorForInsert = [](int i) {
    return BSON("_id" << i << "x" << i);
};
const std::function<BSONObj(int)> docGeneratorForUpdate = [](int i) {
    return BSON("_id" << i << "x" << i << "y" << i * 2);
};

TEST_F(ReplicatedFastCountTest, UncommittedChangesResetOnCommit) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const int numDocs = 5;
    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numDocs,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);
}

TEST_F(ReplicatedFastCountTest, UncommittedChangesResetOnRollback) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const int numDocs = 5;
    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numDocs,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert,
                                                   /*abortWithoutCommit=*/true);
}

TEST_F(ReplicatedFastCountTest, UpdatesAreCorrectlyAccountedFor) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocs = 10;
    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numDocs,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    replicated_fast_count_test_helpers::updateDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   /*startIdx=*/0,
                                                   /*endIdx=*/4,
                                                   /*startingCount=*/numDocs,
                                                   /*startingSize=*/numDocs *
                                                       sampleDocForInsert.objsize(),
                                                   docGeneratorForUpdate,
                                                   sampleDocForInsert,
                                                   sampleDocForUpdate);
}

TEST_F(ReplicatedFastCountTest, DeletesAreCorrectlyAccountedFor) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocs = 10;
    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numDocs,
                                                   /*startingSize=*/0,
                                                   /*startingCount=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    replicated_fast_count_test_helpers::deleteDocsByIDRange(_opCtx,
                                                            _fastCountManager,
                                                            _nss1,
                                                            3,
                                                            8,
                                                            numDocs,
                                                            numDocs * sampleDocForInsert.objsize(),
                                                            sampleDocForInsert);
}

TEST_F(ReplicatedFastCountTest, DirtyMetadataWrittenToInternalCollection) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocsColl1 = 5;
    const int numDocsColl2 = 10;

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numDocsColl1,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss2,
                                                   numDocsColl2,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);


    // Verify that the committed changes have not been written to the internal fast count collection
    // yet.
    replicated_fast_count_test_helpers::checkFastCountMetadataInFastCountStoreCollection(
        _opCtx,
        _uuid1,
        /*expectPersisted=*/false,
        /*expectedCount=*/0,
        /*expectedSize=*/0);
    replicated_fast_count_test_helpers::checkFastCountMetadataInFastCountStoreCollection(
        _opCtx,
        _uuid2,
        /*expectPersisted=*/false,
        /*expectedCount=*/0,
        /*expectedSize=*/0);

    // Manually trigger an iteration to write dirty metadata to the internal collection.
    _fastCountManager->flushSync(_opCtx);

    replicated_fast_count_test_helpers::checkFastCountMetadataInFastCountStoreCollection(
        _opCtx,
        _uuid1,
        /*expectPersisted=*/true,
        numDocsColl1,
        numDocsColl1 * sampleDocForInsert.objsize());
    replicated_fast_count_test_helpers::checkFastCountMetadataInFastCountStoreCollection(
        _opCtx,
        _uuid2,
        /*expectPersisted=*/true,
        numDocsColl2,
        numDocsColl2 * sampleDocForInsert.objsize());
}

TEST_F(ReplicatedFastCountTest, DirtyMetadataWrittenAsSingleApplyOpsEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocsColl1 = 5;
    const int numDocsColl2 = 10;

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numDocsColl1,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss2,
                                                   numDocsColl2,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    _fastCountManager->flushSync(_opCtx);

    auto applyOpsEntries = replicated_fast_count_test_helpers::getApplyOpsForNss(
        _opCtx, replicated_fast_count_test_helpers::replicatedFastCountStoreNss);

    EXPECT_EQ(applyOpsEntries.size(), 1u);
    auto& applyOpsEntry = applyOpsEntries.front();

    replicated_fast_count_test_helpers::assertFastCountApplyOpsMatches(
        applyOpsEntry,
        {
            {_uuid1,
             replicated_fast_count_test_helpers::FastCountOpType::kInsert,
             numDocsColl1,
             numDocsColl1 * sampleDocForInsert.objsize()},
            {_uuid2,
             replicated_fast_count_test_helpers::FastCountOpType::kInsert,
             numDocsColl2,
             numDocsColl2 * sampleDocForInsert.objsize()},
        });
}

TEST_F(ReplicatedFastCountTest, UpdatesWrittenToApplyOpsCorrectly) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocsColl1 = 10;
    const int numDocsColl2 = 20;

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numDocsColl1,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss2,
                                                   numDocsColl2,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    _fastCountManager->flushSync(_opCtx);

    // Now, dirty metadata for the same collections, and check that an applyOps entry with the
    // correct update information is generated.

    const int startIdxColl1 = 1;
    const int endIdxColl1 = 7;
    const int totalUpdatesColl1 = endIdxColl1 - startIdxColl1 + 1;

    const int startIdxColl2 = 2;
    const int endIdxColl2 = 8;
    const int totalUpdatesColl2 = endIdxColl2 - startIdxColl2 + 1;

    replicated_fast_count_test_helpers::updateDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   startIdxColl1,
                                                   endIdxColl1,
                                                   /*startingCount=*/numDocsColl1,
                                                   /*startingSize=*/numDocsColl1 *
                                                       sampleDocForInsert.objsize(),
                                                   docGeneratorForUpdate,
                                                   sampleDocForInsert,
                                                   sampleDocForUpdate);

    replicated_fast_count_test_helpers::updateDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss2,
                                                   startIdxColl2,
                                                   endIdxColl2,
                                                   /*startingCount=*/numDocsColl2,
                                                   /*startingSize=*/numDocsColl2 *
                                                       sampleDocForInsert.objsize(),
                                                   docGeneratorForUpdate,
                                                   sampleDocForInsert,
                                                   sampleDocForUpdate);

    // Write to the internal collection again, this time recording the updates.
    _fastCountManager->flushSync(_opCtx);

    auto applyOpsEntry = replicated_fast_count_test_helpers::getLatestApplyOpsForNss(
        _opCtx, replicated_fast_count_test_helpers::replicatedFastCountStoreNss);

    const int64_t sizeDelta = sampleDocForUpdate.objsize() - sampleDocForInsert.objsize();
    const int64_t newExpectedSizeColl1 =
        (numDocsColl1 * sampleDocForInsert.objsize()) + (sizeDelta * totalUpdatesColl1);
    const int64_t newExpectedSizeColl2 =
        (numDocsColl2 * sampleDocForInsert.objsize()) + (sizeDelta * totalUpdatesColl2);

    replicated_fast_count_test_helpers::assertFastCountApplyOpsMatches(
        applyOpsEntry,
        {
            {_uuid1,
             replicated_fast_count_test_helpers::FastCountOpType::kUpdate,
             /*expectedCount=*/boost::none,
             /*expectedSize=*/newExpectedSizeColl1},
            {_uuid2,
             replicated_fast_count_test_helpers::FastCountOpType::kUpdate,
             /*expectedCount=*/boost::none,
             /*expectedSize=*/newExpectedSizeColl2},
        });
}

TEST_F(ReplicatedFastCountTest, MixedUpdatesAndInsertInApplyOps) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocsColl1 = 25;
    const int numDocsColl2 = 40;

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numDocsColl1,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    _fastCountManager->flushSync(_opCtx);

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss2,
                                                   numDocsColl2,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    const int startIdxColl = 2;
    const int endIdxColl = 8;
    const int totalUpdatesColl1 = endIdxColl - startIdxColl + 1;

    replicated_fast_count_test_helpers::updateDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   startIdxColl,
                                                   endIdxColl,
                                                   /*startingCount=*/numDocsColl1,
                                                   /*startingSize=*/numDocsColl1 *
                                                       sampleDocForInsert.objsize(),
                                                   docGeneratorForUpdate,
                                                   sampleDocForInsert,
                                                   sampleDocForUpdate);

    _fastCountManager->flushSync(_opCtx);

    auto applyOpsEntry = replicated_fast_count_test_helpers::getLatestApplyOpsForNss(
        _opCtx, replicated_fast_count_test_helpers::replicatedFastCountStoreNss);

    const int64_t sizeDelta = sampleDocForUpdate.objsize() - sampleDocForInsert.objsize();
    const int64_t newExpectedSizeColl1 =
        (numDocsColl1 * sampleDocForInsert.objsize()) + (sizeDelta * totalUpdatesColl1);

    replicated_fast_count_test_helpers::assertFastCountApplyOpsMatches(
        applyOpsEntry,
        {
            {_uuid1,
             replicated_fast_count_test_helpers::FastCountOpType::kUpdate,
             /*expectedCount=*/boost::none,
             /*expectedSize=*/newExpectedSizeColl1},
            {_uuid2,
             replicated_fast_count_test_helpers::FastCountOpType::kInsert,
             /*expectedCount=*/numDocsColl2,
             /*expectedSize=*/numDocsColl2 * sampleDocForInsert.objsize()},
        });
}

TEST_F(ReplicatedFastCountTest, StartupFailsIfFastCountCollectionNotPresent) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    {
        repl::UnreplicatedWritesBlock uwb(_opCtx);
        ASSERT_OK(
            storageInterface()->dropCollection(_opCtx,
                                               NamespaceString::makeGlobalConfigCollection(
                                                   NamespaceString::kReplicatedFastCountStore)));
    }

    ASSERT_THROWS_CODE(_fastCountManager->startup(_opCtx), DBException, 11718600);
}

TEST_F(ReplicatedFastCountTest, InitializePopulatesMetadataFromExistingInternalCollection) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    // Pre-populate the internal replicated fast count collection with two entries.
    UUID uuid1 = UUID::gen();
    UUID uuid2 = UUID::gen();

    const int64_t expectedCount1 = 5;
    const int64_t expectedSize1 = 100;

    const int64_t expectedCount2 = 10;
    const int64_t expectedSize2 = 250;

    {
        AutoGetCollection fastCountColl(
            _opCtx,
            NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
            LockMode::MODE_IX);
        ASSERT(fastCountColl);

        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};

        ASSERT_OK(
            Helpers::insert(_opCtx,
                            *fastCountColl,
                            BSON("_id" << uuid1 << replicated_fast_count::kValidAsOfKey
                                       << Timestamp(1, 1) << replicated_fast_count::kMetadataKey
                                       << BSON(replicated_fast_count::kCountKey
                                               << expectedCount1 << replicated_fast_count::kSizeKey
                                               << expectedSize1))));

        ASSERT_OK(
            Helpers::insert(_opCtx,
                            *fastCountColl,
                            BSON("_id" << uuid2 << replicated_fast_count::kValidAsOfKey
                                       << Timestamp(1, 1) << replicated_fast_count::kMetadataKey
                                       << BSON(replicated_fast_count::kCountKey
                                               << expectedCount2 << replicated_fast_count::kSizeKey
                                               << expectedSize2))));

        wuow.commit();
    }

    replicated_fast_count_test_helpers::checkFastCountMetadataInFastCountStoreCollection(
        _opCtx, uuid1, /*expectPersisted=*/true, expectedCount1, expectedSize1);
    replicated_fast_count_test_helpers::checkFastCountMetadataInFastCountStoreCollection(
        _opCtx, uuid2, /*expectPersisted=*/true, expectedCount2, expectedSize2);

    replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
        uuid1, _fastCountManager, /*expectedCount=*/0, /*expectedSize=*/0);
    replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
        uuid2, _fastCountManager, /*expectedCount=*/0, /*expectedSize=*/0);

    _fastCountManager->initializeMetadata(_opCtx);

    // The in-memory _metadata map should reflect the persisted values.
    replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
        uuid1, _fastCountManager, expectedCount1, expectedSize1);
    replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
        uuid2, _fastCountManager, expectedCount2, expectedSize2);
}

TEST_F(ReplicatedFastCountTest, DirtyWriteNotLostIfWrittenAfterMetadataSnapshot) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int64_t numInitialDocs = 5;
    const int64_t initialSize = numInitialDocs * sampleDocForInsert.objsize();
    const int64_t numExtraDocs = 5;
    const int64_t numTotalDocs = numInitialDocs + numExtraDocs;
    const int64_t totalSize = numTotalDocs * sampleDocForInsert.objsize();

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numInitialDocs,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
        _uuid1, _fastCountManager, numInitialDocs, initialSize);

    stdx::thread iterThread;

    {
        FailPointEnableBlock fp("hangAfterReplicatedFastCountSnapshot");
        auto initialTimesEntered = fp.initialTimesEntered();

        iterThread = stdx::thread([this] {
            auto clientForThread = getService()->makeClient("ReplicatedFastCountBackground");
            auto opCtxHolder = clientForThread->makeOperationContext();
            auto* opCtxForThread = opCtxHolder.get();
            // Hang after we make a copy of the _metadata map which should include our initial
            // inserts to the collection, but before we actually write to disk and change our dirty
            // flag for the collection we wrote to.
            _fastCountManager->flushSync(opCtxForThread);
        });

        fp->waitForTimesEntered(initialTimesEntered + 1);

        // Insert more documents into the same collection, creating new dirty metadata for that
        // collection.
        replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                       _fastCountManager,
                                                       _nss1,
                                                       numExtraDocs,
                                                       /*startingCount=*/numInitialDocs,
                                                       /*startingSize=*/numInitialDocs *
                                                           sampleDocForInsert.objsize(),
                                                       docGeneratorForInsert,
                                                       sampleDocForInsert);

        replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
            _uuid1, _fastCountManager, numTotalDocs, totalSize);

        // Disable failpoint by letting it go out of scope.
    }

    iterThread.join();

    // If the dirty metadata wasn't incorrectly cleared, this flush should persist our second batch
    // of inserts.
    _fastCountManager->flushSync(_opCtx);

    // Verify that all of our writes were persisted to disk.
    replicated_fast_count_test_helpers::checkFastCountMetadataInFastCountStoreCollection(
        _opCtx,
        _uuid1,
        /*expectPersisted=*/true,
        numTotalDocs,
        totalSize);
}

// TODO SERVER-118457: Parameterize test and test variety of operations with different sizes and
// counts. Test for drop, create.

/**
 * Helpers to build applyOps-style CRUD operations.
 */
BSONObj makeApplyOpsInsertOp(const NamespaceString& nss, const UUID& uuid, const BSONObj& doc) {
    return BSON("op" << "i"
                     << "ns" << nss.ns_forTest() << "ui" << uuid << "o" << doc);
}

BSONObj makeApplyOpsUpdateOp(const NamespaceString& nss,
                             const UUID& uuid,
                             int id,
                             const BSONObj& updatedDoc) {
    // Match on _id, and apply a $set to the full updated document.
    return BSON("op" << "u"
                     << "ns" << nss.ns_forTest() << "ui" << uuid << "o2" << BSON("_id" << id) << "o"
                     << BSON("$set" << updatedDoc));
}

BSONObj makeApplyOpsDeleteOp(const NamespaceString& nss, const UUID& uuid, int id) {
    return BSON("op" << "d"
                     << "ns" << nss.ns_forTest() << "ui" << uuid << "o" << BSON("_id" << id));
}

TEST_F(ReplicatedFastCountTest, CheckApplyOpsStructureAcceptsValidApplyOps) {
    auto uuid = UUID::gen();
    NamespaceString innerNss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto insertOp = repl::MutableOplogEntry::makeInsertOperation(
        innerNss, uuid, docGeneratorForInsert(0), BSON("_id" << 0));
    OperationSessionInfo sessionInfo;
    repl::OpTime opTime(Timestamp(1, 1), 1);
    auto applyOpsEntry = repl::makeApplyOpsOplogEntry(opTime,
                                                      {insertOp},
                                                      sessionInfo,
                                                      /*wallClockTime=*/Date_t::now(),
                                                      /*stmtIds=*/{},
                                                      /*prevOpTime=*/repl::OpTime());
    EXPECT_TRUE(replicated_fast_count_test_helpers::isApplyOpsEntryStructureValid(applyOpsEntry));
}

TEST_F(ReplicatedFastCountTest, CheckApplyOpsStructureRejectsNonCommandApplyOpsEntry) {
    auto uuid = UUID::gen();
    NamespaceString innerNss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto insertOp = repl::MutableOplogEntry::makeInsertOperation(
        innerNss, uuid, docGeneratorForInsert(0), BSON("_id" << 0));

    OperationSessionInfo sessionInfo;
    repl::OpTime opTime(Timestamp(1, 1), 1);

    auto validApplyOpsEntry = repl::makeApplyOpsOplogEntry(opTime,
                                                           {insertOp},
                                                           sessionInfo,
                                                           /*wallClockTime=*/Date_t::now(),
                                                           /*stmtIds=*/{},
                                                           /*prevOpTime=*/repl::OpTime());

    BSONObj validApplyOpsBSON = validApplyOpsEntry.getEntry().toBSON();
    BSONObjBuilder bob;

    // Mutate the applyOps entry to make the top-level entry something other than "c".
    for (auto&& elem : validApplyOpsBSON) {
        if (elem.fieldNameStringData() == repl::OplogEntry::kOpTypeFieldName) {
            bob.append(repl::OplogEntry::kOpTypeFieldName, "i");
        } else {
            bob.append(elem);
        }
    }

    auto invalidApplyOpsBSON = bob.obj();

    auto swBad = repl::OplogEntry::parse(invalidApplyOpsBSON);
    ASSERT_OK(swBad.getStatus());
    auto badApplyOpsEntry = swBad.getValue();

    EXPECT_FALSE(
        replicated_fast_count_test_helpers::isApplyOpsEntryStructureValid(badApplyOpsEntry));
}

TEST_F(ReplicatedFastCountTest, ApplyOpsInsertsAreCorrectlyAccountedFor) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocsColl1 = 3;
    const int numDocsColl2 = 4;

    // Build a single applyOps with inserts into both collections.
    BSONArrayBuilder opsBuilder;
    for (int i = 0; i < numDocsColl1; ++i) {
        BSONObj doc = docGeneratorForInsert(i);
        opsBuilder.append(makeApplyOpsInsertOp(_nss1, _uuid1, doc));
    }
    for (int i = 0; i < numDocsColl2; ++i) {
        BSONObj doc = docGeneratorForInsert(i);
        opsBuilder.append(makeApplyOpsInsertOp(_nss2, _uuid2, doc));
    }

    BSONObj cmd = BSON("applyOps" << opsBuilder.arr());

    BSONObjBuilder resultBob;
    // Use kApplyOpsCmd mode to simulate a user-issued applyOps on the primary.
    ASSERT_OK(repl::applyOps(
        _opCtx, _nss1.dbName(), cmd, repl::OplogApplication::Mode::kApplyOpsCmd, &resultBob));

    const int64_t expectedSizeColl1 = numDocsColl1 * sampleDocForInsert.objsize();
    const int64_t expectedSizeColl2 = numDocsColl2 * sampleDocForInsert.objsize();

    replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
        _uuid1, _fastCountManager, numDocsColl1, expectedSizeColl1);
    replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
        _uuid2, _fastCountManager, numDocsColl2, expectedSizeColl2);

    replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(_opCtx, _uuid1, 0, 0);
    replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(_opCtx, _uuid2, 0, 0);
}

TEST_F(ReplicatedFastCountTest, ApplyOpsUpdatesAreCorrectlyAccountedFor) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocs = 5;

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   /*numDocs=*/numDocs,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    const int startIdx = 1;
    const int endIdx = 3;
    const int totalUpdates = endIdx - startIdx + 1;

    BSONArrayBuilder opsBuilder;
    for (int i = startIdx; i <= endIdx; ++i) {
        BSONObj updated = docGeneratorForUpdate(i);
        opsBuilder.append(makeApplyOpsUpdateOp(_nss1, _uuid1, i, updated));
    }

    BSONObj cmd = BSON("applyOps" << opsBuilder.arr());
    BSONObjBuilder resultBob;

    ASSERT_OK(repl::applyOps(
        _opCtx, _nss1.dbName(), cmd, repl::OplogApplication::Mode::kApplyOpsCmd, &resultBob));

    const int64_t sizeDelta = sampleDocForUpdate.objsize() - sampleDocForInsert.objsize();
    const int64_t startingSize = numDocs * sampleDocForInsert.objsize();
    const int64_t expectedNewSize = startingSize + (totalUpdates * sizeDelta);


    replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
        _uuid1, _fastCountManager, numDocs, expectedNewSize);
    replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(_opCtx, _uuid1, 0, 0);
}

TEST_F(ReplicatedFastCountTest, ApplyOpsDeletesAreCorrectlyAccountedFor) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocs = 10;

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   /*numDocs=*/numDocs,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    const int startIdx = 2;
    const int endIdx = 7;
    const int numDeletes = endIdx - startIdx + 1;

    BSONArrayBuilder opsBuilder;
    for (int i = startIdx; i <= endIdx; ++i) {
        opsBuilder.append(makeApplyOpsDeleteOp(_nss1, _uuid1, i));
    }

    BSONObj cmd = BSON("applyOps" << opsBuilder.arr());
    BSONObjBuilder resultBob;

    ASSERT_OK(repl::applyOps(
        _opCtx, _nss1.dbName(), cmd, repl::OplogApplication::Mode::kApplyOpsCmd, &resultBob));

    const int64_t startingSize = numDocs * sampleDocForInsert.objsize();
    const int64_t deletedSize = numDeletes * sampleDocForInsert.objsize();
    const int64_t expectedCount = numDocs - numDeletes;
    const int64_t expectedSize = startingSize - deletedSize;

    replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
        _uuid1, _fastCountManager, expectedCount, expectedSize);
    replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(_opCtx, _uuid1, 0, 0);
}

TEST_F(ReplicatedFastCountTest, CappedDeletesUpdateFastCountWhenHittingCapCount) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    // Make this an unreplicated block so that the capped insert and delete combo doesn't violate
    // the multi‑timestamp constraint.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    const int cappedCollMaxCount = 5;

    NamespaceString nssCapped = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_test", "cappedWithMaxCount");

    auto uuidCapped = UUID::gen();

    ASSERT_OK(
        createCollection(_opCtx,
                         nssCapped.dbName(),
                         BSON("create" << nssCapped.coll() << "capped" << true << "size"
                                       << cappedCollMaxCount * sampleDocForInsert.objsize() *
                                  5  // Make the size larger so that count is the limiting bound
                                       << "max" << cappedCollMaxCount)));

    // Insert up until the max capped doc count.
    AutoGetCollection cappedColl(_opCtx, nssCapped, LockMode::MODE_IX);

    uuidCapped = cappedColl->uuid();

    for (int i = 0; i < cappedCollMaxCount; ++i) {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(Helpers::insert(_opCtx, *cappedColl, docGeneratorForInsert(i)));
        replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(
            _opCtx, uuidCapped, 1, sampleDocForInsert.objsize());
        wuow.commit();
        const auto expectedCommittedCount = i + 1;
        replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
            uuidCapped,
            _fastCountManager,
            expectedCommittedCount,
            expectedCommittedCount * sampleDocForInsert.objsize());
    }

    // Insert more docs. Our committed count and size should still be at the cap.
    for (int i = cappedCollMaxCount + 1; i < 3 * cappedCollMaxCount; ++i) {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(Helpers::insert(_opCtx, *cappedColl, docGeneratorForInsert(i)));
        // Insert + delete of same size cancel each other out.
        replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(
            _opCtx, uuidCapped, 0, 0);
        wuow.commit();
        replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
            uuidCapped,
            _fastCountManager,
            cappedCollMaxCount,
            cappedCollMaxCount * sampleDocForInsert.objsize());
    }
}

TEST_F(ReplicatedFastCountTest, CappedDeletesUpdateFastCountWhenHittingCapSize) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    // Make this an unreplicated block so that the capped insert and delete combo doesn't violate
    // the multi‑timestamp constraint.
    repl::UnreplicatedWritesBlock uwb(_opCtx);

    const int cappedCollMaxCount = 5;

    NamespaceString nssCapped = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_test", "cappedWithMaxSize");

    auto uuidCapped = UUID::gen();

    ASSERT_OK(
        createCollection(_opCtx,
                         nssCapped.dbName(),
                         BSON("create" << nssCapped.coll() << "capped" << true << "size"
                                       << cappedCollMaxCount * sampleDocForInsert.objsize())));

    // Insert up until the max capped doc size.
    AutoGetCollection cappedColl(_opCtx, nssCapped, LockMode::MODE_IX);

    uuidCapped = cappedColl->uuid();

    for (int i = 0; i < cappedCollMaxCount; ++i) {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(Helpers::insert(_opCtx, *cappedColl, docGeneratorForInsert(i)));
        replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(
            _opCtx, uuidCapped, 1, sampleDocForInsert.objsize());
        wuow.commit();
        const auto expectedCommittedCount = i + 1;
        replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
            uuidCapped,
            _fastCountManager,
            expectedCommittedCount,
            expectedCommittedCount * sampleDocForInsert.objsize());
    }

    // Insert more docs. Our committed count and size should still be at the cap.
    for (int i = cappedCollMaxCount + 1; i < 3 * cappedCollMaxCount; ++i) {
        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(Helpers::insert(_opCtx, *cappedColl, docGeneratorForInsert(i)));
        // Insert + delete of same size cancel each other out.
        replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(
            _opCtx, uuidCapped, 0, 0);
        wuow.commit();
        replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
            uuidCapped,
            _fastCountManager,
            cappedCollMaxCount,
            cappedCollMaxCount * sampleDocForInsert.objsize());
    }
}

TEST_F(ReplicatedFastCountTest, ReplicatedFastCountDoesNotTrackLocalCollections) {
    const NamespaceString internalNss =
        NamespaceString::createNamespaceString_forTest("local.coll");
    ASSERT_OK(createCollection(_opCtx, internalNss.dbName(), BSON("create" << internalNss.coll())));

    AutoGetCollection internalColl(_opCtx, internalNss, LockMode::MODE_IX);
    const UUID internalUuid = internalColl->uuid();
    const long long docsToInsertCount = 10;

    long long expectedSize = 0;
    WriteUnitOfWork wuow(_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
    for (size_t i = 0; i < docsToInsertCount; ++i) {
        const BSONObj document = docGeneratorForInsert(i);
        ASSERT_OK(Helpers::insert(_opCtx, *internalColl, document));
        expectedSize += document.objsize();
    }

    replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
        internalUuid, _fastCountManager, 0, 0);
    replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(
        _opCtx, internalUuid, 0, 0);

    wuow.commit();

    // Replicated fast count collection has no record of the writes to `internalColl`.
    replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
        internalUuid, _fastCountManager, 0, 0);
    replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(
        _opCtx, internalUuid, 0, 0);

    // Size and count data for `internalColl` are still tracked through the record store.
    EXPECT_EQ(internalColl->numRecords(_opCtx), docsToInsertCount);
    EXPECT_EQ(internalColl->dataSize(_opCtx), expectedSize);
}

TEST_F(ReplicatedFastCountTest, ReplicatedFastCountTracksNonLocalInternalCollections) {
    for (const auto& internalDbName : {"config", "admin"}) {
        const NamespaceString internalNss =
            NamespaceString::createNamespaceString_forTest(internalDbName, "coll");
        ASSERT_OK(
            createCollection(_opCtx, internalNss.dbName(), BSON("create" << internalNss.coll())));

        AutoGetCollection internalColl(_opCtx, internalNss, LockMode::MODE_IX);
        const UUID internalUuid = internalColl->uuid();
        const long long docsToInsertCount = 10;

        long long expectedSize = 0;
        WriteUnitOfWork wuow(_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        for (size_t i = 0; i < docsToInsertCount; ++i) {
            const BSONObj document = docGeneratorForInsert(i);
            ASSERT_OK(Helpers::insert(_opCtx, *internalColl, document));
            expectedSize += document.objsize();
        }

        replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
            internalUuid, _fastCountManager, 0, 0);
        replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(
            _opCtx, internalUuid, docsToInsertCount, expectedSize);

        wuow.commit();

        // Replicated fast count collection has record of the writes to `internalColl`.
        replicated_fast_count_test_helpers::checkCommittedFastCountChanges(
            internalUuid, _fastCountManager, docsToInsertCount, expectedSize);
        replicated_fast_count_test_helpers::checkUncommittedFastCountChanges(
            _opCtx, internalUuid, 0, 0);
    }
}

TEST_F(ReplicatedFastCountTest, FlushWritesToTimestampCollection) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocsColl = 5;

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numDocsColl,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    // There should be no records in the timestamps collection before we flush.
    replicated_fast_count_test_helpers::checkFastCountMetadataInTimestampsCollection(
        _opCtx, /*stripe=*/0, /*expectPersisted=*/false, Timestamp());

    Timestamp firstAllDurableTs =
        _opCtx->getServiceContext()->getStorageEngine()->getAllDurableTimestamp();

    _fastCountManager->flushSync(_opCtx);

    replicated_fast_count_test_helpers::checkFastCountMetadataInTimestampsCollection(
        _opCtx, /*stripe=*/0, /*expectPersisted=*/true, firstAllDurableTs);

    // Insert more docs and flush again, and check that the stored timestamp advances.
    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numDocsColl,
                                                   /*startingCount=*/numDocsColl,
                                                   /*startingSize=*/numDocsColl *
                                                       sampleDocForInsert.objsize(),
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);

    Timestamp secondAllDurableTs =
        _opCtx->getServiceContext()->getStorageEngine()->getAllDurableTimestamp();

    _fastCountManager->flushSync(_opCtx);

    replicated_fast_count_test_helpers::checkFastCountMetadataInTimestampsCollection(
        _opCtx, /*stripe=*/0, /*expectPersisted=*/true, secondAllDurableTs);
}

TEST_F(ReplicatedFastCountTest, WritesToTimestampCollectionReflectedInApplyOps) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocsColl = 5;

    replicated_fast_count_test_helpers::insertDocs(_opCtx,
                                                   _fastCountManager,
                                                   _nss1,
                                                   numDocsColl,
                                                   /*startingCount=*/0,
                                                   /*startingSize=*/0,
                                                   docGeneratorForInsert,
                                                   sampleDocForInsert);
    const Timestamp allDurableTs =
        _opCtx->getServiceContext()->getStorageEngine()->getAllDurableTimestamp();

    _fastCountManager->flushSync(_opCtx);

    replicated_fast_count_test_helpers::ExpectedFastCountTimestampsOp expectedOp{0, allDurableTs};

    auto applyOpsEntry = replicated_fast_count_test_helpers::getLatestApplyOpsForNss(
        _opCtx, replicated_fast_count_test_helpers::replicatedFastCountStoreTimestampsNss);

    replicated_fast_count_test_helpers::assertFastCountTimestampsApplyOpsMatches(applyOpsEntry,
                                                                                 expectedOp);
};

/**
 * Tests that operations with replicated size and count are logged with 'sizeMetadata' (information
 * stored in the top-level 'm' field) in their oplog entries.
 */
class SizeMetadataLoggingTest : public ReplicatedFastCountTest {};

TEST_F(SizeMetadataLoggingTest, BasicInsertOplogEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    AutoGetCollection coll(_opCtx, _nss1, LockMode::MODE_IX);

    const auto docToInsert = docGeneratorForInsert(0);
    const auto expectedSizeDelta = docToInsert.objsize();

    {
        WriteUnitOfWork wuow{_opCtx};
        ASSERT_OK(Helpers::insert(_opCtx, *coll, docToInsert));
        wuow.commit();
    }

    const auto insertOplogEntry = replicated_fast_count_test_helpers::getMostRecentOplogEntry(
        _opCtx, _nss1, repl::OpTypeEnum::kInsert);
    ASSERT_TRUE(insertOplogEntry.has_value());

    // Confirm the generated oplog entry includes replicated size count information.
    replicated_fast_count_test_helpers::assertReplicatedSizeCountMeta(*insertOplogEntry,
                                                                      expectedSizeDelta);
}

TEST_F(SizeMetadataLoggingTest, BasicUpdateOplogEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    auto coll = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss1, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    std::string smallNote("hi");
    const auto docInitial = BSON("_id" << 0 << "note" << smallNote);
    {
        WriteUnitOfWork wuow{_opCtx};
        ASSERT_OK(Helpers::insert(_opCtx, coll.getCollectionPtr(), docInitial));
        wuow.commit();
    }

    std::string largerNote("a slightly larger note to increase the size when we update");
    {
        WriteUnitOfWork wuow{_opCtx};
        Helpers::update(_opCtx, coll, BSON("_id" << 0), BSON("$set" << BSON("note" << largerNote)));
        wuow.commit();
    }
    const auto docAfterUpdate = Helpers::findOneForTesting(_opCtx, coll, BSON("_id" << 0));
    const auto expectedSizeDelta = docAfterUpdate.objsize() - docInitial.objsize();

    const auto updateOplogEntry = replicated_fast_count_test_helpers::getMostRecentOplogEntry(
        _opCtx, _nss1, repl::OpTypeEnum::kUpdate);
    ASSERT_TRUE(updateOplogEntry.has_value());

    // Confirm the generated oplog entry includes replicated size count information.
    replicated_fast_count_test_helpers::assertReplicatedSizeCountMeta(*updateOplogEntry,
                                                                      expectedSizeDelta);
}

TEST_F(SizeMetadataLoggingTest, BasicUpdateOplogEntryWithNegativeDelta) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    auto coll = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss1, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    std::string largeNote(
        "a slightly large note to force the doc to start out as larger before the update");
    const auto docInitial = BSON("_id" << 0 << "note" << largeNote);
    {
        WriteUnitOfWork wuow{_opCtx};
        ASSERT_OK(Helpers::insert(_opCtx, coll.getCollectionPtr(), docInitial));
        wuow.commit();
    }

    std::string smallerNote("hi");
    {
        WriteUnitOfWork wuow{_opCtx};
        Helpers::update(
            _opCtx, coll, BSON("_id" << 0), BSON("$set" << BSON("note" << smallerNote)));
        wuow.commit();
    }
    const auto docAfterUpdate = Helpers::findOneForTesting(_opCtx, coll, BSON("_id" << 0));
    const auto expectedSizeDelta = docAfterUpdate.objsize() - docInitial.objsize();

    const auto updateOplogEntry = replicated_fast_count_test_helpers::getMostRecentOplogEntry(
        _opCtx, _nss1, repl::OpTypeEnum::kUpdate);
    ASSERT_TRUE(updateOplogEntry.has_value());
    replicated_fast_count_test_helpers::assertReplicatedSizeCountMeta(*updateOplogEntry,
                                                                      expectedSizeDelta);
}

TEST_F(SizeMetadataLoggingTest, BasicDeleteOplogEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    auto coll = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss1, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    const auto doc = BSON("_id" << 0 << "x" << 0);
    {
        WriteUnitOfWork wuow{_opCtx};
        ASSERT_OK(Helpers::insert(_opCtx, coll.getCollectionPtr(), doc));
        wuow.commit();
    }

    const auto rid = Helpers::findOne(_opCtx, coll, BSON("_id" << 0));
    ASSERT_FALSE(rid.isNull());

    {
        WriteUnitOfWork wuow{_opCtx};
        Helpers::deleteByRid(_opCtx, coll, rid);
        wuow.commit();
    }
    const auto expectedSizeDelta = doc.objsize();
    const auto deleteOplogEntry = replicated_fast_count_test_helpers::getMostRecentOplogEntry(
        _opCtx, _nss1, repl::OpTypeEnum::kDelete);
    ASSERT_TRUE(deleteOplogEntry.has_value());
    replicated_fast_count_test_helpers::assertReplicatedSizeCountMeta(*deleteOplogEntry,
                                                                      -expectedSizeDelta);
}

TEST_F(SizeMetadataLoggingTest, BasicGroupCommit) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    auto coll = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss1, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    const auto doc1 = BSON("_id" << 0 << "x" << 0);
    const auto doc2 = BSON("_id" << 1 << "abcdefg" << 1);
    {
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        ASSERT_OK(Helpers::insert(_opCtx, coll.getCollectionPtr(), doc1));
        ASSERT_OK(Helpers::insert(_opCtx, coll.getCollectionPtr(), doc2));
        wuow.commit();
    }
    std::vector<replicated_fast_count_test_helpers::OpValidationSpec> expectedOps{
        {.uuid = _uuid1, .opType = repl::OpTypeEnum::kInsert, .expectedSizeDelta = doc1.objsize()},
        {.uuid = _uuid1, .opType = repl::OpTypeEnum::kInsert, .expectedSizeDelta = doc2.objsize()}};

    const auto applyOpsOplogEntry =
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1);
    std::vector<repl::OplogEntry> innerEntries;
    repl::ApplyOps::extractOperationsTo(
        applyOpsOplogEntry, applyOpsOplogEntry.getEntry().toBSON(), &innerEntries);
    replicated_fast_count_test_helpers::assertOpsMatchSpecs(innerEntries, expectedOps);
}

// TODO SERVER-122523: Move 'ReplicatedFastCountDeltaUtilsTest' and
// 'AggregateSizeCountFromOplogTest' to a designated delta-utils test file.
class ReplicatedFastCountDeltaUtilsTest : public ReplicatedFastCountTest {
protected:
    CollectionAcquisition acquireCollForWrite(const NamespaceString& nss) {
        return acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IX);
    }
};

repl::OplogEntrySizeMetadata makeOperationSizeMetadata(int32_t replicatedSizeDelta) {
    repl::OplogEntrySizeMetadata m;
    m.setSz(replicatedSizeDelta);
    return m;
}

repl::OplogEntry makeOplogEntryWithSizeMeta(const NamespaceString& nss,
                                            repl::OpTypeEnum opType,
                                            int32_t sizeDelta) {

    auto sizeMetadata = makeOperationSizeMetadata(sizeDelta);
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = opType,
        .nss = nss,
        .uuid = UUID::gen(),
        .oField = BSONObj(),
        .sizeMetadata = sizeMetadata,
        .wallClockTime = Date_t::now(),
    }};
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaForInsert) {
    const int32_t sizeDelta = 400;
    const auto insertOp = makeOplogEntryWithSizeMeta(_nss1, repl::OpTypeEnum::kInsert, sizeDelta);
    const auto extractedSizeCount = replicated_fast_count::extractSizeCountDeltaForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    // Insert means count increases by 1.
    ASSERT_EQ(1, extractedSizeCount->count);
    ASSERT_EQ(sizeDelta, extractedSizeCount->size);
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaForUpdate) {
    const int32_t sizeDelta = 400;
    const auto insertOp = makeOplogEntryWithSizeMeta(_nss1, repl::OpTypeEnum::kUpdate, sizeDelta);
    const auto extractedSizeCount = replicated_fast_count::extractSizeCountDeltaForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    // Updates imply no new documents, count delta is 0.
    ASSERT_EQ(0, extractedSizeCount->count);
    ASSERT_EQ(sizeDelta, extractedSizeCount->size);
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaForDelete) {
    const int32_t sizeDelta = 400;
    const auto insertOp = makeOplogEntryWithSizeMeta(_nss1, repl::OpTypeEnum::kDelete, sizeDelta);
    const auto extractedSizeCount = replicated_fast_count::extractSizeCountDeltaForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    // Delete implies one less document.
    ASSERT_EQ(-1, extractedSizeCount->count);
    ASSERT_EQ(sizeDelta, extractedSizeCount->size);
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, NoSizeCountDeltaWhenAbsentFromOplogEntry) {
    // 'OpTypeEnum::kInsert' supports replicated fast count information, but none is extracted
    // because the 'm' field is absent from the oplog entry.
    repl::OplogEntry insertOpNoSizeMetadata{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kInsert,
        .nss = _nss1,
        .oField = BSONObj(),
        .wallClockTime = Date_t::now(),
    }}};
    const auto extractedSizeCount =
        replicated_fast_count::extractSizeCountDeltaForOp(insertOpNoSizeMetadata);
    ASSERT_FALSE(insertOpNoSizeMetadata.getSizeMetadata().has_value());
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, NoSizeCountDeltaWhenAbsentAndIncompatibleOpType) {
    // 'OpTypeEnum::kCommand' does not support top level 'sizeMetadata' field 'm', and in absence of
    // the 'sizeMetadata', nothing is returned when trying to extract size count deltas.
    repl::OplogEntry commandOpNoSizeMetadata{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = _nss1,
        .oField = BSONObj(),
        .wallClockTime = Date_t::now(),
    }}};
    const auto extractedSizeCount =
        replicated_fast_count::extractSizeCountDeltaForOp(commandOpNoSizeMetadata);
    ASSERT_FALSE(commandOpNoSizeMetadata.getSizeMetadata().has_value());
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaOnUnsupportedOpType) {
    const auto oplogEntry =
        makeOplogEntryWithSizeMeta(_nss1, repl::OpTypeEnum::kNoop, 400 /* sizeDelta */);

    // Size metadata is only supported for 'insert', 'delete', and 'update' operations. All other
    // operations are incompatible with a top-level 'm' field.
    // TODO SERVER-121175: Include truncates in accepted opTypes comment.
    ASSERT_THROWS_CODE(
        replicated_fast_count::extractSizeCountDeltaForOp(oplogEntry), DBException, 12115900);
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaOnNonEligibleNss) {
    const NamespaceString localNss =
        NamespaceString::createNamespaceString_forTest("local", "coll1");
    EXPECT_FALSE(isReplicatedFastCountEligible(localNss));

    const auto oplogEntry =
        makeOplogEntryWithSizeMeta(localNss, repl::OpTypeEnum::kNoop, 400 /* sizeDelta */);

    // Local namespaces are not eligible for replicated size count.
    ASSERT_THROWS_CODE(
        replicated_fast_count::extractSizeCountDeltaForOp(oplogEntry), DBException, 12115900);
}

TEST_F(ReplicatedFastCountDeltaUtilsTest,
       ExtractSizeCountDeltaOnNonEligibleNssWithoutSizeMetadata) {
    const NamespaceString localNss =
        NamespaceString::createNamespaceString_forTest("local", "coll1");
    EXPECT_FALSE(isReplicatedFastCountEligible(localNss));

    repl::OplogEntry insertOpLocalNs{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kInsert,
        .nss = localNss,
        .oField = BSONObj(),
        .wallClockTime = Date_t::now(),
    }}};

    // Local namespace without sizeMetadata shouldn't throw an error.
    ASSERT_FALSE(replicated_fast_count::extractSizeCountDeltaForOp(insertOpLocalNs).has_value());
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaForApplyOpsInsertsSingleUUID) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const std::vector<BSONObj> docs{
        BSON("_id" << 0),
        BSON("_id" << 1),
        BSON("_id" << 2),
    };

    // Confirm this starts with an empty collection.
    ASSERT_EQ(CollectionSizeCount{},
              replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1));
    {
        // Insert documents and confirm the aggregation.
        auto acq = acquireCollForWrite(_nss1);
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        ASSERT_OK(Helpers::insert(_opCtx, acq.getCollectionPtr(), docs));
        wuow.commit();
    }

    // Size and count were both 0 before the operation, so we expect the deltas to aggregate to the
    // totals.
    CollectionSizeCount totalCollSizeCount0 =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);

    // Validate extracted deltas for first round of applyOps.
    const auto deltas0 = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(1u, deltas0.size());
    ASSERT_TRUE(deltas0.contains(_uuid1));
    ASSERT_EQ(totalCollSizeCount0, deltas0.at(_uuid1));

    // Insert documents into a non-empty collection to demonstrate correct delta computation.
    const std::vector<BSONObj> docsNewInserts{
        BSON("_id" << 3),
        BSON("_id" << 4 << "x" << 7),
    };
    {
        auto acq = acquireCollForWrite(_nss1);
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        ASSERT_OK(Helpers::insert(_opCtx, acq.getCollectionPtr(), docsNewInserts));
        wuow.commit();
    }
    const auto totalCollSizeCount1 =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas1 = totalCollSizeCount1 - totalCollSizeCount0;

    // Validate extracted deltas for second round of applyOps.
    const auto deltas1 = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(1u, deltas1.size());
    ASSERT_TRUE(deltas1.contains(_uuid1));
    ASSERT_EQ(expectedDeltas1, deltas1.at(_uuid1));
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaForApplyOpsUpdatesSingleUUID) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const std::vector<BSONObj> docs{
        BSON("_id" << 0),
        BSON("_id" << 1),
        BSON("_id" << 2),
    };

    {
        // Pre-populate collection
        auto acq = acquireCollForWrite(_nss1);
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        ASSERT_OK(Helpers::insert(_opCtx, acq.getCollectionPtr(), docs));
        wuow.commit();
    }
    CollectionSizeCount originalSizeCount =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);

    {
        // Update 2 of the documents.
        auto collAcq = acquireCollForWrite(_nss1);
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        Helpers::update(
            _opCtx, collAcq, BSON("_id" << 0), BSON("$set" << BSON("greeting" << "Howdy")));
        Helpers::update(
            _opCtx, collAcq, BSON("_id" << 2), BSON("$set" << BSON("greeting" << "Hi")));
        wuow.commit();
    }

    CollectionSizeCount sizeCountAfterUpdates =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas = sizeCountAfterUpdates - originalSizeCount;

    const auto deltas = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(1u, deltas.size());
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_EQ(expectedDeltas, deltas.at(_uuid1));
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaForApplyOpsDeletesSingleUUID) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const std::vector<BSONObj> docs{
        BSON("_id" << 0),
        BSON("_id" << 1),
        BSON("_id" << 2),
    };

    {
        // Pre-populate collection
        auto acq = acquireCollForWrite(_nss1);
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        ASSERT_OK(Helpers::insert(_opCtx, acq.getCollectionPtr(), docs));
        wuow.commit();
    }
    CollectionSizeCount originalSizeCount =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);

    {
        // Delete 2 of the documents.
        auto collAcq = acquireCollForWrite(_nss1);
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        const std::vector<BSONObj> removeFilters{BSON("_id" << 0), BSON("_id" << 2)};
        for (const auto& docFilter : removeFilters) {
            const auto rid = Helpers::findOne(_opCtx, collAcq, docFilter);
            ASSERT_FALSE(rid.isNull());
            Helpers::deleteByRid(_opCtx, collAcq, rid);
        }
        wuow.commit();
    }

    CollectionSizeCount sizeCountAfterUpdates =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas = sizeCountAfterUpdates - originalSizeCount;

    const auto deltas = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(1u, deltas.size());
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_EQ(expectedDeltas, deltas.at(_uuid1));
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaForApplyOpsMultiOpsSingleUUID) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const BSONObj doc0 = BSON("_id" << 0 << "x" << "0");
    const BSONObj doc1 = BSON("_id" << 1 << "x" << "0");

    {
        auto collAcq = acquireCollForWrite(_nss1);
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        // Insert doc0 and doc1.
        ASSERT_OK(
            Helpers::insert(_opCtx, collAcq.getCollectionPtr(), std::vector<BSONObj>{doc0, doc1}));

        // Update doc0.
        Helpers::update(_opCtx, collAcq, BSON("_id" << 0), BSON("$set" << BSON("y" << 0)));

        // Delete doc1.
        const auto rid = Helpers::findOne(_opCtx, collAcq, BSON("_id" << 1));
        ASSERT_FALSE(rid.isNull());
        Helpers::deleteByRid(_opCtx, collAcq, rid);

        wuow.commit();
    }

    // Expected Result: Only an updated doc0 exists in the collection.
    const auto expectedDeltas =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    ASSERT_EQ(1, expectedDeltas.count);
    ASSERT_NE(expectedDeltas.size, doc0.objsize());

    // Deltas correctly account for inserts, update, and delete which impact each other.
    const auto deltas = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(1u, deltas.size());
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_EQ(expectedDeltas, deltas.at(_uuid1));
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaForApplyOpsMultiUUID) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const BSONObj doc1 = BSON("_id" << 0 << "x" << "0");
    const BSONObj doc2 = BSON("_id" << 1 << "x" << "0" << "y" << 1);

    // Both collections begin empty.
    ASSERT_EQ(CollectionSizeCount{},
              replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1));
    ASSERT_EQ(CollectionSizeCount{},
              replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss2));

    {
        // In a grouped applyOps, insert one document into each collection.
        auto collAcq = acquireCollForWrite(_nss1);
        auto collAcq2 = acquireCollForWrite(_nss2);
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        ASSERT_OK(Helpers::insert(_opCtx, collAcq.getCollectionPtr(), doc1));
        ASSERT_OK(Helpers::insert(_opCtx, collAcq2.getCollectionPtr(), doc2));

        wuow.commit();
    }

    // Expected deltas are the total count and size since the collections began empty.
    const auto expectedDeltas1 =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas2 =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss2);
    ASSERT_EQ(expectedDeltas1.count, 1);
    ASSERT_EQ(expectedDeltas1.size, doc1.objsize());
    ASSERT_EQ(expectedDeltas2.count, 1);
    ASSERT_EQ(expectedDeltas2.size, doc2.objsize());

    // Extract applyOps deltas and verify.
    auto deltas = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(deltas.size(), 2u);
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_TRUE(deltas.contains(_uuid2));
    ASSERT_EQ(deltas.at(_uuid1), expectedDeltas1);
    ASSERT_EQ(deltas.at(_uuid2), expectedDeltas2);
}

TEST_F(ReplicatedFastCountDeltaUtilsTest,
       ExtractSizeCountDeltaForApplyOpsDoesNotAcceptNonApplyOps) {
    repl::OplogEntry ungroupedInsertOplogEntry{
        repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(),
            .opType = repl::OpTypeEnum::kInsert,
            .nss = _nss1,
            .oField = BSONObj(),
            .wallClockTime = Date_t::now(),
        }}};

    // applyOps extraction enforces the input is an applyOps type.
    ASSERT_THROWS_CODE(replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
                           ungroupedInsertOplogEntry),
                       DBException,
                       12116000);
}

TEST_F(ReplicatedFastCountDeltaUtilsTest,
       ExtractSizeCountDeltaForApplyOpsRequiresUUIDSpecification) {
    // Replicated count and size is tracked per collection through the UUID. Tests that an applyOps
    // oplog entry with an inner op missing the collection's UUID fails to parse the replicated fast
    // count.
    const auto adminDbName = DatabaseName::createDatabaseName_forTest(boost::none, "admin");
    const NamespaceString adminCmdNss =
        NamespaceString::createNamespaceString_forTest("admin", "$cmd");

    const BSONObj docA = BSON("_id" << 0 << "x" << "0");
    BSONObj insertOpMissingUUID = BSON("op" << "i"
                                            << "ns" << _nss1.ns_forTest() << "o" << docA << "m"
                                            << BSON("sz" << docA.objsize()));
    BSONObj applyOpsCmd = BSON("applyOps" << BSON_ARRAY(insertOpMissingUUID));

    repl::OplogEntry applyOpsEntryMissingInnerUi{
        repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = applyOpsCmd,
            .wallClockTime = Date_t::now(),
        }}};

    // applyOps extraction requires a UUID for each inner op with size tracking.
    ASSERT_THROWS_CODE(replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
                           applyOpsEntryMissingInnerUi),
                       DBException,
                       12116001);
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaForNestedApplyOpsMultiUUID) {
    // Nested applyOps are allowed from user commands. Tests that extraction of replicated size and
    // count works across nested applyOps for multiple collections.
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const BSONObj docA = BSON("_id" << 0 << "x" << "0");
    const BSONObj docB = BSON("_id" << 1 << "x" << "0" << "y" << 1);

    // Admin command namespace for applyOps commands.
    const auto adminDbName = DatabaseName::createDatabaseName_forTest(boost::none, "admin");
    const NamespaceString adminCmdNss =
        NamespaceString::createNamespaceString_forTest("admin", "$cmd");

    // The resulting BSON structure is:
    //
    // {
    //   applyOps: [   // Top-level array: contains the first-level applyOps command
    //     {
    //       op: "c",
    //       ns: "admin.$cmd",
    //       o: {
    //         applyOps: [
    //           <insert docB into _nss1>,
    //           {
    //             op: "c",
    //             ns: "admin.$cmd",
    //             o: {
    //               applyOps: [
    //                 <insert docA into _nss1>,
    //                 <insert docA into _nss2>
    //               ]
    //             }
    //           }
    //         ]
    //       }
    //     }
    //   ]
    // }
    BSONObj innerMostInsertNs1 = BSON("op" << "i"
                                           << "ns" << _nss1.ns_forTest() << "ui" << _uuid1 << "o"
                                           << docA << "m" << BSON("sz" << docA.objsize()));
    BSONObj innerMostInsertNs2 = BSON("op" << "i"
                                           << "ns" << _nss2.ns_forTest() << "ui" << _uuid2 << "o"
                                           << docA << "m" << BSON("sz" << docA.objsize()));
    BSONObj nestedInnerApplyOpsCmdOp =
        BSON("op" << "c"
                  << "ns" << adminCmdNss.ns_forTest() << "o"
                  << BSON("applyOps" << BSON_ARRAY(innerMostInsertNs1 << innerMostInsertNs2)));
    BSONObj firstLevelInsert = BSON("op" << "i"
                                         << "ns" << _nss1.ns_forTest() << "ui" << _uuid1 << "o"
                                         << docA << "m" << BSON("sz" << docB.objsize()));
    BSONObj firstLevelApplyOpsCmdOp =
        BSON("op" << "c"
                  << "ns" << adminCmdNss.ns_forTest() << "o"
                  << BSON("applyOps" << BSON_ARRAY(firstLevelInsert << nestedInnerApplyOpsCmdOp)));
    BSONObj topLevelApplyOpsCmd = BSON("applyOps" << BSON_ARRAY(firstLevelApplyOpsCmdOp));

    repl::OplogEntry applyOpsEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = adminCmdNss,
        .oField = topLevelApplyOpsCmd,
        .wallClockTime = Date_t::now(),
    }}};

    const CollectionSizeCount expectedDeltasNss1{docA.objsize() + docB.objsize(), 2};
    const CollectionSizeCount expectedDeltasNss2{docA.objsize(), 1};

    const auto deltas =
        replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(applyOpsEntry);

    ASSERT_EQ(deltas.size(), 2u);
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_TRUE(deltas.contains(_uuid2));
    ASSERT_EQ(deltas.at(_uuid1), expectedDeltasNss1);
    ASSERT_EQ(deltas.at(_uuid2), expectedDeltasNss2);
}

class AggregateSizeCountFromOplogTest : public CatalogTestFixture {
protected:
    struct NsAndUUID {
        NamespaceString nss;
        UUID uuid;
    };

    repl::OplogEntry mockOplogEntry(const Timestamp ts,
                                    NsAndUUID userColl,
                                    repl::OpTypeEnum opType,
                                    int32_t sizeDelta) {
        return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts, 1),
            .opType = opType,
            .nss = userColl.nss,
            .uuid = userColl.uuid,
            .oField = BSONObj(),
            .sizeMetadata = makeOperationSizeMetadata(sizeDelta),
            .wallClockTime = Date_t::now(),
        }};
    }
    repl::OplogEntry mockOplogEntry(const Timestamp ts,
                                    NsAndUUID userColl,
                                    repl::OpTypeEnum opType) {
        return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts, 1),
            .opType = opType,
            .nss = userColl.nss,
            .uuid = userColl.uuid,
            .oField = BSONObj(),
            .wallClockTime = Date_t::now(),
        }};
    }

    /**
     * Test methods should default to testing aggregate size count with this method, as it checks
     * both methods of aggregation (acquiring a map of deltas across uuids and aggregating deltas
     * for a single uuid) yield equivalent results for the 'uuid'.
     */
    void assertExpectedAggregateDelta(const CollectionSizeCount& expectedAggDelta,
                                      const UUID& uuid,
                                      const Timestamp& seekAfterTS,
                                      SeekableRecordCursor& oplogCursor) {
        // Deltas across UUIDs.
        const auto deltas =
            replicated_fast_count::aggregateSizeCountDeltasInOplog(oplogCursor, seekAfterTS);
        ASSERT_TRUE(deltas.contains(uuid));
        EXPECT_EQ(deltas.at(uuid), expectedAggDelta);

        // Also correct when filtered explicitly by 'uuid'
        const auto filteredDeltas =
            replicated_fast_count::aggregateSizeCountDeltasInOplog(oplogCursor, seekAfterTS, uuid);
        ASSERT_TRUE(filteredDeltas.contains(uuid));
        ASSERT_EQ(expectedAggDelta, filteredDeltas.at(uuid));
    }

    /**
     * Bundles information about a user "collection" needed for CRUD oplog entries.
     */
    NsAndUUID _collA = {
        .nss = NamespaceString::createNamespaceString_forTest("agg_size_count_from_oplog", "collA"),
        .uuid = UUID::gen()};
    NsAndUUID _collB = {
        .nss = NamespaceString::createNamespaceString_forTest("agg_size_count_from_oplog", "collB"),
        .uuid = UUID::gen()};
};

/**
 * Allows explicit control over the contents of the "oplog" used to aggregate size and count. This
 * test cursor does not have visibility rules specific to the oplog, but should suffice for
 * targeted testing of aggregation logic.
 */
class OplogCursorMock : public SeekableRecordCursor {
public:
    OplogCursorMock(std::list<repl::OplogEntry> entries) {
        for (const auto& entry : entries) {
            _records.emplace_back(RecordId(entry.getTimestamp().asULL()),
                                  entry.getEntry().toBSON().getOwned());
        }
    }

    ~OplogCursorMock() override {}

    boost::optional<Record> next() override {
        if (_records.empty()) {
            return boost::none;
        }

        if (!_initialized) {
            _initialized = true;
            _it = _records.cbegin();
        } else {
            ++_it;
        }

        if (_it == _records.cend()) {
            _initialized = false;
            return boost::none;
        }

        return Record{_it->first, RecordData(_it->second.objdata(), _it->second.objsize())};
    }

    boost::optional<Record> seekExact(const RecordId& id) override {
        for (auto it = _records.cbegin(); it != _records.cend(); ++it) {
            if (it->first == id) {
                _initialized = true;
                _it = it;
                return Record{it->first, RecordData(it->second.objdata(), it->second.objsize())};
            }
        }
        _initialized = false;
        return boost::none;
    }

    void save() override {}
    bool restore(RecoveryUnit&, bool) override {
        return true;
    }
    void detachFromOperationContext() override {}
    void reattachToOperationContext(OperationContext*) override {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) override {}

    boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion) override {
        invariant(boundInclusion == BoundInclusion::kExclude);
        for (auto it = _records.cbegin(); it != _records.cend(); ++it) {
            if (it->first > start) {
                _initialized = true;
                _it = it;
                return Record{it->first, RecordData(it->second.objdata(), it->second.objsize())};
            }
        }
        _initialized = false;
        return {};
    }

private:
    bool _initialized = false;
    std::list<std::pair<RecordId, BSONObj>> _records;
    std::list<std::pair<RecordId, BSONObj>>::const_iterator _it;
};

TEST_F(AggregateSizeCountFromOplogTest, AggregateSingleColl) {
    const Timestamp ts1{1, 2};
    const Timestamp ts2{2, 2};
    const Timestamp ts3{3, 2};

    std::list<repl::OplogEntry> entries{
        mockOplogEntry(ts1, _collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/),
        mockOplogEntry(ts2, _collA, repl::OpTypeEnum::kUpdate, 100 /*sizeDelta=*/),
        mockOplogEntry(ts2, _collA, repl::OpTypeEnum::kDelete, -110 /*sizeDelta=*/),
    };
    OplogCursorMock oplogCursor(std::move(entries));
    const auto& uuidA = _collA.uuid;

    // (1) Aggregate size count deltas after Timestamp::min().
    // Since there were oplog entries with replicated size count, an entry exists, but its
    // aggregates should sum to 0 as the only document inserted was eventually deleted.
    assertExpectedAggregateDelta(
        CollectionSizeCount{.size = 0, .count = 0}, uuidA, Timestamp::min(), oplogCursor);

    // (2) Aggregate size count deltas after ts1 accounts for update and delete.
    assertExpectedAggregateDelta(
        CollectionSizeCount{.size = -10, .count = -1}, uuidA, ts1, oplogCursor);

    // (3) Timestamp at or past the last entry yields no deltas.
    // Check the result without a uuid filter.
    const auto deltasMap = replicated_fast_count::aggregateSizeCountDeltasInOplog(oplogCursor, ts3);
    EXPECT_EQ(deltasMap.size(), 0u);

    // Check the result with a uuid filter.
    EXPECT_FALSE(replicated_fast_count::aggregateSizeCountDeltasInOplog(oplogCursor, ts3, uuidA)
                     .contains(uuidA));
}

TEST_F(AggregateSizeCountFromOplogTest, AggregateMultipleCollections) {
    // Synthetic timestamps, ordered oldest -> newest.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    const Timestamp ts4{1, 4};
    const Timestamp ts5{1, 5};

    // Size deltas for respective ops on _collA and _collB.
    const int32_t insertA1 = 50;
    const int32_t insertA2 = 60;
    const int32_t insertB1 = 70;
    const int32_t delA1 = -50;
    const int32_t delB1 = -70;

    // Two inserts for _coll1, one insert for _collB, then one delete each.
    std::list<repl::OplogEntry> entries{
        mockOplogEntry(ts1, _collA, repl::OpTypeEnum::kInsert, insertA1),
        mockOplogEntry(ts2, _collA, repl::OpTypeEnum::kInsert, insertA2),
        mockOplogEntry(ts3, _collB, repl::OpTypeEnum::kInsert, insertB1),
        mockOplogEntry(ts4, _collA, repl::OpTypeEnum::kDelete, delA1),
        mockOplogEntry(ts5, _collB, repl::OpTypeEnum::kDelete, delB1),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    // Aggregating from Timestamp::min() aggregates all entries.
    {
        // 2 collections tracked.
        EXPECT_EQ(
            replicated_fast_count::aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min())
                .size(),
            2u);
        assertExpectedAggregateDelta(
            CollectionSizeCount{.size = (insertA1 + insertA2 + delA1), .count = 1},
            _collA.uuid,
            Timestamp::min(),
            oplogCursor);
        // Deltas sum to 0.
        assertExpectedAggregateDelta(
            CollectionSizeCount{}, _collB.uuid, Timestamp::min(), oplogCursor);
    }

    // Aggregating after ts3 (the last insert) only sees the two deletes.
    {
        EXPECT_EQ(replicated_fast_count::aggregateSizeCountDeltasInOplog(oplogCursor, ts3).size(),
                  2u);
        assertExpectedAggregateDelta(
            CollectionSizeCount{.size = delA1, .count = -1}, _collA.uuid, ts3, oplogCursor);
        assertExpectedAggregateDelta(
            CollectionSizeCount{.size = delB1, .count = -1}, _collB.uuid, ts3, oplogCursor);
    }

    // Aggregating with ts5 doesn't yield deltas because the aggregation excludes the timestamp
    // provided.
    {
        const auto deltas =
            replicated_fast_count::aggregateSizeCountDeltasInOplog(oplogCursor, ts5);
        EXPECT_EQ(deltas.size(), 0u);
    }

    {
        // Timestamp::max() is too large a value to extract a RecordId from the oplog from.
        ASSERT_THROWS_CODE(
            replicated_fast_count::aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::max()),
            DBException,
            ErrorCodes::BadValue);
    }
}

using ReplicatedFastCountDeathTest = ReplicatedFastCountTest;

// TODO SERVER-120203: Re-enable this test.
//  DEATH_TEST_REGEX_F(ReplicatedFastCountDeathTest,
//                     CannotHaveNegativeCommittedSizeOrCount,
//                     R"(Invariant failure.*Expected fast count size and count to be
//                     non-negative)") {
//      boost::container::flat_map<UUID, CollectionSizeCount> changes;
//      changes[UUID::gen()] = CollectionSizeCount{-10, -1};

//     _fastCountManager->commit(changes, boost::none);
// }

}  // namespace
}  // namespace mongo
