// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_applier.h"
#include "mongo/db/repl/oplog_applier_impl.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/repl_server_parameters_gen.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/server_options.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace repl {
namespace {

using replicated_fast_count::test_helpers::CheckpointWriteSpec;
using replicated_fast_count::test_helpers::expectValidatorMetrics;
using replicated_fast_count::test_helpers::makeCheckpointEntry;
using replicated_fast_count::test_helpers::makeCreateOplogEntry;
using replicated_fast_count::test_helpers::makeDropOplogEntry;
using replicated_fast_count::test_helpers::makeWatermarkEntry;
using replicated_fast_count::test_helpers::NsAndUUID;

const Timestamp kT0{Seconds(100), 0};

class CheckpointValidationTest : public OplogApplierImplTest {
protected:
    Options makeServiceContextOptions() override {
        return OplogApplierImplTest::makeServiceContextOptions().setPersistenceProvider(
            std::make_unique<replicated_fast_count::test_helpers::
                                 ContinuousInternodeValidationTestPersistenceProvider>());
    }

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

        // The `SizeCountCheckpointValidator` needs to adopt one checkpoint before it can begin
        // performing validation, so we do that here.
        const Timestamp adoptWatermarkTs = nextOpTime().getTimestamp();
        std::vector<OplogEntry> adoptOplog;
        adoptOplog.push_back(makeWatermarkEntry(adoptWatermarkTs));
        adoptOplog.push_back(makeCheckpointEntry(
            adoptWatermarkTs,
            {{.uuid = adoptUuid,
              .metadata = CollectionReplicatedMetadata{
                  .sizeCount = CollectionSizeCount{.size = 5, .count = 1}, .hash = boost::none}}}));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(adoptOplog)));
    }

    virtual boost::optional<size_t> workerCount() const {
        return boost::none;
    }

    Status runThroughApplyOplogBatch(std::vector<OplogEntry> oplog) {
        if (!oplogApplier) {
            _workerPool = workerCount() ? makeReplWorkerPool(*workerCount()) : makeReplWorkerPool();
            oplogApplier = std::make_unique<OplogApplierImpl>(
                /*executor=*/nullptr,
                /*oplogBuffer=*/nullptr,
                &noopOplogApplierObserver,
                ReplicationCoordinator::get(_opCtx.get()),
                getConsistencyMarkers(),
                getStorageInterface(),
                OplogApplier::Options(OplogApplication::Mode::kSecondary),
                _workerPool.get());
        }
        const StatusWith<OpTime> applyResult =
            oplogApplier->applyOplogBatch(_opCtx.get(), std::move(oplog));
        if (!applyResult.isOK()) {
            return applyResult.getStatus();
        }
        getStorageInterface()->oplogDiskLocRegister(
            _opCtx.get(), applyResult.getValue().getTimestamp(), /*orderedCommit=*/true);
        return Status::OK();
    }

    std::unique_ptr<OplogApplierImpl> oplogApplier;

private:
    const UUID adoptUuid = UUID::gen();
    std::unique_ptr<ThreadPool> _workerPool;
    unittest::ServerParameterGuard _recordIdsFlag{"featureFlagRecordIdsReplicated", true};
    unittest::ServerParameterGuard _fastCountFlag{"featureFlagReplicatedFastCount", true};
    unittest::ServerParameterGuard _containerWritesFlag{"featureFlagContainerWrites", true};
    unittest::ServerParameterGuard _perDocumentFlag{
        "featureFlagContinuousInternodeValidationPerDocument", true};
};

class SingleWorkerCheckpointValidationTest : public CheckpointValidationTest {
protected:
    boost::optional<size_t> workerCount() const override {
        return 1;
    }
};

using SingleWorkerCheckpointValidationDeathTest = SingleWorkerCheckpointValidationTest;

