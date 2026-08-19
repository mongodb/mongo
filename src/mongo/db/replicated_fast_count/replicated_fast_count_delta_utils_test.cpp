// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"

#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/replicated_fast_count/durable_size_metadata_gen.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_streaming_oplog_delta_accumulator.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/server_parameter_guard.h"

#include <string_view>

namespace mongo::replicated_fast_count {
namespace {
using namespace std::literals::string_view_literals;

class ReadAndIncrementSizeCountsTest : public CatalogTestFixture {
protected:
    void doReadAndIncrement(CollectionSizeCountStore& store, ReplicatedMetadataDeltas& deltas) {
        Lock::GlobalLock lk(operationContext(), MODE_IS);
        store.readAndIncrementReplicatedMetadata(operationContext(), deltas);
    }
};

TEST_F(ReadAndIncrementSizeCountsTest, IncrementZeros) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    CollectionSizeCountStore store;

    const UUID uuid = UUID::gen();
    ReplicatedMetadataDeltas deltas;
    deltas[uuid] =
        ReplicatedMetadataDelta{.metadata = {.sizeCount = {0, 0}}, .state = DDLState::kNone};

    // Read before the document exists.
    doReadAndIncrement(store, deltas);

    EXPECT_EQ(deltas.size(), 1);
    ASSERT_TRUE(deltas.contains(uuid));
    EXPECT_EQ(deltas[uuid].metadata.sizeCount.size, 0);
    EXPECT_EQ(deltas[uuid].metadata.sizeCount.count, 0);

    test_helpers::insertSizeCountEntry(
        operationContext(),
        store,
        uuid,
        SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 0, .count = 0});

    // Read after (0,0) document exists.
    doReadAndIncrement(store, deltas);

    EXPECT_EQ(deltas.size(), 1);
    ASSERT_TRUE(deltas.contains(uuid));
    EXPECT_EQ(deltas[uuid].metadata.sizeCount.size, 0);
    EXPECT_EQ(deltas[uuid].metadata.sizeCount.count, 0);
}

TEST_F(ReadAndIncrementSizeCountsTest, NegativeResult) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    CollectionSizeCountStore store;

    const UUID uuid = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        store,
        uuid,
        SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    ReplicatedMetadataDeltas deltas;
    deltas[uuid] = ReplicatedMetadataDelta{.metadata = {.sizeCount = {.size = -400, .count = -20}},
                                           .state = DDLState::kNone};

    doReadAndIncrement(store, deltas);

    EXPECT_EQ(deltas.size(), 1);
    ASSERT_TRUE(deltas.contains(uuid));
    EXPECT_EQ(deltas[uuid].metadata.sizeCount.size, -200);
    EXPECT_EQ(deltas[uuid].metadata.sizeCount.count, -10);
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {}
 * document UUIDs ∩ delta UUIDs = {}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadEmptySet) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    CollectionSizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        store,
        uuid1,
        SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        store,
        uuid2,
        SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

    ReplicatedMetadataDeltas deltas;

    doReadAndIncrement(store, deltas);

    EXPECT_TRUE(deltas.empty());
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {uuid1, uuid2}
 * document UUIDs ∩ delta UUIDs = {uuid1, uuid2}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentEqualSet) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    CollectionSizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        store,
        uuid1,
        SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        store,
        uuid2,
        SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

    ReplicatedMetadataDeltas deltas;
    deltas[uuid1] =
        ReplicatedMetadataDelta{.metadata = {.sizeCount = {5, 1}}, .state = DDLState::kNone};
    deltas[uuid2] =
        ReplicatedMetadataDelta{.metadata = {.sizeCount = {50, 10}}, .state = DDLState::kNone};

    doReadAndIncrement(store, deltas);

    EXPECT_EQ(deltas.size(), 2);
    ASSERT_TRUE(deltas.contains(uuid1));
    EXPECT_EQ(deltas[uuid1].metadata.sizeCount.size, 205);
    EXPECT_EQ(deltas[uuid1].metadata.sizeCount.count, 11);
    ASSERT_TRUE(deltas.contains(uuid2));
    EXPECT_EQ(deltas[uuid2].metadata.sizeCount.size, 150);
    EXPECT_EQ(deltas[uuid2].metadata.sizeCount.count, 15);
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {uuid1}
 * document UUIDs ∩ delta UUIDs = {uuid1}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentSubset) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    CollectionSizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        store,
        uuid1,
        SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        store,
        uuid2,
        SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

    ReplicatedMetadataDeltas deltas;
    deltas[uuid1] =
        ReplicatedMetadataDelta{.metadata = {.sizeCount = {5, 1}}, .state = DDLState::kNone};

    doReadAndIncrement(store, deltas);

    EXPECT_EQ(deltas.size(), 1);
    ASSERT_TRUE(deltas.contains(uuid1));
    EXPECT_EQ(deltas[uuid1].metadata.sizeCount.size, 205);
    EXPECT_EQ(deltas[uuid1].metadata.sizeCount.count, 11);
}

