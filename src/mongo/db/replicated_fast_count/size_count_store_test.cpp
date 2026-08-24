// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_store.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/unittest/death_test.h"

#include <limits>

namespace mongo::replicated_fast_count {
namespace {

using test_helpers::insertSizeCountEntry;

int64_t makeTestHash() {
    return 0x0102030405060708;
}

// Builds the `meta` subdocument of the on-disk shape, optionally carrying an `h` value.
BSONObj makeMeta(boost::optional<int64_t> hash) {
    BSONObjBuilder meta;
    meta.append(kCountKey, int64_t{7});
    meta.append(kSizeKey, int64_t{42});
    if (hash) {
        meta.append(kHashKey, *hash);
    }
    return meta.obj();
}

// Builds a persisted metadata document of the on-disk shape, optionally carrying an `h` value.
BSONObj makeMetadataDoc(boost::optional<int64_t> hash) {
    return BSON(kValidAsOfKey << Timestamp(10, 1) << kMetadataKey << makeMeta(hash));
}

// Builds a persisted metadata document whose `h` field carries `hashElem`'s value and type.
BSONObj makeMetadataDocWithRawHash(const BSONElement& hashElem) {
    BSONObjBuilder meta;
    meta.append(kCountKey, int64_t{7});
    meta.append(kSizeKey, int64_t{42});
    meta.appendAs(hashElem, kHashKey);
    return BSON(kValidAsOfKey << Timestamp(10, 1) << kMetadataKey << meta.obj());
}

SizeCountStore::Entry parseDoc(const BSONObj& doc) {
    return SizeCountStore::parseContainerValue(
        {doc.objdata(), static_cast<std::size_t>(doc.objsize())});
}

class SizeCountStoreTest : public CatalogTestFixture {
public:
    SizeCountStoreTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>())) {}

protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        store = test_helpers::createContainerFastCountStores(operationContext()).sizeCountStore;
    }

    std::unique_ptr<ContainerSizeCountStore> store;
};

TEST_F(SizeCountStoreTest, ReadReturnsNoneWhenEmpty) {
    Lock::GlobalLock readLock(operationContext(), MODE_IS);
    EXPECT_FALSE(store->read(operationContext(), UUID::gen()).has_value());
}

TEST_F(SizeCountStoreTest, ReadWriteRoundTripNewEntry) {
    // Hold MODE_IX (not MODE_IS) for the whole test: the store reads require the global lock and
    // insertSizeCountEntry reacquires it in MODE_IX, which cannot be upgraded from MODE_IS.
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    {
        const UUID uuid = UUID::gen();
        const SizeCountStore::Entry entry{
            .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = boost::none};

        insertSizeCountEntry(operationContext(), *store, uuid, entry);

        const auto result = store->read(operationContext(), uuid);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(entry, *result);
    }
    {
        const UUID uuid = UUID::gen();
        const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1),
                                          .size = 42,
                                          .count = 7,
                                          .hash = static_cast<int64_t>(0xDEADBEEFDEADBEEF)};

        insertSizeCountEntry(operationContext(), *store, uuid, entry);

        const auto result = store->read(operationContext(), uuid);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(entry, *result);
    }
}

TEST_F(SizeCountStoreTest, WriteUpdateExistingEntry) {
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry initialEntry{
        .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = boost::none};
    insertSizeCountEntry(operationContext(), *store, uuid, initialEntry);

    const SizeCountStore::Entry updatedEntry{.timestamp = initialEntry.timestamp + 1,
                                             .size = initialEntry.size - 2,
                                             .count = initialEntry.count - 1,
                                             .hash = boost::none};
    EXPECT_NE(initialEntry, updatedEntry);

    insertSizeCountEntry(operationContext(), *store, uuid, updatedEntry);
    const auto result = store->read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(updatedEntry, *result);
}