TEST_F(CheckpointValidationTest, CheckpointValidationLogsErrorsWhenInternodeValidationDisabled) {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", false);
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    std::vector<OplogEntry> oplog;
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    oplog.push_back(
        makeCheckpointEntry(watermarkTs,
                            {{.uuid = UUID::gen(),
                              .metadata = CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 999, .count = 999}}}}));

    unittest::LogCaptureGuard logs;
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626001)), 1);
}

TEST_F(CheckpointValidationTest, NoMetricsOrValidationWhenPerCollectionValidationDisabled) {
    otel::metrics::OtelMetricsCapturer capturer;
    disableContinuousInternodeValidationPerCollection.store(true);
    ON_BLOCK_EXIT([] { disableContinuousInternodeValidationPerCollection.store(false); });

    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    std::vector<OplogEntry> oplog;
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    oplog.push_back(makeCheckpointEntry(
        watermarkTs,
        {{.uuid = UUID::gen(),
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = 999, .count = 999}, .hash = boost::none}}}));

    unittest::LogCaptureGuard logs;
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626001)), 0);
    expectValidatorMetrics(capturer, {.recorded = 0, .evicted = 0, .skipped = 0, .validated = 0});
}

TEST_F(CheckpointValidationTest, CheckpointValidatesCrudDeltas) {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

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

TEST_F(SingleWorkerCheckpointValidationTest,
       CheckpointValidatesGroupedInsertsWithInterleavedWatermark) {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

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
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

TEST_F(CheckpointValidationTest, TwoWatermarksTwoCheckpointsValidateCumulativeDeltas) {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

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
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

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

TEST_F(CheckpointValidationTest, TwoWatermarksNoCheckpointsPasses) {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

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

TEST_F(CheckpointValidationTest, CheckpointValidatesDroppedCollection) {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointDropped");
    const NsAndUUID coll{.nss = nss, .uuid = UUID::gen()};

    {
        std::vector<OplogEntry> createOplog;
        createOplog.push_back(makeCreateOplogEntry(nextOpTime().getTimestamp(), coll));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(createOplog)));
        const Timestamp watermarkTs = nextOpTime().getTimestamp();
        createOplog.push_back(makeWatermarkEntry(watermarkTs));
        createOplog.push_back(makeCheckpointEntry(
            watermarkTs,
            {{.uuid = coll.uuid,
              .metadata = CollectionReplicatedMetadata{
                  .sizeCount = CollectionSizeCount{.size = 0, .count = 0}, .hash = boost::none}}}));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(createOplog)));
    }

    {
        std::vector<OplogEntry> dropOplog;
        dropOplog.push_back(makeDropOplogEntry(nextOpTime().getTimestamp(), coll));
        const Timestamp watermarkTs = nextOpTime().getTimestamp();
        dropOplog.push_back(makeWatermarkEntry(watermarkTs));
        dropOplog.push_back(
            makeCheckpointEntry(watermarkTs, {{.uuid = coll.uuid, .metadata = boost::none}}));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(dropOplog)));
    }
}

class CheckpointRenameValidationTest : public CheckpointValidationTest {
protected:
    // Applies the creation of `colls` and a checkpoint covering them.
    void createCollections(const std::vector<NsAndUUID>& colls) {
        std::vector<OplogEntry> oplog;
        for (const auto& coll : colls) {
            oplog.push_back(makeCreateOplogEntry(nextOpTime().getTimestamp(), coll));
        }
        const Timestamp watermarkTs = nextOpTime().getTimestamp();
        oplog.push_back(makeWatermarkEntry(watermarkTs));
        std::vector<CheckpointWriteSpec> writes;
        for (const auto& coll : colls) {
            writes.push_back({.uuid = coll.uuid, .metadata = kEmpty});
        }
        oplog.push_back(makeCheckpointEntry(watermarkTs, writes));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
    }