/**
 * document UUIDs:  {uuid1}
 * delta UUIDs:     {uuid1, uuid2}
 * document UUIDs ∩ delta UUIDs = {uuid1}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentSuperset) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    CollectionSizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        store,
        uuid1,
        SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    ReplicatedMetadataDeltas deltas;
    deltas[uuid1] =
        ReplicatedMetadataDelta{.metadata = {.sizeCount = {5, 1}}, .state = DDLState::kNone};
    deltas[uuid2] =
        ReplicatedMetadataDelta{.metadata = {.sizeCount = {50, 10}}, .state = DDLState::kNone};

    doReadAndIncrement(store, deltas);

    EXPECT_EQ(deltas.size(), 2);
    ASSERT_TRUE(deltas.contains(uuid1));
    EXPECT_EQ(deltas[uuid1].metadata.sizeCount.size, 205);
    EXPECT_EQ(deltas[uuid1].metadata.sizeCount.count, 11);
    ASSERT_TRUE(deltas.contains(uuid2));
    EXPECT_EQ(deltas[uuid2].metadata.sizeCount.size, 50);
    EXPECT_EQ(deltas[uuid2].metadata.sizeCount.count, 10);
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {uuid3}
 * document UUIDs ∩ delta UUIDs = {}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentsDisjointSet) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    CollectionSizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        store,
        uuid1,
        SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        store,
        uuid2,
        SizeCountStore::Entry{.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

    const UUID uuid3 = UUID::gen();
    ReplicatedMetadataDeltas deltas;
    deltas[uuid3] =
        ReplicatedMetadataDelta{.metadata = {.sizeCount = {5, 1}}, .state = DDLState::kNone};

    doReadAndIncrement(store, deltas);

    EXPECT_EQ(deltas.size(), 1);
    ASSERT_TRUE(deltas.contains(uuid3));
    EXPECT_EQ(deltas[uuid3].metadata.sizeCount.size, 5);
    EXPECT_EQ(deltas[uuid3].metadata.sizeCount.count, 1);
}

// ---------------------------------------------------------------------------
// Helpers for constructing oplog entries with size metadata.
// ---------------------------------------------------------------------------

repl::OplogEntrySizeMetadata makeOperationSizeMetadata(
    int32_t replicatedSizeDelta, boost::optional<int64_t> hash = boost::none) {
    SingleOpSizeMetadata m;
    m.setSz(replicatedSizeDelta);
    m.setH(hash);
    return m;
}

repl::OplogEntry makeOplogEntryWithSizeMetadata(const NamespaceString& nss,
                                                repl::OpTypeEnum opType,
                                                int32_t sizeDelta,
                                                boost::optional<int64_t> hash = boost::none,
                                                boost::optional<UUID> uuid = boost::none) {

    auto sizeMetadata = makeOperationSizeMetadata(sizeDelta, hash);
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = opType,
        .nss = nss,
        .uuid = uuid.value_or(UUID::gen()),
        .oField = BSONObj(),
        .sizeMetadata = sizeMetadata,
        .wallClockTime = Date_t::now(),
    }};
}

// Builds an entry for a single operation whose size metadata is the multi-op form. That form is
// only valid on a commitTransaction entry, so the operation's contribution cannot be determined and
// it makes the collection's hash untrackable.
repl::OplogEntry makeOplogEntryWithMultiOpSizeMetadata(const NamespaceString& nss,
                                                       boost::optional<UUID> uuid) {
    MultiOpSizeMetadata multiOpMetadata;
    multiOpMetadata.setUuid(uuid.value_or(UUID::gen()));
    multiOpMetadata.setSz(0);
    multiOpMetadata.setCt(0);
    const repl::OplogEntrySizeMetadata sizeMetadata{
        std::vector<MultiOpSizeMetadata>{multiOpMetadata}};
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kInsert,
        .nss = nss,
        .uuid = uuid,
        .oField = BSONObj(),
        .sizeMetadata = sizeMetadata,
        .wallClockTime = Date_t::now(),
    }};
}

// Builds an entry whose size metadata carries neither a size delta nor a document hash. Nothing of
// the operation's contribution can be read, so it invalidates the collection's hash.
repl::OplogEntry makeOplogEntryWithEmptySizeMetadata(const NamespaceString& nss,
                                                     boost::optional<UUID> uuid) {
    const repl::OplogEntrySizeMetadata sizeMetadata = SingleOpSizeMetadata{};
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kInsert,
        .nss = nss,
        .uuid = uuid,
        .oField = BSONObj(),
        .sizeMetadata = sizeMetadata,
        .wallClockTime = Date_t::now(),
    }};
}

// Builds an entry whose size metadata carries a document hash but no size delta. The operation's
// contribution cannot be determined, so it makes the collection's hash untrackable.
repl::OplogEntry makeOplogEntryWithHashButNoSizeDelta(const NamespaceString& nss,
                                                      boost::optional<UUID> uuid,
                                                      int64_t hash) {
    SingleOpSizeMetadata perOpMetadata;
    perOpMetadata.setH(hash);
    const repl::OplogEntrySizeMetadata sizeMetadata = perOpMetadata;
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kInsert,
        .nss = nss,
        .uuid = uuid,
        .oField = BSONObj(),
        .sizeMetadata = sizeMetadata,
        .wallClockTime = Date_t::now(),
    }};
}

// Builds an entry with neither size metadata nor a UUID, like the periodic noop.
repl::OplogEntry makeOplogEntryWithoutSizeMetadataOrUuid(const NamespaceString& nss,
                                                         repl::OpTypeEnum opType) {
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = opType,
        .nss = nss,
        .oField = BSONObj(),
        .wallClockTime = Date_t::now(),
    }};
}

// ===========================================================================
// Test fixture for extractReplicatedMetadataForOp() -- lightweight, no real writes.
// ===========================================================================

class ExtractReplicatedMetadataTest : public CatalogTestFixture {
protected:
    NamespaceString _nss1 = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_delta_utils_test", "coll1");
};

// Extraction always reads the hash off the size metadata.
using ExtractCollectionHashTest = ExtractReplicatedMetadataTest;

TEST_F(ExtractReplicatedMetadataTest, NoMetadataForSizeMetadataWithoutSizeDelta) {
    // Size metadata carrying no size delta is opting out of size and count tracking, so the entry
    // is skipped outright and its collection stays out of the accumulated deltas.
    const auto op = makeOplogEntryWithEmptySizeMetadata(_nss1, UUID::gen());

    EXPECT_FALSE(extractReplicatedMetadataForOp(op).has_value());

    ReplicatedMetadataDeltas deltas;
    EXPECT_EQ(0, processOplogEntry(op, deltas));
    EXPECT_TRUE(deltas.empty());
}

TEST_F(ExtractReplicatedMetadataTest, NoMetadataForOpOptingOutOfSizeCountTracking) {
    // An entry with no size metadata at all is opting out of size and count tracking, so it is
    // skipped outright rather than treated as a contribution we failed to read.
    const auto noopOp = makeOplogEntryWithoutSizeMetadataOrUuid(_nss1, repl::OpTypeEnum::kNoop);

    EXPECT_FALSE(extractReplicatedMetadataForOp(noopOp).has_value());

    ReplicatedMetadataDeltas deltas;
    EXPECT_EQ(processOplogEntry(noopOp, deltas), 0);
    EXPECT_TRUE(deltas.empty());
}

TEST_F(ExtractReplicatedMetadataTest, HashCarriedWithoutSizeDeltaIsReportedAndFolded) {
    // An entry carrying a hash with no size delta is reported rather than being fatal, because it
    // is durable in the oplog. The hash is still the operation's real contribution, so it is folded
    // in against a zero size and count delta. The log id is asserted because log ingestion is what
    // surfaces this.
    const int64_t hash = 0x0123456789abcdef;
    const auto op = makeOplogEntryWithHashButNoSizeDelta(_nss1, UUID::gen(), hash);

    unittest::LogCaptureGuard logs;
    const auto extracted = extractReplicatedMetadataForOp(op);
    logs.stop();

    ASSERT_TRUE(extracted.has_value());
    EXPECT_EQ(extracted->sizeCount, (CollectionSizeCount{.size = 0, .count = 0}));
    EXPECT_EQ(extracted->hash, hash);
    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13321400)), 1);
}

TEST_F(ExtractReplicatedMetadataTest, HashCarriedWithoutSizeDeltaOrUuidIsSkipped) {
    // Without a UUID there is no collection to attribute the hash to, and callers dereference the
    // entry's UUID as soon as a value is returned.
    const auto op = makeOplogEntryWithHashButNoSizeDelta(_nss1, boost::none, 0x0123456789abcdef);

    EXPECT_FALSE(extractReplicatedMetadataForOp(op).has_value());
}

TEST_F(ExtractReplicatedMetadataTest, NoMetadataWhenSingleOpEntryCarriesMultiOpMetadata) {
    // Multi-op metadata on an entry for a single operation is malformed, so there is nothing to
    // record for it.
    const auto op = makeOplogEntryWithMultiOpSizeMetadata(_nss1, UUID::gen());

    EXPECT_FALSE(extractReplicatedMetadataForOp(op).has_value());
}

TEST_F(ExtractReplicatedMetadataTest, HashWithoutSizeDeltaOnIneligibleNamespaceIsSkipped) {
    // The size-metadata contract only binds operations whose hash would be folded in. An entry for
    // a namespace fast count never tracks is skipped rather than being treated as malformed, so
    // reaching the end of this test at all is the assertion.
    const auto ineligibleNss =
        NamespaceString::createNamespaceString_forTest("mydb", "system.profile");
    ASSERT_FALSE(isReplicatedFastCountEligible(ineligibleNss));
    const auto op =
        makeOplogEntryWithHashButNoSizeDelta(ineligibleNss, UUID::gen(), 0x0123456789abcdef);

    EXPECT_FALSE(extractReplicatedMetadataForOp(op).has_value());
}

TEST_F(ExtractReplicatedMetadataTest, NoMetadataForIneligibleNamespaceCarryingHash) {
    // A namespace fast count never tracks has no hash to accumulate, so an entry for it is skipped
    // outright rather than contributing its hash to anything.
    const auto ineligibleNss =
        NamespaceString::createNamespaceString_forTest("mydb", "system.profile");
    const auto op = makeOplogEntryWithSizeMetadata(
        ineligibleNss, repl::OpTypeEnum::kInsert, 400, int64_t{0x0123456789abcdef}, UUID::gen());

    EXPECT_FALSE(extractReplicatedMetadataForOp(op).has_value());

    ReplicatedMetadataDeltas deltas;
    EXPECT_EQ(0, processOplogEntry(op, deltas));
    EXPECT_TRUE(deltas.empty());
}

TEST_F(ExtractReplicatedMetadataTest, OpWithoutHashInvalidatesAccumulatedHash) {
    // A collection accumulates a hash, then an operation that carries none arrives. The accumulated
    // hash must go absent rather than stay at a value that is now missing a contribution, while the
    // size and count keep accumulating.
    const auto uuid = UUID::gen();
    const int32_t sizeDelta = 400;
    const int64_t hash = 0x0123456789abcdef;

    ReplicatedMetadataDeltas deltas;
    const auto insertOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kInsert, sizeDelta, hash, uuid);
    EXPECT_EQ(1, processOplogEntry(insertOp, deltas));
    ASSERT_TRUE(deltas.contains(uuid));
    EXPECT_EQ(hash, deltas.at(uuid).metadata.hash);

    const auto opWithoutHash = makeOplogEntryWithSizeMetadata(
        _nss1, repl::OpTypeEnum::kInsert, sizeDelta, boost::none, uuid);
    processOplogEntry(opWithoutHash, deltas);

    EXPECT_FALSE(deltas.at(uuid).metadata.hash);
    EXPECT_EQ(2 * sizeDelta, deltas.at(uuid).metadata.sizeCount.size);
    EXPECT_EQ(2, deltas.at(uuid).metadata.sizeCount.count);
}

TEST_F(ExtractReplicatedMetadataTest, InvalidatedHashIsNotResurrectedByLaterOps) {
    // Absence is absorbing: once a contribution has been lost, folding in later operations that do
    // carry a hash must not produce a value again.
    const auto uuid = UUID::gen();
    ReplicatedMetadataDeltas deltas;

    processOplogEntry(
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kInsert, 400, boost::none, uuid),
        deltas);
    processOplogEntry(makeOplogEntryWithSizeMetadata(
                          _nss1, repl::OpTypeEnum::kInsert, 400, int64_t{0x2222}, uuid),
                      deltas);

    EXPECT_FALSE(deltas.at(uuid).metadata.hash);
    EXPECT_EQ(800, deltas.at(uuid).metadata.sizeCount.size);
}

TEST_F(ExtractReplicatedMetadataTest, ExtractSizeCountDeltaForInsert) {
    const int32_t sizeDelta = 400;
    const auto insertOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kInsert, sizeDelta);
    const auto extractedSizeCount = extractReplicatedMetadataForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    // Insert means count increases by 1.
    EXPECT_EQ(1, extractedSizeCount->sizeCount.count);
    EXPECT_EQ(sizeDelta, extractedSizeCount->sizeCount.size);
}

TEST_F(ExtractReplicatedMetadataTest, ExtractSizeCountDeltaForUpdate) {
    const int32_t sizeDelta = 400;
    const auto insertOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kUpdate, sizeDelta);
    const auto extractedSizeCount = extractReplicatedMetadataForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    // Updates imply no new documents, count delta is 0.
    EXPECT_EQ(0, extractedSizeCount->sizeCount.count);
    EXPECT_EQ(sizeDelta, extractedSizeCount->sizeCount.size);
}

TEST_F(ExtractReplicatedMetadataTest, ExtractSizeCountDeltaForDelete) {
    const int32_t sizeDelta = 400;
    const auto insertOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kDelete, sizeDelta);
    const auto extractedSizeCount = extractReplicatedMetadataForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    // Delete implies one less document.
    EXPECT_EQ(-1, extractedSizeCount->sizeCount.count);
    EXPECT_EQ(sizeDelta, extractedSizeCount->sizeCount.size);
}

TEST_F(ExtractCollectionHashTest, ExtractHashForInsert) {
    const int64_t hash = 0x0123456789abcdef;
    const auto insertOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kInsert, 400 /* sizeDelta */, hash);
    const auto extractedSizeCount = extractReplicatedMetadataForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    EXPECT_EQ(hash, extractedSizeCount->hash);
}

TEST_F(ExtractCollectionHashTest, ExtractHashForUpdate) {
    // For updates, 'h' is already the pre-image hash XOR-ed with the post-image hash, so it is
    // surfaced unchanged.
    const int64_t hash = 0x0123456789abcdef;
    const auto updateOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kUpdate, 400 /* sizeDelta */, hash);
    const auto extractedSizeCount = extractReplicatedMetadataForOp(updateOp);
    ASSERT(extractedSizeCount.has_value());

    EXPECT_EQ(hash, extractedSizeCount->hash);
}

TEST_F(ExtractCollectionHashTest, ExtractHashForDelete) {
    // For deletes, 'h' is the pre-image hash, and XOR-ing it back out removes the document's
    // contribution, so it is surfaced unchanged as well.
    const int64_t hash = 0x0123456789abcdef;
    const auto deleteOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kDelete, 400 /* sizeDelta */, hash);
    const auto extractedSizeCount = extractReplicatedMetadataForOp(deleteOp);
    ASSERT(extractedSizeCount.has_value());

    EXPECT_EQ(hash, extractedSizeCount->hash);
}

TEST_F(ExtractCollectionHashTest, NoHashWhenHAbsentFromMetadata) {
    // An entry written before the feature was enabled carries 'sz' but no 'h'. Its size and count
    // still count, but the collection hash cannot be tracked across it.
    const int32_t sizeDelta = 400;
    const auto insertOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kInsert, sizeDelta);
    const auto extractedSizeCount = extractReplicatedMetadataForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    EXPECT_EQ(sizeDelta, extractedSizeCount->sizeCount.size);
    EXPECT_EQ(1, extractedSizeCount->sizeCount.count);
    EXPECT_FALSE(extractedSizeCount->hash);
}

