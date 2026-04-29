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

#include "mongo/db/replicated_fast_count/replicated_fast_count_delta_utils.h"

#include "mongo/db/dbhelpers.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_enabled.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_manager.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"

namespace mongo::replicated_fast_count {
namespace {

class ReadAndIncrementSizeCountsTest : public CatalogTestFixture {};

TEST_F(ReadAndIncrementSizeCountsTest, IncrementZeros) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid = UUID::gen();
    SizeCountDeltas deltas;
    deltas[uuid] = SizeCountDelta{.sizeCount = {0, 0}, .state = DDLState::kNone};

    // Read before the document exists.
    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid].sizeCount.size, 0);
    EXPECT_EQ(deltas[uuid].sizeCount.count, 0);

    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid, {.timestamp = Timestamp(1, 1), .size = 0, .count = 0});

    // Read after (0,0) document exists.
    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid].sizeCount.size, 0);
    EXPECT_EQ(deltas[uuid].sizeCount.count, 0);
}

TEST_F(ReadAndIncrementSizeCountsTest, NegativeResult) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    SizeCountDeltas deltas;
    deltas[uuid] =
        SizeCountDelta{.sizeCount = {.size = -400, .count = -20}, .state = DDLState::kNone};

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid].sizeCount.size, -200);
    EXPECT_EQ(deltas[uuid].sizeCount.count, -10);
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {}
 * document UUIDs ∩ delta UUIDs = {}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadEmptySet) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid1, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid2, {.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

    SizeCountDeltas deltas;

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_TRUE(deltas.empty());
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {uuid1, uuid2}
 * document UUIDs ∩ delta UUIDs = {uuid1, uuid2}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentEqualSet) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid1, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid2, {.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

    SizeCountDeltas deltas;
    deltas[uuid1] = SizeCountDelta{.sizeCount = {5, 1}, .state = DDLState::kNone};
    deltas[uuid2] = SizeCountDelta{.sizeCount = {50, 10}, .state = DDLState::kNone};

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 2);
    EXPECT_EQ(deltas[uuid1].sizeCount.size, 205);
    EXPECT_EQ(deltas[uuid1].sizeCount.count, 11);
    EXPECT_EQ(deltas[uuid2].sizeCount.size, 150);
    EXPECT_EQ(deltas[uuid2].sizeCount.count, 15);
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {uuid1}
 * document UUIDs ∩ delta UUIDs = {uuid1}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentSubset) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid1, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid2, {.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

    SizeCountDeltas deltas;
    deltas[uuid1] = SizeCountDelta{.sizeCount = {5, 1}, .state = DDLState::kNone};

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid1].sizeCount.size, 205);
    EXPECT_EQ(deltas[uuid1].sizeCount.count, 11);
}

/**
 * document UUIDs:  {uuid1}
 * delta UUIDs:     {uuid1, uuid2}
 * document UUIDs ∩ delta UUIDs = {uuid1}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentSuperset) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid1, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    SizeCountDeltas deltas;
    deltas[uuid1] = SizeCountDelta{.sizeCount = {5, 1}, .state = DDLState::kNone};
    deltas[uuid2] = SizeCountDelta{.sizeCount = {50, 10}, .state = DDLState::kNone};

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 2);
    EXPECT_EQ(deltas[uuid1].sizeCount.size, 205);
    EXPECT_EQ(deltas[uuid1].sizeCount.count, 11);
    EXPECT_EQ(deltas[uuid2].sizeCount.size, 50);
    EXPECT_EQ(deltas[uuid2].sizeCount.count, 10);
}

/**
 * document UUIDs:  {uuid1, uuid2}
 * delta UUIDs:     {uuid3}
 * document UUIDs ∩ delta UUIDs = {}
 */
TEST_F(ReadAndIncrementSizeCountsTest, ReadDocumentsDisjointSet) {
    ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    SizeCountStore store;

    const UUID uuid1 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid1, {.timestamp = Timestamp(1, 1), .size = 200, .count = 10});

    const UUID uuid2 = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(), store, uuid2, {.timestamp = Timestamp(1, 1), .size = 100, .count = 5});

    const UUID uuid3 = UUID::gen();
    SizeCountDeltas deltas;
    deltas[uuid3] = SizeCountDelta{.sizeCount = {5, 1}, .state = DDLState::kNone};

    readAndIncrementSizeCounts(operationContext(), deltas);

    EXPECT_EQ(deltas.size(), 1);
    EXPECT_EQ(deltas[uuid3].sizeCount.size, 5);
    EXPECT_EQ(deltas[uuid3].sizeCount.count, 1);
}