TEST_F(SizeCountStoreTest, WriteUpdateExistingEntryNoPersistentHash) {
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry initialEntry{
        .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = boost::none};
    insertSizeCountEntry(operationContext(), *store, uuid, initialEntry);

    const SizeCountStore::Entry updatedEntry{.timestamp = Timestamp(11, 1),
                                             .size = 40,
                                             .count = 6,
                                             .hash = static_cast<int64_t>(0xDEADBEEFDEADBEEF)};

    insertSizeCountEntry(operationContext(), *store, uuid, updatedEntry);
    const boost::optional<SizeCountStore::Entry> result = store->read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(
        *result,
        (SizeCountStore::Entry{
            .timestamp = Timestamp(11, 1),
            .size = 40,
            .count = 6,
            .hash =
                boost::none}));  // boost::none because the initial entry does not contain a hash.
}

TEST_F(SizeCountStoreTest, WriteUpdateExistingEntryPersistentHash) {
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry initialEntry{.timestamp = Timestamp(10, 1),
                                             .size = 42,
                                             .count = 7,
                                             .hash = static_cast<int64_t>(0xDEADBEEFDEADBEEF)};
    insertSizeCountEntry(operationContext(), *store, uuid, initialEntry);

    const SizeCountStore::Entry updatedEntry{
        .timestamp = Timestamp(11, 1), .size = 40, .count = 6, .hash = 0x01};

    insertSizeCountEntry(operationContext(), *store, uuid, updatedEntry);
    const boost::optional<SizeCountStore::Entry> result = store->read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result,
              (SizeCountStore::Entry{
                  .timestamp = Timestamp(11, 1), .size = 40, .count = 6, .hash = 0x01}));
}

TEST_F(SizeCountStoreTest, WriteUpdateNoHashExistingEntryPersistentHash) {
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry initialEntry{.timestamp = Timestamp(10, 1),
                                             .size = 42,
                                             .count = 7,
                                             .hash = static_cast<int64_t>(0xDEADBEEFDEADBEEF)};
    insertSizeCountEntry(operationContext(), *store, uuid, initialEntry);

    const SizeCountStore::Entry updatedEntry{
        .timestamp = Timestamp(11, 1), .size = 40, .count = 6, .hash = boost::none};
    insertSizeCountEntry(operationContext(), *store, uuid, updatedEntry);

    const boost::optional<SizeCountStore::Entry> result = store->read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result,
              (SizeCountStore::Entry{
                  .timestamp = Timestamp(11, 1), .size = 40, .count = 6, .hash = boost::none}));
}

TEST_F(SizeCountStoreTest, ReadWriteTwoEntries) {
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    const UUID uuid0 = UUID::gen();
    const UUID uuid1 = UUID::gen();
    const SizeCountStore::Entry entry0{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    const SizeCountStore::Entry entry1{.timestamp = Timestamp(20, 2), .size = 100, .count = 3};

    insertSizeCountEntry(operationContext(), *store, uuid0, entry0);
    insertSizeCountEntry(operationContext(), *store, uuid1, entry1);

    const auto result0 = store->read(operationContext(), uuid0);
    ASSERT_TRUE(result0.has_value());
    EXPECT_EQ(entry0, *result0);

    const auto result1 = store->read(operationContext(), uuid1);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(entry1, *result1);
}

TEST_F(SizeCountStoreTest, WriteUpdateToOneOfTwoEntries) {
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    const UUID uuid0 = UUID::gen();
    const UUID uuid1 = UUID::gen();
    const SizeCountStore::Entry entry0{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    const SizeCountStore::Entry entry1{.timestamp = Timestamp(20, 2), .size = 100, .count = 3};

    insertSizeCountEntry(operationContext(), *store, uuid0, entry0);
    insertSizeCountEntry(operationContext(), *store, uuid1, entry1);

    const SizeCountStore::Entry updatedEntry0{
        .timestamp = entry0.timestamp + 1, .size = entry0.size + 10, .count = entry0.count + 2};
    insertSizeCountEntry(operationContext(), *store, uuid0, updatedEntry0);

    const auto result0 = store->read(operationContext(), uuid0);
    ASSERT_TRUE(result0.has_value());
    EXPECT_EQ(updatedEntry0, *result0);

    const auto result1 = store->read(operationContext(), uuid1);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(entry1, *result1);
}

TEST_F(SizeCountStoreTest, InsertAddsEntry) {
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};

    auto opCtx = operationContext();
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    WriteUnitOfWork wuow{opCtx};
    store->insert(opCtx, uuid, entry);
    wuow.commit();

    const auto result = store->read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(entry, *result);
}

TEST_F(SizeCountStoreTest, DoubleInsertFails) {
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};

    auto opCtx = operationContext();
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    {
        WriteUnitOfWork wuow{opCtx};
        store->insert(opCtx, uuid, entry);
        wuow.commit();
    }

    {
        WriteUnitOfWork wuow{opCtx};
        ASSERT_THROWS(store->insert(opCtx, uuid, entry), DBException);
    }
    // Initial entry should be unchanged.
    const auto result = store->read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(entry, *result);
}