TEST_F(ExtractReplicatedMetadataTest, HashParsedWithEveryFeatureFlagOff) {
    // The hash is read off the size metadata unconditionally, so that a node upgrading into hash
    // accumulation reads the same value from entries written before the flags were on. The fixture
    // leaves every validation flag off, so reaching a value here is the whole contract.
    const int32_t sizeDelta = 400;
    const int64_t hash = 0x0123456789abcdef;
    const auto insertOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kInsert, sizeDelta, hash);
    const auto extractedSizeCount = extractReplicatedMetadataForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    EXPECT_EQ(sizeDelta, extractedSizeCount->sizeCount.size);
    EXPECT_EQ(1, extractedSizeCount->sizeCount.count);
    EXPECT_EQ(hash, extractedSizeCount->hash);
}

TEST_F(ExtractReplicatedMetadataTest, NoSizeCountDeltaWhenSzAbsentFromMetadataOnInsert) {
    SingleOpSizeMetadata metadataWithoutSz;
    ASSERT_FALSE(metadataWithoutSz.getSz().has_value());
    repl::OplogEntry insertOp{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kInsert,
        .nss = _nss1,
        .oField = BSONObj(),
        .sizeMetadata = repl::OplogEntrySizeMetadata{metadataWithoutSz},
        .wallClockTime = Date_t::now(),
    }}};

    // With 'sz' absent, extraction of the insert size delta must return boost::none.
    ASSERT_TRUE(insertOp.getSizeMetadata().has_value());
    EXPECT_EQ(extractReplicatedMetadataForOp(insertOp), boost::none);
}

TEST_F(ExtractReplicatedMetadataTest, NoSizeCountDeltaWhenSzAbsentFromMetadataOnUpdate) {
    SingleOpSizeMetadata metadataWithoutSz;
    ASSERT_FALSE(metadataWithoutSz.getSz().has_value());
    repl::OplogEntry updateOp{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kUpdate,
        .nss = _nss1,
        .oField = BSONObj(),
        .sizeMetadata = repl::OplogEntrySizeMetadata{metadataWithoutSz},
        .wallClockTime = Date_t::now(),
    }}};

    // With 'sz' absent, extraction of the update size delta must return boost::none.
    ASSERT_TRUE(updateOp.getSizeMetadata().has_value());
    EXPECT_EQ(extractReplicatedMetadataForOp(updateOp), boost::none);
}

TEST_F(ExtractReplicatedMetadataTest, NoSizeCountDeltaWhenSzAbsentFromMetadataOnDelete) {
    SingleOpSizeMetadata metadataWithoutSz;
    ASSERT_FALSE(metadataWithoutSz.getSz().has_value());
    repl::OplogEntry deleteOp{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kDelete,
        .nss = _nss1,
        .oField = BSONObj(),
        .sizeMetadata = repl::OplogEntrySizeMetadata{metadataWithoutSz},
        .wallClockTime = Date_t::now(),
    }}};

    // With 'sz' absent, extraction of the delete size delta must return boost::none.
    ASSERT_TRUE(deleteOp.getSizeMetadata().has_value());
    EXPECT_EQ(extractReplicatedMetadataForOp(deleteOp), boost::none);
}

TEST_F(ExtractReplicatedMetadataTest, NoSizeCountDeltaWhenAbsentFromOplogEntry) {
    // 'OpTypeEnum::kInsert' supports replicated fast count information, but none is extracted
    // because the 'm' field is absent from the oplog entry.
    repl::OplogEntry insertOpNoSizeMetadata{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kInsert,
        .nss = _nss1,
        .oField = BSONObj(),
        .wallClockTime = Date_t::now(),
    }}};
    const auto extractedSizeCount = extractReplicatedMetadataForOp(insertOpNoSizeMetadata);
    EXPECT_FALSE(insertOpNoSizeMetadata.getSizeMetadata().has_value());
}

TEST_F(ExtractReplicatedMetadataTest, NoSizeCountDeltaWhenAbsentAndIncompatibleOpType) {
    // 'OpTypeEnum::kCommand' does not support top level 'sizeMetadata' field 'm', and in absence of
    // the 'sizeMetadata', nothing is returned when trying to extract size count deltas.
    repl::OplogEntry commandOpNoSizeMetadata{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = _nss1,
        .oField = BSONObj(),
        .wallClockTime = Date_t::now(),
    }}};
    const auto extractedSizeCount = extractReplicatedMetadataForOp(commandOpNoSizeMetadata);
    EXPECT_FALSE(commandOpNoSizeMetadata.getSizeMetadata().has_value());
}

TEST_F(ExtractReplicatedMetadataTest, ExtractSizeCountDeltaOnUnsupportedOpType) {
    const auto oplogEntry =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kNoop, 400 /* sizeDelta */);

    // Size metadata is only supported for 'insert', 'delete', and 'update' operations. All other
    // operations are incompatible with a top-level 'm' field.
    EXPECT_EQ(extractReplicatedMetadataForOp(oplogEntry), boost::none);
}

TEST_F(ExtractReplicatedMetadataTest, ExtractSizeCountDeltaOnNonEligibleNss) {
    const NamespaceString localNss =
        NamespaceString::createNamespaceString_forTest("local", "coll1");
    EXPECT_FALSE(isReplicatedFastCountEligible(localNss));

    const auto oplogEntry =
        makeOplogEntryWithSizeMetadata(localNss, repl::OpTypeEnum::kNoop, 400 /* sizeDelta */);

    // Even though the oplog entry carries size metadata, ineligible namespaces should be skipped.
    EXPECT_FALSE(extractReplicatedMetadataForOp(oplogEntry).has_value());
}

TEST_F(ExtractReplicatedMetadataTest, ExtractSizeCountDeltaOnNonEligibleNssWithoutSizeMetadata) {
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
    EXPECT_FALSE(extractReplicatedMetadataForOp(insertOpLocalNs).has_value());
}

// ===========================================================================
// Test fixture for extractReplicatedMetadataDeltasForApplyOps() -- needs full infrastructure for
// real writes.
// ===========================================================================

class ExtractSizeCountDeltaForApplyOpsTest : public CatalogTestFixture {
public:
    ExtractSizeCountDeltaForApplyOpsTest()
        : CatalogTestFixture(Options().setPersistenceProvider(
              std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>())) {}

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

        setUpReplicatedFastCount(_opCtx);