// ---------------------------------------------------------------------------
// Helpers for constructing oplog entries with size metadata.
// ---------------------------------------------------------------------------

repl::OplogEntrySizeMetadata makeOperationSizeMetadata(int32_t replicatedSizeDelta) {
    SingleOpSizeMetadata m;
    m.setSz(replicatedSizeDelta);
    return m;
}

repl::OplogEntry makeOplogEntryWithSizeMetadata(const NamespaceString& nss,
                                                repl::OpTypeEnum opType,
                                                int32_t sizeDelta) {

    auto sizeMetadata = makeOperationSizeMetadata(sizeDelta);
    return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = opType,
        .nss = nss,
        .uuid = UUID::gen(),
        .oField = BSONObj(),
        .sizeMetadata = sizeMetadata,
        .wallClockTime = Date_t::now(),
    }};
}

// ===========================================================================
// Test fixture for extractSizeCountDeltaForOp() -- lightweight, no real writes.
// ===========================================================================

class ExtractSizeCountDeltaTest : public CatalogTestFixture {
protected:
    NamespaceString _nss1 = NamespaceString::createNamespaceString_forTest(
        "replicated_fast_count_delta_utils_test", "coll1");
};

TEST_F(ExtractSizeCountDeltaTest, ExtractSizeCountDeltaForInsert) {
    const int32_t sizeDelta = 400;
    const auto insertOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kInsert, sizeDelta);
    const auto extractedSizeCount = replicated_fast_count::extractSizeCountDeltaForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    // Insert means count increases by 1.
    ASSERT_EQ(1, extractedSizeCount->count);
    ASSERT_EQ(sizeDelta, extractedSizeCount->size);
}

TEST_F(ExtractSizeCountDeltaTest, ExtractSizeCountDeltaForUpdate) {
    const int32_t sizeDelta = 400;
    const auto insertOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kUpdate, sizeDelta);
    const auto extractedSizeCount = replicated_fast_count::extractSizeCountDeltaForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    // Updates imply no new documents, count delta is 0.
    ASSERT_EQ(0, extractedSizeCount->count);
    ASSERT_EQ(sizeDelta, extractedSizeCount->size);
}

TEST_F(ExtractSizeCountDeltaTest, ExtractSizeCountDeltaForDelete) {
    const int32_t sizeDelta = 400;
    const auto insertOp =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kDelete, sizeDelta);
    const auto extractedSizeCount = replicated_fast_count::extractSizeCountDeltaForOp(insertOp);
    ASSERT(extractedSizeCount.has_value());

    // Delete implies one less document.
    ASSERT_EQ(-1, extractedSizeCount->count);
    ASSERT_EQ(sizeDelta, extractedSizeCount->size);
}

TEST_F(ExtractSizeCountDeltaTest, NoSizeCountDeltaWhenAbsentFromOplogEntry) {
    // 'OpTypeEnum::kInsert' supports replicated fast count information, but none is extracted
    // because the 'm' field is absent from the oplog entry.
    repl::OplogEntry insertOpNoSizeMetadata{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kInsert,
        .nss = _nss1,
        .oField = BSONObj(),
        .wallClockTime = Date_t::now(),
    }}};
    const auto extractedSizeCount =
        replicated_fast_count::extractSizeCountDeltaForOp(insertOpNoSizeMetadata);
    ASSERT_FALSE(insertOpNoSizeMetadata.getSizeMetadata().has_value());
}

TEST_F(ExtractSizeCountDeltaTest, NoSizeCountDeltaWhenAbsentAndIncompatibleOpType) {
    // 'OpTypeEnum::kCommand' does not support top level 'sizeMetadata' field 'm', and in absence of
    // the 'sizeMetadata', nothing is returned when trying to extract size count deltas.
    repl::OplogEntry commandOpNoSizeMetadata{repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = _nss1,
        .oField = BSONObj(),
        .wallClockTime = Date_t::now(),
    }}};
    const auto extractedSizeCount =
        replicated_fast_count::extractSizeCountDeltaForOp(commandOpNoSizeMetadata);
    ASSERT_FALSE(commandOpNoSizeMetadata.getSizeMetadata().has_value());
}

