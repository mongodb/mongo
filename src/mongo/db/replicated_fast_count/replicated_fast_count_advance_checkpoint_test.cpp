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

#include "mongo/db/replicated_fast_count/replicated_fast_count_advance_checkpoint.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/uuid.h"

namespace mongo::replicated_fast_count {
namespace {
void createReplicatedFastCountCollection(repl::StorageInterface* storageInterface,
                                         OperationContext* opCtx) {
    ASSERT_OK(storageInterface->createCollection(
        opCtx,
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
        CollectionOptions{.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex()}));
}

void createTimestampCollection(repl::StorageInterface* storageInterface, OperationContext* opCtx) {
    ASSERT_OK(storageInterface->createCollection(
        opCtx,
        NamespaceString::makeGlobalConfigCollection(
            NamespaceString::kReplicatedFastCountStoreTimestamps),
        CollectionOptions{.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex()}));
}

class ReplicatedFastCountAdvanceCheckpointTest : public CatalogTestFixture {
protected:
    static constexpr StringData kDbName = "advance_checkpoint_test"_sd;
    const test_helpers::NsAndUUID collA{
        .nss = NamespaceString::createNamespaceString_forTest(kDbName, "collA"),
        .uuid = UUID::gen()};
    const test_helpers::NsAndUUID collB{
        .nss = NamespaceString::createNamespaceString_forTest(kDbName, "collB"),
        .uuid = UUID::gen()};

    void setUp() override {
        CatalogTestFixture::setUp();
        opCtx = operationContext();
        createTimestampCollection(storageInterface(), opCtx);
        createReplicatedFastCountCollection(storageInterface(), opCtx);
    }

    OperationContext* opCtx;
    SizeCountStore sizeCountStore;
    SizeCountTimestampStore timestampStore;
};

// Test: `advanceCheckpoint` when there was no pre-existing entry for a user collection.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, InitialCheckpoint) {
    const Timestamp ts1{1, 1};
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const SizeCountStore::Entry expectedEntry{.timestamp = ts1, .size = 10, .count = 1};
    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);

    const auto tsStoreRes = timestampStore.read(opCtx);
    ASSERT_TRUE(tsStoreRes.has_value());
    ASSERT_EQ(ts1, *tsStoreRes);
}

// Test: `advanceCheckpoint` is a no-op for the `SizeCountStore` when both stores are empty and
// there are no new oplog entries.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, NothingTrackedNothingAdvanced) {
    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    EXPECT_FALSE(timestampStore.read(opCtx).has_value());

    const auto acquisition = acquireFastCountCollectionForRead(opCtx);
    ASSERT_TRUE(acquisition.has_value());
    auto cursor = acquisition->getCollectionPtr()->getRecordStore()->getCursor(
        opCtx, *shard_role_details::getRecoveryUnit(opCtx));
    EXPECT_FALSE(cursor->next());
}

// Test: `advanceCheckpoint` correctly updates the size count and timestamp entries for an
// already-tracked user collection.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, AdvancesExistingSizeCountAndTimestamp) {
    // Setup collection size count tracking: `collA` with 3 documents and a total of 100 bytes.
    {
        WriteUnitOfWork wuow(opCtx);
        sizeCountStore.write(
            opCtx, collA.uuid, {.timestamp = Timestamp(1, 1), .size = 100, .count = 3});
        timestampStore.write(opCtx, Timestamp(1, 1));
        wuow.commit();
    }

    // Simulate oplog for 2 inserts (60 bytes) into `collA`.
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 2), collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/));
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 3), collA, repl::OpTypeEnum::kInsert, 50 /*sizeDelta=*/));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 3), .size = 100 + 60, .count = 3 + 2};
    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);

    const auto tsStoreRes = timestampStore.read(opCtx);
    ASSERT_TRUE(tsStoreRes.has_value());
    ASSERT_EQ(Timestamp(1, 3), *tsStoreRes);
}

// Test: If no oplog entries contained replicated size count information since the last
// `advanceCheckpoint`, the `SizeCountStore` entries remain unchanged but the
// `SizeCountTimetampStore` is updated to reflect entries across the stores are valid as of the most
// recent oplog entry scanned for the checkpoint.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, NoReplicatedSizeCountInOplogForWrite) {
    // `collA` size and count starts out being tracked.
    {
        WriteUnitOfWork wuow(opCtx);
        sizeCountStore.write(
            opCtx, collA.uuid, {.timestamp = Timestamp(1, 1), .size = 100, .count = 3});
        timestampStore.write(opCtx, Timestamp(1, 1));
        wuow.commit();
    }

    // No size count information is included in the no-op oplog entry for `collA`.
    test_helpers::writeToOplog(
        opCtx, test_helpers::makeOplogEntry(Timestamp(50, 1), collA, repl::OpTypeEnum::kNoop));
    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    // Without replicated size count information tracked by the oplog entries, the SizeCountStore
    // entry for `collA` remains unchanged.
    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 1), .size = 100, .count = 3};
    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);

    // The `SizeCountTimetampStore` is updated - and reflects that all entries in the
    // `SizeCountStore` are valid up to atleast its timestamp. Updating the timestamp store, rather
    // than each entry in the `SizeCountStore`, reduces the amount of writes necessary for advancing
    // the checkpoint.
    const auto tsStoreRes = timestampStore.read(opCtx);
    ASSERT_TRUE(tsStoreRes.has_value());
    EXPECT_EQ(Timestamp(50, 1), *tsStoreRes);
}

