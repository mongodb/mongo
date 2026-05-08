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

#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/unittest/unittest.h"

namespace mongo::replicated_fast_count {
namespace {

class ReplicatedFastCountManagerTest : public CatalogTestFixture {
public:
    ReplicatedFastCountManagerTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<replicated_fast_count_test_helpers::
                                   ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();

        ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
        ASSERT_OK(
            createReplicatedFastCountTimestampCollection(storageInterface(), operationContext()));

        manager = std::make_unique<ReplicatedFastCountManager>(
            std::make_unique<CollectionSizeCountStore>(),
            std::make_unique<CollectionSizeCountTimestampStore>());
    }

    test_helpers::NsAndUUID collA = {
        .nss = NamespaceString::createNamespaceString_forTest("find_test", "collA"),
        .uuid = UUID::gen()};
    test_helpers::NsAndUUID collB = {
        .nss = NamespaceString::createNamespaceString_forTest("find_test", "collB"),
        .uuid = UUID::gen()};

    CollectionSizeCountStore sizeCountStore;
    CollectionSizeCountTimestampStore sizeCountTimestampStore;
    std::unique_ptr<ReplicatedFastCountManager> manager;
};

using ReplicatedFastCountManagerIdempotenceTest = ReplicatedFastCountManagerTest;

TEST_F(ReplicatedFastCountManagerIdempotenceTest, IdempotentStartupAndShutdown) {
    // Both startup() and shutdown() should be able to be called successively without failure.
    manager->startup(operationContext());
    manager->startup(operationContext());

    manager->shutdown(operationContext());
    manager->shutdown(operationContext());
}

using ReplicatedFastCountManagerNoCollectionsTest = CatalogTestFixture;

TEST_F(ReplicatedFastCountManagerNoCollectionsTest, InitializeMetadataDoesNothing) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    ReplicatedFastCountManager manager;
    manager.initializeMetadata(operationContext());

    EXPECT_EQ(manager.find(UUID::gen()), CollectionSizeCount(0, 0));
}

using ReplicatedFastCountManagerInitializeMetadataTest = ReplicatedFastCountManagerTest;