        ASSERT_OK(createCollection(_opCtx, _nss1.dbName(), BSON("create" << _nss1.coll())));
        ASSERT_OK(createCollection(_opCtx, _nss2.dbName(), BSON("create" << _nss2.coll())));
        {
            auto coll1 = acquireCollection(_opCtx,
                                           CollectionAcquisitionRequest::fromOpCtx(
                                               _opCtx, _nss1, AcquisitionPrerequisites::kRead),
                                           LockMode::MODE_IS);
            auto coll2 = acquireCollection(_opCtx,
                                           CollectionAcquisitionRequest::fromOpCtx(
                                               _opCtx, _nss2, AcquisitionPrerequisites::kRead),
                                           LockMode::MODE_IS);
            _uuid1 = coll1.uuid();
            _uuid2 = coll2.uuid();
        }
    }

    void tearDown() override {
        _fastCountManager = nullptr;
        CatalogTestFixture::tearDown();
    }

    CollectionAcquisition acquireCollForWrite(const NamespaceString& nss) {
        return acquireCollection(
            _opCtx,
            CollectionAcquisitionRequest(nss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         repl::ReadConcernArgs::get(_opCtx),
                                         AcquisitionPrerequisites::kWrite),
            MODE_IX);
    }

    OperationContext* _opCtx;
    ReplicatedFastCountManager* _fastCountManager;

    NamespaceString _nss1 = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_delta_utils_test", "coll1");
    NamespaceString _nss2 = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_delta_utils_test", "coll2");

    UUID _uuid1 = UUID::gen();
    UUID _uuid2 = UUID::gen();
};

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, ExtractSizeCountDeltaForApplyOpsInsertsSingleUUID) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", true);
    const std::vector<BSONObj> docs{
        BSON("_id" << 0),
        BSON("_id" << 1),
        BSON("_id" << 2),
    };

    // Confirm this starts with an empty collection.
    EXPECT_EQ(CollectionSizeCount{}, test_helpers::scanForAccurateSizeCount(_opCtx, _nss1));
    {
        // Insert documents and confirm the aggregation.
        auto acq = acquireCollForWrite(_nss1);
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        ASSERT_OK(Helpers::insert(_opCtx, acq.getCollectionPtr(), docs));
        wuow.commit();
    }

    // Size and count were both 0 before the operation, so we expect the deltas to aggregate to the
    // totals.
    CollectionSizeCount totalCollSizeCount0 = test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);

    // Validate extracted deltas for first round of applyOps.
    const auto deltas0 = test_helpers::extractSizeCountDeltasForApplyOps(
        test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    EXPECT_EQ(1u, deltas0.size());
    ASSERT_TRUE(deltas0.contains(_uuid1));
    EXPECT_EQ(totalCollSizeCount0, deltas0.at(_uuid1));

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
    const auto totalCollSizeCount1 = test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas1 = totalCollSizeCount1 - totalCollSizeCount0;

    // Validate extracted deltas for second round of applyOps.
    const auto deltas1 = test_helpers::extractSizeCountDeltasForApplyOps(
        test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    EXPECT_EQ(1u, deltas1.size());
    ASSERT_TRUE(deltas1.contains(_uuid1));
    EXPECT_EQ(expectedDeltas1, deltas1.at(_uuid1));
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, ExtractSizeCountDeltaForApplyOpsUpdatesSingleUUID) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", true);
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
    CollectionSizeCount originalSizeCount = test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);

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
        test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas = sizeCountAfterUpdates - originalSizeCount;

    const auto deltas = test_helpers::extractSizeCountDeltasForApplyOps(
        test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    EXPECT_EQ(1u, deltas.size());
    ASSERT_TRUE(deltas.contains(_uuid1));
    EXPECT_EQ(expectedDeltas, deltas.at(_uuid1));
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, ExtractSizeCountDeltaForApplyOpsDeletesSingleUUID) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", true);
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
    CollectionSizeCount originalSizeCount = test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);

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
        test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas = sizeCountAfterUpdates - originalSizeCount;

    const auto deltas = test_helpers::extractSizeCountDeltasForApplyOps(
        test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    EXPECT_EQ(1u, deltas.size());
    ASSERT_TRUE(deltas.contains(_uuid1));
    EXPECT_EQ(expectedDeltas, deltas.at(_uuid1));
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, ExtractSizeCountDeltaForApplyOpsMultiOpsSingleUUID) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", true);
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
    const auto expectedDeltas = test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    EXPECT_EQ(1, expectedDeltas.count);
    EXPECT_NE(expectedDeltas.size, doc0.objsize());

    // Deltas correctly account for inserts, update, and delete which impact each other.
    const auto deltas = test_helpers::extractSizeCountDeltasForApplyOps(
        test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    EXPECT_EQ(1u, deltas.size());
    ASSERT_TRUE(deltas.contains(_uuid1));
    EXPECT_EQ(expectedDeltas, deltas.at(_uuid1));
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, ExtractSizeCountDeltaForApplyOpsMultiUUID) {
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", true);
    const BSONObj doc1 = BSON("_id" << 0 << "x" << "0");
    const BSONObj doc2 = BSON("_id" << 1 << "x" << "0" << "y" << 1);

    // Both collections begin empty.
    EXPECT_EQ(CollectionSizeCount{}, test_helpers::scanForAccurateSizeCount(_opCtx, _nss1));
    EXPECT_EQ(CollectionSizeCount{}, test_helpers::scanForAccurateSizeCount(_opCtx, _nss2));

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
    const auto expectedDeltas1 = test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas2 = test_helpers::scanForAccurateSizeCount(_opCtx, _nss2);
    EXPECT_EQ(expectedDeltas1.count, 1);
    EXPECT_EQ(expectedDeltas1.size, doc1.objsize());
    EXPECT_EQ(expectedDeltas2.count, 1);
    EXPECT_EQ(expectedDeltas2.size, doc2.objsize());

    // Extract applyOps deltas and verify.
    auto deltas = test_helpers::extractSizeCountDeltasForApplyOps(
        test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    EXPECT_EQ(deltas.size(), 2u);
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_TRUE(deltas.contains(_uuid2));
    EXPECT_EQ(deltas.at(_uuid1), expectedDeltas1);
    EXPECT_EQ(deltas.at(_uuid2), expectedDeltas2);
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest,
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
    ASSERT_THROWS_CODE(test_helpers::extractSizeCountDeltasForApplyOps(ungroupedInsertOplogEntry),
                       DBException,
                       12116000);
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest,
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
    ASSERT_THROWS_CODE(test_helpers::extractSizeCountDeltasForApplyOps(applyOpsEntryMissingInnerUi),
                       DBException,
                       12116001);
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, ExtractSizeCountDeltaForNestedApplyOpsMultiUUID) {
    // Nested applyOps are allowed from user commands. Tests that extraction of replicated size and
    // count works across nested applyOps for multiple collections.
    unittest::ServerParameterGuard featureFlag("featureFlagReplicatedFastCount", true);

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

    const auto deltas = test_helpers::extractSizeCountDeltasForApplyOps(applyOpsEntry);

    EXPECT_EQ(deltas.size(), 2u);
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_TRUE(deltas.contains(_uuid2));
    EXPECT_EQ(deltas.at(_uuid1), expectedDeltasNss1);
    EXPECT_EQ(deltas.at(_uuid2), expectedDeltasNss2);
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, NestedApplyOpsFoldsHashesPerCollection) {
    // Hashes fold across nesting levels the same way size and count do, and each collection keeps
    // its own: the two contributions to _uuid1 XOR together while _uuid2 keeps only its own.
    const BSONObj docA = BSON("_id" << 0 << "x" << "0");
    const int64_t hash1 = 0x1111111111111111;
    const int64_t hash2 = 0x2222222222222222;
    const int64_t hash3 = 0x4444444444444444;

    const NamespaceString adminCmdNss =
        NamespaceString::createNamespaceString_forTest("admin", "$cmd");

    const BSONObj innerMostInsertNs1 =
        BSON("op" << "i"
                  << "ns" << _nss1.ns_forTest() << "ui" << _uuid1 << "o" << docA << "m"
                  << BSON("sz" << docA.objsize() << "h" << hash1));
    const BSONObj innerMostInsertNs2 =
        BSON("op" << "i"
                  << "ns" << _nss2.ns_forTest() << "ui" << _uuid2 << "o" << docA << "m"
                  << BSON("sz" << docA.objsize() << "h" << hash2));
    const BSONObj nestedApplyOps =
        BSON("op" << "c"
                  << "ns" << adminCmdNss.ns_forTest() << "o"
                  << BSON("applyOps" << BSON_ARRAY(innerMostInsertNs1 << innerMostInsertNs2)));
    const BSONObj firstLevelInsert =
        BSON("op" << "i"
                  << "ns" << _nss1.ns_forTest() << "ui" << _uuid1 << "o" << docA << "m"
                  << BSON("sz" << docA.objsize() << "h" << hash3));

    const repl::OplogEntry applyOpsEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = adminCmdNss,
        .oField = BSON("applyOps" << BSON_ARRAY(firstLevelInsert << nestedApplyOps)),
        .wallClockTime = Date_t::now(),
    }}};

    ReplicatedMetadataDeltas deltas;
    extractReplicatedMetadataDeltasForApplyOps(applyOpsEntry, deltas);

    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_TRUE(deltas.contains(_uuid2));
    EXPECT_EQ(deltas.at(_uuid1).metadata.hash, hash1 ^ hash3);
    EXPECT_EQ(deltas.at(_uuid2).metadata.hash, hash2);
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, FromMigrateCreateWithNoPriorState) {
    // A fromMigrate kCreate entry with no prior UUID in the deltas map should behave like a normal
    // create.
    const NamespaceString cmdNss = _nss1.getCommandNS();

    const BSONObj createOp = BSON("op" << "c"
                                       << "ns" << cmdNss.ns_forTest() << "ui" << _uuid1 << "o"
                                       << BSON("create" << _nss1.coll()) << "fromMigrate" << true);
    const BSONObj applyOpsCmd = BSON("applyOps" << BSON_ARRAY(createOp));

    const repl::OplogEntry applyOpsEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::kAdminCommandNamespace,
        .oField = applyOpsCmd,
        .wallClockTime = Date_t::now(),
    }}};

    ReplicatedMetadataDeltas replicatedMetadataDeltas;
    extractReplicatedMetadataDeltasForApplyOps(applyOpsEntry, replicatedMetadataDeltas);

    EXPECT_EQ(replicatedMetadataDeltas.size(), 1u);
    ASSERT_TRUE(replicatedMetadataDeltas.contains(_uuid1));
    EXPECT_EQ(replicatedMetadataDeltas.at(_uuid1).metadata.sizeCount,
              (CollectionSizeCount{.size = 0, .count = 0}));
    EXPECT_EQ(replicatedMetadataDeltas.at(_uuid1).state, DDLState::kCreated);
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, FromMigrateCreateAfterDrop) {
    // During shard migration, a collection can be dropped from a shard and then migrated back. The
    // drop and create oplog entries share the same UUID. When the fromMigrate kCreate follows a
    // kDrop for the same UUID, the entry should be reset to (0, 0) with
    // DDLState::kDroppedAndRecreated.
    const NamespaceString cmdNss = _nss1.getCommandNS();

    const BSONObj dropOp = BSON("op" << "c"
                                     << "ns" << cmdNss.ns_forTest() << "ui" << _uuid1 << "o"
                                     << BSON("drop" << _nss1.coll()));
    const BSONObj createFromMigrateOp =
        BSON("op" << "c"
                  << "ns" << cmdNss.ns_forTest() << "ui" << _uuid1 << "o"
                  << BSON("create" << _nss1.coll()) << "fromMigrate" << true);
    const BSONObj applyOpsCmd = BSON("applyOps" << BSON_ARRAY(dropOp << createFromMigrateOp));

    const repl::OplogEntry applyOpsEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::kAdminCommandNamespace,
        .oField = applyOpsCmd,
        .wallClockTime = Date_t::now(),
    }}};

    ReplicatedMetadataDeltas replicatedMetadataDeltas;
    extractReplicatedMetadataDeltasForApplyOps(applyOpsEntry, replicatedMetadataDeltas);

    EXPECT_EQ(replicatedMetadataDeltas.size(), 1u);
    ASSERT_TRUE(replicatedMetadataDeltas.contains(_uuid1));
    EXPECT_EQ(replicatedMetadataDeltas.at(_uuid1).metadata.sizeCount,
              (CollectionSizeCount{.size = 0, .count = 0}));
    EXPECT_EQ(replicatedMetadataDeltas.at(_uuid1).state, DDLState::kDroppedAndRecreated);
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, FromMigrateCreateAfterDropThenInserts) {
    // Subsequent inserts after the re-creation should accumulate correctly on the reset entry.
    const NamespaceString cmdNss = _nss1.getCommandNS();
    const BSONObj document = BSON("_id" << 0 << "x" << "hello");

    const BSONObj dropOp = BSON("op" << "c"
                                     << "ns" << cmdNss.ns_forTest() << "ui" << _uuid1 << "o"
                                     << BSON("drop" << _nss1.coll()));
    const BSONObj createFromMigrateOp =
        BSON("op" << "c"
                  << "ns" << cmdNss.ns_forTest() << "ui" << _uuid1 << "o"
                  << BSON("create" << _nss1.coll()) << "fromMigrate" << true);
    const BSONObj insertOp = BSON("op" << "i"
                                       << "ns" << _nss1.ns_forTest() << "ui" << _uuid1 << "o"
                                       << document << "m" << BSON("sz" << document.objsize()));
    const BSONObj applyOpsCmd =
        BSON("applyOps" << BSON_ARRAY(dropOp << createFromMigrateOp << insertOp));

    const repl::OplogEntry applyOpsEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::kAdminCommandNamespace,
        .oField = applyOpsCmd,
        .wallClockTime = Date_t::now(),
    }}};

    ReplicatedMetadataDeltas replicatedMetadataDeltas;
    extractReplicatedMetadataDeltasForApplyOps(applyOpsEntry, replicatedMetadataDeltas);

    EXPECT_EQ(replicatedMetadataDeltas.size(), 1u);
    ASSERT_TRUE(replicatedMetadataDeltas.contains(_uuid1));
    EXPECT_EQ(replicatedMetadataDeltas.at(_uuid1).metadata.sizeCount,
              (CollectionSizeCount{.size = document.objsize(), .count = 1}));
    EXPECT_EQ(replicatedMetadataDeltas.at(_uuid1).state, DDLState::kDroppedAndRecreated);
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, FromMigrateCreateAfterDropSeedsIdentityHash) {
    // The migration re-creates the collection empty, so its hash restarts from the identity just as
    // it would for a fresh create, rather than carrying anything from before the drop.
    const NamespaceString cmdNss = _nss1.getCommandNS();
    const BSONObj document = BSON("_id" << 0 << "x" << "hello");
    const int64_t hash = 0x0123456789abcdef;

    const BSONObj dropOp = BSON("op" << "c"
                                     << "ns" << cmdNss.ns_forTest() << "ui" << _uuid1 << "o"
                                     << BSON("drop" << _nss1.coll()));
    const BSONObj createFromMigrateOp =
        BSON("op" << "c"
                  << "ns" << cmdNss.ns_forTest() << "ui" << _uuid1 << "o"
                  << BSON("create" << _nss1.coll()) << "fromMigrate" << true);
    const BSONObj insertOp =
        BSON("op" << "i"
                  << "ns" << _nss1.ns_forTest() << "ui" << _uuid1 << "o" << document << "m"
                  << BSON("sz" << document.objsize() << "h" << hash));

    // The re-creation on its own starts from the identity.
    {
        const repl::OplogEntry applyOpsEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = NamespaceString::kAdminCommandNamespace,
            .oField = BSON("applyOps" << BSON_ARRAY(dropOp << createFromMigrateOp)),
            .wallClockTime = Date_t::now(),
        }}};

        ReplicatedMetadataDeltas replicatedMetadataDeltas;
        extractReplicatedMetadataDeltasForApplyOps(applyOpsEntry, replicatedMetadataDeltas);

        ASSERT_TRUE(replicatedMetadataDeltas.contains(_uuid1));
        EXPECT_EQ(replicatedMetadataDeltas.at(_uuid1).state, DDLState::kDroppedAndRecreated);
        EXPECT_EQ(replicatedMetadataDeltas.at(_uuid1).metadata.hash,
                  kEmptyCollectionValidationHash);
    }

    // A document migrated back in folds into that identity.
    {
        const repl::OplogEntry applyOpsEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = NamespaceString::kAdminCommandNamespace,
            .oField = BSON("applyOps" << BSON_ARRAY(dropOp << createFromMigrateOp << insertOp)),
            .wallClockTime = Date_t::now(),
        }}};

        ReplicatedMetadataDeltas replicatedMetadataDeltas;
        extractReplicatedMetadataDeltasForApplyOps(applyOpsEntry, replicatedMetadataDeltas);

        ASSERT_TRUE(replicatedMetadataDeltas.contains(_uuid1));
        EXPECT_EQ(replicatedMetadataDeltas.at(_uuid1).state, DDLState::kDroppedAndRecreated);
        EXPECT_EQ(replicatedMetadataDeltas.at(_uuid1).metadata.hash, hash);
    }
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, FromMigrateCreateWithPreExistingWritesFails) {
    // A fromMigrate kCreate for a UUID that already has non-dropped state (e.g. from prior inserts
    // without an intervening drop) should fail with massert 12554002.
    const NamespaceString cmdNss = _nss1.getCommandNS();
    const BSONObj document = BSON("_id" << 0);

    const BSONObj insertOp = BSON("op" << "i"
                                       << "ns" << _nss1.ns_forTest() << "ui" << _uuid1 << "o"
                                       << document << "m" << BSON("sz" << document.objsize()));
    const BSONObj createFromMigrateOp =
        BSON("op" << "c"
                  << "ns" << cmdNss.ns_forTest() << "ui" << _uuid1 << "o"
                  << BSON("create" << _nss1.coll()) << "fromMigrate" << true);
    const BSONObj applyOpsCmd = BSON("applyOps" << BSON_ARRAY(insertOp << createFromMigrateOp));

    const repl::OplogEntry applyOpsEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::kAdminCommandNamespace,
        .oField = applyOpsCmd,
        .wallClockTime = Date_t::now(),
    }}};

    ReplicatedMetadataDeltas replicatedMetadataDeltas;
    ASSERT_THROWS_CODE(
        extractReplicatedMetadataDeltasForApplyOps(applyOpsEntry, replicatedMetadataDeltas),
        DBException,
        12554002);
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest,
       CreateWithExplicitFromMigrateFalseAfterDropIsNotMigrate) {
    // When a kCreate oplog entry has the fromMigrate field explicitly set to false, it must be
    // treated as a normal (non-migrate) create.
    const NamespaceString cmdNss = _nss1.getCommandNS();

    const BSONObj dropOp = BSON("op" << "c"
                                     << "ns" << cmdNss.ns_forTest() << "ui" << _uuid1 << "o"
                                     << BSON("drop" << _nss1.coll()));
    const BSONObj createWithFromMigrateFalseOp =
        BSON("op" << "c"
                  << "ns" << cmdNss.ns_forTest() << "ui" << _uuid1 << "o"
                  << BSON("create" << _nss1.coll()) << "fromMigrate" << false);
    const BSONObj applyOpsCmd =
        BSON("applyOps" << BSON_ARRAY(dropOp << createWithFromMigrateFalseOp));

    const repl::OplogEntry applyOpsEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::kAdminCommandNamespace,
        .oField = applyOpsCmd,
        .wallClockTime = Date_t::now(),
    }}};

    ReplicatedMetadataDeltas replicatedMetadataDeltas;
    ASSERT_THROWS_CODE(
        extractReplicatedMetadataDeltasForApplyOps(applyOpsEntry, replicatedMetadataDeltas),
        DBException,
        12054100);
}

