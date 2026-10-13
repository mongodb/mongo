// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/storage/ident.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace repl {
namespace {

using replicated_fast_count::test_helpers::CheckpointWriteSpec;
using replicated_fast_count::test_helpers::makeCheckpointEntry;
using replicated_fast_count::test_helpers::makeWatermarkEntry;

const Timestamp kT0{Seconds(100), 0};

class CheckpointValidationTest : public OplogApplierImplTest {
protected:
    void setUp() override {
        OplogApplierImplTest::setUp();
        // We need to create the stores because the oplog applier actually writes the container ops
        // in the checkpoints, and it expects the stores to exist.
        ASSERT_OK(createInternalFastCountContainers(_opCtx.get(),
                                                    NamespaceString::kAdminCommandNamespace,
                                                    ident::kFastCountMetadataStore,
                                                    KeyFormat::String,
                                                    ident::kFastCountMetadataStoreTimestamps,
                                                    KeyFormat::Long,
                                                    /*writeToOplog=*/false));
    }

    Status runThroughApplyOplogBatch(std::vector<OplogEntry> oploog,
                                     boost::optional<size_t> numWorkers = boost::none) {
        NoopOplogApplierObserver observer;
        auto workerPool = numWorkers ? makeReplWorkerPool(*numWorkers) : makeReplWorkerPool();
        OplogApplierImpl oplogApplier(/*executor=*/nullptr,
                                      /*oplogBuffer=*/nullptr,
                                      &observer,
                                      ReplicationCoordinator::get(_opCtx.get()),
                                      getConsistencyMarkers(),
                                      getStorageInterface(),
                                      OplogApplier::Options(OplogApplication::Mode::kSecondary),
                                      workerPool.get());
        const StatusWith<OpTime> applyResult =
            oplogApplier.applyOplogBatch(_opCtx.get(), std::move(oploog));
        if (!applyResult.isOK()) {
            return applyResult.getStatus();
        }
        getStorageInterface()->oplogDiskLocRegister(
            _opCtx.get(), applyResult.getValue().getTimestamp(), /*orderedCommit=*/true);
        return Status::OK();
    }

private:
    unittest::ServerParameterGuard _recordIdsFlag{"featureFlagRecordIdsReplicated", true};
    unittest::ServerParameterGuard _fastCountFlag{"featureFlagReplicatedFastCount", true};
    unittest::ServerParameterGuard _containerWritesFlag{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard _perDocumentFlag{
        "featureFlagContinuousInternodeValidationPerDocument", true};
};

TEST_F(CheckpointValidationTest, CheckpointValidationSkippedWhenInternodeValidationDisabled) {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", false);
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    std::vector<OplogEntry> oplog;
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    oplog.push_back(
        makeCheckpointEntry(watermarkTs,
                            {{.uuid = UUID::gen(),
                              .metadata = CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 999, .count = 999}}}}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

TEST_F(CheckpointValidationTest, CheckpointValidatesCrudDeltas) {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointCrud");
    createCollection(_opCtx.get(), nss, {});
    const UUID uuid = getCollectionUUID(_opCtx.get(), nss);

    const BSONObj doc1 = BSON("_id" << 1);
    const BSONObj doc2 = BSON("_id" << 2 << "x" << 100);
    const int doc2AfterSize = doc2.objsize();

    std::vector<OplogEntry> oplog;
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc1, RecordId(1)));
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc2, RecordId(2)));
    oplog.push_back(makeDeleteOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc1, RecordId(1), -doc1.objsize()));
    oplog.push_back(makeUpdateOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, doc2, BSON("$set" << BSON("x" << 101)), RecordId(2), 0));
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    oplog.push_back(makeCheckpointEntry(
        watermarkTs,
        {{.uuid = uuid,
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = doc2AfterSize, .count = 1},
              .hash = boost::none}}}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

TEST_F(CheckpointValidationTest, CheckpointValidatesGroupedInsertsWithInterleavedWatermark) {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", true);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "test.checkpointGroupedInsertsInterleavedWatermark");
    createCollection(_opCtx.get(), nss, {});
    const UUID uuid = getCollectionUUID(_opCtx.get(), nss);

    const BSONObj doc1 = BSON("_id" << 1 << "x" << 10);
    const BSONObj doc2 = BSON("_id" << 2 << "x" << 20);
    const BSONObj doc3 = BSON("_id" << 3 << "x" << 30);

    std::vector<OplogEntry> oplog;
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc1, RecordId(1)));
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc2, RecordId(2)));
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc3, RecordId(3)));
    oplog.push_back(makeCheckpointEntry(
        watermarkTs,
        {{.uuid = uuid,
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = doc1.objsize() + doc2.objsize(), .count = 2},
              .hash = boost::none}}}));

    // By using exactly one worker thread, the first two inserts are grouped and the grouped insert
    // oplog application path is exercised. The watermark delimits the insert group, so the
    // checkpoint at the watermark must validate against only the first two inserts.
    const size_t numWorkers = 1;
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog), numWorkers));
}

