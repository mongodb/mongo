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
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_uncommitted_changes.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
class ReplicatedFastCountTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        _opCtx = operationContext();
        ASSERT_OK(createFastcountCollection(_opCtx));

        _fastCountManager = &ReplicatedFastCountManager::get(_opCtx->getServiceContext());
        // Allow for control over when we write to our internal collection for testing. We only
        // write to the internal collection when we explicitly call
        // ReplicatedFastCountManager::flush().
        _fastCountManager->disablePeriodicWrites_ForTest();
        _fastCountManager->startup(_opCtx);

        ASSERT_OK(createCollection(_opCtx, _nss1.dbName(), BSON("create" << _nss1.coll())));
        ASSERT_OK(createCollection(_opCtx, _nss2.dbName(), BSON("create" << _nss2.coll())));
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
            _fastCountManager->shutdown();
            _fastCountManager = nullptr;
        }
    }

    OperationContext* _opCtx;
    ReplicatedFastCountManager* _fastCountManager;

    NamespaceString _nss1 =
        NamespaceString::createNamespaceString_forTest("replicated_fast_count_test", "coll1");
    NamespaceString _nss2 =
        NamespaceString::createNamespaceString_forTest("replicated_fast_count_test", "coll2");
};

/**
 * Checks the persisted values of count and size for the given UUID in the internal
 * replicated fast count collection.
 */
void checkFastCountMetadataInInternalCollection(OperationContext* opCtx,
                                                const UUID& uuid,
                                                bool expectPersisted,
                                                int64_t expectedCount,
                                                int64_t expectedSize) {
    {
        AutoGetCollection fastCountColl(opCtx,
                                        NamespaceString::makeGlobalConfigCollection(
                                            NamespaceString::kSystemReplicatedFastCountStore),
                                        LockMode::MODE_IS);

        BSONObj persisted;
        bool found = Helpers::findById(opCtx, fastCountColl->ns(), BSON("_id" << uuid), persisted);

        EXPECT_EQ(found, expectPersisted);
        if (!expectPersisted) {
            return;
        }
        int64_t persistedCount = persisted.getField(ReplicatedFastCountManager::kCountKey).Long();
        int64_t persistedSize = persisted.getField(ReplicatedFastCountManager::kSizeKey).Long();
        EXPECT_EQ(persistedCount, expectedCount);
        EXPECT_EQ(persistedSize, expectedSize);
    }
}

/**
 * Checks the uncommitted fast count changes for the given UUID.
 */
void checkUncommittedFastCountChanges(OperationContext* opCtx,
                                      const UUID& uuid,
                                      int64_t expectedCount,
                                      int64_t expectedSize) {
    auto uncommittedChanges = UncommittedFastCountChange::getForRead(opCtx);
    auto uncommittedSizeAndCount = uncommittedChanges.find(uuid);

    EXPECT_EQ(uncommittedSizeAndCount.count, expectedCount);
    EXPECT_EQ(uncommittedSizeAndCount.size, expectedSize);
}

/**
 * Checks the committed fast count changes for the given UUID.
 */
void checkCommittedFastCountChanges(const UUID& uuid,
                                    ReplicatedFastCountManager* fastCountManager,
                                    int64_t expectedCount,
                                    int64_t expectedSize) {
    auto committedSizeAndCount = fastCountManager->find(uuid);

    EXPECT_EQ(committedSizeAndCount.count, expectedCount);
    EXPECT_EQ(committedSizeAndCount.size, expectedSize);
}


TEST_F(ReplicatedFastCountTest, UncommittedChangesResetOnCommit) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    AutoGetCollection coll(_opCtx, _nss1, LockMode::MODE_IX);

    WriteUnitOfWork wuow{_opCtx};

    auto doc = BSON("_id" << 0 << "x" << 1);
    ASSERT_OK(Helpers::insert(_opCtx, *coll, doc));

    checkCommittedFastCountChanges(coll->uuid(), _fastCountManager, 0, 0);
    checkUncommittedFastCountChanges(_opCtx, coll->uuid(), 1, doc.objsize());

    // On commit, uncommitted changes should be cleared and committed changes should be updated.
    wuow.commit();

    checkCommittedFastCountChanges(coll->uuid(), _fastCountManager, 1, doc.objsize());
    checkUncommittedFastCountChanges(_opCtx, coll->uuid(), 0, 0);
}

TEST_F(ReplicatedFastCountTest, UncommittedChangesResetOnRollback) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    UUID uuid = UUID::gen();
    auto doc = BSON("_id" << 0 << "x" << 1);

    {
        AutoGetCollection coll(_opCtx, _nss1, LockMode::MODE_IX);
        WriteUnitOfWork wuow{_opCtx};

        uuid = coll->uuid();

        ASSERT_OK(Helpers::insert(_opCtx, *coll, doc));

        checkCommittedFastCountChanges(coll->uuid(), _fastCountManager, 0, 0);
        checkUncommittedFastCountChanges(_opCtx, coll->uuid(), 1, doc.objsize());

        // Letting the WriteUnitOfWork go out of scope without committing should trigger a rollback.
    }

    checkCommittedFastCountChanges(uuid, _fastCountManager, 0, 0);
    checkUncommittedFastCountChanges(_opCtx, uuid, 0, 0);
}