const RecordStore* getRecordStoreForUuid(OperationContext* opCtx, const UUID& uuid) {
    const auto catalog = CollectionCatalog::latest(opCtx->getServiceContext());
    const Collection* collection = catalog->lookupCollectionByUUID(opCtx, uuid);
    invariant(collection);
    return collection->getRecordStore();
}

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, InitializeMetadataNoData) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    manager->initializeMetadata(operationContext());
}

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, NoStoreData) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collA.nss, CollectionOptions{.uuid = collA.uuid}));
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collB.nss, CollectionOptions{.uuid = collB.uuid}));

    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));
    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(2, 2), collB, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100));

    manager->initializeMetadata(operationContext());

    {
        const RecordStore* recordStore = getRecordStoreForUuid(operationContext(), collA.uuid);
        EXPECT_EQ(recordStore->accurateNumRecords(), 1);
        EXPECT_EQ(recordStore->accurateDataSize(), 10);
    }
    {
        const RecordStore* recordStore = getRecordStoreForUuid(operationContext(), collB.uuid);
        EXPECT_EQ(recordStore->accurateNumRecords(), 1);
        EXPECT_EQ(recordStore->accurateDataSize(), 100);
    }
}

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, NoOplogData) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collA.nss, CollectionOptions{.uuid = collA.uuid}));
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collB.nss, CollectionOptions{.uuid = collB.uuid}));

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       SizeCountStore::Entry(Timestamp::min(), 5, 1));
    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collB.uuid,
                                       SizeCountStore::Entry(Timestamp::min(), 6, 2));

    manager->initializeMetadata(operationContext());

    {
        const RecordStore* recordStore = getRecordStoreForUuid(operationContext(), collA.uuid);
        EXPECT_EQ(recordStore->accurateNumRecords(), 1);
        EXPECT_EQ(recordStore->accurateDataSize(), 5);
    }
    {
        const RecordStore* recordStore = getRecordStoreForUuid(operationContext(), collB.uuid);
        EXPECT_EQ(recordStore->accurateNumRecords(), 2);
        EXPECT_EQ(recordStore->accurateDataSize(), 6);
    }
}

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, NoOplogAfterTimestamp) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collA.nss, CollectionOptions{.uuid = collA.uuid}));
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collB.nss, CollectionOptions{.uuid = collB.uuid}));

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       SizeCountStore::Entry(Timestamp::min(), 5, 1));
    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collB.uuid,
                                       SizeCountStore::Entry(Timestamp::min(), 6, 2));

    test_helpers::insertSizeCountTimestamp(
        operationContext(), sizeCountTimestampStore, Timestamp(3, 3));

    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));
    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(2, 2), collB, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100));

    manager->initializeMetadata(operationContext());

    {
        const RecordStore* recordStore = getRecordStoreForUuid(operationContext(), collA.uuid);
        EXPECT_EQ(recordStore->accurateNumRecords(), 1);
        EXPECT_EQ(recordStore->accurateDataSize(), 5);
    }
    {
        const RecordStore* recordStore = getRecordStoreForUuid(operationContext(), collB.uuid);
        EXPECT_EQ(recordStore->accurateNumRecords(), 2);
        EXPECT_EQ(recordStore->accurateDataSize(), 6);
    }
}

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, StoreAndOplogData) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collA.nss, CollectionOptions{.uuid = collA.uuid}));
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collB.nss, CollectionOptions{.uuid = collB.uuid}));

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       SizeCountStore::Entry(Timestamp::min(), 5, 1));
    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collB.uuid,
                                       SizeCountStore::Entry(Timestamp::min(), 6, 2));

    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(2, 2), collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));
    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(3, 3), collB, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100));

    manager->initializeMetadata(operationContext());

    {
        const RecordStore* recordStore = getRecordStoreForUuid(operationContext(), collA.uuid);
        EXPECT_EQ(recordStore->accurateNumRecords(), 1 + 1);
        EXPECT_EQ(recordStore->accurateDataSize(), 5 + 10);
    }
    {
        const RecordStore* recordStore = getRecordStoreForUuid(operationContext(), collB.uuid);
        EXPECT_EQ(recordStore->accurateNumRecords(), 2 + 1);
        EXPECT_EQ(recordStore->accurateDataSize(), 6 + 100);
    }
}

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, SkipsDroppedCollections) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    // Insert a size/count for a UUID that has no corresponding collection in the catalog.
    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       UUID::gen(),
                                       SizeCountStore::Entry(Timestamp::min(), 999, 99));

    // Initialization should skip the size/count entry without failing.
    manager->initializeMetadata(operationContext());
}

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, InitializeMetadataTracksOplogSizeCount) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collA.nss, CollectionOptions{.uuid = collA.uuid}));

    const auto catalog = CollectionCatalog::latest(operationContext()->getServiceContext());
    const Collection* oplogColl = catalog->lookupCollectionByNamespace(
        operationContext(), NamespaceString::kRsOplogNamespace);
    ASSERT(oplogColl);
    const UUID oplogUuid = oplogColl->uuid();

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       oplogUuid,
                                       SizeCountStore::Entry(Timestamp::min(), 500, 50));

    const repl::OplogEntry entry1 = test_helpers::makeOplogEntry(
        Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10);
    const repl::OplogEntry entry2 = test_helpers::makeOplogEntry(
        Timestamp(2, 2), collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20);

    const int64_t oplogSizeDelta =
        entry1.getEntry().toBSON().objsize() + entry2.getEntry().toBSON().objsize();

    test_helpers::writeToOplog(operationContext(), entry1);
    test_helpers::writeToOplog(operationContext(), entry2);

    manager->initializeMetadata(operationContext());

    const RecordStore* oplogRecordStore = getRecordStoreForUuid(operationContext(), oplogUuid);
    EXPECT_EQ(oplogRecordStore->accurateNumRecords(), 50 + 2);
    EXPECT_EQ(oplogRecordStore->accurateDataSize(), 500 + oplogSizeDelta);
}

using ReplicatedFastCountManagerCommitTest = ReplicatedFastCountManagerTest;

TEST_F(ReplicatedFastCountManagerCommitTest, CommitNothing) {
    manager->commit(
        operationContext(), boost::container::flat_map<UUID, CollectionSizeCount>{}, boost::none);
}

TEST_F(ReplicatedFastCountManagerCommitTest, CollectionNotFoundDoesNothing) {
    manager->commit(operationContext(),
                    boost::container::flat_map<UUID, CollectionSizeCount>{
                        {collA.uuid, CollectionSizeCount{.size = 42, .count = 2}}},
                    boost::none);
}

TEST_F(ReplicatedFastCountManagerCommitTest, CommitZeros) {
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collA.nss, CollectionOptions{.uuid = collA.uuid}));

    const RecordStore* recordStoreA = getRecordStoreForUuid(operationContext(), collA.uuid);

    EXPECT_EQ(recordStoreA->accurateDataSize(), 0);
    EXPECT_EQ(recordStoreA->accurateNumRecords(), 0);

    manager->commit(operationContext(),
                    boost::container::flat_map<UUID, CollectionSizeCount>{
                        {collA.uuid, CollectionSizeCount{.size = 0, .count = 0}}},
                    boost::none);

    EXPECT_EQ(recordStoreA->accurateDataSize(), 0);
    EXPECT_EQ(recordStoreA->accurateNumRecords(), 0);
}