// ===========================================================================
// Mock oplog cursor for aggregateReplicatedMetadataDeltasInOplog() tests.
// ===========================================================================

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

class AggregateSizeCountFromOplogTest : public CatalogTestFixture {
protected:
    using OplogCursorMock = test_helpers::OplogCursorMock;

    /**
     * Describes one inner op to synthesize inside an applyOps oplog entry.
     * For CRUD ops (kInsert/kUpdate/kDelete), set sizeDelta and includeSizeMetadata.
     * For DDL ops (kCommand), set commandObj to the command document (e.g. BSON("create" << coll)).
     */
    struct InnerOpSpec {
        test_helpers::NsAndUUID coll;
        repl::OpTypeEnum opType;
        int32_t sizeDelta = 0;
        bool includeSizeMetadata = true;
        boost::optional<BSONObj> commandObj;
        boost::optional<int64_t> hash;
    };

    /**
     * Builds the BSON array of inner operations for an applyOps entry.
     */
    BSONArray buildApplyOpsInnerOpsArray(const std::vector<InnerOpSpec>& innerOps) const {
        BSONArrayBuilder innerOpsArray;
        for (const auto& spec : innerOps) {
            if (spec.opType == repl::OpTypeEnum::kCommand) {
                BSONObjBuilder opBuilder;
                opBuilder.append("op", "c");
                opBuilder.append("ns", spec.coll.nss.getCommandNS().ns_forTest());
                spec.coll.uuid.appendToBuilder(&opBuilder, "ui");
                opBuilder.append("o", spec.commandObj.value_or(BSONObj()));
                innerOpsArray.append(opBuilder.obj());
                continue;
            }
            std::string_view opStr;
            switch (spec.opType) {
                case repl::OpTypeEnum::kInsert:
                    opStr = "i"sv;
                    break;
                case repl::OpTypeEnum::kUpdate:
                    opStr = "u"sv;
                    break;
                case repl::OpTypeEnum::kDelete:
                    opStr = "d"sv;
                    break;
                default:
                    opStr = "n"sv;
                    break;
            }
            BSONObjBuilder opBuilder;
            opBuilder.append("op", opStr);
            opBuilder.append("ns", spec.coll.nss.ns_forTest());
            spec.coll.uuid.appendToBuilder(&opBuilder, "ui");
            opBuilder.append("o", BSONObj());
            if (spec.includeSizeMetadata) {
                BSONObjBuilder sizeMetadataBuilder;
                sizeMetadataBuilder.append("sz", spec.sizeDelta);
                if (spec.hash) {
                    sizeMetadataBuilder.append("h", *spec.hash);
                }
                opBuilder.append("m", sizeMetadataBuilder.obj());
            }
            innerOpsArray.append(opBuilder.obj());
        }
        return innerOpsArray.arr();
    }

    /**
     * Constructs a synthetic applyOps oplog entry at 'ts' whose inner ops are described by
     * 'innerOps'. Each inner op carries its own UUID so the aggregation code can attribute deltas
     * per-collection.
     */
    repl::OplogEntry makeApplyOpsOplogEntry(const Timestamp ts,
                                            const std::vector<InnerOpSpec>& innerOps) {
        const NamespaceString adminCmdNss = NamespaceString::kAdminCommandNamespace;
        BSONArray innerOpsArray = buildApplyOpsInnerOpsArray(innerOps);
        return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts, 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = BSON("applyOps" << innerOpsArray),
            .wallClockTime = Date_t::now(),
        }};
    }

    /**
     * Shared collection fixtures for synthesizing oplog entries in test scenarios.
     */
    test_helpers::NsAndUUID collA = {
        .nss = NamespaceString::createNamespaceString_forTest("agg_size_count_from_oplog", "collA"),
        .uuid = UUID::gen()};
    test_helpers::NsAndUUID collB = {
        .nss = NamespaceString::createNamespaceString_forTest("agg_size_count_from_oplog", "collB"),
        .uuid = UUID::gen()};

    UUID oplogUuid = UUID::gen();

    // Runs the aggregation with the fixture's oplog UUID and drops the oplog's own self-delta,
    // leaving only the per-collection deltas the tests assert on.
    OplogScanResult aggregateCollectionReplicatedMetadataDeltas(SeekableRecordCursor& oplogCursor,
                                                                const Timestamp& seekAfterTS) {
        auto result = aggregateReplicatedMetadataDeltasInOplog(oplogCursor, seekAfterTS, oplogUuid);
        result.deltas.erase(oplogUuid);
        return result;
    }

    // Test methods should default to asserting aggregate size count via this method.
    void assertExpectedAggregateDelta(const AggregateDeltaExpectation& expected,
                                      const UUID& uuid,
                                      const Timestamp& seekAfterTS,
                                      SeekableRecordCursor& oplogCursor) {
        const auto deltas = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, seekAfterTS);
        ASSERT_TRUE(deltas.deltas.contains(uuid));
        EXPECT_EQ(deltas.deltas.at(uuid).metadata.sizeCount, expected.delta);
        EXPECT_EQ(deltas.lastTimestamp, expected.lastTimestamp);
    }
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
    const auto oplogScanResult = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, ts3);
    EXPECT_EQ(oplogScanResult.deltas.size(), 0u);
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
        EXPECT_EQ(aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min())
                      .deltas.size(),
                  2u);
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
        EXPECT_EQ(aggregateCollectionReplicatedMetadataDeltas(oplogCursor, ts3).deltas.size(), 2u);
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
        const auto oplogScanResult = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, ts5);
        EXPECT_EQ(oplogScanResult.deltas.size(), 0u);
    }

    {
        // Timestamp::max() is too large a value to extract a RecordId from the oplog from.
        ASSERT_THROWS_CODE(
            aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::max()),
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

    const auto result = aggregateCollectionReplicatedMetadataDeltas(*cursor, Timestamp::min());

    // Only the ts1 entry was visible; ts2 must not appear in the deltas.
    EXPECT_EQ(result.deltas.size(), 1u);
    ASSERT_TRUE(result.deltas.count(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.size, 10);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.count, 1);
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

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.size, 0);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.count, 0);
}

