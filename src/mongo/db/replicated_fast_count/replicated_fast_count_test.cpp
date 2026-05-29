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
#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::replicated_fast_count {
namespace {

using test_helpers::checkCommittedSizeCount;

// Selects whether the fast count metadata is persisted through the collection-backed path or the
// container-backed path. Used to parameterize tests so the same bodies exercise both paths.
enum class FastCountStoreMode { kCollection, kContainer };

/**
 * Shared test fixture for replicated fast count tests. Derived fixtures choose the persistence
 * mode by overriding `getMode()`, which drives whether the container-write feature flags are
 * enabled before `setUpReplicatedFastCount()` decides which backing store to provision.
 */
class ReplicatedFastCountTestBase : public CatalogTestFixture {
public:
    ReplicatedFastCountTestBase()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    virtual FastCountStoreMode getMode() const = 0;

    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();

        if (getMode() == FastCountStoreMode::kContainer) {
            _ffDurability = std::make_unique<RAIIServerParameterControllerForTest>(
                "featureFlagReplicatedFastCountDurability", true);
            _ffContainerWrites = std::make_unique<RAIIServerParameterControllerForTest>(
                "featureFlagContainerWrites", true);
        }

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
        ASSERT_OK(createCollection(_opCtx, _nss3.dbName(), BSON("create" << _nss3.coll())));
        {
            auto coll1 = acquireCollection(_opCtx,
                                           CollectionAcquisitionRequest::fromOpCtx(
                                               _opCtx, _nss1, AcquisitionPrerequisites::kRead),
                                           LockMode::MODE_IS);
            auto coll2 = acquireCollection(_opCtx,
                                           CollectionAcquisitionRequest::fromOpCtx(
                                               _opCtx, _nss2, AcquisitionPrerequisites::kRead),
                                           LockMode::MODE_IS);
            auto coll3 = acquireCollection(_opCtx,
                                           CollectionAcquisitionRequest::fromOpCtx(
                                               _opCtx, _nss3, AcquisitionPrerequisites::kRead),
                                           LockMode::MODE_IS);
            _uuid1 = coll1.uuid();
            _uuid2 = coll2.uuid();
            _uuid3 = coll3.uuid();
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
    NamespaceString _nss3 =
        NamespaceString::createNamespaceString_forTest("replicated_fast_count_test", "coll3");

    UUID _uuid1 = UUID::gen();
    UUID _uuid2 = UUID::gen();
    UUID _uuid3 = UUID::gen();

    BSONObj sampleDocForInsert = BSON("_id" << 0 << "x" << 0);
    BSONObj sampleDocForUpdate = BSON("_id" << 0 << "x" << 0 << "y" << 0);

    std::unique_ptr<RAIIServerParameterControllerForTest> _ffDurability;
    std::unique_ptr<RAIIServerParameterControllerForTest> _ffContainerWrites;
};

/**
 * Parameterized fixture that runs each test case in both collection-backed and container-backed
 * modes.
 */
class ReplicatedFastCountTest : public ReplicatedFastCountTestBase,
                                public ::testing::WithParamInterface<FastCountStoreMode> {
protected:
    FastCountStoreMode getMode() const override {
        return GetParam();
    }
};

/**
 * Collection-only fixture for tests that inherently depend on the collection-backed storage path
 * (e.g. tests that manipulate the internal fast count collection directly or validate the applyOps
 * payload layout emitted by the collection-backed flush path).
 */
class ReplicatedFastCountCollectionOnlyTest : public ReplicatedFastCountTestBase {
protected:
    FastCountStoreMode getMode() const override {
        return FastCountStoreMode::kCollection;
    }
};


inline std::string modeToString(FastCountStoreMode mode) {
    return mode == FastCountStoreMode::kCollection ? "Collection" : "Container";
}

const NamespaceString replicatedFastCountStoreNss =
    NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);

const std::function<BSONObj(int)> docGeneratorForInsert = [](int i) {
    return BSON("_id" << i << "x" << i);
};
const std::function<BSONObj(int)> docGeneratorForUpdate = [](int i) {
    return BSON("_id" << i << "x" << i << "y" << i * 2);
};

TEST_P(ReplicatedFastCountTest, UncommittedChangesResetOnCommit) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const int numDocs = 5;
    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numDocs,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);
}

