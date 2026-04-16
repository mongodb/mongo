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
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
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
             replicated_fast_count_test_helpers::FastCountOpType::kUpdate,
             /*expectedCount=*/boost::none,
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

enum class CapType { kCount, kSize };

class ReplicatedFastCountCappedCollectionTest : public ReplicatedFastCountTest,
                                                public ::testing::WithParamInterface<CapType> {};

TEST_P(ReplicatedFastCountCappedCollectionTest, CorrectSizeCountAfterCapReached) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const int maxDocs = 5;
    const NamespaceString nssCapped = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_test", "cappedWithMaxCount");

    if (GetParam() == CapType::kCount) {
        // The size parameter is not optional when specifying the max count. We intentionally make
        // the size larger here so the count is the limiting bound.
        ASSERT_OK(createCollection(_opCtx,
                                   nssCapped.dbName(),
                                   BSON("create" << nssCapped.coll() << "capped" << true << "size"
                                                 << maxDocs * sampleDocForInsert.objsize() * 5
                                                 << "max" << maxDocs)));
    } else {
        ASSERT_OK(createCollection(_opCtx,
                                   nssCapped.dbName(),
                                   BSON("create" << nssCapped.coll() << "capped" << true << "size"
                                                 << maxDocs * sampleDocForInsert.objsize())));
    }

    AutoGetCollection cappedColl(_opCtx, nssCapped, LockMode::MODE_IX);

    for (int i = 0; i < maxDocs + 5; ++i) {
        // Using the query-level InsertCommandRequest path here lets us avoid handling capped
        // collection multi-timestamp constraints manually.
        write_ops::InsertCommandRequest insertCmd(nssCapped);
        insertCmd.setDocuments({docGeneratorForInsert(i)});
        const auto result = write_ops_exec::performInserts(_opCtx, insertCmd);
        ASSERT_EQ(result.results.size(), 1);
        ASSERT_OK(result.results[0].getStatus());

        if (i < maxDocs - 1) {
            const auto [actualSize, actualCount] = cappedColl->latestSizeCount(_opCtx);
            const long long expectedCount = i + 1;
            EXPECT_EQ(actualSize, expectedCount * sampleDocForInsert.objsize());
            EXPECT_EQ(actualCount, expectedCount);
        } else {
            // After the collection cap has been reached, the size and count should stay the same.
            const auto [actualSize, actualCount] = cappedColl->latestSizeCount(_opCtx);
            EXPECT_EQ(actualSize, maxDocs * sampleDocForInsert.objsize());
            EXPECT_EQ(actualCount, maxDocs);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(,
                         ReplicatedFastCountCappedCollectionTest,
                         ::testing::Values(CapType::kCount, CapType::kSize));

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
    SingleOpSizeMetadata m;
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
    EXPECT_EQ(replicated_fast_count::extractSizeCountDeltaForOp(oplogEntry), boost::none);
}

TEST_F(ReplicatedFastCountDeltaUtilsTest, ExtractSizeCountDeltaOnNonEligibleNss) {
    const NamespaceString localNss =
        NamespaceString::createNamespaceString_forTest("local", "coll1");
    EXPECT_FALSE(isReplicatedFastCountEligible(localNss));

    const auto oplogEntry =
        makeOplogEntryWithSizeMeta(localNss, repl::OpTypeEnum::kNoop, 400 /* sizeDelta */);

    // Even though the oplog entry carries size metadata, ineligible namespaces should be skipped.
    ASSERT_FALSE(replicated_fast_count::extractSizeCountDeltaForOp(oplogEntry).has_value());
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

namespace replicated_fast_count {
namespace {

/**
 * The expected aggregate size and count for a particular user collection yielded from scanning the
 * oplog.
 */
struct AggregateDeltaExpectation {
    CollectionSizeCount delta;

    // The timestamp of the final oplog entry scanned when aggregating size counts for a particular
    // user collection. The final oplog entry does not need to be an oplog entry for the user
    // collection.
    Timestamp lastTimestamp;
};

/**
 * Test methods should default to testing aggregate size count with this method, as it checks
 * both methods of aggregation (acquiring a map of deltas across uuids and aggregating deltas
 * for a single uuid) yield equivalent results for the 'uuid'.
 */
void assertExpectedAggregateDelta(const AggregateDeltaExpectation& expected,
                                  const UUID& uuid,
                                  const Timestamp& seekAfterTS,
                                  SeekableRecordCursor& oplogCursor) {
    // Deltas across UUIDs.
    const auto deltas = aggregateSizeCountDeltasInOplog(oplogCursor, seekAfterTS);
    ASSERT_TRUE(deltas.deltas.contains(uuid));
    EXPECT_EQ(deltas.deltas.at(uuid).sizeCount, expected.delta);
    EXPECT_EQ(deltas.lastTimestamp, expected.lastTimestamp);

    // Also correct when filtered explicitly by 'uuid'
    const auto filteredDeltas = aggregateSizeCountDeltasInOplog(oplogCursor, seekAfterTS, uuid);
    ASSERT_TRUE(filteredDeltas.deltas.contains(uuid));
    ASSERT_EQ(expected.delta, filteredDeltas.deltas.at(uuid).sizeCount);
    EXPECT_EQ(filteredDeltas.lastTimestamp, expected.lastTimestamp);
}

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

class AggregateSizeCountFromOplogTest : public CatalogTestFixture {
protected:
    /**
     * Bundles information about a user "collection" needed for CRUD oplog entries.
     */
    test_helpers::NsAndUUID collA = {
        .nss = NamespaceString::createNamespaceString_forTest("agg_size_count_from_oplog", "collA"),
        .uuid = UUID::gen()};
    test_helpers::NsAndUUID collB = {
        .nss = NamespaceString::createNamespaceString_forTest("agg_size_count_from_oplog", "collB"),
        .uuid = UUID::gen()};
};

TEST_F(AggregateSizeCountFromOplogTest, AggregateSingleColl) {
    const Timestamp ts1{1, 2};
    const Timestamp ts2{2, 2};
    const Timestamp ts3{3, 2};

    std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kUpdate, 100 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(ts3, collA, repl::OpTypeEnum::kDelete, -110 /*sizeDelta=*/),
    };
    OplogCursorMock oplogCursor(std::move(entries));
    const auto& uuidA = collA.uuid;

    // (1) Aggregate size count deltas after Timestamp::min().
    // Since there were oplog entries with replicated size count, an entry exists, but its
    // aggregates should sum to 0 as the only document inserted was eventually deleted.
    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = 0, .count = 0}, .lastTimestamp = ts3},
        uuidA,
        Timestamp::min(),
        oplogCursor);

    // (2) Aggregate size count deltas after ts1 accounts for update and delete.
    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = -10, .count = -1}, .lastTimestamp = ts3},
        uuidA,
        ts1,
        oplogCursor);

    // (3) Timestamp at or past the last entry yields no deltas.
    // Check the result without a uuid filter.
    const auto oplogScanResult = aggregateSizeCountDeltasInOplog(oplogCursor, ts3);
    EXPECT_EQ(oplogScanResult.deltas.size(), 0u);

    // Check the result with a uuid filter.
    EXPECT_FALSE(aggregateSizeCountDeltasInOplog(oplogCursor, ts3, uuidA).deltas.contains(uuidA));
}

