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
#include "mongo/db/repl/oplog_interface_local.h"
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
        // ReplicatedFastCountManager::flushSync_ForTest().
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

const NamespaceString replicatedFastCountStoreNss =
    NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);

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
    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
        _opCtx,
        _uuid1,
        /*expectPersisted=*/false,
        /*expectedCount=*/0,
        /*expectedSize=*/0);
    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
        _opCtx,
        _uuid2,
        /*expectPersisted=*/false,
        /*expectedCount=*/0,
        /*expectedSize=*/0);

    // Manually trigger an iteration to write dirty metadata to the internal collection.
    _fastCountManager->flushSync(_opCtx);

    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
        _opCtx,
        _uuid1,
        /*expectPersisted=*/true,
        numDocsColl1,
        numDocsColl1 * sampleDocForInsert.objsize());
    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
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

    auto applyOpsEntries =
        replicated_fast_count_test_helpers::getApplyOpsForNss(_opCtx, replicatedFastCountStoreNss);

    EXPECT_EQ(applyOpsEntries.size(), 1u);
    auto& applyOpsEntry = applyOpsEntries.front();

    replicated_fast_count_test_helpers::assertFastCountApplyOpsMatches(
        applyOpsEntry,
        replicatedFastCountStoreNss,
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
        _opCtx, replicatedFastCountStoreNss);

    const int64_t sizeDelta = sampleDocForUpdate.objsize() - sampleDocForInsert.objsize();
    const int64_t newExpectedSizeColl1 =
        (numDocsColl1 * sampleDocForInsert.objsize()) + (sizeDelta * totalUpdatesColl1);
    const int64_t newExpectedSizeColl2 =
        (numDocsColl2 * sampleDocForInsert.objsize()) + (sizeDelta * totalUpdatesColl2);

    replicated_fast_count_test_helpers::assertFastCountApplyOpsMatches(
        applyOpsEntry,
        replicatedFastCountStoreNss,
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
        _opCtx, replicatedFastCountStoreNss);

    const int64_t sizeDelta = sampleDocForUpdate.objsize() - sampleDocForInsert.objsize();
    const int64_t newExpectedSizeColl1 =
        (numDocsColl1 * sampleDocForInsert.objsize()) + (sizeDelta * totalUpdatesColl1);

    replicated_fast_count_test_helpers::assertFastCountApplyOpsMatches(
        applyOpsEntry,
        replicatedFastCountStoreNss,
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

        ASSERT_OK(Helpers::insert(
            _opCtx,
            *fastCountColl,
            BSON("_id" << uuid1 << ReplicatedFastCountManager::kValidAsOfKey << Timestamp(1, 1)
                       << ReplicatedFastCountManager::kMetaDataKey
                       << BSON(ReplicatedFastCountManager::kCountKey
                               << expectedCount1 << ReplicatedFastCountManager::kSizeKey
                               << expectedSize1))));

        ASSERT_OK(Helpers::insert(
            _opCtx,
            *fastCountColl,
            BSON("_id" << uuid2 << ReplicatedFastCountManager::kValidAsOfKey << Timestamp(1, 1)
                       << ReplicatedFastCountManager::kMetaDataKey
                       << BSON(ReplicatedFastCountManager::kCountKey
                               << expectedCount2 << ReplicatedFastCountManager::kSizeKey
                               << expectedSize2))));

        wuow.commit();
    }

    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
        _opCtx, uuid1, /*expectPersisted=*/true, expectedCount1, expectedSize1);
    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
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
    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalCollection(
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
};  // namespace mongo