    // Applies `rename` followed by a checkpoint in which the primary marks `_target` dropped.
    void applyRenameDroppingTarget(OplogEntry rename) {
        std::vector<OplogEntry> oplog;
        oplog.push_back(std::move(rename));
        const Timestamp watermarkTs = nextOpTime().getTimestamp();
        oplog.push_back(makeWatermarkEntry(watermarkTs));
        // The source keeps its UUID, so the primary only writes the dropped target.
        oplog.push_back(
            makeCheckpointEntry(watermarkTs, {{.uuid = _target.uuid, .metadata = boost::none}}));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
    }

    inline static const CollectionReplicatedMetadata kEmpty{
        .sizeCount = CollectionSizeCount{.size = 0, .count = 0}, .hash = boost::none};

    const NsAndUUID _source{
        .nss = NamespaceString::createNamespaceString_forTest("test.checkpointRenameSource"),
        .uuid = UUID::gen()};
    const NsAndUUID _target{
        .nss = NamespaceString::createNamespaceString_forTest("test.checkpointRenameTarget"),
        .uuid = UUID::gen()};

private:
    unittest::ServerParameterGuard _enableValidation{
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true};
};

TEST_F(CheckpointRenameValidationTest, CheckpointValidatesCollectionDroppedByRenameDropTarget) {
    createCollections({_source, _target});
    applyRenameDroppingTarget(makeCommandOplogEntry(
        nextOpTime(),
        _source.nss,
        BSON("renameCollection" << _source.nss.ns_forTest() << "to" << _target.nss.ns_forTest()
                                << "stayTemp" << false << "dropTarget" << _target.uuid),
        /*object2=*/boost::none,
        _source.uuid));
}

TEST_F(CheckpointRenameValidationTest,
       CheckpointValidatesCollectionDroppedByRenameDropTargetInApplyOps) {
    createCollections({_source, _target});
    ReplOperation rename;
    rename.setOpType(OpTypeEnum::kCommand);
    rename.setNss(_source.nss.getCommandNS());
    rename.setUuid(_source.uuid);
    rename.setObject(BSON("renameCollection" << _source.nss.ns_forTest() << "to"
                                             << _target.nss.ns_forTest() << "stayTemp" << false
                                             << "dropTarget" << _target.uuid));
    applyRenameDroppingTarget(makeApplyOpsOplogEntry(nextOpTime(),
                                                     {rename},
                                                     OperationSessionInfo{},
                                                     Date_t::now(),
                                                     /*stmtIds=*/{},
                                                     /*prevWriteOpTimeInTransaction=*/boost::none));
}

