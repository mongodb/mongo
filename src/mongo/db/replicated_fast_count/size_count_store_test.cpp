// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_store.h"

#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/unittest/death_test.h"

#include <limits>

namespace mongo::replicated_fast_count {
namespace {

enum class Mode { kCollection, kContainer };

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

// Runs each test case in both collection-backed and container-backed modes.
class SizeCountStoreTest : public CatalogTestFixture, public ::testing::WithParamInterface<Mode> {
protected:
    int expectedReadLockCode() const {
        return GetParam() == Mode::kCollection ? 12915208 : 12915203;
    }

    int expectedReadAndIncrementLockCode() const {
        return GetParam() == Mode::kCollection ? 12915207 : 12915202;
    }

    void setUp() override {
        CatalogTestFixture::setUp();
        auto opCtx = operationContext();
        if (GetParam() == Mode::kCollection) {
            ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), opCtx));
            return;
        }

        _ffContainerWrites =
            std::make_unique<unittest::ServerParameterGuard>("featureFlagContainerWrites", true);

        ASSERT_OK(createInternalFastCountContainers(opCtx,
                                                    NamespaceString::kAdminCommandNamespace,
                                                    ident::kFastCountMetadataStore,
                                                    KeyFormat::String,
                                                    ident::kFastCountMetadataStoreTimestamps,
                                                    KeyFormat::Long,
                                                    /*writeToOplog=*/false));

        auto* engine = opCtx->getServiceContext()->getStorageEngine()->getEngine();
        _recordStore = engine->getRecordStore(opCtx,
                                              NamespaceString::kAdminCommandNamespace,
                                              ident::kFastCountMetadataStore,
                                              RecordStore::Options{.keyFormat = KeyFormat::String},
                                              /*uuid=*/boost::none);
    }

    std::unique_ptr<SizeCountStore> makeStore() {
        if (GetParam() == Mode::kCollection) {
            return std::make_unique<CollectionSizeCountStore>();
        }
        return std::make_unique<ContainerSizeCountStore>(std::move(_recordStore));
    }

    // Persists `doc` for `uuid` bypassing the store's write path, which cannot emit `h`. Used to
    // stage documents in the shape a future writer (or another node) would produce.
    // TODO(SERVER-132687): Remove once writers emit the hash; tests staging a document without
    // `h` will still need it.
    void rawInsert(SizeCountStore& store, const UUID& uuid, const BSONObj& doc) {
        auto opCtx = operationContext();
        WriteUnitOfWork wuow(opCtx);
        if (GetParam() == Mode::kCollection) {
            const auto acquisition = acquireFastCountCollectionForWrite(opCtx).value();
            BSONObjBuilder builder;
            builder.appendElements(BSON("_id" << uuid));
            builder.appendElements(doc);
            ASSERT_OK(collection_internal::insertDocument(opCtx,
                                                          acquisition.getCollectionPtr(),
                                                          InsertStatement(builder.obj()),
                                                          /*opDebug=*/nullptr));
        } else {
            auto containerVariant =
                static_cast<ContainerSizeCountStore&>(store).rs_ForTest()->getContainer();
            auto& container =
                std::get<std::reference_wrapper<StringKeyedContainer>>(containerVariant).get();
            ASSERT_OK(container.insert(*shard_role_details::getRecoveryUnit(opCtx),
                                       test_helpers::uuidSpan(uuid),
                                       test_helpers::bsonSpan(doc),
                                       container::ExistingKeyPolicy::reject));
        }
        wuow.commit();
    }

    std::unique_ptr<unittest::ServerParameterGuard> _ffContainerWrites;
    std::unique_ptr<RecordStore> _recordStore;
};

TEST_P(SizeCountStoreTest, ReadReturnsNoneWhenEmpty) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    Lock::GlobalLock readLock(operationContext(), MODE_IS);
    EXPECT_FALSE(store.read(operationContext(), UUID::gen()).has_value());
}

TEST_P(SizeCountStoreTest, ReadWriteRoundTripNewEntry) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    // Hold MODE_IX (not MODE_IS) for the whole test: the store reads require the global lock and
    // insertSizeCountEntry reacquires it in MODE_IX, which cannot be upgraded from MODE_IS.
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};

    test_helpers::insertSizeCountEntry(operationContext(), store, uuid, entry);

    const auto result = store.read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(entry, *result);
}

TEST_P(SizeCountStoreTest, WriteUpdateExistingEntry) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry initialEntry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    test_helpers::insertSizeCountEntry(operationContext(), store, uuid, initialEntry);

    const SizeCountStore::Entry updatedEntry{.timestamp = initialEntry.timestamp + 1,
                                             .size = initialEntry.size - 2,
                                             .count = initialEntry.count - 1};
    EXPECT_NE(initialEntry, updatedEntry);

    test_helpers::insertSizeCountEntry(operationContext(), store, uuid, updatedEntry);
    const auto result = store.read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(updatedEntry, *result);
}