TEST_F(CheckpointValidationTest, TwoWatermarksTwoCheckpointsValidateCumulativeDeltas) {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", true);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "test.checkpointTwoWatermarksTwoCheckpoints");
    createCollection(_opCtx.get(), nss, {});
    const UUID uuid = getCollectionUUID(_opCtx.get(), nss);

    const BSONObj doc1 = BSON("_id" << 1);
    const BSONObj doc2 = BSON("_id" << 2 << "x" << 100);
    const BSONObj doc3 = BSON("_id" << 3 << "x" << 200);

    std::vector<OplogEntry> oplog;
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc1, RecordId(1)));
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc2, RecordId(2)));
    const Timestamp firstWatermarkTs = nextOpTime().getTimestamp();
    oplog.push_back(makeWatermarkEntry(firstWatermarkTs));
    oplog.push_back(makeCheckpointEntry(
        firstWatermarkTs,
        {{.uuid = uuid,
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = doc1.objsize() + doc2.objsize(), .count = 2},
              .hash = boost::none}}}));
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc3, RecordId(3)));
    const Timestamp secondWatermarkTs = nextOpTime().getTimestamp();
    oplog.push_back(makeWatermarkEntry(secondWatermarkTs));
    oplog.push_back(makeCheckpointEntry(
        secondWatermarkTs,
        {{.uuid = uuid,
          .metadata = CollectionReplicatedMetadata{
              .sizeCount =
                  CollectionSizeCount{.size = doc1.objsize() + doc2.objsize() + doc3.objsize(),
                                      .count = 3},
              .hash = boost::none}}}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

TEST_F(CheckpointValidationTest, TwoWatermarksCheckpointForFirstOnly) {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointTwoWatermarksOneCheckpoint");
    createCollection(_opCtx.get(), nss, {});
    const UUID uuid = getCollectionUUID(_opCtx.get(), nss);

    const BSONObj doc1 = BSON("_id" << 1);

    std::vector<OplogEntry> oplog;
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc1, RecordId(1)));
    const Timestamp firstWatermarkTs = nextOpTime().getTimestamp();
    oplog.push_back(makeWatermarkEntry(firstWatermarkTs));
    oplog.push_back(makeCheckpointEntry(
        firstWatermarkTs,
        {{.uuid = uuid,
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = doc1.objsize(), .count = 1},
              .hash = boost::none}}}));
    const Timestamp secondWatermarkTs = nextOpTime().getTimestamp();
    // Second watermark is not validated yet because there is no corresponding checkpoint.
    oplog.push_back(makeWatermarkEntry(secondWatermarkTs));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

TEST_F(CheckpointValidationTest, TwoCheckpointsNoWatermarksAreSkipped) {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", true);

    std::vector<OplogEntry> oplog;
    // We can see checkpoints without watermarks if the watermarks were written before the standby
    // restarted and the checkpoints were written after.
    oplog.push_back(makeCheckpointEntry(
        nextOpTime().getTimestamp(),
        {{.uuid = UUID::gen(),
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = 999, .count = 999}, .hash = boost::none}}}));
    oplog.push_back(makeCheckpointEntry(
        nextOpTime().getTimestamp(),
        {{.uuid = UUID::gen(),
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = 888, .count = 888}, .hash = boost::none}}}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

TEST_F(CheckpointValidationTest, TwoWatermarksNoCheckpointsPasses) {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointTwoWatermarksNoCheckpoints");
    createCollection(_opCtx.get(), nss, {});
    const UUID uuid = getCollectionUUID(_opCtx.get(), nss);

    const BSONObj doc1 = BSON("_id" << 1);

    std::vector<OplogEntry> oplog;
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc1, RecordId(1)));
    // We can see multiple consecutive watermarks if the primary faults after writing a watermark
    // but before writing the checkpoint, then restarts and is signalled to checkpoint again.
    oplog.push_back(makeWatermarkEntry(nextOpTime().getTimestamp()));
    oplog.push_back(makeWatermarkEntry(nextOpTime().getTimestamp()));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