TEST_F(AggregateSizeCountFromOplogTest, CollectionCreationSeedsIdentityHash) {
    // A collection created empty has folded in no document hashes, so it starts from the identity
    // rather than from an absent hash.
    const Timestamp ts1{1, 1};
    std::list<repl::OplogEntry> entries{test_helpers::makeCreateOplogEntry(ts1, collA)};
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.hash, kEmptyCollectionValidationHash);
}

TEST_F(AggregateSizeCountFromOplogTest, CollectionCreationThenInsertFoldsIntoIdentity) {
    // The identity leaves the folded contribution unchanged, so a create followed by one insert
    // yields exactly that insert's hash.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const int64_t hash = 0x0123456789abcdef;
    std::list<repl::OplogEntry> entries{
        test_helpers::makeCreateOplogEntry(ts1, collA),
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, 10, hash),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.hash, hash);
}

TEST_F(AggregateSizeCountFromOplogTest, CollectionCreationThenSameSizeUpdateFoldsHash) {
    // A same-size update contributes no size and no count, so the hash is the only evidence it
    // happened. Seeding the create with the identity is what makes that contribution visible.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const int64_t hash = 0x0123456789abcdef;
    std::list<repl::OplogEntry> entries{
        test_helpers::makeCreateOplogEntry(ts1, collA),
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kUpdate, 0, hash),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount,
              (CollectionSizeCount{.size = 0, .count = 0}));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.hash, hash);
}

TEST_F(AggregateSizeCountFromOplogTest, CollectionCreationThenOpWithoutHashInvalidatesIdentity) {
    // The seeded identity is a claim that every contribution so far has been folded in, so an
    // operation that carries none must invalidate it rather than leave it standing.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    std::list<repl::OplogEntry> entries{
        test_helpers::makeCreateOplogEntry(ts1, collA),
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, 10),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_FALSE(result.deltas.at(collA.uuid).metadata.hash);
}

TEST_F(AggregateSizeCountFromOplogTest, InsertThenDeleteOfSameDocumentRestoresIdentityHash) {
    // XOR is its own inverse, so folding a document's hash in and back out again returns the
    // collection to the hash it had when it was empty. This is the property the whole accumulation
    // rests on.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    const int64_t hash = 0x0123456789abcdef;
    std::list<repl::OplogEntry> entries{
        test_helpers::makeCreateOplogEntry(ts1, collA),
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, 100, hash),
        test_helpers::makeOplogEntry(ts3, collA, repl::OpTypeEnum::kDelete, -100, hash),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount,
              (CollectionSizeCount{.size = 0, .count = 0}));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.hash, kEmptyCollectionValidationHash);
}

TEST_F(AggregateSizeCountFromOplogTest, TruncateRangeInvalidatesAccumulatedHash) {
    // Truncation removes records this node never hashed individually, so their contributions can
    // never be XOR-ed back out. The hash must go absent rather than keep a value that still
    // includes them.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    std::list<repl::OplogEntry> entries{
        test_helpers::makeCreateOplogEntry(ts1, collA),
        test_helpers::makeOplogEntry(
            ts2, collA, repl::OpTypeEnum::kInsert, 100, int64_t{0x0123456789abcdef}),
        test_helpers::makeTruncateRangeOplogEntry(ts3,
                                                  collA,
                                                  /*bytesDeleted=*/100,
                                                  /*docsDeleted=*/1),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_FALSE(result.deltas.at(collA.uuid).metadata.hash);
}

TEST_F(AggregateSizeCountFromOplogTest, ImportCollectionLeavesHashAbsent) {
    // An import adopts documents this node never hashed, so there is no baseline to fold into and
    // the hash stays absent, unlike a create which starts from the identity.
    const Timestamp ts1{1, 1};
    std::list<repl::OplogEntry> entries{
        test_helpers::makeImportCollectionOplogEntry(ts1,
                                                     collA,
                                                     /*numRecords=*/5,
                                                     /*dataSize=*/500)};
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_FALSE(result.deltas.at(collA.uuid).metadata.hash);
}

TEST_F(AggregateSizeCountFromOplogTest,
       ImportAfterDropLeavesHashAbsentAndLaterWritesCannotRestoreIt) {
    // The re-created entry has no usable baseline, so writes that do carry a hash must not produce
    // a value again. This is the counterpart to a fromMigrate re-create, which does start from the
    // identity because the migration re-creates the collection empty.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    std::list<repl::OplogEntry> entries{
        test_helpers::makeDropOplogEntry(ts1, collA),
        test_helpers::makeImportCollectionOplogEntry(ts2,
                                                     collA,
                                                     /*numRecords=*/5,
                                                     /*dataSize=*/500),
        test_helpers::makeOplogEntry(
            ts3, collA, repl::OpTypeEnum::kInsert, 100, int64_t{0x0123456789abcdef}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kDroppedAndRecreated);
    EXPECT_FALSE(result.deltas.at(collA.uuid).metadata.hash);
}

TEST_F(AggregateSizeCountFromOplogTest, CollectionDropMarksStateDropped) {
    const Timestamp ts1{1, 1};
    std::list<repl::OplogEntry> entries{test_helpers::makeDropOplogEntry(ts1, collA)};
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

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

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.size, 30);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.count, 2);
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

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kDropped);
}

TEST_F(AggregateSizeCountFromOplogTest, AggregateMixedCrudAndApplyOps) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};

    std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, 50),
        makeApplyOpsOplogEntry(
            ts2, {{collA, repl::OpTypeEnum::kInsert, 30}, {collA, repl::OpTypeEnum::kInsert, 40}}),
        test_helpers::makeOplogEntry(ts3, collB, repl::OpTypeEnum::kDelete, -70),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    // collA: 50 + 30 + 40 = 120 bytes across 3 inserts.
    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = 120, .count = 3}, .lastTimestamp = ts3},
        collA.uuid,
        Timestamp::min(),
        oplogCursor);
    // collB: single delete (-70, -1).
    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = -70, .count = -1}, .lastTimestamp = ts3},
        collB.uuid,
        Timestamp::min(),
        oplogCursor);

    // Seeking after ts1 excludes the first insert but still visits the applyOps.
    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = 70, .count = 2}, .lastTimestamp = ts3},
        collA.uuid,
        ts1,
        oplogCursor);
}

TEST_F(AggregateSizeCountFromOplogTest, AggregateApplyOpsWithMixedSizeMetadata) {
    const Timestamp ts1{1, 1};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{collA, repl::OpTypeEnum::kInsert, 50, /*includeSizeMetadata=*/true},
             {collA, repl::OpTypeEnum::kInsert, 100, /*includeSizeMetadata=*/false},
             {collA, repl::OpTypeEnum::kInsert, 30, /*includeSizeMetadata=*/true}}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = 80, .count = 2}, .lastTimestamp = ts1},
        collA.uuid,
        Timestamp::min(),
        oplogCursor);
}

TEST_F(AggregateSizeCountFromOplogTest, AggregateEmptyOplog) {
    OplogCursorMock oplogCursor({});

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_FALSE(result.lastTimestamp.has_value());
}

TEST_F(AggregateSizeCountFromOplogTest, AggregateOplogWithNoSizeMetadata) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};

    std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert),
        makeApplyOpsOplogEntry(
            ts2,
            {{collA, repl::OpTypeEnum::kInsert, 50, /*includeSizeMetadata=*/false},
             {collB, repl::OpTypeEnum::kInsert, 25, /*includeSizeMetadata=*/false}}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());
    EXPECT_TRUE(result.deltas.empty());
}

// When the fast lanes (Layer 2 / Layer 2.5) see a CRUD entry with `m.sz` but no `ui`, they fall
// through to Layer 3, where the massert id 12116001 in `extractReplicatedMetadataForOpImpl`
// surfaces the invariant violation. Both tests below exercise the fast-lane paths via the cursor.

TEST_F(AggregateSizeCountFromOplogTest, AggregateCrudWithSizeMetadataMissingUiThrows) {
    // A direct CRUD entry that carries `m.sz` but lacks `ui` falls through Layer 2 to Layer 3.
    const Timestamp ts1{1, 1};
    SingleOpSizeMetadata sm;
    sm.setSz(10);
    std::list<repl::OplogEntry> entries{
        repl::OplogEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts1, 1),
            .opType = repl::OpTypeEnum::kInsert,
            .nss = collA.nss,
            // .uuid intentionally omitted
            .oField = BSONObj(),
            .sizeMetadata = repl::OplogEntrySizeMetadata{sm},
            .wallClockTime = Date_t::now(),
        }}},
    };
    OplogCursorMock oplogCursor(std::move(entries));
    ASSERT_THROWS_CODE(aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min()),
                       DBException,
                       12116001);
}

TEST_F(AggregateSizeCountFromOplogTest, AggregateApplyOpsInnerOpMissingUiThrows) {
    // An applyOps whose inner CRUD op carries `m.sz` but lacks `ui` falls through Layer 2.5 to
    // Layer 3 by way of the inner-op fast CRUD path.
    const Timestamp ts1{1, 1};
    BSONObj innerOpNoUi =
        BSON("op" << "i"
                  << "ns" << collA.nss.ns_forTest() << "o" << BSONObj() << "m" << BSON("sz" << 10));
    std::list<repl::OplogEntry> entries{
        repl::OplogEntry{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts1, 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = NamespaceString::kAdminCommandNamespace,
            .oField = BSON("applyOps" << BSON_ARRAY(innerOpNoUi)),
            .wallClockTime = Date_t::now(),
        }}},
    };
    OplogCursorMock oplogCursor(std::move(entries));
    ASSERT_THROWS_CODE(aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min()),
                       DBException,
                       12116001);
}

class AggregateSizeCountFromOplogTxnVisibilityTest : public AggregateSizeCountFromOplogTest {
protected:
    static constexpr int64_t kTestTerm = 1;

    repl::OpTime opTimeAt(const Timestamp ts) const {
        return repl::OpTime(ts, kTestTerm);
    }

    struct ApplyOpsTxnFields {
        bool prepared{false};
        bool isPartialTxn{false};
        // prevOpTime = boost::none means standalone (not part of any chain).
        // prevOpTime = repl::OpTime{} (null optime) means first entry in a chain.
        // prevOpTime = non-null optime means continuation of a chain.
        boost::optional<repl::OpTime> prevOpTime;
    };

