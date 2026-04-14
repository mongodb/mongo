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

TEST_F(ReplicatedFastCountAdvanceCheckpointTest,
       CheckpointIsNoOpWhenOplogOnlyContainsTimestampStoreEntries) {
    const test_helpers::NsAndUUID timestampStoreNsAndUUID{
        .nss = NamespaceString::makeGlobalConfigCollection(
            NamespaceString::kReplicatedFastCountStoreTimestamps),
        .uuid = UUID::gen()};
    test_helpers::writeToOplog(opCtx,
                               test_helpers::makeOplogEntry(Timestamp(1, 1),
                                                            timestampStoreNsAndUUID,
                                                            repl::OpTypeEnum::kUpdate));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    EXPECT_FALSE(timestampStore.read(opCtx).has_value());
}

TEST_F(ReplicatedFastCountAdvanceCheckpointTest,
       CheckpointIsNoOpWhenOplogOnlyContainsFastCountStoreEntries) {
    const test_helpers::NsAndUUID fastCountStoreNsAndUUID{
        .nss =
            NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore),
        .uuid = UUID::gen()};
    test_helpers::writeToOplog(opCtx,
                               test_helpers::makeOplogEntry(Timestamp(1, 1),
                                                            fastCountStoreNsAndUUID,
                                                            repl::OpTypeEnum::kUpdate));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    EXPECT_FALSE(timestampStore.read(opCtx).has_value());
}

TEST_F(ReplicatedFastCountAdvanceCheckpointTest,
       ValidAsOfAdvancesToLastUserEntryIgnoresInternalEntries) {
    const Timestamp userWriteTs{1, 1};
    const Timestamp internalEntryTs{1, 2};

    test_helpers::writeToOplog(
        opCtx, test_helpers::makeOplogEntry(userWriteTs, collA, repl::OpTypeEnum::kInsert, 10));
    const test_helpers::NsAndUUID timestampStoreNsAndUUID{
        .nss = NamespaceString::makeGlobalConfigCollection(
            NamespaceString::kReplicatedFastCountStoreTimestamps),
        .uuid = UUID::gen()};
    test_helpers::writeToOplog(opCtx,
                               test_helpers::makeOplogEntry(internalEntryTs,
                                                            timestampStoreNsAndUUID,
                                                            repl::OpTypeEnum::kUpdate));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const auto tsAfterFirstAdvance = timestampStore.read(opCtx);
    ASSERT_TRUE(tsAfterFirstAdvance.has_value());
    ASSERT_EQ(userWriteTs, *tsAfterFirstAdvance);
}

TEST_F(ReplicatedFastCountAdvanceCheckpointTest,
       DoNotAdvanceTimestampForFastCountInternalApplyOps) {
    const Timestamp userWriteTs{1, 1};
    const Timestamp applyOpsTs{1, 2};

    test_helpers::writeToOplog(
        opCtx, test_helpers::makeOplogEntry(userWriteTs, collA, repl::OpTypeEnum::kInsert, 10));
    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const auto tsAfterFirstAdvance = timestampStore.read(opCtx);
    ASSERT_TRUE(tsAfterFirstAdvance.has_value());
    ASSERT_EQ(userWriteTs, *tsAfterFirstAdvance);

    const auto fastCountStoreNss =
        NamespaceString::makeGlobalConfigCollection(NamespaceString::kReplicatedFastCountStore);
    const auto fastCountTimestampNss = NamespaceString::makeGlobalConfigCollection(
        NamespaceString::kReplicatedFastCountStoreTimestamps);

    BSONArrayBuilder innerOpsBuilder;
    innerOpsBuilder.append(BSON("op" << "u"
                                     << "ns" << fastCountStoreNss.ns_forTest() << "ui"
                                     << UUID::gen() << "o" << BSON("$set" << BSON("count" << 1))
                                     << "o2" << BSON("_id" << 1)));
    innerOpsBuilder.append(
        BSON("op" << "u"
                  << "ns" << fastCountTimestampNss.ns_forTest() << "ui" << UUID::gen() << "o"
                  << BSON("$set" << BSON("ts" << userWriteTs)) << "o2" << BSON("_id" << 1)));

    const repl::OplogEntry applyOpsEntry = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(applyOpsTs, 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::createNamespaceString_forTest("admin", "$cmd"),
        .oField = BSON("applyOps" << innerOpsBuilder.arr()),
        .wallClockTime = Date_t::now(),
    }};
    test_helpers::writeToOplog(opCtx, applyOpsEntry);

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const auto tsAfterSecondAdvance = timestampStore.read(opCtx);
    ASSERT_TRUE(tsAfterSecondAdvance.has_value());
    ASSERT_EQ(userWriteTs, *tsAfterSecondAdvance);
}