TEST_F(ReplicatedFastCountTest, DirtyMetadataWrittenToInternalCollection) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    auto sampleDoc = BSON("_id" << 0 << "x" << 0);

    UUID uuid1 = UUID::gen();
    int expectedCountColl1 = 5;
    int expectedSizeColl1 = expectedCountColl1 * sampleDoc.objsize();

    UUID uuid2 = UUID::gen();
    int expectedCountColl2 = 10;
    int expectedSizeColl2 = expectedCountColl2 * sampleDoc.objsize();

    // Insert documents to create dirty metadata. TODO SERVER-118457: Extract shared logic into
    // helpers, extend to operate on more collections.
    {
        AutoGetCollection coll(_opCtx, _nss1, LockMode::MODE_IX);
        uuid1 = coll->uuid();

        for (int i = 0; i < expectedCountColl1; i++) {
            WriteUnitOfWork wuow{_opCtx};

            auto doc = BSON("_id" << i << "x" << i);
            ASSERT_OK(Helpers::insert(_opCtx, *coll, doc));

            wuow.commit();
        }

        AutoGetCollection coll2(_opCtx, _nss2, LockMode::MODE_IX);
        uuid2 = coll2->uuid();

        for (int i = 0; i < expectedCountColl2; i++) {
            WriteUnitOfWork wuow{_opCtx};

            auto doc = BSON("_id" << i << "x" << i);
            ASSERT_OK(Helpers::insert(_opCtx, *coll2, doc));

            wuow.commit();
        }
    }

    // Verify that the changes in size and count are stored in memory as committed changes, and that
    // no uncommitted changes remain.
    checkCommittedFastCountChanges(uuid1, _fastCountManager, expectedCountColl1, expectedSizeColl1);
    checkUncommittedFastCountChanges(_opCtx, uuid1, 0, 0);

    checkCommittedFastCountChanges(uuid2, _fastCountManager, expectedCountColl2, expectedSizeColl2);
    checkUncommittedFastCountChanges(_opCtx, uuid2, 0, 0);

    // Verify that the committed changes have not been written to the internal fast count collection
    // yet.
    checkFastCountMetadataInInternalCollection(
        _opCtx, uuid1, /*expectPersisted=*/false, /*expectedCount=*/0, /*expectedSize=*/0);
    checkFastCountMetadataInInternalCollection(
        _opCtx, uuid2, /*expectPersisted=*/false, /*expectedCount=*/0, /*expectedSize=*/0);

    // Manually trigger an iteration to write dirty metadata to the internal collection.
    _fastCountManager->flush(_opCtx);

    checkFastCountMetadataInInternalCollection(_opCtx,
                                               uuid1,
                                               /*expectPersisted=*/true,
                                               expectedCountColl1,
                                               expectedSizeColl1);
    checkFastCountMetadataInInternalCollection(_opCtx,
                                               uuid2,
                                               /*expectPersisted=*/true,
                                               expectedCountColl2,
                                               expectedSizeColl2);
}

TEST_F(ReplicatedFastCountTest, UpdatesAreCorrectlyAccountedFor) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    auto sampleDoc = BSON("_id" << 0 << "x" << 0);

    UUID uuid = UUID::gen();
    int expectedCount = 5;
    int expectedSize = expectedCount * sampleDoc.objsize();

    // Insert documents.
    {
        AutoGetCollection coll(_opCtx, _nss1, LockMode::MODE_IX);
        uuid = coll->uuid();

        for (int i = 0; i < expectedCount; i++) {
            WriteUnitOfWork wuow{_opCtx};

            auto doc = BSON("_id" << i << "x" << i);
            ASSERT_OK(Helpers::insert(_opCtx, *coll, doc));

            wuow.commit();
        }
    }

    checkCommittedFastCountChanges(uuid, _fastCountManager, expectedCount, expectedSize);
    checkUncommittedFastCountChanges(_opCtx, uuid, 0, 0);

    auto sampleDocAfterUpdate = BSON("_id" << 0 << "x" << 0 << "y" << 0);
    int expectedSizeAfterUpdate = expectedCount * sampleDocAfterUpdate.objsize();

    // Update documents.
    {
        auto coll = acquireCollection(_opCtx,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          _opCtx, _nss1, AcquisitionPrerequisites::kWrite),
                                      MODE_IX);

        for (int i = 0; i < expectedCount; i++) {
            WriteUnitOfWork wuow{_opCtx};

            auto doc = BSON("_id" << i << "x" << i << "y" << i * 2);
            Helpers::update(_opCtx, coll, BSON("_id" << i), BSON("$set" << doc));

            wuow.commit();
        }
    }

    checkCommittedFastCountChanges(uuid, _fastCountManager, expectedCount, expectedSizeAfterUpdate);
    checkUncommittedFastCountChanges(_opCtx, uuid, 0, 0);
}