TEST_F(AggregateSizeCountFromOplogTest, AggregateMultipleCollections) {
    // Synthetic timestamps, ordered oldest -> newest.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    const Timestamp ts4{1, 4};
    const Timestamp ts5{1, 5};

    // Size deltas for respective ops on collA and collB.
    const int32_t insertA1 = 50;
    const int32_t insertA2 = 60;
    const int32_t insertB1 = 70;
    const int32_t delA1 = -50;
    const int32_t delB1 = -70;

    // Two inserts for _coll1, one insert for collB, then one delete each.
    std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, insertA1),
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, insertA2),
        test_helpers::makeOplogEntry(ts3, collB, repl::OpTypeEnum::kInsert, insertB1),
        test_helpers::makeOplogEntry(ts4, collA, repl::OpTypeEnum::kDelete, delA1),
        test_helpers::makeOplogEntry(ts5, collB, repl::OpTypeEnum::kDelete, delB1),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    // Aggregating from Timestamp::min() aggregates all entries.
    {
        // 2 collections tracked.
        EXPECT_EQ(aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min()).deltas.size(), 2u);
        assertExpectedAggregateDelta(
            {.delta = CollectionSizeCount{.size = (insertA1 + insertA2 + delA1), .count = 1},
             .lastTimestamp = ts5},
            collA.uuid,
            Timestamp::min(),
            oplogCursor);
        // Deltas sum to 0.
        assertExpectedAggregateDelta({.delta = CollectionSizeCount{}, .lastTimestamp = ts5},
                                     collB.uuid,
                                     Timestamp::min(),
                                     oplogCursor);
    }

    // Aggregating after ts3 (the last insert) only sees the two deletes.
    {
        EXPECT_EQ(aggregateSizeCountDeltasInOplog(oplogCursor, ts3).deltas.size(), 2u);
        assertExpectedAggregateDelta(
            {.delta = CollectionSizeCount{.size = delA1, .count = -1}, .lastTimestamp = ts5},
            collA.uuid,
            ts3,
            oplogCursor);
        assertExpectedAggregateDelta(
            {.delta = CollectionSizeCount{.size = delB1, .count = -1}, .lastTimestamp = ts5},
            collB.uuid,
            ts3,
            oplogCursor);
    }

    // Aggregating with ts5 doesn't yield deltas because the aggregation excludes the timestamp
    // provided.
    {
        const auto oplogScanResult = aggregateSizeCountDeltasInOplog(oplogCursor, ts5);
        EXPECT_EQ(oplogScanResult.deltas.size(), 0u);
    }

    {
        // Timestamp::max() is too large a value to extract a RecordId from the oplog from.
        ASSERT_THROWS_CODE(aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::max()),
                           DBException,
                           ErrorCodes::BadValue);
    }
}