TEST_F(ReplicatedFastCountAdvanceCheckpointTest, CollectionCreationAddsEntry) {
    const Timestamp ts1{1, 1};
    test_helpers::writeToOplog(opCtx, test_helpers::makeCreateOplogEntry(ts1, collA));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->size, 0);
    EXPECT_EQ(entry->count, 0);
    EXPECT_EQ(timestampStore.read(opCtx), ts1);
}

TEST_F(ReplicatedFastCountAdvanceCheckpointTest, CreateAndInsertSameCheckpoint) {
    test_helpers::writeToOplog(opCtx, test_helpers::makeCreateOplogEntry(Timestamp(1, 1), collA));
    test_helpers::writeToOplog(
        opCtx, test_helpers::makeOplogEntry(Timestamp(1, 2), collA, repl::OpTypeEnum::kInsert, 10));
    test_helpers::writeToOplog(
        opCtx, test_helpers::makeOplogEntry(Timestamp(1, 3), collA, repl::OpTypeEnum::kInsert, 20));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->size, 30);
    EXPECT_EQ(entry->count, 2);
}

TEST_F(ReplicatedFastCountAdvanceCheckpointTest, DropCollectionRemovesEntry) {
    {
        WriteUnitOfWork wuow(opCtx);
        sizeCountStore.write(
            opCtx, collA.uuid, {.timestamp = Timestamp(1, 1), .size = 100, .count = 5});
        timestampStore.write(opCtx, Timestamp(1, 1));
        wuow.commit();
    }

    const Timestamp ts2{1, 2};
    test_helpers::writeToOplog(opCtx, test_helpers::makeDropOplogEntry(ts2, collA));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    EXPECT_FALSE(sizeCountStore.read(opCtx, collA.uuid).has_value());
    EXPECT_EQ(timestampStore.read(opCtx), ts2);
}

TEST_F(ReplicatedFastCountAdvanceCheckpointTest, CreateAndDropSameCheckpoint) {
    const Timestamp ts2{1, 2};
    test_helpers::writeToOplog(opCtx, test_helpers::makeCreateOplogEntry(Timestamp(1, 1), collA));
    test_helpers::writeToOplog(opCtx, test_helpers::makeDropOplogEntry(ts2, collA));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    // Create and drop cancel each other out, so there is no entry in collA, but we should still
    // have advanced the timestamp store.
    EXPECT_FALSE(sizeCountStore.read(opCtx, collA.uuid).has_value());
    EXPECT_EQ(timestampStore.read(opCtx), ts2);
}

TEST_F(ReplicatedFastCountAdvanceCheckpointTest, CreateInApplyOpsUsesApplyOpsTimestamp) {
    const Timestamp ts1{1, 1};

    BSONArrayBuilder innerOpsBuilder;
    innerOpsBuilder.append(BSON("op" << "c"
                                     << "ns" << collA.nss.getCommandNS().ns_forTest() << "ui"
                                     << collA.uuid << "o" << BSON("create" << collA.nss.coll())));
    const repl::OplogEntry applyOpsEntry = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(ts1, 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::createNamespaceString_forTest("admin", "$cmd"),
        .oField = BSON("applyOps" << innerOpsBuilder.arr()),
        .wallClockTime = Date_t::now(),
    }};
    test_helpers::writeToOplog(opCtx, applyOpsEntry);

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->size, 0);
    EXPECT_EQ(entry->count, 0);
    EXPECT_EQ(entry->timestamp, ts1);
    EXPECT_EQ(timestampStore.read(opCtx), ts1);
}