TEST_P(ReplicatedFastCountTest, UncommittedChangesResetOnRollback) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const int numDocs = 5;
    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numDocs,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert,
                             /*abortWithoutCommit=*/true);
}

TEST_P(ReplicatedFastCountTest, UpdatesAreCorrectlyAccountedFor) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocs = 10;
    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numDocs,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);

    test_helpers::updateDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             /*startIdx=*/0,
                             /*endIdx=*/4,
                             /*startingCount=*/numDocs,
                             /*startingSize=*/numDocs * sampleDocForInsert.objsize(),
                             docGeneratorForUpdate,
                             sampleDocForInsert,
                             sampleDocForUpdate);
}

TEST_P(ReplicatedFastCountTest, DeletesAreCorrectlyAccountedFor) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocs = 10;
    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numDocs,
                             /*startingSize=*/0,
                             /*startingCount=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);

    test_helpers::deleteDocsByIDRange(_opCtx,
                                      _fastCountManager,
                                      _nss1,
                                      3,
                                      8,
                                      numDocs,
                                      numDocs * sampleDocForInsert.objsize(),
                                      sampleDocForInsert);
}

TEST_P(ReplicatedFastCountTest, DirtyMetadataWrittenToInternalCollection) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocsColl1 = 5;
    const int numDocsColl2 = 10;

    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numDocsColl1,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);

    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss2,
                             numDocsColl2,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);


    // Verify that the committed changes have not been written to the internal fast count collection
    // yet.
    test_helpers::checkFastCountMetadataInInternalStore(_opCtx,
                                                        _fastCountManager,
                                                        _uuid1,
                                                        /*expectPersisted=*/false,
                                                        /*expectedCount=*/0,
                                                        /*expectedSize=*/0);
    test_helpers::checkFastCountMetadataInInternalStore(_opCtx,
                                                        _fastCountManager,
                                                        _uuid2,
                                                        /*expectPersisted=*/false,
                                                        /*expectedCount=*/0,
                                                        /*expectedSize=*/0);

    // Manually trigger an iteration to write dirty metadata to the internal collection.
    _fastCountManager->flushSync(_opCtx);

    test_helpers::checkFastCountMetadataInInternalStore(_opCtx,
                                                        _fastCountManager,
                                                        _uuid1,
                                                        /*expectPersisted=*/true,
                                                        numDocsColl1,
                                                        numDocsColl1 *
                                                            sampleDocForInsert.objsize());
    test_helpers::checkFastCountMetadataInInternalStore(_opCtx,
                                                        _fastCountManager,
                                                        _uuid2,
                                                        /*expectPersisted=*/true,
                                                        numDocsColl2,
                                                        numDocsColl2 *
                                                            sampleDocForInsert.objsize());
}

TEST_P(ReplicatedFastCountTest, DirtyMetadataWrittenAsSingleApplyOpsEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocsColl1 = 5;
    const int numDocsColl2 = 10;

    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numDocsColl1,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);

    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss2,
                             numDocsColl2,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);

    _fastCountManager->flushSync(_opCtx);

    auto applyOpsEntries = test_helpers::getApplyOpsForFastCountStore(_opCtx);

    EXPECT_EQ(applyOpsEntries.size(), 1u);
    auto& applyOpsEntry = applyOpsEntries.front();

    test_helpers::assertFastCountApplyOpsMatches(applyOpsEntry,
                                                 {
                                                     {_uuid1,
                                                      test_helpers::FastCountOpType::kInsert,
                                                      numDocsColl1,
                                                      numDocsColl1 * sampleDocForInsert.objsize()},
                                                     {_uuid2,
                                                      test_helpers::FastCountOpType::kInsert,
                                                      numDocsColl2,
                                                      numDocsColl2 * sampleDocForInsert.objsize()},
                                                 });
}