// Verifies that the forward oplog cursor respects the oplog visibility timestamp: entries committed
// beyond the visibility point are excluded from the size count aggregation.
TEST_F(AggregateSizeCountFromOplogTest, ForwardCursorRespectsOplogVisibilityTimestamp) {
    auto opCtx = operationContext();
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};

    // Insert two committed, durable oplog entries for collA.
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/));
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, 20 /*sizeDelta=*/));

    // Cap visibility to ts1. ScopedOplogVisibleTimestamp opens the WT transaction and overrides
    // _oplogVisibleTs before the cursor is created, so initVisibility() captures ts1.
    ScopedOplogVisibleTimestamp scopedVisibility(shard_role_details::getRecoveryUnit(opCtx),
                                                 static_cast<int64_t>(ts1.asULL()));
    AutoGetOplogFastPath oplogRead(opCtx, OplogAccessMode::kRead);
    const auto& oplogColl = oplogRead.getCollection();
    auto cursor =
        oplogColl->getRecordStore()->getCursor(opCtx, *shard_role_details::getRecoveryUnit(opCtx));

    const auto result = aggregateSizeCountDeltasInOplog(*cursor, Timestamp::min());

    // Only the ts1 entry was visible; ts2 must not appear in the deltas.
    ASSERT_EQ(result.deltas.size(), 1u);
    ASSERT_TRUE(result.deltas.count(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 10);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.count, 1);
    ASSERT_TRUE(result.lastTimestamp.has_value());
    EXPECT_EQ(result.lastTimestamp.value(), ts1);
}

TEST_F(AggregateSizeCountFromOplogTest, AggregateTruncateRangeInsideApplyOps) {
    const Timestamp ts1{1, 1};
    const int64_t bytesDeleted = 120;
    const int64_t docsDeleted = 3;

    // Build an applyOps entry with a truncateRange inner op. The 'o' field is taken from the
    // truncateRange entry produced by the test helper.
    const auto truncateEntry =
        test_helpers::makeTruncateRangeOplogEntry(ts1, collA, bytesDeleted, docsDeleted);
    const NamespaceString adminCmdNss =
        NamespaceString::createNamespaceString_forTest("admin", "$cmd");
    BSONObj truncateInnerOp = BSON("op" << "c"
                                        << "ns" << collA.nss.getCommandNS().ns_forTest() << "ui"
                                        << collA.uuid << "o" << truncateEntry.getObject());

    std::list<repl::OplogEntry> entries{
        repl::OplogEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts1, 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = BSON("applyOps" << BSON_ARRAY(truncateInnerOp)),
            .wallClockTime = Date_t::now(),
        }}},
    };
    OplogCursorMock oplogCursor(std::move(entries));

    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = -bytesDeleted, .count = -docsDeleted},
         .lastTimestamp = ts1},
        collA.uuid,
        Timestamp::min(),
        oplogCursor);
}