TEST_F(ReplicatedFastCountAdvanceCheckpointTest, CreateAndInsertsInApplyOps) {
    const Timestamp ts1{1, 1};

    BSONArrayBuilder innerOpsBuilder;
    innerOpsBuilder.append(BSON("op" << "c"
                                     << "ns" << collA.nss.getCommandNS().ns_forTest() << "ui"
                                     << collA.uuid << "o" << BSON("create" << collA.nss.coll())));
    innerOpsBuilder.append(BSON("op" << "i"
                                     << "ns" << collA.nss.ns_forTest() << "ui" << collA.uuid << "o"
                                     << BSON("_id" << 1) << "m" << BSON("sz" << 10)));
    innerOpsBuilder.append(BSON("op" << "i"
                                     << "ns" << collA.nss.ns_forTest() << "ui" << collA.uuid << "o"
                                     << BSON("_id" << 2) << "m" << BSON("sz" << 20)));

    const repl::OplogEntry applyOpsEntry = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(ts1, 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::createNamespaceString_forTest("admin", "$cmd"),
        .oField = BSON("applyOps" << innerOpsBuilder.arr()),
        .wallClockTime = Date_t::now(),
    }};
    test_helpers::writeToOplog(opCtx, applyOpsEntry);

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->size, 30);
    EXPECT_EQ(entry->count, 2);
    EXPECT_EQ(entry->timestamp, ts1);
    EXPECT_EQ(timestampStore.read(opCtx), ts1);
}

// Test: A truncateRange oplog entry correctly applies negative size/count deltas.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, TruncateRangeAppliesNegativeDelta) {
    // Setup collection size count tracking: `collA` with 5 documents and 200 bytes.
    {
        WriteUnitOfWork wuow(opCtx);
        sizeCountStore.write(
            opCtx, collA.uuid, {.timestamp = Timestamp(1, 1), .size = 200, .count = 5});
        timestampStore.write(opCtx, Timestamp(1, 1));
        wuow.commit();
    }

    // Simulate a truncateRange that removes 3 documents worth 120 bytes.
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeTruncateRangeOplogEntry(
            Timestamp(1, 2), collA, /*bytesDeleted=*/120, /*docsDeleted=*/3));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const SizeCountStore::Entry expectedEntry{
        .timestamp = Timestamp(1, 2), .size = 200 - 120, .count = 5 - 3};
    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);

    const auto tsStoreRes = timestampStore.read(opCtx);
    ASSERT_TRUE(tsStoreRes.has_value());
    ASSERT_EQ(Timestamp(1, 2), *tsStoreRes);
}

// Test: Inserts followed by a truncateRange for the same collection accumulate correctly.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, InsertThenTruncateRangeAccumulates) {
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, 50 /*sizeDelta=*/));
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 2), collA, repl::OpTypeEnum::kInsert, 50 /*sizeDelta=*/));
    test_helpers::writeToOplog(opCtx,
                               test_helpers::makeTruncateRangeOplogEntry(
                                   Timestamp(1, 3), collA, /*bytesDeleted=*/30, /*docsDeleted=*/1));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    // Net: size = 50+50-30 = 70, count = 1+1-1 = 1.
    const SizeCountStore::Entry expectedEntry{.timestamp = Timestamp(1, 3), .size = 70, .count = 1};
    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// Test: A truncateRange for collB does not affect collA's tracked size/count.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, TruncateRangeOnlyAffectsTargetCollection) {
    {
        WriteUnitOfWork wuow(opCtx);
        sizeCountStore.write(
            opCtx, collA.uuid, {.timestamp = Timestamp(1, 1), .size = 100, .count = 4});
        timestampStore.write(opCtx, Timestamp(1, 1));
        wuow.commit();
    }

    test_helpers::writeToOplog(opCtx,
                               test_helpers::makeTruncateRangeOplogEntry(
                                   Timestamp(1, 2), collB, /*bytesDeleted=*/80, /*docsDeleted=*/2));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    // collA's entry is unchanged.
    const SizeCountStore::Entry expectedCollAEntry{
        .timestamp = Timestamp(1, 1), .size = 100, .count = 4};
    const auto collAEntry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(collAEntry.has_value());
    EXPECT_EQ(expectedCollAEntry, *collAEntry);

    // collB gets a new entry from the truncateRange.
    const SizeCountStore::Entry expectedCollBEntry{
        .timestamp = Timestamp(1, 2), .size = -80, .count = -2};
    const auto collBEntry = sizeCountStore.read(opCtx, collB.uuid);
    ASSERT_TRUE(collBEntry.has_value());
    EXPECT_EQ(expectedCollBEntry, *collBEntry);
}

