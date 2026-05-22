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

#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/write_unit_of_work.h"
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
    RAIIServerParameterControllerForTest featureFlagDurability(
        "featureFlagReplicatedFastCountDurability", true);

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

    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        collA.uuid,
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 5, .count = 1});
    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        collB.uuid,
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 6, .count = 2});

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

    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        collA.uuid,
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 5, .count = 1});
    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        collB.uuid,
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 6, .count = 2});

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
    RAIIServerParameterControllerForTest featureFlagDurability(
        "featureFlagReplicatedFastCountDurability", true);

    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collA.nss, CollectionOptions{.uuid = collA.uuid}));
    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collB.nss, CollectionOptions{.uuid = collB.uuid}));

    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        collA.uuid,
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 5, .count = 1});
    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        collB.uuid,
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 6, .count = 2});

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
    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        UUID::gen(),
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 999, .count = 99});

    // Initialization should skip the size/count entry without failing.
    manager->initializeMetadata(operationContext());
}

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, InitializeMetadataTracksOplogSizeCount) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    RAIIServerParameterControllerForTest featureFlagDurability(
        "featureFlagReplicatedFastCountDurability", true);

    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collA.nss, CollectionOptions{.uuid = collA.uuid}));

    const auto catalog = CollectionCatalog::latest(operationContext()->getServiceContext());
    const Collection* oplogColl = catalog->lookupCollectionByNamespace(
        operationContext(), NamespaceString::kRsOplogNamespace);
    ASSERT(oplogColl);
    const UUID oplogUuid = oplogColl->uuid();

    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        oplogUuid,
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 500, .count = 50});

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
    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        collA.uuid,
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 5, .count = 1});
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
    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        collA.uuid,
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 42, .count = 7});
    test_helpers::insertSizeCountTimestamp(
        operationContext(), sizeCountTimestampStore, Timestamp::min());

    const CollectionSizeCount result = manager->findLatest(operationContext(), collA.uuid);
    EXPECT_EQ(result.size, 42);
    EXPECT_EQ(result.count, 7);
}

TEST_F(ReplicatedFastCountManagerFindLatestTest, FindLatestFiltersToRequestedUuid) {
    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        collA.uuid,
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 5, .count = 1});
    test_helpers::insertSizeCountEntry(
        operationContext(),
        sizeCountStore,
        collB.uuid,
        SizeCountStore::Entry{.timestamp = Timestamp::min(), .size = 100, .count = 10});
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

/**
 * Cold-boot fixture that bypasses `setUpReplicatedFastCount()` so tests can exercise
 * `initializeMetadata` against on-disk state that the manager has not yet been told about.
 * Use this for tests that validate the manager's ability to discover existing fast count
 * containers directly from the storage engine at startup.
 */
class ReplicatedFastCountManagerColdBootTest : public CatalogTestFixture {
public:
    ReplicatedFastCountManagerColdBootTest()
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
        _fastCountManager->disablePeriodicWrites_ForTest();
        // Intentionally do NOT call setUpReplicatedFastCount() — the test exercises the
        // cold-boot path where on-disk containers exist before the manager is configured.

        // Create two collections so their UUIDs exist in the CollectionCatalog
        ASSERT_OK(storageInterface()->createCollection(
            _opCtx, _coll1.nss, CollectionOptions{.uuid = _coll1.uuid}));
        ASSERT_OK(storageInterface()->createCollection(
            _opCtx, _coll2.nss, CollectionOptions{.uuid = _coll2.uuid}));
    }

    OperationContext* _opCtx;
    ReplicatedFastCountManager* _fastCountManager;

    test_helpers::NsAndUUID _coll1 = {
        .nss = NamespaceString::createNamespaceString_forTest("coldboot_test", "coll1"),
        .uuid = UUID::gen()};
    test_helpers::NsAndUUID _coll2 = {
        .nss = NamespaceString::createNamespaceString_forTest("coldboot_test", "coll2"),
        .uuid = UUID::gen()};
};