TEST_F(ExtractSizeCountDeltaTest, ExtractSizeCountDeltaOnUnsupportedOpType) {
    const auto oplogEntry =
        makeOplogEntryWithSizeMetadata(_nss1, repl::OpTypeEnum::kNoop, 400 /* sizeDelta */);

    // Size metadata is only supported for 'insert', 'delete', and 'update' operations. All other
    // operations are incompatible with a top-level 'm' field.
    EXPECT_EQ(replicated_fast_count::extractSizeCountDeltaForOp(oplogEntry), boost::none);
}

TEST_F(ExtractSizeCountDeltaTest, ExtractSizeCountDeltaOnNonEligibleNss) {
    const NamespaceString localNss =
        NamespaceString::createNamespaceString_forTest("local", "coll1");
    EXPECT_FALSE(isReplicatedFastCountEligible(localNss));

    const auto oplogEntry =
        makeOplogEntryWithSizeMetadata(localNss, repl::OpTypeEnum::kNoop, 400 /* sizeDelta */);

    // Even though the oplog entry carries size metadata, ineligible namespaces should be skipped.
    ASSERT_FALSE(replicated_fast_count::extractSizeCountDeltaForOp(oplogEntry).has_value());
}

TEST_F(ExtractSizeCountDeltaTest, ExtractSizeCountDeltaOnNonEligibleNssWithoutSizeMetadata) {
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
    ASSERT_FALSE(replicated_fast_count::extractSizeCountDeltaForOp(insertOpLocalNs).has_value());
}

// ===========================================================================
// Test fixture for extractSizeCountDeltasForApplyOps() -- needs full infrastructure for real
// writes.
// ===========================================================================

class ExtractSizeCountDeltaForApplyOpsTest : public CatalogTestFixture {
public:
    ExtractSizeCountDeltaForApplyOpsTest()
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

        setUpReplicatedFastCount(_opCtx);

        ASSERT_OK(createCollection(_opCtx, _nss1.dbName(), BSON("create" << _nss1.coll())));
        ASSERT_OK(createCollection(_opCtx, _nss2.dbName(), BSON("create" << _nss2.coll())));
        {
            AutoGetCollection coll1(_opCtx, _nss1, LockMode::MODE_IS);
            AutoGetCollection coll2(_opCtx, _nss2, LockMode::MODE_IS);
            _uuid1 = coll1->uuid();
            _uuid2 = coll2->uuid();
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
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const std::vector<BSONObj> docs{
        BSON("_id" << 0),
        BSON("_id" << 1),
        BSON("_id" << 2),
    };

    // Confirm this starts with an empty collection.
    ASSERT_EQ(CollectionSizeCount{},
              replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1));
    {
        // Insert documents and confirm the aggregation.
        auto acq = acquireCollForWrite(_nss1);
        WriteUnitOfWork wuow{_opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations};
        ASSERT_OK(Helpers::insert(_opCtx, acq.getCollectionPtr(), docs));
        wuow.commit();
    }