// Test: A mix of inserts, updates, deletes, and a truncateRange for the same collection all
// accumulate into the correct net size and count delta.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, MixedOpsWithTruncateRangeAccumulates) {
    // 3 inserts: +150 bytes, +3 docs
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, 40 /*sizeDelta=*/));
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 2), collA, repl::OpTypeEnum::kInsert, 60 /*sizeDelta=*/));
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 3), collA, repl::OpTypeEnum::kInsert, 50 /*sizeDelta=*/));
    // 1 update: -10 bytes (doc shrank), 0 docs
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 4), collA, repl::OpTypeEnum::kUpdate, -10 /*sizeDelta=*/));
    // 1 delete: -40 bytes, -1 doc
    test_helpers::writeToOplog(
        opCtx,
        test_helpers::makeOplogEntry(
            Timestamp(1, 5), collA, repl::OpTypeEnum::kDelete, -40 /*sizeDelta=*/));
    // 1 truncateRange: -60 bytes, -2 docs
    test_helpers::writeToOplog(opCtx,
                               test_helpers::makeTruncateRangeOplogEntry(
                                   Timestamp(1, 6), collA, /*bytesDeleted=*/60, /*docsDeleted=*/2));

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    // Net: size = 40+60+50-10-40-60 = 40, count = 1+1+1+0-1-2 = 0
    const SizeCountStore::Entry expectedEntry{.timestamp = Timestamp(1, 6), .size = 40, .count = 0};
    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// Test: A truncateRange inside an applyOps entry applies the correct negative delta.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, TruncateRangeInsideApplyOps) {
    const Timestamp ts1{1, 2};
    const int64_t bytesDeleted = 90;
    const int64_t docsDeleted = 2;

    const auto truncateEntry =
        test_helpers::makeTruncateRangeOplogEntry(ts1, collA, bytesDeleted, docsDeleted);
    const NamespaceString adminCmdNss =
        NamespaceString::createNamespaceString_forTest("admin", "$cmd");
    BSONObj truncateInnerOp = BSON("op" << "c"
                                        << "ns" << collA.nss.getCommandNS().ns_forTest() << "ui"
                                        << collA.uuid << "o" << truncateEntry.getObject());

    test_helpers::writeToOplog(
        opCtx,
        repl::OplogEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts1, 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = BSON("applyOps" << BSON_ARRAY(truncateInnerOp)),
            .wallClockTime = Date_t::now(),
        }}});

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const SizeCountStore::Entry expectedEntry{
        .timestamp = ts1, .size = -bytesDeleted, .count = -docsDeleted};
    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

// Test: A truncateRange nested inside a nested applyOps applies the correct negative delta.
TEST_F(ReplicatedFastCountAdvanceCheckpointTest, TruncateRangeInsideNestedApplyOps) {
    const Timestamp ts1{1, 2};
    const int64_t bytesDeleted = 60;
    const int64_t docsDeleted = 1;

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

    test_helpers::writeToOplog(
        opCtx,
        repl::OplogEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts1, 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = BSON("applyOps" << BSON_ARRAY(innerApplyOpsOp)),
            .wallClockTime = Date_t::now(),
        }}});

    advanceCheckpoint(opCtx, sizeCountStore, timestampStore);

    const SizeCountStore::Entry expectedEntry{
        .timestamp = ts1, .size = -bytesDeleted, .count = -docsDeleted};
    const auto entry = sizeCountStore.read(opCtx, collA.uuid);
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(expectedEntry, *entry);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