TEST_P(ReplicatedFastCountTest, UpdatesWrittenToApplyOpsCorrectly) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocsColl1 = 10;
    const int numDocsColl2 = 20;

    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numDocsColl1,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);

    test_helpers::insertDocs(_opCtx,
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

    test_helpers::updateDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             startIdxColl1,
                             endIdxColl1,
                             /*startingCount=*/numDocsColl1,
                             /*startingSize=*/numDocsColl1 * sampleDocForInsert.objsize(),
                             docGeneratorForUpdate,
                             sampleDocForInsert,
                             sampleDocForUpdate);

    test_helpers::updateDocs(_opCtx,
                             _fastCountManager,
                             _nss2,
                             startIdxColl2,
                             endIdxColl2,
                             /*startingCount=*/numDocsColl2,
                             /*startingSize=*/numDocsColl2 * sampleDocForInsert.objsize(),
                             docGeneratorForUpdate,
                             sampleDocForInsert,
                             sampleDocForUpdate);

    _fastCountManager->flushSync(_opCtx);

    auto applyOpsEntry = test_helpers::getLatestApplyOpsForFastCountStore(_opCtx);

    const int64_t sizeDelta = sampleDocForUpdate.objsize() - sampleDocForInsert.objsize();
    const int64_t newExpectedSizeColl1 =
        (numDocsColl1 * sampleDocForInsert.objsize()) + (sizeDelta * totalUpdatesColl1);
    const int64_t newExpectedSizeColl2 =
        (numDocsColl2 * sampleDocForInsert.objsize()) + (sizeDelta * totalUpdatesColl2);

    test_helpers::assertFastCountApplyOpsMatches(
        applyOpsEntry,
        {
            {_uuid1, test_helpers::FastCountOpType::kUpdate, numDocsColl1, newExpectedSizeColl1},
            {_uuid2, test_helpers::FastCountOpType::kUpdate, numDocsColl2, newExpectedSizeColl2},
        });
}

TEST_P(ReplicatedFastCountTest, MixedUpdatesAndInsertInApplyOps) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocsColl1 = 25;
    const int numDocsColl2 = 40;

    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numDocsColl1,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);

    _fastCountManager->flushSync(_opCtx);

    test_helpers::insertDocs(_opCtx,
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

    test_helpers::updateDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             startIdxColl,
                             endIdxColl,
                             /*startingCount=*/numDocsColl1,
                             /*startingSize=*/numDocsColl1 * sampleDocForInsert.objsize(),
                             docGeneratorForUpdate,
                             sampleDocForInsert,
                             sampleDocForUpdate);

    ASSERT_OK(storageInterface()->dropCollection(_opCtx, _nss3));

    _fastCountManager->flushSync(_opCtx);

    auto applyOpsEntry = test_helpers::getLatestApplyOpsForFastCountStore(_opCtx);

    const int64_t sizeDelta = sampleDocForUpdate.objsize() - sampleDocForInsert.objsize();
    const int64_t newExpectedSizeColl1 =
        (numDocsColl1 * sampleDocForInsert.objsize()) + (sizeDelta * totalUpdatesColl1);

    test_helpers::assertFastCountApplyOpsMatches(
        applyOpsEntry,
        {
            {_uuid1, test_helpers::FastCountOpType::kUpdate, numDocsColl1, newExpectedSizeColl1},
            {_uuid2,
             test_helpers::FastCountOpType::kUpdate,
             numDocsColl2,
             numDocsColl2 * sampleDocForInsert.objsize()},
        });
}

TEST_P(ReplicatedFastCountTest, DropsWrittenToApplyOpsCorrectly) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocs = 5;

    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numDocs,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);

    _fastCountManager->flushSync(_opCtx);

    ASSERT_OK(storageInterface()->dropCollection(_opCtx, _nss1));

    // We should detect that we dropped the collection _nss1 and delete the fast count entry for it.
    _fastCountManager->flushSync(_opCtx);

    auto applyOpsEntry = test_helpers::getLatestApplyOpsForFastCountStore(_opCtx);

    test_helpers::assertFastCountApplyOpsMatches(
        applyOpsEntry, {{_uuid1, test_helpers::FastCountOpType::kDelete}});
}