TEST_F(ReplicatedFastCountManagerColdBootTest,
       InitializePopulatesMetadataFromExistingInternalCollection) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    // Pre-populate the internal replicated fast count collection with two entries.
    const int64_t expectedCount1 = 5;
    const int64_t expectedSize1 = 100;

    const int64_t expectedCount2 = 10;
    const int64_t expectedSize2 = 250;

    {
        ASSERT_OK(repl::StorageInterface::get(_opCtx->getServiceContext())
                      ->createCollection(
                          _opCtx,
                          NamespaceString::makeGlobalConfigCollection(
                              NamespaceString::kReplicatedFastCountStore),
                          CollectionOptions{.clusteredIndex =
                                                clustered_util::makeDefaultClusteredIdIndex()}));
        ASSERT_OK(repl::StorageInterface::get(_opCtx->getServiceContext())
                      ->createCollection(
                          _opCtx,
                          NamespaceString::makeGlobalConfigCollection(
                              NamespaceString::kReplicatedFastCountStoreTimestamps),
                          CollectionOptions{.clusteredIndex =
                                                clustered_util::makeDefaultClusteredIdIndex()}));

        AutoGetCollection fastCountColl(
            _opCtx,
            NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
            LockMode::MODE_IX);
        ASSERT(fastCountColl);

        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};

        ASSERT_OK(Helpers::insert(
            _opCtx,
            *fastCountColl,
            BSON("_id" << _coll1.uuid << kValidAsOfKey << Timestamp(1, 1) << kMetadataKey
                       << BSON(kCountKey << expectedCount1 << kSizeKey << expectedSize1))));

        ASSERT_OK(Helpers::insert(
            _opCtx,
            *fastCountColl,
            BSON("_id" << _coll2.uuid << kValidAsOfKey << Timestamp(1, 1) << kMetadataKey
                       << BSON(kCountKey << expectedCount2 << kSizeKey << expectedSize2))));

        wuow.commit();
    }

    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalStore(
        _opCtx,
        _fastCountManager,
        _coll1.uuid,
        /*expectPersisted=*/true,
        expectedCount1,
        expectedSize1);
    replicated_fast_count_test_helpers::checkFastCountMetadataInInternalStore(
        _opCtx,
        _fastCountManager,
        _coll2.uuid,
        /*expectPersisted=*/true,
        expectedCount2,
        expectedSize2);

    const auto* rs1 = getRecordStoreForUuid(_opCtx, _coll1.uuid);
    const auto* rs2 = getRecordStoreForUuid(_opCtx, _coll2.uuid);

    EXPECT_EQ(rs1->accurateNumRecords(), 0);
    EXPECT_EQ(rs1->accurateDataSize(), 0);
    EXPECT_EQ(rs2->accurateNumRecords(), 0);
    EXPECT_EQ(rs2->accurateDataSize(), 0);

    _fastCountManager->initializeMetadata(_opCtx);

    // The in-memory RecordStore should reflect the persisted values.
    EXPECT_EQ(rs1->accurateNumRecords(), expectedCount1);
    EXPECT_EQ(rs1->accurateDataSize(), expectedSize1);
    EXPECT_EQ(rs2->accurateNumRecords(), expectedCount2);
    EXPECT_EQ(rs2->accurateDataSize(), expectedSize2);
}