    // Size and count were both 0 before the operation, so we expect the deltas to aggregate to the
    // totals.
    CollectionSizeCount totalCollSizeCount0 =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);

    // Validate extracted deltas for first round of applyOps.
    const auto deltas0 = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(1u, deltas0.size());
    ASSERT_TRUE(deltas0.contains(_uuid1));
    ASSERT_EQ(totalCollSizeCount0, deltas0.at(_uuid1));

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
    const auto totalCollSizeCount1 =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas1 = totalCollSizeCount1 - totalCollSizeCount0;

    // Validate extracted deltas for second round of applyOps.
    const auto deltas1 = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(1u, deltas1.size());
    ASSERT_TRUE(deltas1.contains(_uuid1));
    ASSERT_EQ(expectedDeltas1, deltas1.at(_uuid1));
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, ExtractSizeCountDeltaForApplyOpsUpdatesSingleUUID) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
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
    CollectionSizeCount originalSizeCount =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);

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
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas = sizeCountAfterUpdates - originalSizeCount;

    const auto deltas = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(1u, deltas.size());
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_EQ(expectedDeltas, deltas.at(_uuid1));
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, ExtractSizeCountDeltaForApplyOpsDeletesSingleUUID) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
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
    CollectionSizeCount originalSizeCount =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);

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
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas = sizeCountAfterUpdates - originalSizeCount;

    const auto deltas = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(1u, deltas.size());
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_EQ(expectedDeltas, deltas.at(_uuid1));
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, ExtractSizeCountDeltaForApplyOpsMultiOpsSingleUUID) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
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
    const auto expectedDeltas =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    ASSERT_EQ(1, expectedDeltas.count);
    ASSERT_NE(expectedDeltas.size, doc0.objsize());

    // Deltas correctly account for inserts, update, and delete which impact each other.
    const auto deltas = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(1u, deltas.size());
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_EQ(expectedDeltas, deltas.at(_uuid1));
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, ExtractSizeCountDeltaForApplyOpsMultiUUID) {
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);
    const BSONObj doc1 = BSON("_id" << 0 << "x" << "0");
    const BSONObj doc2 = BSON("_id" << 1 << "x" << "0" << "y" << 1);

    // Both collections begin empty.
    ASSERT_EQ(CollectionSizeCount{},
              replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1));
    ASSERT_EQ(CollectionSizeCount{},
              replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss2));

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
    const auto expectedDeltas1 =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss1);
    const auto expectedDeltas2 =
        replicated_fast_count_test_helpers::scanForAccurateSizeCount(_opCtx, _nss2);
    ASSERT_EQ(expectedDeltas1.count, 1);
    ASSERT_EQ(expectedDeltas1.size, doc1.objsize());
    ASSERT_EQ(expectedDeltas2.count, 1);
    ASSERT_EQ(expectedDeltas2.size, doc2.objsize());

    // Extract applyOps deltas and verify.
    auto deltas = replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
        replicated_fast_count_test_helpers::getLatestApplyOpsForNss(_opCtx, _nss1));
    ASSERT_EQ(deltas.size(), 2u);
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_TRUE(deltas.contains(_uuid2));
    ASSERT_EQ(deltas.at(_uuid1), expectedDeltas1);
    ASSERT_EQ(deltas.at(_uuid2), expectedDeltas2);
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
    ASSERT_THROWS_CODE(replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
                           ungroupedInsertOplogEntry),
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
    ASSERT_THROWS_CODE(replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(
                           applyOpsEntryMissingInnerUi),
                       DBException,
                       12116001);
}

TEST_F(ExtractSizeCountDeltaForApplyOpsTest, ExtractSizeCountDeltaForNestedApplyOpsMultiUUID) {
    // Nested applyOps are allowed from user commands. Tests that extraction of replicated size and
    // count works across nested applyOps for multiple collections.
    RAIIServerParameterControllerForTest featureFlag("featureFlagReplicatedFastCount", true);

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

    const auto deltas =
        replicated_fast_count_test_helpers::extractSizeCountDeltasForApplyOps(applyOpsEntry);

    ASSERT_EQ(deltas.size(), 2u);
    ASSERT_TRUE(deltas.contains(_uuid1));
    ASSERT_TRUE(deltas.contains(_uuid2));
    ASSERT_EQ(deltas.at(_uuid1), expectedDeltasNss1);
    ASSERT_EQ(deltas.at(_uuid2), expectedDeltasNss2);
}

// ===========================================================================
// Mock oplog cursor for aggregateSizeCountDeltasInOplog() tests.
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

/**
 * Test methods should default to testing aggregate size count with this method, as it checks
 * both methods of aggregation (acquiring a map of deltas across uuids and aggregating deltas
 * for a single uuid) yield equivalent results for the 'uuid'.
 */
void assertExpectedAggregateDelta(const AggregateDeltaExpectation& expected,
                                  const UUID& uuid,
                                  const Timestamp& seekAfterTS,
                                  SeekableRecordCursor& oplogCursor) {
    // Deltas across UUIDs.
    const auto deltas = aggregateSizeCountDeltasInOplog(oplogCursor, seekAfterTS);
    ASSERT_TRUE(deltas.deltas.contains(uuid));
    EXPECT_EQ(deltas.deltas.at(uuid).sizeCount, expected.delta);
    EXPECT_EQ(deltas.lastTimestamp, expected.lastTimestamp);

    // Also correct when filtered explicitly by 'uuid'
    const auto filteredDeltas = aggregateSizeCountDeltasInOplog(oplogCursor, seekAfterTS, uuid);
    ASSERT_TRUE(filteredDeltas.deltas.contains(uuid));
    ASSERT_EQ(expected.delta, filteredDeltas.deltas.at(uuid).sizeCount);
    EXPECT_EQ(filteredDeltas.lastTimestamp, expected.lastTimestamp);
}