TEST_F(SizeCountStoreTest, RemoveRemovesEntry) {
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};

    auto opCtx = operationContext();
    Lock::GlobalLock writeLock(opCtx, MODE_IX);

    {
        WriteUnitOfWork wuow{opCtx};
        store->insert(opCtx, uuid, entry);
        wuow.commit();
    }

    const auto result = store->read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(entry, *result);

    {
        WriteUnitOfWork wuow{opCtx};
        EXPECT_EQ(store->remove(opCtx, uuid), 1);
        wuow.commit();
    }

    EXPECT_FALSE(store->read(operationContext(), uuid).has_value());
}

TEST_F(SizeCountStoreTest, RemoveNonExistentEntryIsNoOp) {
    const UUID uuid = UUID::gen();

    auto opCtx = operationContext();
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    {
        WriteUnitOfWork wuow{opCtx};
        EXPECT_EQ(store->remove(opCtx, uuid), 0);
        wuow.commit();
    }
    EXPECT_FALSE(store->read(operationContext(), uuid).has_value());
}

TEST_F(SizeCountStoreTest, ReadMassertsWithoutGlobalReadLock) {
    ASSERT_THROWS_CODE(store->read(operationContext(), UUID::gen()), DBException, 12915203);
}

TEST_F(SizeCountStoreTest, ReadAndIncrementSizeCountsMassertsWithoutGlobalReadLock) {
    ReplicatedMetadataDeltas deltas;
    ASSERT_THROWS_CODE(store->readAndIncrementReplicatedMetadata(operationContext(), deltas),
                       DBException,
                       12915202);
}

TEST_F(SizeCountStoreTest, WriteMassertsWithoutWriteUnitOfWork) {
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    ASSERT_THROWS_CODE(store->write(operationContext(), uuid, entry), DBException, 12915205);
}

TEST_F(SizeCountStoreTest, WriteMassertsWithoutGlobalWriteLock) {
    auto opCtx = operationContext();
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    Lock::GlobalLock readLock(opCtx, MODE_IS);
    WriteUnitOfWork wuow(opCtx);
    ASSERT_THROWS_CODE(store->write(opCtx, uuid, entry), DBException, 12915204);
}

TEST_F(SizeCountStoreTest, InsertMassertsWithoutWriteUnitOfWork) {
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    ASSERT_THROWS_CODE(store->insert(operationContext(), uuid, entry), DBException, 12915205);
}

TEST_F(SizeCountStoreTest, InsertMassertsWithoutGlobalWriteLock) {
    auto opCtx = operationContext();
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    Lock::GlobalLock readLock(opCtx, MODE_IS);
    WriteUnitOfWork wuow(opCtx);
    ASSERT_THROWS_CODE(store->insert(opCtx, uuid, entry), DBException, 12915204);
}

TEST_F(SizeCountStoreTest, RemoveMassertsWithoutWriteUnitOfWork) {
    const UUID uuid = UUID::gen();
    ASSERT_THROWS_CODE(store->remove(operationContext(), uuid), DBException, 12915205);
}

TEST_F(SizeCountStoreTest, RemoveMassertsWithoutGlobalWriteLock) {
    auto opCtx = operationContext();
    const UUID uuid = UUID::gen();
    Lock::GlobalLock readLock(opCtx, MODE_IS);
    WriteUnitOfWork wuow(opCtx);
    ASSERT_THROWS_CODE(store->remove(opCtx, uuid), DBException, 12915204);
}