TEST_F(AggregateSizeCountFromOplogTest, AggregateTruncateRangeInsideNestedApplyOps) {
    const Timestamp ts1{1, 1};
    const int64_t bytesDeleted = 80;
    const int64_t docsDeleted = 2;

    const auto truncateEntry =
        test_helpers::makeTruncateRangeOplogEntry(ts1, collA, bytesDeleted, docsDeleted);
    const NamespaceString adminCmdNss =
        NamespaceString::createNamespaceString_forTest("admin", "$cmd");
    BSONObj truncateInnerOp = BSON("op" << "c"
                                        << "ns" << collA.nss.getCommandNS().ns_forTest() << "ui"
                                        << collA.uuid << "o" << truncateEntry.getObject());

    // Wrap the truncateRange in an inner applyOps, then in an outer applyOps.
    BSONObj innerApplyOpsOp = BSON("op" << "c" << "ns" << adminCmdNss.ns_forTest() << "o"
                                        << BSON("applyOps" << BSON_ARRAY(truncateInnerOp)));

    std::list<repl::OplogEntry> entries{
        repl::OplogEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts1, 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = BSON("applyOps" << BSON_ARRAY(innerApplyOpsOp)),
            .wallClockTime = Date_t::now(),
        }}},
    };
    OplogCursorMock oplogCursor(std::move(entries));

    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = -bytesDeleted, .count = -docsDeleted},
         .lastTimestamp = ts1},
        collA.uuid,
        Timestamp::min(),
        oplogCursor);
}

TEST_F(AggregateSizeCountFromOplogTest, AggregateSingleTruncateRange) {
    const Timestamp ts1{1, 1};
    const int64_t bytesDeleted = 150;
    const int64_t docsDeleted = 3;

    std::list<repl::OplogEntry> entries{
        test_helpers::makeTruncateRangeOplogEntry(ts1, collA, bytesDeleted, docsDeleted),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = -bytesDeleted, .count = -docsDeleted},
         .lastTimestamp = ts1},
        collA.uuid,
        Timestamp::min(),
        oplogCursor);
}

TEST_F(AggregateSizeCountFromOplogTest, AggregateTruncateRangeMixedWithCRUD) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    const Timestamp ts4{1, 4};
    const Timestamp ts5{1, 5};
    const Timestamp ts6{1, 6};

    // 3 inserts (+270 bytes, +3 docs), 1 update (-10 bytes, 0 docs), 1 delete (-90 bytes, -1 doc),
    // 1 truncateRange (-80 bytes, -1 doc).
    std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100),
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/90),
        test_helpers::makeOplogEntry(ts3, collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/80),
        test_helpers::makeOplogEntry(ts4, collA, repl::OpTypeEnum::kUpdate, /*sizeDelta=*/-10),
        test_helpers::makeOplogEntry(ts5, collA, repl::OpTypeEnum::kDelete, /*sizeDelta=*/-90),
        test_helpers::makeTruncateRangeOplogEntry(
            ts6, collA, /*bytesDeleted=*/80, /*docsDeleted=*/1),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    // Net: size = 100+90+80-10-90-80 = 90, count = 1+1+1+0-1-1 = 1.
    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = 90, .count = 1}, .lastTimestamp = ts6},
        collA.uuid,
        Timestamp::min(),
        oplogCursor);
}

TEST_F(AggregateSizeCountFromOplogTest, CollectionCreationMarksStateCreated) {
    const Timestamp ts1{1, 1};
    std::list<repl::OplogEntry> entries{test_helpers::makeCreateOplogEntry(ts1, collA)};
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 0);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.count, 0);
}

TEST_F(AggregateSizeCountFromOplogTest, CollectionDropMarksStateDropped) {
    const Timestamp ts1{1, 1};
    std::list<repl::OplogEntry> entries{test_helpers::makeDropOplogEntry(ts1, collA)};
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kDropped);
}

TEST_F(AggregateSizeCountFromOplogTest, CollectionCreationThenInsertsMarkedCreated) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    std::list<repl::OplogEntry> entries{
        test_helpers::makeCreateOplogEntry(ts1, collA),
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, 10),
        test_helpers::makeOplogEntry(ts3, collA, repl::OpTypeEnum::kInsert, 20),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 30);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.count, 2);
}

TEST_F(AggregateSizeCountFromOplogTest, InsertAndDropMarkedDropped) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, 10),
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, 20),
        test_helpers::makeDropOplogEntry(ts3, collA),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kDropped);
}
}  // namespace
}  // namespace replicated_fast_count
}  // namespace mongo