/**
 * Allows explicit control over the contents of the "oplog" used to aggregate size and count. This
 * test cursor does not have visibility rules specific to the oplog, but should suffice for
 * targeted testing of aggregation logic.
 */
class OplogCursorMock : public SeekableRecordCursor {
public:
    OplogCursorMock(std::list<repl::OplogEntry> entries) {
        for (const auto& entry : entries) {
            _records.emplace_back(RecordId(entry.getTimestamp().asULL()),
                                  entry.getEntry().toBSON().getOwned());
        }
    }

    ~OplogCursorMock() override {}

    boost::optional<Record> next() override {
        if (_records.empty()) {
            return boost::none;
        }

        if (!_initialized) {
            _initialized = true;
            _it = _records.cbegin();
        } else {
            ++_it;
        }

        if (_it == _records.cend()) {
            _initialized = false;
            return boost::none;
        }

        return Record{_it->first, RecordData(_it->second.objdata(), _it->second.objsize())};
    }

    boost::optional<Record> seekExact(const RecordId& id) override {
        for (auto it = _records.cbegin(); it != _records.cend(); ++it) {
            if (it->first == id) {
                _initialized = true;
                _it = it;
                return Record{it->first, RecordData(it->second.objdata(), it->second.objsize())};
            }
        }
        _initialized = false;
        return boost::none;
    }

    void save() override {}
    bool restore(RecoveryUnit&, bool) override {
        return true;
    }
    void detachFromOperationContext() override {}
    void reattachToOperationContext(OperationContext*) override {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) override {}

    boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion) override {
        invariant(boundInclusion == BoundInclusion::kExclude);
        for (auto it = _records.cbegin(); it != _records.cend(); ++it) {
            if (it->first > start) {
                _initialized = true;
                _it = it;
                return Record{it->first, RecordData(it->second.objdata(), it->second.objsize())};
            }
        }
        _initialized = false;
        return {};
    }

private:
    bool _initialized = false;
    std::list<std::pair<RecordId, BSONObj>> _records;
    std::list<std::pair<RecordId, BSONObj>>::const_iterator _it;
};

class AggregateSizeCountFromOplogTest : public CatalogTestFixture {
protected:
    /**
     * Describes one inner CRUD op to synthesize inside an applyOps oplog entry.
     */
    struct InnerOpSpec {
        test_helpers::NsAndUUID coll;
        repl::OpTypeEnum opType;
        int32_t sizeDelta;
        bool includeSizeMetadata = true;
    };

    /**
     * Constructs a synthetic applyOps oplog entry at 'ts' whose inner ops are described by
     * 'innerOps'. Each inner op carries its own UUID so the aggregation code can attribute deltas
     * per-collection.
     */
    repl::OplogEntry makeApplyOpsOplogEntry(const Timestamp ts,
                                            const std::vector<InnerOpSpec>& innerOps) {
        const NamespaceString adminCmdNss = NamespaceString::kAdminCommandNamespace;

        BSONArrayBuilder innerOpsArray;
        for (const auto& spec : innerOps) {
            StringData opStr;
            switch (spec.opType) {
                case repl::OpTypeEnum::kInsert:
                    opStr = "i"_sd;
                    break;
                case repl::OpTypeEnum::kUpdate:
                    opStr = "u"_sd;
                    break;
                case repl::OpTypeEnum::kDelete:
                    opStr = "d"_sd;
                    break;
                default:
                    opStr = "n"_sd;
                    break;
            }
            BSONObjBuilder opBuilder;
            opBuilder.append("op", opStr);
            opBuilder.append("ns", spec.coll.nss.ns_forTest());
            spec.coll.uuid.appendToBuilder(&opBuilder, "ui");
            opBuilder.append("o", BSONObj());
            if (spec.includeSizeMetadata) {
                opBuilder.append("m", BSON("sz" << spec.sizeDelta));
            }
            innerOpsArray.append(opBuilder.obj());
        }

        return repl::DurableOplogEntry{repl::DurableOplogEntryParams{
            .opTime = repl::OpTime(ts, 1),
            .opType = repl::OpTypeEnum::kCommand,
            .nss = adminCmdNss,
            .oField = BSON("applyOps" << innerOpsArray.arr()),
            .wallClockTime = Date_t::now(),
        }};
    }