TEST_F(SizeCountStoreTest, WriteLeavesHashUnsetWhenAbsent) {
    auto opCtx = operationContext();
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{
        .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = boost::none};

    insertSizeCountEntry(operationContext(), *store, uuid, entry);

    Lock::GlobalLock readLock(operationContext(), MODE_IS);

    const auto result = store->read(opCtx, uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->hash.has_value());
    EXPECT_EQ(result->size, 42);
    EXPECT_EQ(result->count, 7);
}

TEST_F(SizeCountStoreTest, WritePersistsHashWhenPresent) {
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{
        .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = makeTestHash()};

    insertSizeCountEntry(operationContext(), *store, uuid, entry);

    Lock::GlobalLock readLock(operationContext(), MODE_IS);
    const auto result = store->read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->hash.has_value());
    EXPECT_EQ(*result->hash, entry.hash);
    EXPECT_EQ(result->size, 42);
    EXPECT_EQ(result->count, 7);
}

TEST_F(SizeCountStoreTest, InsertPersistsHashWhenPresent) {
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{
        .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = makeTestHash()};

    {
        Lock::GlobalLock writeLock(operationContext(), MODE_IX);
        WriteUnitOfWork wuow{operationContext()};
        store->insert(operationContext(), uuid, entry);
        wuow.commit();
    }

    Lock::GlobalLock readLock(operationContext(), MODE_IS);
    const auto result = store->read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->hash.has_value());
    EXPECT_EQ(*result->hash, entry.hash);
    EXPECT_EQ(result->size, 42);
    EXPECT_EQ(result->count, 7);
}

TEST_F(SizeCountStoreTest, InsertLeavesHashUnsetWhenAbsent) {
    auto opCtx = operationContext();
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{
        .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = boost::none};

    {
        Lock::GlobalLock writeLock(operationContext(), MODE_IX);
        WriteUnitOfWork wuow{operationContext()};
        store->insert(operationContext(), uuid, entry);
        wuow.commit();
    }

    Lock::GlobalLock readLock(operationContext(), MODE_IS);
    const auto result = store->read(opCtx, uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->hash.has_value());
    EXPECT_EQ(result->size, 42);
    EXPECT_EQ(result->count, 7);
}

// If the SizeCountStore contains an entry with a hash for the provided UUID, but the corresponding
// delta does not, the hash should be set to boost::none.
TEST_F(SizeCountStoreTest, EntryHashNoDeltaHashPreExistingHash) {
    auto opCtx = operationContext();
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{
        .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = makeTestHash()};

    insertSizeCountEntry(operationContext(), *store, uuid, entry);

    ReplicatedMetadataDeltas deltas;
    deltas[uuid] = ReplicatedMetadataDelta{
        .metadata = {.sizeCount = {.size = 8, .count = 1}, .hash = boost::none}};
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    store->readAndIncrementReplicatedMetadata(opCtx, deltas);
    const ReplicatedMetadataDeltas expectedDeltas{
        {uuid,
         ReplicatedMetadataDelta{
             .metadata = {.sizeCount = {.size = 42 + 8, .count = 7 + 1}, .hash = boost::none}}}};

    EXPECT_EQ(deltas, expectedDeltas);
}

// If the SizeCountStore contains an entry without hash for the provided UUID, but the corresponding
// delta does, the hash should be set to boost::none.
TEST_F(SizeCountStoreTest, EntryHashDeltaHashNoPreExistingHash) {
    auto opCtx = operationContext();
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{
        .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = boost::none};

    insertSizeCountEntry(operationContext(), *store, uuid, entry);

    ReplicatedMetadataDeltas deltas;
    deltas[uuid] = ReplicatedMetadataDelta{
        .metadata = {.sizeCount = {.size = 8, .count = 1}, .hash = 0xDEADBEEF}};
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    store->readAndIncrementReplicatedMetadata(opCtx, deltas);
    const ReplicatedMetadataDeltas expectedDeltas{
        {uuid,
         ReplicatedMetadataDelta{
             .metadata = {.sizeCount = {.size = 42 + 8, .count = 7 + 1}, .hash = boost::none}}}};

    EXPECT_EQ(deltas, expectedDeltas);
}