TEST_F(ReplicatedFastCountManagerColdBootTest,
       InitializePopulatesMetadataFromExistingInternalContainer) {
    RAIIServerParameterControllerForTest ffFastCount("featureFlagReplicatedFastCount", true);
    RAIIServerParameterControllerForTest ffDurability("featureFlagReplicatedFastCountDurability",
                                                      true);
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);

    ASSERT_OK(createInternalFastCountContainers(_opCtx,
                                                NamespaceString::kAdminCommandNamespace,
                                                ident::kFastCountMetadataStore,
                                                KeyFormat::String,
                                                ident::kFastCountMetadataStoreTimestamps,
                                                KeyFormat::Long,
                                                /*writeToOplog=*/true));

    auto* engine = _opCtx->getServiceContext()->getStorageEngine()->getEngine();
    auto metadataRS = engine->getRecordStore(_opCtx,
                                             NamespaceString::kAdminCommandNamespace,
                                             ident::kFastCountMetadataStore,
                                             RecordStore::Options{.keyFormat = KeyFormat::String},
                                             /*uuid=*/boost::none);
    auto timestampsRS = engine->getRecordStore(_opCtx,
                                               NamespaceString::kAdminCommandNamespace,
                                               ident::kFastCountMetadataStoreTimestamps,
                                               RecordStore::Options{.keyFormat = KeyFormat::Long},
                                               /*uuid=*/boost::none);

    const int64_t expectedCount1 = 5;
    const int64_t expectedSize1 = 100;

    const int64_t expectedCount2 = 10;
    const int64_t expectedSize2 = 250;

    const BSONObj entry1Bson = test_helpers::makeEntryBson(expectedCount1, expectedSize1);
    const BSONObj entry2Bson = test_helpers::makeEntryBson(expectedCount2, expectedSize2);

    {
        auto containerVariant = metadataRS->getContainer();
        auto& container =
            std::get<std::reference_wrapper<StringKeyedContainer>>(containerVariant).get();
        auto& ru = *shard_role_details::getRecoveryUnit(_opCtx);

        WriteUnitOfWork wuow(_opCtx);
        ASSERT_OK(container.insert(ru,
                                   test_helpers::uuidSpan(_coll1.uuid),
                                   test_helpers::bsonSpan(entry1Bson),
                                   container::ExistingKeyPolicy::reject));
        ASSERT_OK(container.insert(ru,
                                   test_helpers::uuidSpan(_coll2.uuid),
                                   test_helpers::bsonSpan(entry2Bson),
                                   container::ExistingKeyPolicy::reject));
        wuow.commit();
    }

    auto checkFastCountMetadataInContainer =
        [&](const UUID& uuid, int64_t expectedCount, int64_t expectedSize) {
            BSONObj persisted;
            const bool found = replicated_fast_count_test_helpers::findPersistedDocInContainer(
                _opCtx, uuid, persisted);
            ASSERT_TRUE(found);

            const int64_t persistedCount = persisted.getField(replicated_fast_count::kMetadataKey)
                                               .Obj()
                                               .getField(replicated_fast_count::kCountKey)
                                               .Long();
            const int64_t persistedSize = persisted.getField(replicated_fast_count::kMetadataKey)
                                              .Obj()
                                              .getField(replicated_fast_count::kSizeKey)
                                              .Long();
            EXPECT_EQ(persistedCount, expectedCount);
            EXPECT_EQ(persistedSize, expectedSize);

            ASSERT_TRUE(persisted.hasField(replicated_fast_count::kValidAsOfKey));
        };

    checkFastCountMetadataInContainer(_coll1.uuid, expectedCount1, expectedSize1);
    checkFastCountMetadataInContainer(_coll2.uuid, expectedCount2, expectedSize2);

    const auto* rs1 = getRecordStoreForUuid(_opCtx, _coll1.uuid);
    const auto* rs2 = getRecordStoreForUuid(_opCtx, _coll2.uuid);

    EXPECT_EQ(rs1->accurateNumRecords(), 0);
    EXPECT_EQ(rs1->accurateDataSize(), 0);
    EXPECT_EQ(rs2->accurateNumRecords(), 0);
    EXPECT_EQ(rs2->accurateDataSize(), 0);

    _fastCountManager->initializeMetadata(_opCtx);

    // The in-memory RecordStore should reflect the persisted values.
    EXPECT_EQ(rs1->accurateNumRecords(), expectedCount1);
    EXPECT_EQ(rs1->accurateDataSize(), expectedSize1);
    EXPECT_EQ(rs2->accurateNumRecords(), expectedCount2);
    EXPECT_EQ(rs2->accurateDataSize(), expectedSize2);
}