    /**
     * Bundles information about a user "collection" needed for CRUD oplog entries.
     */
    test_helpers::NsAndUUID collA = {
        .nss = NamespaceString::createNamespaceString_forTest("agg_size_count_from_oplog", "collA"),
        .uuid = UUID::gen()};
    test_helpers::NsAndUUID collB = {
        .nss = NamespaceString::createNamespaceString_forTest("agg_size_count_from_oplog", "collB"),
        .uuid = UUID::gen()};
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
    const auto oplogScanResult = aggregateSizeCountDeltasInOplog(oplogCursor, ts3);
    EXPECT_EQ(oplogScanResult.deltas.size(), 0u);

    // Check the result with a uuid filter.
    EXPECT_FALSE(aggregateSizeCountDeltasInOplog(oplogCursor, ts3, uuidA).deltas.contains(uuidA));
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
        EXPECT_EQ(aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min()).deltas.size(), 2u);
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
        EXPECT_EQ(aggregateSizeCountDeltasInOplog(oplogCursor, ts3).deltas.size(), 2u);
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
        const auto oplogScanResult = aggregateSizeCountDeltasInOplog(oplogCursor, ts5);
        EXPECT_EQ(oplogScanResult.deltas.size(), 0u);
    }

    {
        // Timestamp::max() is too large a value to extract a RecordId from the oplog from.
        ASSERT_THROWS_CODE(aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::max()),
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

    const auto result = aggregateSizeCountDeltasInOplog(*cursor, Timestamp::min());

    // Only the ts1 entry was visible; ts2 must not appear in the deltas.
    ASSERT_EQ(result.deltas.size(), 1u);
    ASSERT_TRUE(result.deltas.count(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 10);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.count, 1);
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

    const auto result = aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 0);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.count, 0);
}

TEST_F(AggregateSizeCountFromOplogTest, CollectionDropMarksStateDropped) {
    const Timestamp ts1{1, 1};
    std::list<repl::OplogEntry> entries{test_helpers::makeDropOplogEntry(ts1, collA)};
    OplogCursorMock oplogCursor(std::move(entries));

    const auto result = aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min());

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

    const auto result = aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min());

    ASSERT_TRUE(result.deltas.contains(collA.uuid));
    EXPECT_EQ(result.deltas.at(collA.uuid).state, DDLState::kCreated);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.size, 30);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount.count, 2);
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

    const auto result = aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min());

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

TEST_F(AggregateSizeCountFromOplogTest, AggregateApplyOpsFilteredByUUID) {
    const Timestamp ts1{1, 1};

    std::list<repl::OplogEntry> entries{
        makeApplyOpsOplogEntry(
            ts1,
            {{collA, repl::OpTypeEnum::kInsert, 100}, {collB, repl::OpTypeEnum::kInsert, 200}}),
    };
    OplogCursorMock oplogCursor(std::move(entries));

    {
        const auto result =
            aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min(), collA.uuid);
        ASSERT_TRUE(result.deltas.contains(collA.uuid));
        EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount,
                  (CollectionSizeCount{.size = 100, .count = 1}));
        EXPECT_FALSE(result.deltas.contains(collB.uuid));
    }

    {
        const auto result =
            aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min(), collB.uuid);
        ASSERT_TRUE(result.deltas.contains(collB.uuid));
        EXPECT_EQ(result.deltas.at(collB.uuid).sizeCount,
                  (CollectionSizeCount{.size = 200, .count = 1}));
        EXPECT_FALSE(result.deltas.contains(collA.uuid));
    }

    const auto result = aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min());
    EXPECT_EQ(result.deltas.size(), 2u);
    EXPECT_EQ(result.deltas.at(collA.uuid).sizeCount,
              (CollectionSizeCount{.size = 100, .count = 1}));
    EXPECT_EQ(result.deltas.at(collB.uuid).sizeCount,
              (CollectionSizeCount{.size = 200, .count = 1}));
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

    const auto result = aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min());
    EXPECT_TRUE(result.deltas.empty());
    EXPECT_FALSE(result.lastTimestamp.has_value());

    const auto filtered =
        aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min(), collA.uuid);
    EXPECT_TRUE(filtered.deltas.empty());
    EXPECT_FALSE(filtered.lastTimestamp.has_value());
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

    const auto result = aggregateSizeCountDeltasInOplog(oplogCursor, Timestamp::min());
    EXPECT_TRUE(result.deltas.empty());
}

}  // namespace
}  // namespace mongo::replicated_fast_count