TEST_P(ReplicatedFastCountTest, InsertsAndDropToCollectionSameFlush) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocs = 5;

    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numDocs,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);

    _fastCountManager->flushSync(_opCtx);

    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numDocs,
                             /*startingCount=*/numDocs,
                             /*startingSize=*/sampleDocForInsert.objsize() * numDocs,
                             docGeneratorForInsert,
                             sampleDocForInsert);

    ASSERT_OK(storageInterface()->dropCollection(_opCtx, _nss1));

    _fastCountManager->flushSync(_opCtx);

    auto applyOpsEntry = test_helpers::getLatestApplyOpsForFastCountStore(_opCtx);

    // We should only see the oplog entry removing the fast count entry for the collection we
    // dropped, and not any entries for the inserts we did before dropping the collection.
    test_helpers::assertFastCountApplyOpsMatches(
        applyOpsEntry, {{_uuid1, test_helpers::FastCountOpType::kDelete}});
}

TEST_F(ReplicatedFastCountCollectionOnlyTest, StartupFailsIfFastCountCollectionNotPresent) {
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

TEST_P(ReplicatedFastCountTest, DirtyWriteNotLostIfWrittenAfterMetadataSnapshot) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int64_t numInitialDocs = 5;
    const int64_t initialSize = numInitialDocs * sampleDocForInsert.objsize();
    const int64_t numExtraDocs = 5;
    const int64_t numTotalDocs = numInitialDocs + numExtraDocs;
    const int64_t totalSize = numTotalDocs * sampleDocForInsert.objsize();

    test_helpers::insertDocs(_opCtx,
                             _fastCountManager,
                             _nss1,
                             numInitialDocs,
                             /*startingCount=*/0,
                             /*startingSize=*/0,
                             docGeneratorForInsert,
                             sampleDocForInsert);

    checkCommittedSizeCount(_opCtx, _uuid1, {.size = initialSize, .count = numInitialDocs});

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
        test_helpers::insertDocs(_opCtx,
                                 _fastCountManager,
                                 _nss1,
                                 numExtraDocs,
                                 /*startingCount=*/numInitialDocs,
                                 /*startingSize=*/numInitialDocs * sampleDocForInsert.objsize(),
                                 docGeneratorForInsert,
                                 sampleDocForInsert);

        checkCommittedSizeCount(_opCtx, _uuid1, {.size = totalSize, .count = numTotalDocs});

        // Disable failpoint by letting it go out of scope.
    }

    iterThread.join();

    // If the dirty metadata wasn't incorrectly cleared, this flush should persist our second batch
    // of inserts.
    _fastCountManager->flushSync(_opCtx);

    // Verify that all of our writes were persisted to disk.
    test_helpers::checkFastCountMetadataInInternalStore(_opCtx,
                                                        _fastCountManager,
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

TEST_P(ReplicatedFastCountTest, ApplyOpsInsertsAreCorrectlyAccountedFor) {
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

    checkCommittedSizeCount(_opCtx, _uuid1, {.size = expectedSizeColl1, .count = numDocsColl1});
    checkCommittedSizeCount(_opCtx, _uuid2, {.size = expectedSizeColl2, .count = numDocsColl2});

    test_helpers::checkUncommittedFastCountChanges(_opCtx, _uuid1, 0, 0);
    test_helpers::checkUncommittedFastCountChanges(_opCtx, _uuid2, 0, 0);
}

TEST_P(ReplicatedFastCountTest, ApplyOpsUpdatesAreCorrectlyAccountedFor) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocs = 5;

    test_helpers::insertDocs(_opCtx,
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


    checkCommittedSizeCount(_opCtx, _uuid1, {.size = expectedNewSize, .count = numDocs});
    test_helpers::checkUncommittedFastCountChanges(_opCtx, _uuid1, 0, 0);
}

TEST_P(ReplicatedFastCountTest, ApplyOpsDeletesAreCorrectlyAccountedFor) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    const int numDocs = 10;

    test_helpers::insertDocs(_opCtx,
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

    checkCommittedSizeCount(_opCtx, _uuid1, {.size = expectedSize, .count = expectedCount});
    test_helpers::checkUncommittedFastCountChanges(_opCtx, _uuid1, 0, 0);
}

enum class CapType { kCount, kSize };

inline std::string capTypeToString(CapType type) {
    return type == CapType::kCount ? "Count" : "Size";
}

// Parameterized over (FastCountStoreMode, CapType) so the capped-collection eviction path is
// exercised against both the collection-backed and container-backed fast count stores.
class ReplicatedFastCountCappedCollectionTest
    : public ReplicatedFastCountTestBase,
      public ::testing::WithParamInterface<std::tuple<FastCountStoreMode, CapType>> {
protected:
    FastCountStoreMode getMode() const override {
        return std::get<0>(GetParam());
    }

    CapType getCapType() const {
        return std::get<1>(GetParam());
    }
};

TEST_P(ReplicatedFastCountCappedCollectionTest, CorrectSizeCountAfterCapReached) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const int maxDocs = 5;
    const NamespaceString nssCapped = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_test", "cappedWithMaxCount");

    if (getCapType() == CapType::kCount) {
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

    auto cappedColl = acquireCollection(_opCtx,
                                        CollectionAcquisitionRequest::fromOpCtx(
                                            _opCtx, nssCapped, AcquisitionPrerequisites::kWrite),
                                        LockMode::MODE_IX);

    for (int i = 0; i < maxDocs + 5; ++i) {
        // Using the query-level InsertCommandRequest path here lets us avoid handling capped
        // collection multi-timestamp constraints manually.
        write_ops::InsertCommandRequest insertCmd(nssCapped);
        insertCmd.setDocuments({docGeneratorForInsert(i)});
        const auto result = write_ops_exec::performInserts(_opCtx, insertCmd);
        ASSERT_EQ(result.results.size(), 1);
        ASSERT_OK(result.results[0].getStatus());

        if (i < maxDocs - 1) {
            const auto [actualSize, actualCount] =
                cappedColl.getCollectionPtr()->latestSizeCount(_opCtx);
            const long long expectedCount = i + 1;
            EXPECT_EQ(actualSize, expectedCount * sampleDocForInsert.objsize());
            EXPECT_EQ(actualCount, expectedCount);
        } else {
            // After the collection cap has been reached, the size and count should stay the same.
            const auto [actualSize, actualCount] =
                cappedColl.getCollectionPtr()->latestSizeCount(_opCtx);
            EXPECT_EQ(actualSize, maxDocs * sampleDocForInsert.objsize());
            EXPECT_EQ(actualCount, maxDocs);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    ,
    ReplicatedFastCountCappedCollectionTest,
    ::testing::Combine(::testing::Values(FastCountStoreMode::kCollection,
                                         FastCountStoreMode::kContainer),
                       ::testing::Values(CapType::kCount, CapType::kSize)),
    [](const ::testing::TestParamInfo<std::tuple<FastCountStoreMode, CapType>>& info) {
        return modeToString(std::get<0>(info.param)) + capTypeToString(std::get<1>(info.param));
    });

TEST_P(ReplicatedFastCountTest, ReplicatedFastCountDoesNotTrackLocalCollections) {
    const NamespaceString internalNss =
        NamespaceString::createNamespaceString_forTest("local.coll");
    ASSERT_OK(createCollection(_opCtx, internalNss.dbName(), BSON("create" << internalNss.coll())));

    auto internalColl =
        acquireCollection(_opCtx,
                          CollectionAcquisitionRequest::fromOpCtx(
                              _opCtx, internalNss, AcquisitionPrerequisites::kWrite),
                          LockMode::MODE_IX);
    const UUID internalUuid = internalColl.uuid();
    const long long docsToInsertCount = 10;

    long long expectedSize = 0;
    WriteUnitOfWork wuow(_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
    for (size_t i = 0; i < docsToInsertCount; ++i) {
        const BSONObj document = docGeneratorForInsert(i);
        ASSERT_OK(Helpers::insert(_opCtx, internalColl.getCollectionPtr(), document));
        expectedSize += document.objsize();
    }

    checkCommittedSizeCount(
        operationContext(), internalUuid, CollectionSizeCount{.size = 0, .count = 0});
    test_helpers::checkUncommittedFastCountChanges(_opCtx, internalUuid, 0, 0);

    wuow.commit();

    // Replicated fast count collection has no record of the writes to `internalColl`.
    checkCommittedSizeCount(
        operationContext(), internalUuid, CollectionSizeCount{.size = 0, .count = 0});
    test_helpers::checkUncommittedFastCountChanges(_opCtx, internalUuid, 0, 0);

    // Size and count data for `internalColl` are still tracked through the record store.
    EXPECT_EQ(internalColl.getCollectionPtr()->numRecords(_opCtx), docsToInsertCount);
    EXPECT_EQ(internalColl.getCollectionPtr()->dataSize(_opCtx), expectedSize);
}

TEST_P(ReplicatedFastCountTest, ReplicatedFastCountTracksNonLocalInternalCollections) {
    for (const auto& internalDbName : {"config", "admin"}) {
        const NamespaceString internalNss =
            NamespaceString::createNamespaceString_forTest(internalDbName, "coll");
        ASSERT_OK(
            createCollection(_opCtx, internalNss.dbName(), BSON("create" << internalNss.coll())));

        auto internalColl =
            acquireCollection(_opCtx,
                              CollectionAcquisitionRequest::fromOpCtx(
                                  _opCtx, internalNss, AcquisitionPrerequisites::kWrite),
                              LockMode::MODE_IX);
        const UUID internalUuid = internalColl.uuid();
        const long long docsToInsertCount = 10;

        long long expectedSize = 0;
        WriteUnitOfWork wuow(_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        for (size_t i = 0; i < docsToInsertCount; ++i) {
            const BSONObj document = docGeneratorForInsert(i);
            ASSERT_OK(Helpers::insert(_opCtx, internalColl.getCollectionPtr(), document));
            expectedSize += document.objsize();
        }

        checkCommittedSizeCount(
            operationContext(), internalUuid, CollectionSizeCount{.size = 0, .count = 0});
        test_helpers::checkUncommittedFastCountChanges(
            _opCtx, internalUuid, docsToInsertCount, expectedSize);

        wuow.commit();

        // Replicated fast count collection has record of the writes to `internalColl`.
        checkCommittedSizeCount(
            _opCtx, internalUuid, {.size = expectedSize, .count = docsToInsertCount});
        test_helpers::checkUncommittedFastCountChanges(_opCtx, internalUuid, 0, 0);
    }
}

/**
 * Minimal fixture for tests that only inspect oplog entries for size metadata fields. Does not
 * require the fast count persistence infrastructure.
 */
class SizeMetadataLoggingTest : public CatalogTestFixture {
public:
    SizeMetadataLoggingTest() = default;

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();

        auto* registry = dynamic_cast<OpObserverRegistry*>(getServiceContext()->getOpObserver());
        ASSERT(registry);
        registry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

        ASSERT_OK(createCollection(_opCtx, _nss.dbName(), BSON("create" << _nss.coll())));
        auto coll = acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss, AcquisitionPrerequisites::kRead),
            LockMode::MODE_IS);
        _uuid = coll.uuid();
    }

    OperationContext* _opCtx;
    NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest("replicated_fast_count_test", "coll1");
    UUID _uuid = UUID::gen();
};

TEST_F(SizeMetadataLoggingTest, BasicInsertOplogEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    auto coll = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss, AcquisitionPrerequisites::kWrite),
        LockMode::MODE_IX);

    const auto docToInsert = docGeneratorForInsert(0);
    const auto expectedSizeDelta = docToInsert.objsize();

    {
        WriteUnitOfWork wuow{_opCtx};
        ASSERT_OK(Helpers::insert(_opCtx, coll.getCollectionPtr(), docToInsert));
        wuow.commit();
    }

    const auto insertOplogEntry =
        test_helpers::getMostRecentOplogEntry(_opCtx, _nss, repl::OpTypeEnum::kInsert);
    ASSERT_TRUE(insertOplogEntry.has_value());

    // Confirm the generated oplog entry includes replicated size count information.
    test_helpers::assertReplicatedSizeCountMeta(*insertOplogEntry, expectedSizeDelta);
}

