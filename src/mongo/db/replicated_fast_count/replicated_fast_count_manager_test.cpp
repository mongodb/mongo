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

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, InitializeMetadataNoData) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

    manager->initializeMetadata(operationContext());

    EXPECT_EQ(manager->find(UUID::gen()), CollectionSizeCount(0, 0));
}

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, NoStoreData) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

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

        const CollectionSizeCount result = CollectionSizeCount{.size = 10, .count = 1};
        EXPECT_EQ(manager->find(collA.uuid), result);
    }
    {

        const CollectionSizeCount result = CollectionSizeCount{.size = 100, .count = 1};
        EXPECT_EQ(manager->find(collB.uuid), result);
    }
}

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, NoOplogData) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

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

        const CollectionSizeCount result = CollectionSizeCount{.size = 5, .count = 1};
        EXPECT_EQ(manager->find(collA.uuid), result);
    }
    {

        const CollectionSizeCount result = CollectionSizeCount{.size = 6, .count = 2};
        EXPECT_EQ(manager->find(collB.uuid), result);
    }
}

TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, NoOplogAfterTimestamp) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

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

        const CollectionSizeCount result = CollectionSizeCount{.size = 5, .count = 1};
        EXPECT_EQ(manager->find(collA.uuid), result);
    }
    {

        const CollectionSizeCount result = CollectionSizeCount{.size = 6, .count = 2};
        EXPECT_EQ(manager->find(collB.uuid), result);
    }
}


TEST_F(ReplicatedFastCountManagerInitializeMetadataTest, StoreAndOplogData) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

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

        const CollectionSizeCount result = CollectionSizeCount{.size = 5 + 10, .count = 1 + 1};
        EXPECT_EQ(manager->find(collA.uuid), result);
    }
    {

        const CollectionSizeCount result = CollectionSizeCount{.size = 6 + 100, .count = 2 + 1};
        EXPECT_EQ(manager->find(collB.uuid), result);
    }
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