TEST_F(ReplicatedFastCountManagerCommitTest, CommitUpdatesRecordStoreSizeCount) {
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collA.nss, CollectionOptions{.uuid = collA.uuid}));
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collB.nss, CollectionOptions{.uuid = collB.uuid}));

    const RecordStore* recordStoreA = getRecordStoreForUuid(operationContext(), collA.uuid);
    const RecordStore* recordStoreB = getRecordStoreForUuid(operationContext(), collB.uuid);

    EXPECT_EQ(recordStoreA->accurateDataSize(), 0);
    EXPECT_EQ(recordStoreA->accurateNumRecords(), 0);
    EXPECT_EQ(recordStoreB->accurateDataSize(), 0);
    EXPECT_EQ(recordStoreB->accurateNumRecords(), 0);

    manager->commit(operationContext(),
                    boost::container::flat_map<UUID, CollectionSizeCount>{
                        {collA.uuid, CollectionSizeCount{.size = 42, .count = 2}},
                        {collB.uuid, CollectionSizeCount{.size = 111, .count = 17}}},
                    boost::none);

    EXPECT_EQ(recordStoreA->accurateDataSize(), 42);
    EXPECT_EQ(recordStoreA->accurateNumRecords(), 2);
    EXPECT_EQ(recordStoreB->accurateDataSize(), 111);
    EXPECT_EQ(recordStoreB->accurateNumRecords(), 17);

    manager->commit(operationContext(),
                    boost::container::flat_map<UUID, CollectionSizeCount>{
                        {collA.uuid, CollectionSizeCount{.size = -10, .count = -3}},
                        {collB.uuid, CollectionSizeCount{.size = -11, .count = -4}}},
                    boost::none);

    EXPECT_EQ(recordStoreA->accurateDataSize(), 42 - 10);
    EXPECT_EQ(recordStoreA->accurateNumRecords(), 2 - 3);
    EXPECT_EQ(recordStoreB->accurateDataSize(), 111 - 11);
    EXPECT_EQ(recordStoreB->accurateNumRecords(), 17 - 4);
}

using ReplicatedFastCountManagerFindLatestTest = ReplicatedFastCountManagerTest;

TEST_F(ReplicatedFastCountManagerFindLatestTest, FindLatestCombinesStoredValuesWithOplogDeltas) {
    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       SizeCountStore::Entry(Timestamp::min(), 5, 1));
    test_helpers::insertSizeCountTimestamp(
        operationContext(), sizeCountTimestampStore, Timestamp::min());

    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));
    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(2, 2), collA, repl::OpTypeEnum::kUpdate, /*sizeDelta=*/100));
    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(3, 3), collA, repl::OpTypeEnum::kDelete, /*sizeDelta=*/-50));

    const CollectionSizeCount result = manager->findLatest(operationContext(), collA.uuid);
    EXPECT_EQ(result.size, 5 + 10 + 100 - 50);
    EXPECT_EQ(result.count, 1 + 1 - 1);
}

TEST_F(ReplicatedFastCountManagerFindLatestTest, FindLatestReturnsStoredValuesWhenNoOplogDeltas) {
    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       SizeCountStore::Entry(Timestamp::min(), 42, 7));
    test_helpers::insertSizeCountTimestamp(
        operationContext(), sizeCountTimestampStore, Timestamp::min());

    const CollectionSizeCount result = manager->findLatest(operationContext(), collA.uuid);
    EXPECT_EQ(result.size, 42);
    EXPECT_EQ(result.count, 7);
}

TEST_F(ReplicatedFastCountManagerFindLatestTest, FindLatestFiltersToRequestedUuid) {
    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       SizeCountStore::Entry(Timestamp::min(), 5, 1));
    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collB.uuid,
                                       SizeCountStore::Entry(Timestamp::min(), 100, 10));
    test_helpers::insertSizeCountTimestamp(
        operationContext(), sizeCountTimestampStore, Timestamp::min());

    // Write interleaved oplog entries for both collections.
    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));
    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(2, 2), collB, repl::OpTypeEnum::kInsert, /*sizeDelta=*/200));
    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(3, 3), collA, repl::OpTypeEnum::kDelete, /*sizeDelta=*/-3));

    const CollectionSizeCount resultA = manager->findLatest(operationContext(), collA.uuid);
    EXPECT_EQ(resultA.size, 5 + 10 - 3);
    EXPECT_EQ(resultA.count, 1 + 1 - 1);

    const CollectionSizeCount resultB = manager->findLatest(operationContext(), collB.uuid);
    EXPECT_EQ(resultB.size, 100 + 200);
    EXPECT_EQ(resultB.count, 10 + 1);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