// readAndIncrementReplicatedMetadata() increments deltas with pre-existing SizeCountStore entries.
TEST_F(SizeCountStoreTest, ReadAndIncrementPreExistingEntries) {
    auto opCtx = operationContext();

    const UUID uuid1 = UUID::gen();
    const UUID uuid2 = UUID::gen();
    const SizeCountStore::Entry entry1{
        .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = makeTestHash()};
    const SizeCountStore::Entry entry2{
        .timestamp = Timestamp(10, 1), .size = 81, .count = 5, .hash = boost::none};
    insertSizeCountEntry(operationContext(), *store, uuid1, entry1);
    insertSizeCountEntry(operationContext(), *store, uuid2, entry2);

    ReplicatedMetadataDeltas deltas{
        {uuid1,
         ReplicatedMetadataDelta{.metadata = {.sizeCount = {.size = 8, .count = 1},
                                              .hash = static_cast<int64_t>(0xDEADBEEFDEADBEEF)}}},
        {uuid2,
         ReplicatedMetadataDelta{
             .metadata = {.sizeCount = {.size = 29, .count = 14}, .hash = boost::none}}}};
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    store->readAndIncrementReplicatedMetadata(opCtx, deltas);

    const ReplicatedMetadataDeltas expectedDeltas{
        {uuid1,
         ReplicatedMetadataDelta{
             .metadata = {.sizeCount = {.size = 42 + 8, .count = 7 + 1},
                          .hash = static_cast<int64_t>(*entry1.hash ^ 0xDEADBEEFDEADBEEF)}}},
        {uuid2,
         ReplicatedMetadataDelta{
             .metadata = {.sizeCount = {.size = 81 + 29, .count = 5 + 14}, .hash = boost::none}}}};

    EXPECT_EQ(deltas, expectedDeltas);
}

// readAndIncrementReplicatedMetadata() does not modify deltas with no pre-existing SizeCountStore
// entry.
TEST_F(SizeCountStoreTest, ReadAndIncrementNoPreExistingEntries) {
    auto opCtx = operationContext();

    const UUID uuid1 = UUID::gen();
    const UUID uuid2 = UUID::gen();

    ReplicatedMetadataDeltas deltas{
        {uuid1,
         ReplicatedMetadataDelta{.metadata = {.sizeCount = {.size = 8, .count = 1},
                                              .hash = static_cast<int64_t>(0xDEADBEEFDEADBEEF)}}},
        {uuid2,
         ReplicatedMetadataDelta{
             .metadata = {.sizeCount = {.size = 29, .count = 14}, .hash = boost::none}}}};
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    store->readAndIncrementReplicatedMetadata(opCtx, deltas);

    const ReplicatedMetadataDeltas expectedDeltas{
        {uuid1,
         ReplicatedMetadataDelta{.metadata = {.sizeCount = {.size = 8, .count = 1},
                                              .hash = static_cast<int64_t>(0xDEADBEEFDEADBEEF)}}},
        {uuid2,
         ReplicatedMetadataDelta{
             .metadata = {.sizeCount = {.size = 29, .count = 14}, .hash = boost::none}}}};

    EXPECT_EQ(deltas, expectedDeltas);
}

// readAndIncrementReplicatedMetadata() does not persist a hash if either the pre-existing hash or
// the new hash are boost::none.
TEST_F(SizeCountStoreTest, ReadAndIncrementHashExistenceMismatch) {
    auto opCtx = operationContext();

    const UUID uuid1 = UUID::gen();
    const UUID uuid2 = UUID::gen();
    const SizeCountStore::Entry entry1{
        .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = makeTestHash()};
    const SizeCountStore::Entry entry2{
        .timestamp = Timestamp(10, 1), .size = 81, .count = 5, .hash = boost::none};
    insertSizeCountEntry(operationContext(), *store, uuid1, entry1);
    insertSizeCountEntry(operationContext(), *store, uuid2, entry2);

    ReplicatedMetadataDeltas deltas{
        {uuid1,
         ReplicatedMetadataDelta{
             .metadata = {.sizeCount = {.size = 8, .count = 1}, .hash = boost::none}}},
        {uuid2,
         ReplicatedMetadataDelta{.metadata = {.sizeCount = {.size = 29, .count = 14},
                                              .hash = static_cast<int64_t>(0xBEEFDEADBEEFDEAD)}}}};
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    store->readAndIncrementReplicatedMetadata(opCtx, deltas);

    const ReplicatedMetadataDeltas expectedDeltas{
        {uuid1,
         ReplicatedMetadataDelta{
             .metadata = {.sizeCount = {.size = 42 + 8, .count = 7 + 1}, .hash = boost::none}}},
        {uuid2,
         ReplicatedMetadataDelta{
             .metadata = {.sizeCount = {.size = 81 + 29, .count = 5 + 14}, .hash = boost::none}}}};

    EXPECT_EQ(deltas, expectedDeltas);
}