// Test: The size count updates to `collA` sum to 0, but the timestamp store is updated to reflect
// the newest timestamp for which the size count totals are valid.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, TimestampUpdatedForSum0SizeCountChanges) {
    // Setup collection size count tracking: `collA` with 3 documents and a total of 100 bytes.
    {
        WriteUnitOfWork wuow(opCtx);
        sizeCountStore.write(
            opCtx, collA.uuid, {.timestamp = Timestamp(1, 1), .size = 100, .count = 3});
        timestampStore.write(opCtx, Timestamp(1, 1));
        wuow.commit();
    }

    // Simulate one document inserted then deleted from `collA`.
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 2), collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/));
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 3), collA, repl::OpTypeEnum::kDelete, -10 /*sizeDelta=*/));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    // Net 0 change from the original size and count for `collA`.
    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 3), .size = 100, .count = 3};
    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);

    const auto tsStoreRes = timestampStore.read(opCtx);
    ASSERT_TRUE(tsStoreRes.has_value());
    ASSERT_EQ(Timestamp(1, 3), *tsStoreRes);
}

// Test: `advanceCheckpoint` correctly tracks size and count for multiple user collections
// simultaneously.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, TrackTwoUserCollections) {
    // Simulate 1 document inserted into `collA` and `collB`.
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 2), collA, repl::OpTypeEnum::kInsert, 50 /*sizeDelta=*/));
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 3), collB, repl::OpTypeEnum::kInsert, 40 /*sizeDelta=*/));

    // The larger of the timestamps between the oplog insert entries.
    const Timestamp largestValidAsOf = Timestamp(1, 3);
    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const SizeCountStore::Entry expectedSizeCountCollA{
        .timestamp = largestValidAsOf, .size = 50, .count = 1};
    const auto entryCollA = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entryCollA.has_value());
    EXPECT_EQ(expectedSizeCountCollA, *entryCollA);

    const SizeCountStore::Entry expectedSizeCountCollB{
        .timestamp = largestValidAsOf, .size = 40, .count = 1};
    const auto entryCollB = sizeCountStore.read(opCtx, collB.uuid);
    ASSERT_TRUE(entryCollB.has_value());
    EXPECT_EQ(expectedSizeCountCollB, *entryCollB);

    const auto tsStoreRes = timestampStore.read(opCtx);
    ASSERT_TRUE(tsStoreRes.has_value());
    ASSERT_EQ(largestValidAsOf, *tsStoreRes);
}

TEST_F(ReplicatedFastCountAdvanceCheckpointTest,
       CheckpointAdvancementDoesntUpdateUserCollectionWithNoNewUpdates) {
    // Setup collection size count tracking: `collA` with 3 documents and a total of 100 bytes.
    {
        WriteUnitOfWork wuow(opCtx);
        sizeCountStore.write(
            opCtx, collA.uuid, {.timestamp = Timestamp(1, 1), .size = 100, .count = 3});
        timestampStore.write(opCtx, Timestamp(1, 1));
        wuow.commit();
    }

    // Since the last checkpoint, Timestamp(1,1), only write oplog entries for `collB`.
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 3), collB, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    // The `collA` entry should be unchanged - since there were no updates to its size count in the
    // last checkpoint.
    const SizeCountStore::Entry expectedCollAEntry{
        .timestamp = Timestamp(1, 1), .size = 100, .count = 3};
    const auto collAEntry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(collAEntry.has_value());
    EXPECT_EQ(expectedCollAEntry, *collAEntry);

    // The checkpoint created a size count store entry for `collB`.
    const SizeCountStore::Entry expectedCollBEntry{
        .timestamp = Timestamp(1, 3), .size = 10, .count = 1};
    const auto collBEntry = sizeCountStore.read(opCtx, collB.uuid);
    ASSERT_TRUE(collBEntry.has_value());
    EXPECT_EQ(expectedCollBEntry, *collBEntry);

    // The timestamp store represents the global valid-as-of, which should be updated from the
    // checkpoint advancement.
    const auto tsStoreRes = timestampStore.read(opCtx);
    ASSERT_TRUE(tsStoreRes.has_value());
    ASSERT_EQ(Timestamp(1, 3), *tsStoreRes);
}


}  // namespace
}  // namespace mongo::replicated_fast_count