TEST_F(SizeMetadataLoggingTest, BasicUpdateOplogEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    auto coll = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss, AcquisitionPrerequisites::kWrite),
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

    const auto updateOplogEntry =
        test_helpers::getMostRecentOplogEntry(_opCtx, _nss, repl::OpTypeEnum::kUpdate);
    ASSERT_TRUE(updateOplogEntry.has_value());

    // Confirm the generated oplog entry includes replicated size count information.
    test_helpers::assertReplicatedSizeCountMeta(*updateOplogEntry, expectedSizeDelta);
}

TEST_F(SizeMetadataLoggingTest, BasicUpdateOplogEntryWithNegativeDelta) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    auto coll = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss, AcquisitionPrerequisites::kWrite),
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

    const auto updateOplogEntry =
        test_helpers::getMostRecentOplogEntry(_opCtx, _nss, repl::OpTypeEnum::kUpdate);
    ASSERT_TRUE(updateOplogEntry.has_value());
    test_helpers::assertReplicatedSizeCountMeta(*updateOplogEntry, expectedSizeDelta);
}

TEST_F(SizeMetadataLoggingTest, BasicDeleteOplogEntry) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    auto coll = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss, AcquisitionPrerequisites::kWrite),
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
    const auto deleteOplogEntry =
        test_helpers::getMostRecentOplogEntry(_opCtx, _nss, repl::OpTypeEnum::kDelete);
    ASSERT_TRUE(deleteOplogEntry.has_value());
    test_helpers::assertReplicatedSizeCountMeta(*deleteOplogEntry, -expectedSizeDelta);
}