TEST(ParseContainerValueTest, HashNoneWhenAbsent) {
    EXPECT_FALSE(parseDoc(makeMetadataDoc(boost::none)).hash.has_value());
}

TEST(ParseContainerValueTest, PopulatesHashWhenPresent) {
    const int64_t hash = makeTestHash();
    const auto entry = parseDoc(makeMetadataDoc(hash));
    ASSERT_TRUE(entry.hash.has_value());
    EXPECT_EQ(hash, *entry.hash);
    EXPECT_EQ(42, entry.size);
    EXPECT_EQ(7, entry.count);
}

// A persisted zero is a real hash value and must be distinguishable from a missing field. This is
// the property that motivates `hash` being optional rather than defaulting to 0.
TEST(ParseContainerValueTest, RoundTripsZeroHash) {
    const auto entry = parseDoc(makeMetadataDoc(int64_t{0}));
    ASSERT_TRUE(entry.hash.has_value());
    EXPECT_EQ(0, *entry.hash);
}

// The hash is a 64-bit value reinterpreted as int64_t, so roughly half of all real hashes are
// negative and the extremes must survive a round trip.
TEST(ParseContainerValueTest, RoundTripsNegativeAndBoundaryHashes) {
    for (const int64_t hash : {int64_t{-1},
                               int64_t{-0x0102030405060708},
                               std::numeric_limits<int64_t>::min(),
                               std::numeric_limits<int64_t>::max()}) {
        const auto entry = parseDoc(makeMetadataDoc(hash));
        ASSERT_TRUE(entry.hash.has_value()) << "hash: " << hash;
        EXPECT_EQ(hash, *entry.hash) << "hash: " << hash;
    }
}

DEATH_TEST_REGEX(ParseContainerValueDeathTest, TassertsOnWrongType, "13197400") {
    const BSONObj hash = BSON(kHashKey << "not-a-long");
    parseDoc(makeMetadataDocWithRawHash(hash.firstElement()));
}

// A 32-bit NumberInt is the realistic mis-encoding to guard against: `h` must be a NumberLong.
DEATH_TEST_REGEX(ParseContainerValueDeathTest, TassertsOnInt32, "13197400") {
    const BSONObj hash = BSON(kHashKey << int32_t{5});
    parseDoc(makeMetadataDocWithRawHash(hash.firstElement()));
}

// A double is what a manual mongosh update to `h` would leave behind.
DEATH_TEST_REGEX(ParseContainerValueDeathTest, TassertsOnDouble, "13197400") {
    const BSONObj hash = BSON(kHashKey << 5.0);
    parseDoc(makeMetadataDocWithRawHash(hash.firstElement()));
}

// An explicit null is present as far as eoo() is concerned, so it is rejected rather than treated
// as a missing field.
DEATH_TEST_REGEX(ParseContainerValueDeathTest, TassertsOnNull, "13197400") {
    const BSONObj hash = BSON(kHashKey << BSONNULL);
    parseDoc(makeMetadataDocWithRawHash(hash.firstElement()));
}

// Entry equality is defaulted, so it now discriminates on the hash. Existing comparisons rely on
// this staying true once writers begin emitting `h`.
TEST(EntryEqualityTest, DiscriminatesOnHash) {
    const SizeCountStore::Entry base{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    SizeCountStore::Entry withHash = base;
    withHash.hash = makeTestHash();

    EXPECT_NE(base, withHash);
    EXPECT_EQ(withHash, withHash);

    SizeCountStore::Entry otherHash = withHash;
    otherHash.hash = makeTestHash() + 1;
    EXPECT_NE(withHash, otherHash);
}

}  // namespace
}  // namespace mongo::replicated_fast_count