// Regression test for SERVER-127435: initializeMetadata() in container mode used to always scan
// the oplog from Timestamp::min() because _timestampStore->read() returned boost::none
// (the collection-backed implementation was still in place before initializeContainerStores()).
TEST_F(ReplicatedFastCountManagerInitializeMetadataTest,
       ContainerModeReadsTimestampFromIdentToFilterOplogScan) {
    RAIIServerParameterControllerForTest ffFastCount("featureFlagReplicatedFastCount", true);
    RAIIServerParameterControllerForTest ffDurability("featureFlagReplicatedFastCountDurability",
                                                      true);
    RAIIServerParameterControllerForTest ffContainerWrites("featureFlagContainerWrites", true);

    ASSERT_OK(storageInterface()->createCollection(
        operationContext(), collA.nss, CollectionOptions{.uuid = collA.uuid}));

    ASSERT_OK(createInternalFastCountContainers(operationContext(),
                                                NamespaceString::kAdminCommandNamespace,
                                                ident::kFastCountMetadataStore,
                                                KeyFormat::String,
                                                ident::kFastCountMetadataStoreTimestamps,
                                                KeyFormat::Long,
                                                /*writeToOplog=*/false));

    auto* engine = operationContext()->getServiceContext()->getStorageEngine()->getEngine();

    // Persist metadata: collA has 5 records, 100 bytes.
    {
        auto metadataRS =
            engine->getRecordStore(operationContext(),
                                   NamespaceString::kAdminCommandNamespace,
                                   ident::kFastCountMetadataStore,
                                   RecordStore::Options{.keyFormat = KeyFormat::String},
                                   /*uuid=*/boost::none);
        auto containerVariant = metadataRS->getContainer();
        auto& container =
            std::get<std::reference_wrapper<StringKeyedContainer>>(containerVariant).get();
        auto& ru = *shard_role_details::getRecoveryUnit(operationContext());

        const BSONObj entryBson = test_helpers::makeEntryBson(/*count=*/5, /*size=*/100);
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(container.insert(ru,
                                   test_helpers::uuidSpan(collA.uuid),
                                   test_helpers::bsonSpan(entryBson),
                                   container::ExistingKeyPolicy::reject));
        wuow.commit();
    }

    // Persist checkpoint timestamp Timestamp(3, 3) directly to the container
    // (bypass container_write to avoid the oplog write path).
    const Timestamp checkpointTs(3, 3);
    {
        auto timestampsRS =
            engine->getRecordStore(operationContext(),
                                   NamespaceString::kAdminCommandNamespace,
                                   ident::kFastCountMetadataStoreTimestamps,
                                   RecordStore::Options{.keyFormat = KeyFormat::Long},
                                   /*uuid=*/boost::none);
        auto containerVariant = timestampsRS->getContainer();
        auto& container =
            std::get<std::reference_wrapper<IntegerKeyedContainer>>(containerVariant).get();
        auto& ru = *shard_role_details::getRecoveryUnit(operationContext());
        const BSONObj tsVal = BSON(kValidAsOfKey << checkpointTs);
        const std::span<const char> valSpan{tsVal.objdata(), static_cast<size_t>(tsVal.objsize())};
        WriteUnitOfWork wuow(operationContext());
        ASSERT_OK(container.insert(ru, int64_t{0}, valSpan, container::ExistingKeyPolicy::reject));
        wuow.commit();
    }

    // Write oplog entries: two at or before the checkpoint, one after. No OpObserverImpl is
    // registered in this fixture so these are the only oplog entries — no conflicting creates.
    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/10));
    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(3, 3), collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/20));
    test_helpers::writeToOplog(
        operationContext(),
        test_helpers::makeOplogEntry(
            Timestamp(4, 4), collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/30));

    manager->initializeMetadata(operationContext());

    // Only the entry at Timestamp(4, 4) should be accumulated — entries at or before
    // checkpointTs are already captured in the persisted metadata.
    const RecordStore* rs = getRecordStoreForUuid(operationContext(), collA.uuid);
    EXPECT_EQ(rs->accurateNumRecords(), 5 + 1);
    EXPECT_EQ(rs->accurateDataSize(), 100 + 30);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