TEST_F(CheckpointRenameValidationTest, RenameWithoutDropTargetRecordsNoDelta) {
    createCollections({_source});

    // A checkpoint needs a write, so it carries a collection created in the same interval. A delta
    // recorded for the rename would be missing from the checkpoint.
    const NsAndUUID other{
        .nss = NamespaceString::createNamespaceString_forTest("test.checkpointRenameOther"),
        .uuid = UUID::gen()};
    std::vector<OplogEntry> oplog;
    oplog.push_back(makeCommandOplogEntry(nextOpTime(),
                                          _source.nss,
                                          BSON("renameCollection"
                                               << _source.nss.ns_forTest() << "to"
                                               << _target.nss.ns_forTest() << "stayTemp" << false),
                                          /*object2=*/boost::none,
                                          _source.uuid));
    oplog.push_back(makeCreateOplogEntry(nextOpTime().getTimestamp(), other));
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    oplog.push_back(makeCheckpointEntry(watermarkTs, {{.uuid = other.uuid, .metadata = kEmpty}}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

using CheckpointValidationDeathTest = CheckpointValidationTest;

TEST_F(CheckpointValidationTest,
       DroppedCollectionRecreatedWithSameUuidDoesNotFailInShardedCluster) {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

    serverGlobalParams.clusterRole = {ClusterRole::ShardServer};
    ON_BLOCK_EXIT([] { serverGlobalParams.clusterRole = ClusterRole::None; });

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointDroppedRecreatedSharded");
    const NsAndUUID coll{.nss = nss, .uuid = UUID::gen()};

    std::vector<OplogEntry> oplog;
    oplog.push_back(makeCreateOplogEntry(nextOpTime().getTimestamp(), coll));
    oplog.push_back(makeWatermarkEntry(nextOpTime().getTimestamp()));
    oplog.push_back(makeDropOplogEntry(nextOpTime().getTimestamp(), coll));
    oplog.push_back(makeCreateOplogEntry(nextOpTime().getTimestamp(), coll));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

DEATH_TEST_F(CheckpointValidationDeathTest,
             CheckpointValidationFailsWhenStandbyMissingEntry,
             "13626001") {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);
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
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointCrud");
    createCollection(_opCtx.get(), nss, {});
    const UUID uuidA = getCollectionUUID(_opCtx.get(), nss);
    const NamespaceString missingNss =
        NamespaceString::createNamespaceString_forTest("test.checkpointCrudMissing");
    createCollection(_opCtx.get(), missingNss, {});
    const UUID uuidB = getCollectionUUID(_opCtx.get(), missingNss);

    const BSONObj docA = BSON("_id" << 1);
    const BSONObj docB = BSON("_id" << 2);
    std::vector<OplogEntry> oplog;
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), nss, uuidA, docA, RecordId(1)));
    oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
        nextOpTime(), missingNss, uuidB, docB, RecordId(2)));
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    oplog.push_back(makeWatermarkEntry(watermarkTs));
    // Checkpoint is missing an entry for uuidB, so the standby applied an oplog entry the primary
    // did not.
    oplog.push_back(makeCheckpointEntry(
        watermarkTs,
        {{.uuid = uuidA,
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = docA.objsize(), .count = 1},
              .hash = boost::none}}}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}


DEATH_TEST_F(CheckpointValidationDeathTest,
             CheckpointValidationFailsOnCrudSizeMismatch,
             "13626005") {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

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
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

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

DEATH_TEST_F(SingleWorkerCheckpointValidationDeathTest,
             CheckpointValidationFailsOnGroupedInsertSizeMismatch,
             "13626005") {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

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
    ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
}

DEATH_TEST_F(CheckpointValidationDeathTest,
             CheckpointValidationFailsWhenPrimaryWritesCreatedAndDroppedCollection,
             "13626001") {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointCreatedDroppedWrites");
    const NsAndUUID coll{.nss = nss, .uuid = UUID::gen()};

    std::vector<OplogEntry> createOplog;
    createOplog.push_back(makeCreateOplogEntry(nextOpTime().getTimestamp(), coll));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(createOplog)));

    std::vector<OplogEntry> dropOplog;
    dropOplog.push_back(makeDropOplogEntry(nextOpTime().getTimestamp(), coll));
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    dropOplog.push_back(makeWatermarkEntry(watermarkTs));
    // There should be no entry for `uuid` because the collection was created and dropped in the
    // same checkpoint interval.
    dropOplog.push_back(makeCheckpointEntry(
        watermarkTs,
        {{.uuid = coll.uuid,
          .metadata = CollectionReplicatedMetadata{
              .sizeCount = CollectionSizeCount{.size = 0, .count = 0}, .hash = boost::none}}}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(dropOplog)));
}

DEATH_TEST_F(CheckpointValidationDeathTest,
             CheckpointValidationFailsWhenPrimaryDeletesCreatedAndDroppedCollection,
             "13626002") {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointCreatedDroppedDeletes");
    const NsAndUUID coll{.nss = nss, .uuid = UUID::gen()};

    std::vector<OplogEntry> createOplog;
    createOplog.push_back(makeCreateOplogEntry(nextOpTime().getTimestamp(), coll));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(createOplog)));

    std::vector<OplogEntry> dropOplog;
    dropOplog.push_back(makeDropOplogEntry(nextOpTime().getTimestamp(), coll));
    const Timestamp watermarkTs = nextOpTime().getTimestamp();
    dropOplog.push_back(makeWatermarkEntry(watermarkTs));
    // There should be no entry for `uuid` because the collection was created and dropped in the
    // same checkpoint interval.
    dropOplog.push_back(
        makeCheckpointEntry(watermarkTs, {{.uuid = coll.uuid, .metadata = boost::none}}));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(dropOplog)));
}