using CheckpointValidationDeathTest = CheckpointValidationTest;

DEATH_TEST_F(CheckpointValidationDeathTest,
             CheckpointValidationFailsWhenStandbyMissingEntry,
             "13626001") {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", true);
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    // Oplog contains no replicated metadata deltas, so the standby missed an entry the primary
    // applied.
    std::vector<OplogEntry> oplog;
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    oplog.push_back(makeCheckpointEntry(
        watermarkTs,
        {{.uuid = UUID::gen(),
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = 999, .count = 999}, .hash = boost::none}}}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

DEATH_TEST_F(CheckpointValidationDeathTest,
             CheckpointValidationFailsWhenPrimaryMissingEntry,
             "13626006") {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointCrud");
    createCollection(_opCtx.get(), nss, {});
    const UUID uuid = getCollectionUUID(_opCtx.get(), nss);

    const BSONObj doc = BSON("_id" << 1);
    std::vector<OplogEntry> oplog;
    oplog.push_back(
        makeInsertOplogEntryWithRecordIdAndSizeMetadata(nextOpTime(), nss, uuid, doc, RecordId(1)));
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    // Checkpoint is empty, so the standby applied an oplog entry the primary did not.
    oplog.push_back(makeCheckpointEntry(watermarkTs, {}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}


DEATH_TEST_F(CheckpointValidationDeathTest,
             CheckpointValidationFailsOnCrudSizeMismatch,
             "13626005") {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointCrud");
    createCollection(_opCtx.get(), nss, {});
    const UUID uuid = getCollectionUUID(_opCtx.get(), nss);

    const BSONObj doc = BSON("_id" << 1);
    std::vector<OplogEntry> oplog;
    oplog.push_back(
        makeInsertOplogEntryWithRecordIdAndSizeMetadata(nextOpTime(), nss, uuid, doc, RecordId(1)));
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    // The standby and primary disagree on the size of the collection.
    oplog.push_back(makeCheckpointEntry(
        watermarkTs,
        {{.uuid = uuid,
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = 999, .count = 1}, .hash = boost::none}}}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

DEATH_TEST_F(CheckpointValidationDeathTest,
             CheckpointValidationFailsOnCrudCountMismatch,
             "13626005") {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointCrud");
    createCollection(_opCtx.get(), nss, {});
    const UUID uuid = getCollectionUUID(_opCtx.get(), nss);

    const BSONObj doc = BSON("_id" << 1);
    std::vector<OplogEntry> oplog;
    oplog.push_back(
        makeInsertOplogEntryWithRecordIdAndSizeMetadata(nextOpTime(), nss, uuid, doc, RecordId(1)));
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    // The standby and primary disagree on the count of the collection.
    oplog.push_back(makeCheckpointEntry(
        watermarkTs,
        {{.uuid = uuid,
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = doc.objsize(), .count = 2},
              .hash = boost::none}}}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

DEATH_TEST_F(CheckpointValidationDeathTest,
             CheckpointValidationFailsOnGroupedInsertSizeMismatch,
             "13626005") {
    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation(
        "enableContinuousInternodePerCollectionValidation", true);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest(
        "test.checkpointGroupedInsertsInterleavedWatermark");
    createCollection(_opCtx.get(), nss, {});
    const UUID uuid = getCollectionUUID(_opCtx.get(), nss);

    const BSONObj doc1 = BSON("_id" << 1 << "x" << 10);
    const BSONObj doc2 = BSON("_id" << 2 << "x" << 20);
    const BSONObj doc3 = BSON("_id" << 3 << "x" << 30);

    std::vector<OplogEntry> oplog;
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc1, RecordId(1)));
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc2, RecordId(2)));
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    // The checkpoint is missing the size of the first grouped insert, so the standby and primary
    // disagree on the size of the collection.
    oplog.push_back(makeCheckpointEntry(
        watermarkTs,
        {{.uuid = uuid,
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = doc2.objsize(), .count = 2},
              .hash = boost::none}}}));
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuid, doc3, RecordId(3)));

    // By using exactly one worker thread, the first two inserts are grouped and the grouped insert
    // oplog application path is exercised.
    const size_t numWorkers = 1;
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog), numWorkers));
}

}  // namespace
}  // namespace repl
}  // namespace mongo