TEST_P(SizeCountStoreTest, ReadWriteTwoEntries) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    const UUID uuid0 = UUID::gen();
    const UUID uuid1 = UUID::gen();
    const SizeCountStore::Entry entry0{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    const SizeCountStore::Entry entry1{.timestamp = Timestamp(20, 2), .size = 100, .count = 3};

    test_helpers::insertSizeCountEntry(operationContext(), store, uuid0, entry0);
    test_helpers::insertSizeCountEntry(operationContext(), store, uuid1, entry1);

    const auto result0 = store.read(operationContext(), uuid0);
    ASSERT_TRUE(result0.has_value());
    EXPECT_EQ(entry0, *result0);

    const auto result1 = store.read(operationContext(), uuid1);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(entry1, *result1);
}

TEST_P(SizeCountStoreTest, WriteUpdateToOneOfTwoEntries) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    const UUID uuid0 = UUID::gen();
    const UUID uuid1 = UUID::gen();
    const SizeCountStore::Entry entry0{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    const SizeCountStore::Entry entry1{.timestamp = Timestamp(20, 2), .size = 100, .count = 3};

    test_helpers::insertSizeCountEntry(operationContext(), store, uuid0, entry0);
    test_helpers::insertSizeCountEntry(operationContext(), store, uuid1, entry1);

    const SizeCountStore::Entry updatedEntry0{
        .timestamp = entry0.timestamp + 1, .size = entry0.size + 10, .count = entry0.count + 2};
    test_helpers::insertSizeCountEntry(operationContext(), store, uuid0, updatedEntry0);

    const auto result0 = store.read(operationContext(), uuid0);
    ASSERT_TRUE(result0.has_value());
    EXPECT_EQ(updatedEntry0, *result0);

    const auto result1 = store.read(operationContext(), uuid1);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(entry1, *result1);
}

TEST_P(SizeCountStoreTest, InsertAddsEntry) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};

    auto opCtx = operationContext();
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    WriteUnitOfWork wuow{opCtx};
    store.insert(opCtx, uuid, entry);
    wuow.commit();

    const auto result = store.read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(entry, *result);
}

TEST_P(SizeCountStoreTest, DoubleInsertFails) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};

    auto opCtx = operationContext();
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    {
        WriteUnitOfWork wuow{opCtx};
        store.insert(opCtx, uuid, entry);
        wuow.commit();
    }

    {
        WriteUnitOfWork wuow{opCtx};
        ASSERT_THROWS(store.insert(opCtx, uuid, entry), DBException);
    }
    // Initial entry should be unchanged.
    const auto result = store.read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(entry, *result);
}

TEST_P(SizeCountStoreTest, RemoveRemovesEntry) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};

    auto opCtx = operationContext();
    Lock::GlobalLock writeLock(opCtx, MODE_IX);

    {
        WriteUnitOfWork wuow{opCtx};
        store.insert(opCtx, uuid, entry);
        wuow.commit();
    }

    const auto result = store.read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(entry, *result);

    {
        WriteUnitOfWork wuow{opCtx};
        EXPECT_EQ(store.remove(opCtx, uuid), 1);
        wuow.commit();
    }

    EXPECT_FALSE(store.read(operationContext(), uuid).has_value());
}

TEST_P(SizeCountStoreTest, RemoveNonExistentEntryIsNoOp) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    const UUID uuid = UUID::gen();

    auto opCtx = operationContext();
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    {
        WriteUnitOfWork wuow{opCtx};
        EXPECT_EQ(store.remove(opCtx, uuid), 0);
        wuow.commit();
    }
    EXPECT_FALSE(store.read(operationContext(), uuid).has_value());
}

TEST_P(SizeCountStoreTest, ReadMassertsWithoutGlobalReadLock) {
    auto storePtr = makeStore();
    ASSERT_THROWS_CODE(
        storePtr->read(operationContext(), UUID::gen()), DBException, expectedReadLockCode());
}

TEST_P(SizeCountStoreTest, ReadAndIncrementSizeCountsMassertsWithoutGlobalReadLock) {
    auto storePtr = makeStore();
    SizeCountDeltas deltas;
    ASSERT_THROWS_CODE(storePtr->readAndIncrementSizeCounts(operationContext(), deltas),
                       DBException,
                       expectedReadAndIncrementLockCode());
}

TEST_P(SizeCountStoreTest, WriteMassertsWithoutWriteUnitOfWork) {
    auto storePtr = makeStore();
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    ASSERT_THROWS_CODE(storePtr->write(operationContext(), uuid, entry), DBException, 12915205);
}

TEST_P(SizeCountStoreTest, WriteMassertsWithoutGlobalWriteLock) {
    auto storePtr = makeStore();
    auto opCtx = operationContext();
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    Lock::GlobalLock readLock(opCtx, MODE_IS);
    WriteUnitOfWork wuow(opCtx);
    ASSERT_THROWS_CODE(storePtr->write(opCtx, uuid, entry), DBException, 12915204);
}

TEST_P(SizeCountStoreTest, InsertMassertsWithoutWriteUnitOfWork) {
    auto storePtr = makeStore();
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    ASSERT_THROWS_CODE(storePtr->insert(operationContext(), uuid, entry), DBException, 12915205);
}