DEATH_TEST_F(CheckpointValidationDeathTest,
             CheckpointValidationFailsWhenPrimaryWritesDroppedCollection,
             "13626004") {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointDroppedWrites");
    const NsAndUUID coll{.nss = nss, .uuid = UUID::gen()};

    std::vector<OplogEntry> createOplog;
    createOplog.push_back(makeCreateOplogEntry(nextOpTime().getTimestamp(), coll));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(createOplog)));

    {
        std::vector<OplogEntry> checkpointOplog;
        const Timestamp watermarkTs = nextOpTime().getTimestamp();
        checkpointOplog.push_back(makeWatermarkEntry(watermarkTs));
        checkpointOplog.push_back(makeCheckpointEntry(
            watermarkTs,
            {{.uuid = coll.uuid,
              .metadata = CollectionReplicatedMetadata{
                  .sizeCount = CollectionSizeCount{.size = 0, .count = 0}, .hash = boost::none}}}));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(checkpointOplog)));
    }

    {
        std::vector<OplogEntry> dropOplog;
        dropOplog.push_back(makeDropOplogEntry(nextOpTime().getTimestamp(), coll));
        const Timestamp watermarkTs = nextOpTime().getTimestamp();
        dropOplog.push_back(makeWatermarkEntry(watermarkTs));
        // The `.metadata` entry should be boost::none because the collection was dropped across two
        // checkpoints.
        dropOplog.push_back(makeCheckpointEntry(
            watermarkTs,
            {{.uuid = coll.uuid,
              .metadata = CollectionReplicatedMetadata{
                  .sizeCount = CollectionSizeCount{.size = 0, .count = 0}, .hash = boost::none}}}));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(dropOplog)));
    }
}

DEATH_TEST_F(CheckpointValidationDeathTest,
             CheckpointValidationFailsWhenPrimaryOmitsDroppedCollection,
             "13626006") {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointDroppedOmitted");
    const NsAndUUID coll{.nss = nss, .uuid = UUID::gen()};
    // We need two collections so the second checkpoint includes one or more container writes while
    // `coll` is omitted.
    const NamespaceString otherNss =
        NamespaceString::createNamespaceString_forTest("test.checkpointDroppedOmittedOther");
    const NsAndUUID otherColl{.nss = otherNss, .uuid = UUID::gen()};

    std::vector<OplogEntry> createOplog;
    createOplog.push_back(makeCreateOplogEntry(nextOpTime().getTimestamp(), coll));
    createOplog.push_back(makeCreateOplogEntry(nextOpTime().getTimestamp(), otherColl));
    ASSERT_OK(runThroughApplyOplogBatch(std::move(createOplog)));

    {
        std::vector<OplogEntry> checkpointOplog;
        const Timestamp watermarkTs = nextOpTime().getTimestamp();
        checkpointOplog.push_back(makeWatermarkEntry(watermarkTs));
        checkpointOplog.push_back(makeCheckpointEntry(
            watermarkTs,
            {{.uuid = coll.uuid,
              .metadata = CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 0,
                                                                                        .count = 0},
                                                       .hash = boost::none}},
             {.uuid = otherColl.uuid,
              .metadata = CollectionReplicatedMetadata{
                  .sizeCount = CollectionSizeCount{.size = 0, .count = 0}, .hash = boost::none}}}));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(checkpointOplog)));
    }

    {
        const BSONObj docB = BSON("_id" << 2);
        std::vector<OplogEntry> dropOplog;
        dropOplog.push_back(makeDropOplogEntry(nextOpTime().getTimestamp(), coll));
        dropOplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
            nextOpTime(), otherNss, otherColl.uuid, docB, RecordId(1)));
        const Timestamp watermarkTs = nextOpTime().getTimestamp();
        dropOplog.push_back(makeWatermarkEntry(watermarkTs));
        dropOplog.push_back(makeCheckpointEntry(
            watermarkTs,
            {{.uuid = otherColl.uuid,
              .metadata = CollectionReplicatedMetadata{
                  .sizeCount = CollectionSizeCount{.size = docB.objsize(), .count = 1},
                  .hash = boost::none}}}));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(dropOplog)));
    }
}