TEST_F(SizeMetadataLoggingTest, BasicGroupCommit) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    auto coll = acquireCollection(
        _opCtx,
        CollectionAcquisitionRequest::fromOpCtx(_opCtx, _nss, AcquisitionPrerequisites::kWrite),
        MODE_IX);
    const auto doc1 = BSON("_id" << 0 << "x" << 0);
    const auto doc2 = BSON("_id" << 1 << "abcdefg" << 1);
    {
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        ASSERT_OK(Helpers::insert(_opCtx, coll.getCollectionPtr(), doc1));
        ASSERT_OK(Helpers::insert(_opCtx, coll.getCollectionPtr(), doc2));
        wuow.commit();
    }
    std::vector<test_helpers::OpValidationSpec> expectedOps{
        {.uuid = _uuid, .opType = repl::OpTypeEnum::kInsert, .expectedSizeDelta = doc1.objsize()},
        {.uuid = _uuid, .opType = repl::OpTypeEnum::kInsert, .expectedSizeDelta = doc2.objsize()}};

    const auto applyOpsOplogEntry = test_helpers::getLatestApplyOpsForNss(_opCtx, _nss);
    std::vector<repl::OplogEntry> innerEntries;
    repl::ApplyOps::extractOperationsTo(
        applyOpsOplogEntry, applyOpsOplogEntry.getEntry().toBSON(), &innerEntries);
    test_helpers::assertOpsMatchSpecs(innerEntries, expectedOps);
}

using ReplicatedFastCountDeathTest = ReplicatedFastCountTest;

// TODO SERVER-120203: Re-enable this test. It doesn't seem like DEATH_TEST_REGEX_P exists, so also
// add a container version of this test.
//  DEATH_TEST_REGEX_F(ReplicatedFastCountDeathTest,
//                     CannotHaveNegativeCommittedSizeOrCount,
//                     R"(Invariant failure.*Expected fast count size and count to be
//                     non-negative)") {
//      boost::container::flat_map<UUID, CollectionSizeCount> changes;
//      changes[UUID::gen()] = CollectionSizeCount{-10, -1};

//     _fastCountManager->commit(changes, boost::none);
// }

INSTANTIATE_TEST_SUITE_P(,
                         ReplicatedFastCountTest,
                         ::testing::Values(FastCountStoreMode::kCollection,
                                           FastCountStoreMode::kContainer),
                         [](const ::testing::TestParamInfo<FastCountStoreMode>& info) {
                             return modeToString(info.param);
                         });

}  // namespace

}  // namespace mongo::replicated_fast_count