TEST_P(SizeCountStoreTest, InsertMassertsWithoutGlobalWriteLock) {
    auto storePtr = makeStore();
    auto opCtx = operationContext();
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{.timestamp = Timestamp(10, 1), .size = 42, .count = 7};
    Lock::GlobalLock readLock(opCtx, MODE_IS);
    WriteUnitOfWork wuow(opCtx);
    ASSERT_THROWS_CODE(storePtr->insert(opCtx, uuid, entry), DBException, 12915204);
}

TEST_P(SizeCountStoreTest, RemoveMassertsWithoutWriteUnitOfWork) {
    auto storePtr = makeStore();
    const UUID uuid = UUID::gen();
    ASSERT_THROWS_CODE(storePtr->remove(operationContext(), uuid), DBException, 12915205);
}

TEST_P(SizeCountStoreTest, RemoveMassertsWithoutGlobalWriteLock) {
    auto storePtr = makeStore();
    auto opCtx = operationContext();
    const UUID uuid = UUID::gen();
    Lock::GlobalLock readLock(opCtx, MODE_IS);
    WriteUnitOfWork wuow(opCtx);
    ASSERT_THROWS_CODE(storePtr->remove(opCtx, uuid), DBException, 12915204);
}

// A record carrying `h` must be readable through the public read() API on both backends: live
// on-disk data may contain the field before this node's writers ever emit it.
TEST_P(SizeCountStoreTest, ReadPopulatesHash) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    auto opCtx = operationContext();
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    const UUID uuid = UUID::gen();
    const int64_t hash = makeTestHash();

    rawInsert(store, uuid, makeMetadataDoc(hash));

    const auto result = store.read(opCtx, uuid);
    ASSERT_TRUE(result.has_value());
    ASSERT_TRUE(result->hash.has_value());
    EXPECT_EQ(hash, *result->hash);
    EXPECT_EQ(42, result->size);
    EXPECT_EQ(7, result->count);
}

TEST_P(SizeCountStoreTest, ReadLeavesHashUnsetWhenAbsent) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    auto opCtx = operationContext();
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    const UUID uuid = UUID::gen();

    rawInsert(store, uuid, makeMetadataDoc(boost::none));

    const auto result = store.read(opCtx, uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->hash.has_value());
}

// The write path does not emit `h` yet, so a hash set on an Entry must not survive a round trip.
// This pins the parse-only scope of this change: readers must ship everywhere before any node
// writes the field.
// TODO(SERVER-132687): Invert this once writers emit `h` behind the FCV gate.
TEST_P(SizeCountStoreTest, WriteDoesNotPersistHash) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    Lock::GlobalLock writeLock(operationContext(), MODE_IX);
    const UUID uuid = UUID::gen();
    const SizeCountStore::Entry entry{
        .timestamp = Timestamp(10, 1), .size = 42, .count = 7, .hash = makeTestHash()};

    test_helpers::insertSizeCountEntry(operationContext(), store, uuid, entry);

    const auto result = store.read(operationContext(), uuid);
    ASSERT_TRUE(result.has_value());
    EXPECT_FALSE(result->hash.has_value());
}

// An `h`-bearing record must still accumulate correctly on the checkpoint path, which reads the
// same documents but only sums size and count.
TEST_P(SizeCountStoreTest, ReadAndIncrementSizeCountsIgnoresHash) {
    auto storePtr = makeStore();
    auto& store = *storePtr;
    auto opCtx = operationContext();
    Lock::GlobalLock writeLock(opCtx, MODE_IX);
    const UUID uuid = UUID::gen();

    rawInsert(store, uuid, makeMetadataDoc(makeTestHash()));

    SizeCountDeltas deltas;
    deltas[uuid] = SizeCountDelta{.sizeCount = {.size = 8, .count = 1}};
    store.readAndIncrementSizeCounts(opCtx, deltas);

    EXPECT_EQ(50, deltas[uuid].sizeCount.size);
    EXPECT_EQ(8, deltas[uuid].sizeCount.count);
}

INSTANTIATE_TEST_SUITE_P(,
                         SizeCountStoreTest,
                         ::testing::Values(Mode::kCollection, Mode::kContainer),
                         [](const ::testing::TestParamInfo<Mode>& info) {
                             return info.param == Mode::kCollection ? "Collection" : "Container";
                         });

// Collection-only case: container mode provisions must pass an existing RecordStore to the
// SizeCountTimestampStore constructor, so there is no equivalent "backing storage does not exist"
// scenario to exercise.
class SizeCountStoreCollectionModeTest : public CatalogTestFixture {};

TEST_F(SizeCountStoreCollectionModeTest, ReadReturnsNoneWhenCollectionDoesNotExist) {
    const CollectionSizeCountStore store;
    Lock::GlobalLock readLock(operationContext(), MODE_IS);
    EXPECT_FALSE(store.read(operationContext(), UUID::gen()).has_value());
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