    repl::OplogEntry makeApplyOpsOplogEntry(const Timestamp ts,
                                            const std::vector<InnerOpSpec>& innerOps,
                                            const ApplyOpsTxnFields& txnFields) {
        const NamespaceString adminCmdNss = NamespaceString::kAdminCommandNamespace;
        BSONArray innerOpsArray = buildApplyOpsInnerOpsArray(innerOps);

        BSONObjBuilder oBuilder;
        oBuilder.append("applyOps", innerOpsArray);
        if (txnFields.prepared) {
            oBuilder.append("prepare", true);
        }
        if (txnFields.isPartialTxn) {
            oBuilder.append("partialTxn", true);
        }

        return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = opTimeAt(ts),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = oBuilder.obj(),
            .wallClockTime = Date_t::now(),
            .prevWriteOpTimeInTransaction = txnFields.prevOpTime,
        }};
    }

    repl::OplogEntry makeCommitTxnOplogEntry(
        const Timestamp ts,
        std::vector<MultiOpSizeMetadata> sizeMetadata,
        boost::optional<repl::OpTime> prevOpTime = boost::none) {
        const NamespaceString adminCmdNss = NamespaceString::kAdminCommandNamespace;
        return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = opTimeAt(ts),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = BSON("commitTransaction" << 1),
            .sizeMetadata = repl::OplogEntrySizeMetadata{std::move(sizeMetadata)},
            .wallClockTime = Date_t::now(),
            .prevWriteOpTimeInTransaction = prevOpTime,
        }};
    }

    repl::OplogEntry makeAbortTxnOplogEntry(
        const Timestamp ts, boost::optional<repl::OpTime> prevOpTime = boost::none) {
        const NamespaceString adminCmdNss = NamespaceString::kAdminCommandNamespace;
        return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = opTimeAt(ts),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = BSON("abortTransaction" << 1),
            .wallClockTime = Date_t::now(),
            .prevWriteOpTimeInTransaction = prevOpTime,
        }};
    }
};

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest, PreparedTxnBasicVisibilityNoCommit) {
    const Timestamp ts2{2, 2};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts2,
            {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta*/ 50, /*includeSizeMetadata=*/true},
             {collB, repl::OpTypeEnum::kInsert, /*sizeDelta*/ 25, /*includeSizeMetadata=*/true}},
            ApplyOpsTxnFields{.prepared = true}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());
    EXPECT_TRUE(result.deltas.empty());
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest, PreparedTxnBasicVisibilityWithCommit) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{2, 2};

    MultiOpSizeMetadata metaA;
    metaA.setUuid(collA.uuid);
    metaA.setSz(50);
    metaA.setCt(1);

    MultiOpSizeMetadata metaB;
    metaB.setUuid(collB.uuid);
    metaB.setSz(25);
    metaB.setCt(1);

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(ts1,
                               {{collA,
                                 repl::OpTypeEnum::kInsert,
                                 /*sizeDelta*/ 50,
                                 /*includeSizeMetadata=*/true},
                                {collB,
                                 repl::OpTypeEnum::kInsert,
                                 /*sizeDelta*/ 25,
                                 /*includeSizeMetadata=*/true}},
                               ApplyOpsTxnFields{.prepared = true, .prevOpTime = repl::OpTime{}}),
        makeCommitTxnOplogEntry(ts2, {metaA, metaB}, opTimeAt(ts1)),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    EXPECT_EQ(
        aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min()).deltas.size(),
        2u);
    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = 50, .count = 1}, .lastTimestamp = ts2},
        collA.uuid,
        Timestamp::min(),
        oplogCursor);
    assertExpectedAggregateDelta(
        {.delta = CollectionSizeCount{.size = 25, .count = 1}, .lastTimestamp = ts2},
        collB.uuid,
        Timestamp::min(),
        oplogCursor);
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest, NotPreparedChainAccountedForAtTerminalEntry) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        makeApplyOpsOplogEntry(
            ts2,
            {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/200}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = opTimeAt(ts1)}),
        makeApplyOpsOplogEntry(ts3,
                               {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/50}},
                               ApplyOpsTxnFields{.prevOpTime = opTimeAt(ts2)}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount,
              (CollectionSizeCount{.size = 350, .count = 3}));
    EXPECT_EQ(result.lastTimestamp, ts3);
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest,
       NonTxnEntryInterleavedInOpenChainDropsChainDeltas) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        test_helpers::makeOplogEntry(ts2, collB, repl::OpTypeEnum::kInsert, /*sizeDelta=*/70),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    // collA's partial chain (100 bytes) is discarded when the non-txn entry interrupts it.
    EXPECT_FALSE(result.deltas.contains(collA.uuid));
    // collB's regular insert is still counted normally.
    ASSERT_TRUE(result.deltas.contains(collB.uuid));
    EXPECT_EQ(result.deltas.at(collB.uuid).metadata.sizeCount,
              (CollectionSizeCount{.size = 70, .count = 1}));
}

using AggregateSizeCountFromOplogTxnVisibilityDeathTest =
    AggregateSizeCountFromOplogTxnVisibilityTest;


TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest, PartialTxnOpenAtEndOfLogIsDiscarded) {
    // Verifies that an in-flight partial chain that reaches end-of-cursor without a terminal
    // entry contributes nothing to the result (the commit has not appeared within the scanned
    // oplog range).
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        makeApplyOpsOplogEntry(
            ts2,
            {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/200}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = opTimeAt(ts1)}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    EXPECT_FALSE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.lastTimestamp, ts2);
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest,
       MultiEntryPreparedTxnUsesCommitSizeMetadataNotPartialAccumulation) {
    // Verifies that for a prepared transaction with multiple partial applyOps entries, the
    // accumulated partial-chain state is discarded at the prepare and the commitTransaction's
    // size metadata is the authoritative source of truth. In production the commit metadata
    // will always equal the sum of the partial entries; the discrepancy here is intentional to
    // confirm the discard behavior under test rather than an accident of matching values.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    const Timestamp ts4{1, 4};

    MultiOpSizeMetadata commitMeta;
    commitMeta.setUuid(collA.uuid);
    commitMeta.setSz(300);
    commitMeta.setCt(3);

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        makeApplyOpsOplogEntry(
            ts2,
            {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/150}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = opTimeAt(ts1)}),
        makeApplyOpsOplogEntry(ts3,
                               {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/50}},
                               ApplyOpsTxnFields{.prepared = true, .prevOpTime = opTimeAt(ts2)}),
        makeCommitTxnOplogEntry(ts4, {commitMeta}, opTimeAt(ts3)),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    // The commit's size metadata (300 bytes, 3 docs) is used, not the sum of the partial
    // entries (100+150 = 250 bytes) or the prepared applyOps (50 bytes).
    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount,
              (CollectionSizeCount{.size = 300, .count = 3}));
    EXPECT_EQ(result.lastTimestamp, ts4);
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest, PreparedTxnFollowedByAbortProducesNoDeltas) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(ts1,
                               {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100}},
                               ApplyOpsTxnFields{.prepared = true, .prevOpTime = repl::OpTime{}}),
        makeAbortTxnOplogEntry(ts2, opTimeAt(ts1)),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    EXPECT_TRUE(result.deltas.empty());
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest,
       PartialChainFollowedByAbortWithoutPrepareProducesNoDeltas) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        makeApplyOpsOplogEntry(
            ts2,
            {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/150}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = opTimeAt(ts1)}),
        makeAbortTxnOplogEntry(ts3, opTimeAt(ts2)),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    EXPECT_TRUE(result.deltas.empty());
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest,
       ChainedTxnWithCreateThenInsertHasKCreatedState) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{.coll = collA,
              .opType = repl::OpTypeEnum::kCommand,
              .includeSizeMetadata = false,
              .commandObj = BSON("create" << collA.nss.coll())}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        makeApplyOpsOplogEntry(ts2,
                               {{collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/100}},
                               ApplyOpsTxnFields{.prevOpTime = opTimeAt(ts1)}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount,
              (CollectionSizeCount{.size = 100, .count = 1}));
}

// ---------------------------------------------------------------------------
// mergeDeltas: a committed transaction chain's buffered deltas folded into the scan result.
// ---------------------------------------------------------------------------

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest, ChainedTxnCreateThenSameSizeUpdateFoldsHash) {
    // A same-size update contributes no size and no count, so the hash is the only evidence the
    // transaction touched the collection at all. Merging must fold it into the created collection's
    // seeded identity rather than skip it for having nothing else to contribute.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const int64_t hash = 0x0123456789abcdef;

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{.coll = collA,
              .opType = repl::OpTypeEnum::kCommand,
              .includeSizeMetadata = false,
              .commandObj = BSON("create" << collA.nss.coll())}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        makeApplyOpsOplogEntry(
            ts2,
            {{.coll = collA, .opType = repl::OpTypeEnum::kUpdate, .sizeDelta = 0, .hash = hash}},
            ApplyOpsTxnFields{.prevOpTime = opTimeAt(ts1)}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount,
              (CollectionSizeCount{.size = 0, .count = 0}));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.hash, hash);
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest,
       ChainedTxnFoldsHashesOfEveryCollectionTouched) {
    // Each collection in the chain keeps its own hash; contributions must not cross UUIDs.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const int64_t hashA = 0x1111111111111111;
    const int64_t hashB = 0x2222222222222222;

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{.coll = collA, .opType = repl::OpTypeEnum::kInsert, .sizeDelta = 10, .hash = hashA}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        makeApplyOpsOplogEntry(
            ts2,
            {{.coll = collB, .opType = repl::OpTypeEnum::kInsert, .sizeDelta = 20, .hash = hashB}},
            ApplyOpsTxnFields{.prevOpTime = opTimeAt(ts1)}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    ASSERT_TRUE(result.deltas.contains(collB.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.hash, hashA);
    EXPECT_EQ(result.deltas.at(collB.uuid).metadata.hash, hashB);
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest, ChainedTxnHashDiscardedWithUnterminatedChain) {
    // A chain that never reaches its terminal entry contributes nothing, hash included.
    const Timestamp ts1{1, 1};

    std::list<repl::OplogEntry> entries{makeApplyOpsOplogEntry(
        ts1,
        {{.coll = collA,
          .opType = repl::OpTypeEnum::kInsert,
          .sizeDelta = 10,
          .hash = 0x0123456789abcdef}},
        ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}})};
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    EXPECT_FALSE(result.deltas.contains(collA.uuid));
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest, PreparedTxnCommitInvalidatesHash) {
    // A prepared transaction's per-operation hashes are dropped at the prepare, and the
    // commitTransaction entry summarizes only size and count. The collection hash therefore goes
    // absent rather than keeping a value that is missing the transaction's contributions.
    // TODO SERVER-133315: Carry the transaction's accumulated hash on the commitTransaction entry.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};

    MultiOpSizeMetadata metaA;
    metaA.setUuid(collA.uuid);
    metaA.setSz(50);
    metaA.setCt(1);

    std::list<repl::OplogEntry> entries{
        test_helpers::makeCreateOplogEntry(ts1, collA),
        makeApplyOpsOplogEntry(ts2,
                               {{.coll = collA,
                                 .opType = repl::OpTypeEnum::kInsert,
                                 .sizeDelta = 50,
                                 .hash = 0x0123456789abcdef}},
                               ApplyOpsTxnFields{.prepared = true, .prevOpTime = repl::OpTime{}}),
        makeCommitTxnOplogEntry(ts3, {metaA}, opTimeAt(ts2)),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount,
              (CollectionSizeCount{.size = 50, .count = 1}));
    EXPECT_FALSE(result.deltas.at(collA.uuid).metadata.hash);
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest, ChainedTxnWithCreateThenDropCancelsOut) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{.coll = collA,
              .opType = repl::OpTypeEnum::kCommand,
              .includeSizeMetadata = false,
              .commandObj = BSON("create" << collA.nss.coll())}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        makeApplyOpsOplogEntry(ts2,
                               {{.coll = collA,
                                 .opType = repl::OpTypeEnum::kCommand,
                                 .includeSizeMetadata = false,
                                 .commandObj = BSON("drop" << collA.nss.coll())}},
                               ApplyOpsTxnFields{.prevOpTime = opTimeAt(ts1)}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    // A create followed by a drop within the same transaction chain cancels out.
    EXPECT_FALSE(result.deltas.contains(collA.uuid));
}