TEST_F(CheckpointValidationTest, CheckpointValidationAfterStepdownResetAdoptsCheckpoint) {
    otel::metrics::OtelMetricsCapturer capturer;
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure(
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true);

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest("test.checkpointAfterStepdownReset");
    createCollection(_opCtx.get(), nss, {});
    const UUID uuid = getCollectionUUID(_opCtx.get(), nss);

    const BSONObj doc1 = BSON("_id" << 1);
    {
        std::vector<OplogEntry> oplog;
        oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
            nextOpTime(), nss, uuid, doc1, RecordId(1)));
        const Timestamp watermarkTs = nextOpTime().getTimestamp();
        oplog.push_back(makeWatermarkEntry(watermarkTs));
        oplog.push_back(makeCheckpointEntry(
            watermarkTs,
            {{.uuid = uuid,
              .metadata = CollectionReplicatedMetadata{
                  .sizeCount = CollectionSizeCount{.size = doc1.objsize(), .count = 1},
                  .hash = boost::none}}}));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
    }
    expectValidatorMetrics(capturer, {.recorded = 1, .evicted = 1, .skipped = 0, .validated = 1});

    oplogApplier->resetCheckpointValidator();

    const BSONObj doc2 = BSON("_id" << 2);
    {
        std::vector<OplogEntry> oplog;
        oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
            nextOpTime(), nss, uuid, doc2, RecordId(2)));
        const Timestamp watermarkTs = nextOpTime().getTimestamp();
        oplog.push_back(makeWatermarkEntry(watermarkTs));
        // The validator adopts this checkpoint without validating it.
        oplog.push_back(makeCheckpointEntry(
            watermarkTs,
            {{.uuid = uuid,
              .metadata = CollectionReplicatedMetadata{
                  .sizeCount = CollectionSizeCount{.size = doc1.objsize() + doc2.objsize() + 100,
                                                   .count = 2},
                  .hash = boost::none}}}));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
    }
    expectValidatorMetrics(capturer, {.recorded = 2, .evicted = 2, .skipped = 1, .validated = 1});

    const BSONObj doc3 = BSON("_id" << 3);
    {
        std::vector<OplogEntry> oplog;
        oplog.push_back(makeInsertOplogEntryWithRecordIdAndSizeMetadata(
            nextOpTime(), nss, uuid, doc3, RecordId(3)));
        const Timestamp watermarkTs = nextOpTime().getTimestamp();
        oplog.push_back(makeWatermarkEntry(watermarkTs));
        oplog.push_back(makeCheckpointEntry(
            watermarkTs,
            {{.uuid = uuid,
              .metadata = CollectionReplicatedMetadata{
                  .sizeCount = CollectionSizeCount{.size = doc1.objsize() + doc2.objsize() +
                                                       doc3.objsize() + 100,
                                                   .count = 3},
                  .hash = boost::none}}}));
        ASSERT_OK(runThroughApplyOplogBatch(std::move(oplog)));
    }
    expectValidatorMetrics(capturer, {.recorded = 3, .evicted = 3, .skipped = 1, .validated = 2});
}

}  // namespace
}  // namespace repl
}  // namespace mongo