TEST_F(ReplicatedFastCountTest, DeletesAreCorrectlyAccountedFor) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    auto sampleDoc = BSON("_id" << 0 << "x" << 0);
    UUID uuid = UUID::gen();
    int expectedCount = 5;
    int expectedSize = expectedCount * sampleDoc.objsize();

    // Insert documents.
    {
        AutoGetCollection coll(_opCtx, _nss1, LockMode::MODE_IX);
        uuid = coll->uuid();

        for (int i = 0; i < expectedCount; i++) {
            WriteUnitOfWork wuow{_opCtx};

            auto doc = BSON("_id" << i << "x" << i);
            ASSERT_OK(Helpers::insert(_opCtx, *coll, doc));

            wuow.commit();
        }
    }

    checkCommittedFastCountChanges(uuid, _fastCountManager, expectedCount, expectedSize);
    checkUncommittedFastCountChanges(_opCtx, uuid, 0, 0);

    int documentsToDelete = 3;
    int expectedSizeAfterDeletes = expectedSize - (documentsToDelete * sampleDoc.objsize());

    // Delete documents.
    {
        auto coll = acquireCollection(_opCtx,
                                      CollectionAcquisitionRequest::fromOpCtx(
                                          _opCtx, _nss1, AcquisitionPrerequisites::kWrite),
                                      MODE_IX);

        for (int i = 0; i < documentsToDelete; i++) {
            WriteUnitOfWork wuow{_opCtx};

            RecordId rid = Helpers::findOne(_opCtx, coll, BSON("_id" << i));
            Helpers::deleteByRid(_opCtx, coll, rid);

            wuow.commit();
        }
    }

    checkCommittedFastCountChanges(
        uuid, _fastCountManager, expectedCount - documentsToDelete, expectedSizeAfterDeletes);
    checkUncommittedFastCountChanges(_opCtx, uuid, 0, 0);
}

TEST_F(ReplicatedFastCountTest, DirtyWriteNotLostIfWrittenAfterMetadataSnapshot) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    auto sampleDoc = BSON("_id" << 0 << "x" << 0);
    UUID uuid = UUID::gen();

    const int64_t numInitialDocs = 5;
    const int64_t initialSize = numInitialDocs * sampleDoc.objsize();
    const int64_t numExtraDocs = 5;
    const int64_t numTotalDocs = numInitialDocs + numExtraDocs;
    const int64_t totalSize = numTotalDocs * sampleDoc.objsize();

    {
        AutoGetCollection coll(_opCtx, _nss1, LockMode::MODE_IX);
        uuid = coll->uuid();
        for (int i = 0; i < numInitialDocs; ++i) {
            WriteUnitOfWork wuow{_opCtx};
            auto doc = BSON("_id" << i << "x" << i);
            ASSERT_OK(Helpers::insert(_opCtx, *coll, doc));
            wuow.commit();
        }
    }

    checkCommittedFastCountChanges(uuid, _fastCountManager, numInitialDocs, initialSize);

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
            _fastCountManager->flush(opCtxForThread);
        });

        fp->waitForTimesEntered(initialTimesEntered + 1);

        // Insert more documents into the same collection, creating new dirty metadata for that
        // collection.
        {
            AutoGetCollection coll(_opCtx, _nss1, LockMode::MODE_IX);
            for (int i = numInitialDocs; i < numTotalDocs; ++i) {
                WriteUnitOfWork wuow{_opCtx};
                auto doc = BSON("_id" << i << "x" << i);
                ASSERT_OK(Helpers::insert(_opCtx, *coll, doc));
                wuow.commit();
            }
        }

        checkCommittedFastCountChanges(uuid, _fastCountManager, numTotalDocs, totalSize);

        // Disable failpoint by letting it go out of scope.
    }

    iterThread.join();

    // If the dirty metadata wasn't incorrectly cleared, this flush should persist our second batch
    // of inserts.
    _fastCountManager->flush(_opCtx);

    // Verify that all of our writes were persisted to disk.
    checkFastCountMetadataInInternalCollection(_opCtx,
                                               uuid,
                                               /*expectPersisted=*/true,
                                               numTotalDocs,
                                               totalSize);
}

// TODO SERVER-118457: Parameterize test and test variety of operations with different sizes and
// counts. Test for drop, create.

// TODO SERVER-119033: Add tests for applyOps command.

}  // namespace
};  // namespace mongo