DEATH_TEST_F(AggregateSizeCountFromOplogTxnVisibilityDeathTest,
             ChainedTxnDropAssertsInMergeDeltas,
             "12406403") {
    // Drops are disallowed in multi document transactions. Verifies that chained transaction
    // parsing throws if there is a drop within a transaction as the code is not written to support
    // such an operation.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};

    std::list<repl::OplogEntry> entries{
        // Non-txn insert establishes collA in the global result with kNone state.
        test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, /*sizeDelta=*/50),
        makeApplyOpsOplogEntry(
            ts2,
            {{.coll = collA,
              .opType = repl::OpTypeEnum::kCommand,
              .includeSizeMetadata = false,
              .commandObj = BSON("drop" << collA.nss.coll())}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        makeApplyOpsOplogEntry(ts3,
                               {{collB, repl::OpTypeEnum::kInsert, /*sizeDelta=*/30}},
                               ApplyOpsTxnFields{.prevOpTime = opTimeAt(ts2)}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest,
       NoSessionPartialTxnChainAccountedForAtTerminalEntry) {
    // Verifies that large batched operations (partialTxn: true, no lsid/txnNumber) are tracked
    // via prevOpTime linkage and only become visible at the terminal entry.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{collA, repl::OpTypeEnum::kDelete, /*sizeDelta=*/-100}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        makeApplyOpsOplogEntry(
            ts2,
            {{collA, repl::OpTypeEnum::kDelete, /*sizeDelta=*/-150}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = opTimeAt(ts1)}),
        makeApplyOpsOplogEntry(ts3,
                               {{collA, repl::OpTypeEnum::kDelete, /*sizeDelta=*/-50}},
                               ApplyOpsTxnFields{.prevOpTime = opTimeAt(ts2)}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount,
              (CollectionSizeCount{.size = -300, .count = -3}));
    EXPECT_EQ(result.lastTimestamp, ts3);
}

TEST_F(AggregateSizeCountFromOplogTxnVisibilityTest, NoSessionPartialTxnOpenAtEndOfLogIsDiscarded) {
    // Verifies that an in-flight no-session batched chain that reaches end-of-cursor without a
    // terminal entry contributes nothing to the result.
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{collA, repl::OpTypeEnum::kDelete, /*sizeDelta=*/-100}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = repl::OpTime{}}),
        makeApplyOpsOplogEntry(
            ts2,
            {{collA, repl::OpTypeEnum::kDelete, /*sizeDelta=*/-150}},
            ApplyOpsTxnFields{.isPartialTxn = true, .prevOpTime = opTimeAt(ts1)}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    EXPECT_FALSE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.lastTimestamp, ts2);
}

TEST_F(AggregateSizeCountFromOplogTest, ImportCollectionCreatesEntry) {
    const Timestamp ts1{1, 1};
    const int64_t numRecords = 100;
    const int64_t dataSize = 5000;
    std::list<repl::OplogEntry> entries{
        test_helpers::makeImportCollectionOplogEntry(ts1, collA, numRecords, dataSize)};
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.count, numRecords);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.size, dataSize);
}

TEST_F(AggregateSizeCountFromOplogTest, DryRunImportCollectionIsIgnored) {
    const Timestamp ts1{1, 1};
    const int64_t numRecords = 100;
    const int64_t dataSize = 5000;
    std::list<repl::OplogEntry> entries{test_helpers::makeImportCollectionOplogEntry(
        ts1, collA, numRecords, dataSize, /*dryRun=*/true)};
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    EXPECT_FALSE(result.deltas.contains(collA.uuid));
}

TEST_F(AggregateSizeCountFromOplogTest, ImportCollectionAndInsert) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};

    const int64_t importedNumRecords = 50;
    const int64_t importedDataSize = 2000;

    const int64_t sizeDelta1 = 100;
    const int64_t sizeDelta2 = 200;
    std::list<repl::OplogEntry> entries{
        test_helpers::makeImportCollectionOplogEntry(
            ts1, collA, importedNumRecords, importedDataSize),
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, sizeDelta1),
        test_helpers::makeOplogEntry(ts3, collA, repl::OpTypeEnum::kInsert, sizeDelta2),
    };

    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.count, importedNumRecords + 1 + 1);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.size,
              importedDataSize + sizeDelta1 + sizeDelta2);
}

TEST_F(AggregateSizeCountFromOplogTest, ImportCollectionInsertAndDrop) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    const Timestamp ts4{1, 4};

    const int64_t importedNumRecords = 50;
    const int64_t importedDataSize = 2000;

    const int64_t sizeDelta1 = 100;

    const int64_t sizeDelta2 = 200;
    std::list<repl::OplogEntry> entries{
        test_helpers::makeImportCollectionOplogEntry(
            ts1, collA, importedNumRecords, importedDataSize),
        test_helpers::makeOplogEntry(ts2, collA, repl::OpTypeEnum::kInsert, sizeDelta1),
        test_helpers::makeOplogEntry(ts3, collA, repl::OpTypeEnum::kInsert, sizeDelta2),
        test_helpers::makeDropOplogEntry(ts4, collA),
    };

    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    EXPECT_FALSE(result.deltas.contains(collA.uuid));
}

TEST_F(AggregateSizeCountFromOplogTest, ImportCollectionFilterWithMatchingUUID) {
    const Timestamp ts1{1, 1};
    const int64_t numRecords = 100;
    const int64_t dataSize = 5000;
    std::list<repl::OplogEntry> entries{
        test_helpers::makeImportCollectionOplogEntry(ts1, collA, numRecords, dataSize)};
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.count, numRecords);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.size, dataSize);
}

TEST_F(AggregateSizeCountFromOplogTest, DropThenImportCollection) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const int64_t numRecords = 100;
    const int64_t dataSize = 5000;
    std::list<repl::OplogEntry> entries{
        test_helpers::makeDropOplogEntry(ts1, collA),
        test_helpers::makeImportCollectionOplogEntry(ts2, collA, numRecords, dataSize),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kDroppedAndRecreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.count, numRecords);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.size, dataSize);
}

TEST_F(AggregateSizeCountFromOplogTest, DropThenImportCollectionThenInserts) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const Timestamp ts3{1, 3};
    const int64_t numRecords = 50;
    const int64_t dataSize = 2000;
    const int64_t sizeDelta = 100;
    std::list<repl::OplogEntry> entries{
        test_helpers::makeDropOplogEntry(ts1, collA),
        test_helpers::makeImportCollectionOplogEntry(ts2, collA, numRecords, dataSize),
        test_helpers::makeOplogEntry(ts3, collA, repl::OpTypeEnum::kInsert, sizeDelta),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kDroppedAndRecreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.count, numRecords + 1);
    EXPECT_EQ(result.deltas.at(collA.uuid).metadata.sizeCount.size, dataSize + sizeDelta);
}

TEST_F(AggregateSizeCountFromOplogTest, ImportCollectionWithPreExistingWritesFails) {
    const Timestamp ts1{1, 1};
    const Timestamp ts2{1, 2};
    const int64_t sizeDelta = 50;
    const int64_t numRecords = 1;
    const int64_t dataSize = 100;
    std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(ts1, collA, repl::OpTypeEnum::kInsert, sizeDelta),
        test_helpers::makeImportCollectionOplogEntry(ts2, collA, numRecords, dataSize),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    ASSERT_THROWS_CODE(aggregateCollectionReplicatedMetadataDeltas(oplogCursor, Timestamp::min()),
                       DBException,
                       12601900);
}

// Verifies that the local `isFastCountEligibleNonStore` helper agrees with the canonical
// `isReplicatedFastCountEligible` for every namespace outside the fast-count-store. The fast-scan
// cursor path uses the local helper to skip two redundant `NamespaceString` constructions per
// CRUD entry. This test guards against future drift if either function's predicates change.
TEST(FastCountEligibilityParityTest, NonStoreNamespacesAgreeWithCanonical) {
    struct TestCase {
        std::string_view db;
        std::string_view coll;
    };
    const std::vector<TestCase> cases{
        // Eligible user collections.
        {"mydb"sv, "users"sv},
        {"app"sv, "events"sv},
        // Ineligible: local DB.
        {"local"sv, "oplog.rs"sv},
        {"local"sv, "replset.minvalid"sv},
        {"local"sv, "anything"sv},
        // Ineligible: server configuration collection (admin.system.version).
        {"admin"sv, "system.version"sv},
        // Ineligible: system.profile in any user DB.
        {"mydb"sv, "system.profile"sv},
        {"app"sv, "system.profile"sv},
    };

    for (const auto& c : cases) {
        const auto nss = NamespaceString::createNamespaceString_forTest(c.db, c.coll);
        EXPECT_EQ(isReplicatedFastCountEligible(nss), isFastCountEligibleNonStore(nss))
            << "Disagreement for " << nss.toStringForErrorMsg();
    }
}
}  // namespace
}  // namespace mongo::replicated_fast_count
