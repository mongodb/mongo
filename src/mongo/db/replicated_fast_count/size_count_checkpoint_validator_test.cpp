// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_validator.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/replicated_fast_count/size_count_checkpoint_flusher.h"
#include "mongo/db/replicated_fast_count/size_count_store.h"
#include "mongo/db/replicated_fast_count/size_count_timestamp_store.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/otel/metrics/metric_names.h"
#include "mongo/otel/metrics/metrics_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace replicated_fast_count {
namespace {

TEST(ValidateCheckpointTest, MatchingCheckpointContentsPass) {
    const UUID uuidA = UUID::gen();
    const UUID uuidB = UUID::gen();
    CheckpointContents expected;
    expected[uuidA] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 10, .count = 1}};
    expected[uuidB] = boost::none;
    CheckpointContents actual;
    actual[uuidA] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 10, .count = 1}};
    actual[uuidB] = boost::none;
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 136260)), 0);
}

DEATH_TEST(ValidateCheckpointDeathTest, UnobservedWriteFasserts, "13626001") {
    CheckpointContents expected;
    CheckpointContents actual;
    actual[UUID::gen()] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 5, .count = 1}};
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

TEST(ValidateCheckpointTest, UnobservedWriteLogsError) {
    CheckpointContents expected;
    CheckpointContents actual;
    actual[UUID::gen()] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 5, .count = 1}};

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626001)), 1);
}

DEATH_TEST(ValidateCheckpointDeathTest, UnobservedRemovalFasserts, "13626002") {
    CheckpointContents expected;
    CheckpointContents actual;
    actual[UUID::gen()] = boost::none;
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

TEST(ValidateCheckpointTest, UnobservedRemovalLogsError) {
    CheckpointContents expected;
    CheckpointContents actual;
    actual[UUID::gen()] = boost::none;

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626002)), 1);
}

DEATH_TEST(ValidateCheckpointDeathTest, RemovalForLiveCollectionFasserts, "13626003") {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 5, .count = 1}};
    CheckpointContents actual;
    actual[uuid] = boost::none;
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

TEST(ValidateCheckpointTest, RemovalForLiveCollectionLogsError) {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 5, .count = 1}};
    CheckpointContents actual;
    actual[uuid] = boost::none;

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626003)), 1);
}

DEATH_TEST(ValidateCheckpointDeathTest, WriteForObservedDropFasserts, "13626004") {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = boost::none;
    CheckpointContents actual;
    actual[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 5, .count = 1}};
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

TEST(ValidateCheckpointTest, WriteForObservedDropLogsError) {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = boost::none;
    CheckpointContents actual;
    actual[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 5, .count = 1}};

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626004)), 1);
}

DEATH_TEST(ValidateCheckpointDeathTest, SizeMismatchFasserts, "13626005") {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 10, .count = 1}};
    CheckpointContents actual;
    actual[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 999, .count = 1}};
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

TEST(ValidateCheckpointTest, SizeMismatchLogsError) {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 10, .count = 1}};
    CheckpointContents actual;
    actual[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 999, .count = 1}};

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626005)), 1);
}

DEATH_TEST(ValidateCheckpointDeathTest, CountMismatchFasserts, "13626005") {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 10, .count = 1}};
    CheckpointContents actual;
    actual[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 10, .count = 9}};
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

TEST(ValidateCheckpointTest, CountMismatchLogsError) {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 10, .count = 1}};
    CheckpointContents actual;
    actual[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 10, .count = 9}};

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626005)), 1);
}

TEST(ValidateCheckpointTest, MatchingHashPasses) {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = 42};
    CheckpointContents actual;
    actual[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = 42};
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

DEATH_TEST(ValidateCheckpointDeathTest, HashMismatchFasserts, "13626005") {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = 42};
    CheckpointContents actual;
    actual[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = 43};
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

TEST(ValidateCheckpointTest, HashMismatchLogsError) {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = 42};
    CheckpointContents actual;
    actual[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = 43};

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626005)), 1);
}

DEATH_TEST(ValidateCheckpointDeathTest, HashAbsentOnStandbyFasserts, "13626005") {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = boost::none};
    CheckpointContents actual;
    actual[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = 42};
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

TEST(ValidateCheckpointTest, HashAbsentOnStandbyLogsError) {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = boost::none};
    CheckpointContents actual;
    actual[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = 42};

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626005)), 1);
}

DEATH_TEST(ValidateCheckpointDeathTest, HashAbsentOnPrimaryFasserts, "13626005") {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = 42};
    CheckpointContents actual;
    actual[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = boost::none};
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

TEST(ValidateCheckpointTest, HashAbsentOnPrimaryLogsError) {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = 42};
    CheckpointContents actual;
    actual[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = boost::none};

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626005)), 1);
}

TEST(ValidateCheckpointTest, HashAbsentOnBothPasses) {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = CollectionReplicatedMetadata{
        .sizeCount = CollectionSizeCount{.size = 10, .count = 1}, .hash = boost::none};
    CheckpointContents actual = expected;
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

DEATH_TEST(ValidateCheckpointDeathTest, MissingWriteFasserts, "13626006") {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 10, .count = 1}};
    CheckpointContents actual;
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

TEST(ValidateCheckpointTest, MissingWriteLogsError) {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] =
        CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 10, .count = 1}};
    CheckpointContents actual;

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626006)), 1);
}

DEATH_TEST(ValidateCheckpointDeathTest, MissingRemovalFasserts, "13626006") {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = boost::none;
    CheckpointContents actual;
    validateCheckpoint(expected, actual, ReportingStrategy::kFassert);
}

TEST(ValidateCheckpointTest, MissingRemovalLogsError) {
    const UUID uuid = UUID::gen();
    CheckpointContents expected;
    expected[uuid] = boost::none;
    CheckpointContents actual;

    unittest::LogCaptureGuard logs;
    validateCheckpoint(expected, actual, ReportingStrategy::kLogError);
    logs.stop();

    EXPECT_EQ(logs.countBSONContainingSubset(BSON("id" << 13626006)), 1);
}

using test_helpers::getOplogEntriesMatching;
using test_helpers::makeWatermarkEntry;

class CheckpointOplogParsingTest : public CatalogTestFixture {
public:
    Options makeServiceContextOptions() override {
        return CatalogTestFixture::makeServiceContextOptions().setPersistenceProvider(
            std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>());
    }

protected:
    void setUp() override {
        CatalogTestFixture::setUp();

        test_helpers::registerOpObserverForTest(getServiceContext());

        auto stores = test_helpers::createContainerFastCountStores(operationContext());
        sizeCountStore = std::move(stores.sizeCountStore);
        timestampStore = std::move(stores.timestampStore);

        ASSERT_OK(createCollection(
            operationContext(), userNss.dbName(), BSON("create" << userNss.coll())));
    }

    void insertUserDoc(const BSONObj& doc) {
        auto coll =
            acquireCollection(operationContext(),
                              CollectionAcquisitionRequest::fromOpCtx(
                                  operationContext(), userNss, AcquisitionPrerequisites::kWrite),
                              MODE_IX);
        WriteUnitOfWork wuow(operationContext(), WriteUnitOfWork::nonAtomicGroup);
        ASSERT_OK(Helpers::insert(operationContext(), coll.getCollectionPtr(), doc));
        wuow.commit();
    }

    std::unique_ptr<SizeCountStore> sizeCountStore;
    std::unique_ptr<SizeCountTimestampStore> timestampStore;

    const NamespaceString userNss =
        NamespaceString::createNamespaceString_forTest("checkpoint_validator_test", "userColl");
};

TEST_F(CheckpointOplogParsingTest, ClassifiesEntriesOnRealOplog) {
    // Write a user write, a checkpoint, and a watermark to the oplog.
    insertUserDoc(BSON("_id" << 0 << "x" << 0));
    const OplogScanResult batch{
        .deltas = {{UUID::gen(),
                    ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}}}},
        .lastTimestamp = Timestamp(1, 2)};
    ASSERT_TRUE(
        flusher::flush(operationContext(), *sizeCountStore, *timestampStore, batch).has_value());
    flusher::writeWatermark(operationContext());

    // Check isCheckpointApplyOpsEntry() correctly matches on checkpoint.
    const std::vector<repl::OplogEntry> checkpointEntries =
        getOplogEntriesMatching(operationContext(), [](const repl::OplogEntry& entry) {
            return isCheckpointApplyOpsEntry(entry);
        });
    ASSERT_EQ(checkpointEntries.size(), 1);
    EXPECT_EQ(checkpointEntries.front().getOpType(), repl::OpTypeEnum::kCommand);
    EXPECT_EQ(checkpointEntries.front().getCommandType(), repl::CommandTypeEnum::kApplyOps);

    // Check isCheckpointApplyOpsEntry() does not match on watermark.
    const std::vector<repl::OplogEntry> watermarkEntries =
        getOplogEntriesMatching(operationContext(), [](const repl::OplogEntry& entry) {
            return entry.getOpType() == repl::OpTypeEnum::kNoop &&
                entry.getObject()["msg"].valueStringData() == kWatermarkMsg;
        });
    ASSERT_EQ(watermarkEntries.size(), 1);
    EXPECT_FALSE(isCheckpointApplyOpsEntry(watermarkEntries.front()));

    // Check isCheckpointApplyOpsEntry() does not match on user write.
    const std::vector<repl::OplogEntry> insertEntries =
        getOplogEntriesMatching(operationContext(), [](const repl::OplogEntry& entry) {
            return entry.getOpType() == repl::OpTypeEnum::kInsert;
        });
    ASSERT_EQ(insertEntries.size(), 1);
    EXPECT_FALSE(isCheckpointApplyOpsEntry(insertEntries.front()));

    // Check isCheckpointApplyOpsEntry() does not match on user applyOps.
    const Timestamp watermarkTs = watermarkEntries.front().getTimestamp();
    BSONArrayBuilder innerOpsBuilder;
    innerOpsBuilder.append(BSON("op" << "i"
                                     << "ns" << userNss.ns_forTest() << "ui" << UUID::gen() << "o"
                                     << BSON("_id" << 1)));
    const repl::OplogEntry userApplyOps = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(Timestamp(watermarkTs.getSecs(), watermarkTs.getInc() + 1), 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::createNamespaceString_forTest("admin", "$cmd"),
        .oField = BSON("applyOps" << innerOpsBuilder.arr()),
        .wallClockTime = Date_t::now(),
    }};
    test_helpers::writeToOplog(operationContext(), userApplyOps);

    EXPECT_FALSE(isCheckpointApplyOpsEntry(userApplyOps));

    const std::vector<repl::OplogEntry> userApplyOpsEntries =
        getOplogEntriesMatching(operationContext(), [](const repl::OplogEntry& entry) {
            return entry.getCommandType() == repl::CommandTypeEnum::kApplyOps &&
                !isCheckpointApplyOpsEntry(entry);
        });
    ASSERT_EQ(userApplyOpsEntries.size(), 1);
}

TEST_F(CheckpointOplogParsingTest, ParseCheckpointReadsRealFlushedCheckpoints) {
    const UUID uuidA = UUID::gen();
    const UUID uuidB = UUID::gen();

    {
        const OplogScanResult batch{
            .deltas = {{uuidA,
                        ReplicatedMetadataDelta{
                            .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 2},
                                         .hash = 12345}}},
                       {uuidB,
                        ReplicatedMetadataDelta{
                            .metadata = {.sizeCount =
                                             CollectionSizeCount{.size = 50, .count = 5}}}}},
            .lastTimestamp = Timestamp(1, 1)};
        // Use flush() so we parse the actual oplog entry written in production.
        ASSERT_TRUE(flusher::flush(operationContext(), *sizeCountStore, *timestampStore, batch)
                        .has_value());

        const CheckpointContents contents =
            parseCheckpoint(test_helpers::getLatestApplyOpsForFastCountStore(operationContext()));
        ASSERT_EQ(contents.size(), 2);
        ASSERT_TRUE(contents.contains(uuidA));
        ASSERT_TRUE(contents.contains(uuidB));
        EXPECT_EQ(contents.at(uuidA)->sizeCount.size, 30);
        EXPECT_EQ(contents.at(uuidA)->sizeCount.count, 2);
        ASSERT_TRUE(contents.at(uuidA)->hash.has_value());
        EXPECT_EQ(*contents.at(uuidA)->hash, int64_t(12345));
        EXPECT_EQ(contents.at(uuidB)->sizeCount.size, 50);
        EXPECT_EQ(contents.at(uuidB)->sizeCount.count, 5);
        EXPECT_FALSE(contents.at(uuidB)->hash.has_value());
    }

    {
        const OplogScanResult batch{
            .deltas = {{uuidA,
                        ReplicatedMetadataDelta{
                            .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 2},
                                         .hash = 5}}},
                       {uuidB,
                        ReplicatedMetadataDelta{
                            .metadata = {.sizeCount =
                                             CollectionSizeCount{.size = 50, .count = 5}}}}},
            .lastTimestamp = Timestamp(1, 2)};
        ASSERT_TRUE(flusher::flush(operationContext(), *sizeCountStore, *timestampStore, batch)
                        .has_value());

        const CheckpointContents contents =
            parseCheckpoint(test_helpers::getLatestApplyOpsForFastCountStore(operationContext()));
        ASSERT_EQ(contents.size(), 2);
        EXPECT_EQ(contents.at(uuidA)->sizeCount.size, 60);
        EXPECT_EQ(contents.at(uuidA)->sizeCount.count, 4);
        ASSERT_TRUE(contents.at(uuidA)->hash.has_value());
        EXPECT_EQ(*contents.at(uuidA)->hash, int64_t(12348));
        EXPECT_EQ(contents.at(uuidB)->sizeCount.size, 100);
        EXPECT_EQ(contents.at(uuidB)->sizeCount.count, 10);
    }

    {
        const OplogScanResult dropBatch{
            .deltas = {{uuidA, ReplicatedMetadataDelta{.state = DDLState::kDropped}}},
            .lastTimestamp = Timestamp(1, 3)};
        ASSERT_TRUE(flusher::flush(operationContext(), *sizeCountStore, *timestampStore, dropBatch)
                        .has_value());

        const CheckpointContents contents =
            parseCheckpoint(test_helpers::getLatestApplyOpsForFastCountStore(operationContext()));
        ASSERT_EQ(contents.size(), 1);
        EXPECT_FALSE(contents.at(uuidA).has_value());
    }
}

TEST_F(CheckpointOplogParsingTest, SplitFlushEntriesAreNotClassifiedAsCheckpoints) {
    otel::metrics::OtelMetricsCapturer capturer;
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure{
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true};
    // When this server parameter is set to 1, the replicated metadata checkpoint is split into two
    // applyOps entries: one for the metadata store container write, and one for the timestamp store
    // container write.
    unittest::ServerParameterGuard maxOpsPerOplogEntry{
        "maxNumberOfBatchedOperationsInSingleOplogEntry", 1};

    const UUID uuid = UUID::gen();
    const OplogScanResult batch{
        .deltas = {{uuid,
                    ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 2}}}}},
        .lastTimestamp = Timestamp(1, 2)};
    ASSERT_TRUE(
        flusher::flush(operationContext(), *sizeCountStore, *timestampStore, batch).has_value());

    const std::vector<repl::OplogEntry> applyOpsEntries =
        getOplogEntriesMatching(operationContext(), [](const repl::OplogEntry& entry) {
            return entry.getCommandType() == repl::CommandTypeEnum::kApplyOps;
        });
    ASSERT_EQ(applyOpsEntries.size(), 2);
    for (const auto& entry : applyOpsEntries) {
        ASSERT_FALSE(isCheckpointApplyOpsEntry(entry));
    }

    SizeCountCheckpointValidator validator;
    validator.preprocessOplogEntry(makeWatermarkEntry(*batch.lastTimestamp));
    for (const auto& entry : applyOpsEntries) {
        validator.preprocessOplogEntry(entry);
    }
    validator.mergeAndValidate(operationContext(), {}, UUID::gen());

    if (capturer.canReadMetrics()) {
        // mergeAndValidate() should validate zero checkpoints because no checkpoint oplog entries
        // were found.
        EXPECT_EQ(capturer.readInt64Counter(
                      otel::metrics::MetricNames::kReplicatedFastCountCheckpointsValidated),
                  0);
        // A split checkpoint is not recognized as a checkpoint, so it is not counted as skipped.
        EXPECT_EQ(capturer.readInt64Counter(
                      otel::metrics::MetricNames::kReplicatedFastCountCheckpointsSkipped),
                  0);
    }
}

TEST(ParseCheckpointTest, ContainerInsertMissingValueMasserts) {
    BSONArrayBuilder innerOpsBuilder;
    innerOpsBuilder.append(BSON(
        "op" << "ci" << "ns" << NamespaceString::kContainerNamespace.ns_forTest() << "container"
             << ident::kFastCountMetadataStore << "o" << BSON("k" << UUID::gen())));

    const repl::OplogEntry checkpointEntry = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(Timestamp(1, 1), 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::kAdminCommandNamespace,
        .oField = BSON("applyOps" << innerOpsBuilder.arr()),
        .wallClockTime = Date_t::now(),
    }};

    ASSERT_THROWS_CODE(parseCheckpoint(checkpointEntry), DBException, 13647900);
}

DEATH_TEST(ParseCheckpointDeathTest, TimestampContainerDeleteTasserts, "13647901") {
    BSONArrayBuilder innerOpsBuilder;
    innerOpsBuilder.append(BSON(
        "op" << "cd" << "ns" << NamespaceString::kContainerNamespace.ns_forTest() << "container"
             << ident::kFastCountMetadataStoreTimestamps << "o" << BSON("k" << UUID::gen())));

    const repl::OplogEntry checkpointEntry = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(Timestamp(1, 1), 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::kAdminCommandNamespace,
        .oField = BSON("applyOps" << innerOpsBuilder.arr()),
        .wallClockTime = Date_t::now(),
    }};

    isCheckpointApplyOpsEntry(checkpointEntry);
}

DEATH_TEST(ParseCheckpointDeathTest, NonContainerOpInFastCountLedEntryTasserts, "13722400") {
    BSONArrayBuilder innerOpsBuilder;
    innerOpsBuilder.append(BSON(
        "op" << "ci" << "ns" << NamespaceString::kContainerNamespace.ns_forTest() << "container"
             << ident::kFastCountMetadataStore << "o" << BSON("k" << UUID::gen())));
    innerOpsBuilder.append(BSON("op" << "i"
                                     << "ns" << NamespaceString::kContainerNamespace.ns_forTest()
                                     << "o" << BSON("_id" << 1)));

    const repl::OplogEntry checkpointEntry = repl::DurableOplogEntry{repl::DurableOplogEntryParams{
        .opTime = repl::OpTime(Timestamp(1, 1), 1),
        .opType = repl::OpTypeEnum::kCommand,
        .nss = NamespaceString::kAdminCommandNamespace,
        .oField = BSON("applyOps" << innerOpsBuilder.arr()),
        .wallClockTime = Date_t::now(),
    }};

    isCheckpointApplyOpsEntry(checkpointEntry);
}

const Timestamp kAdopt{Seconds(50), 0};
const Timestamp kT0{Seconds(100), 0};
const Timestamp kT1{Seconds(200), 0};
const Timestamp kT2{Seconds(250), 0};
const Timestamp kT3{Seconds(300), 0};
const Timestamp kT4{Seconds(350), 0};
const Timestamp kEnd{Seconds(1000), 0};

ValidatorDeltaRecord makeDelta(Timestamp ts, UUID uuid, CollectionSizeCount sizeCount) {
    return {.ts = ts,
            .uuid = uuid,
            .delta = ReplicatedMetadataDelta{.metadata = {.sizeCount = sizeCount}}};
}

CollectionReplicatedMetadata makeSizeCount(CollectionSizeCount sizeCount) {
    return CollectionReplicatedMetadata{.sizeCount = sizeCount};
}

using test_helpers::expectValidatorMetrics;
using test_helpers::makeCheckpointEntry;

class SizeCountCheckpointValidatorRecoveryTest : public CatalogTestFixture {
public:
    Options makeServiceContextOptions() override {
        return CatalogTestFixture::makeServiceContextOptions().setPersistenceProvider(
            std::make_unique<test_helpers::ReplicatedFastCountTestPersistenceProvider>());
    }

protected:
    void setUp() override {
        CatalogTestFixture::setUp();

        auto stores = test_helpers::createContainerFastCountStores(operationContext());
        sizeCountStore = std::move(stores.sizeCountStore);
        timestampStore = std::move(stores.timestampStore);
    }

    /**
     * Performs preprocessing on the provided `ops`, then records each delta in `records`, then
     * validates the deltas against the watermark(s)/checkpoint(s) in `ops`.
     */
    void applyBatch(SizeCountCheckpointValidator& validator,
                    const std::vector<repl::OplogEntry>& ops,
                    const std::vector<ValidatorDeltaRecord>& records) {
        for (const auto& entry : ops) {
            validator.preprocessOplogEntry(entry);
        }
        std::vector<ValidatorDeltaRecord> buffer;
        {
            SizeCountCheckpointValidator::ScopedThreadBuffer scopedBuffer(&buffer);
            for (const auto& record : records) {
                SizeCountCheckpointValidator::recordDelta(record.uuid, record.ts, record.delta);
            }
        }
        validator.mergeAndValidate(operationContext(), {buffer}, oplogUuid);
    }

    std::unique_ptr<SizeCountStore> sizeCountStore;
    std::unique_ptr<SizeCountTimestampStore> timestampStore;

    SizeCountCheckpointValidator validator;
    const UUID oplogUuid = UUID::gen();

    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure{
        "fassertOnPerCollectionContinuousInternodeValidationFailure", true};
};

TEST_F(SizeCountCheckpointValidatorRecoveryTest, BatchWithNoCheckpointNotAdopted) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    applyBatch(validator,
               /*ops=*/{},
               {{.ts = kT0,
                 .uuid = uuid,
                 .delta = ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}}});
    applyBatch(
        validator,
        {makeWatermarkEntry(kT1),
         makeCheckpointEntry(kT1,
                             {{uuid,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 999, .count = 9}}}})},
        {});
    expectValidatorMetrics(capturer, {.recorded = 1, .evicted = 1, .skipped = 1, .validated = 0});
}

TEST_F(SizeCountCheckpointValidatorRecoveryTest, CheckpointWithoutWatermarkNotAdopted) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    applyBatch(
        validator,
        {makeCheckpointEntry(kT1,
                             {{uuid,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}})},
        {});
    // The checkpoint is skipped, and with no watermark nothing is recorded or evicted.
    expectValidatorMetrics(capturer, {.recorded = 0, .evicted = 0, .skipped = 1, .validated = 0});
    applyBatch(
        validator,
        {makeWatermarkEntry(kT2),
         makeCheckpointEntry(kT2,
                             {{uuid,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 999, .count = 9}}}})},
        {});
    expectValidatorMetrics(capturer, {.recorded = 1, .evicted = 1, .skipped = 2, .validated = 0});
}

TEST_F(SizeCountCheckpointValidatorRecoveryTest, SingleCheckpointBatchAdopted) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        *sizeCountStore,
        uuid,
        SizeCountStore::Entry{.timestamp = kT2, .size = 10, .count = 1});
    applyBatch(
        validator,
        {makeWatermarkEntry(kT2),
         makeCheckpointEntry(kT2,
                             {{uuid,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}})},
        // This delta is ignored.
        {{.ts = kT1,
          .uuid = uuid,
          .delta = ReplicatedMetadataDelta{
              .metadata = {.sizeCount = CollectionSizeCount{.size = 7, .count = 3}}}}});
    applyBatch(
        validator,
        {makeWatermarkEntry(kEnd),
         makeCheckpointEntry(kEnd,
                             {{uuid,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 15, .count = 1}}}})},
        // This delta is not ignored!
        {{.ts = kT3,
          .uuid = uuid,
          .delta = ReplicatedMetadataDelta{
              .metadata = {.sizeCount = CollectionSizeCount{.size = 5, .count = 0}}}}});
    expectValidatorMetrics(capturer, {.recorded = 2, .evicted = 2, .skipped = 1, .validated = 1});
}

TEST_F(SizeCountCheckpointValidatorRecoveryTest, MultiCheckpointBatchAdopted) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        *sizeCountStore,
        uuid,
        SizeCountStore::Entry{.timestamp = kT2, .size = 60, .count = 6});
    applyBatch(
        validator,
        {makeWatermarkEntry(kT1),
         // This checkpoint is ignored.
         makeCheckpointEntry(kT1,
                             {{uuid,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 50, .count = 5}}}}),
         makeWatermarkEntry(kT3),
         makeCheckpointEntry(kT3,
                             {{uuid,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 60, .count = 6}}}})},
        // This delta is also ignored.
        {{.ts = kT2,
          .uuid = uuid,
          .delta = ReplicatedMetadataDelta{
              .metadata = {.sizeCount = CollectionSizeCount{.size = 7, .count = 3}}}}});
    applyBatch(
        validator,
        {makeWatermarkEntry(kEnd),
         makeCheckpointEntry(kEnd,
                             {{uuid,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 65, .count = 6}}}})},
        {{.ts = kT4,
          .uuid = uuid,
          .delta = ReplicatedMetadataDelta{
              .metadata = {.sizeCount = CollectionSizeCount{.size = 5, .count = 0}}}}});
    expectValidatorMetrics(capturer, {.recorded = 3, .evicted = 3, .skipped = 2, .validated = 1});
}

TEST_F(SizeCountCheckpointValidatorRecoveryTest, SingleCheckpointAdoptedMultipleCollections) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuidA = UUID::gen();
    const UUID uuidB = UUID::gen();
    test_helpers::insertSizeCountEntry(
        operationContext(),
        *sizeCountStore,
        uuidA,
        SizeCountStore::Entry{.timestamp = kT0, .size = 10, .count = 3});
    test_helpers::insertSizeCountEntry(
        operationContext(),
        *sizeCountStore,
        uuidB,
        SizeCountStore::Entry{.timestamp = kT1, .size = 4, .count = 8});
    applyBatch(
        validator,
        {makeWatermarkEntry(kT2),
         makeCheckpointEntry(kT2,
                             {{uuidA,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 10, .count = 3}}},
                              {uuidB,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 4, .count = 8}}}})},
        {});
    expectValidatorMetrics(capturer, {.recorded = 1, .evicted = 1, .skipped = 1, .validated = 0});
    applyBatch(
        validator,
        {makeWatermarkEntry(kEnd),
         makeCheckpointEntry(kEnd,
                             {{uuidA,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 21, .count = 1}}}})},
        {{.ts = kT3,
          .uuid = uuidA,
          .delta = ReplicatedMetadataDelta{
              .metadata = {.sizeCount = CollectionSizeCount{.size = 11, .count = -2}}}}});
    expectValidatorMetrics(capturer, {.recorded = 2, .evicted = 2, .skipped = 1, .validated = 1});
}

class SizeCountCheckpointValidatorTest : public SizeCountCheckpointValidatorRecoveryTest {
protected:
    void setUp() override {
        SizeCountCheckpointValidatorRecoveryTest::setUp();

        // `validator` needs to adopt one checkpoint before it can begin performing validation, so
        // we do that here.
        test_helpers::insertSizeCountEntry(
            operationContext(),
            *sizeCountStore,
            adoptUuid,
            SizeCountStore::Entry{.timestamp = kAdopt, .size = 5, .count = 1});
        validator.preprocessOplogEntry(makeWatermarkEntry(kAdopt));
        validator.preprocessOplogEntry(
            makeCheckpointEntry(kAdopt,
                                {{adoptUuid,
                                  CollectionReplicatedMetadata{
                                      .sizeCount = CollectionSizeCount{.size = 5, .count = 1}}}}));
        validator.mergeAndValidate(operationContext(), {}, oplogUuid);
    }

    const UUID adoptUuid = UUID::gen();
};

TEST_F(SizeCountCheckpointValidatorTest, NoWatermarkNoCheckpointPasses) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    applyBatch(validator,
               /*ops=*/{},
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT1,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}}});
    expectValidatorMetrics(capturer, {.recorded = 0, .evicted = 0, .skipped = 0, .validated = 0});
}

TEST_F(SizeCountCheckpointValidatorTest, WatermarkThenCheckpointAcrossBatches) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    applyBatch(validator,
               /*ops=*/{},
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT1,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}}});
    applyBatch(validator, {makeWatermarkEntry(kT1)}, {});
    // The watermark survives the batch boundary until its checkpoint arrives.
    expectValidatorMetrics(capturer, {.recorded = 1, .evicted = 0, .skipped = 0, .validated = 0});
    applyBatch(
        validator,
        {makeCheckpointEntry(kT1,
                             {{uuid,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}})},
        {});
    expectValidatorMetrics(capturer, {.recorded = 1, .evicted = 1, .skipped = 0, .validated = 1});
}

TEST_F(SizeCountCheckpointValidatorTest, TwoWatermarksTwoCheckpoints) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 50, .count = 5}}}}),
        makeWatermarkEntry(kT3),
        makeCheckpointEntry(kT3,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 60, .count = 6}}}})};
    applyBatch(
        validator,
        ops,
        std::vector<ValidatorDeltaRecord>{
            {.ts = kT0,
             .uuid = uuid,
             .delta =
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 50, .count = 5}}}},
            {.ts = kT2,
             .uuid = uuid,
             .delta = ReplicatedMetadataDelta{
                 .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}}});
    expectValidatorMetrics(capturer, {.recorded = 2, .evicted = 2, .skipped = 0, .validated = 2});
}

TEST_F(SizeCountCheckpointValidatorTest, TwoWatermarksCheckpointForSecondOnly) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuidA = UUID::gen();
    const UUID uuidB = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeWatermarkEntry(kT3),
        makeWatermarkEntry(kT4),
        makeCheckpointEntry(kT3,
                            {{uuidA,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 10, .count = 1}}},
                             {uuidB,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}})};
    applyBatch(
        validator,
        ops,
        std::vector<ValidatorDeltaRecord>{
            {.ts = kT0,
             .uuid = uuidA,
             .delta =
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}},
            {.ts = kT2,
             .uuid = uuidB,
             .delta = ReplicatedMetadataDelta{
                 .metadata = {.sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}}});
    // The checkpoint evicts kT1 and kT3 but not the newer kT4 watermark.
    expectValidatorMetrics(capturer, {.recorded = 3, .evicted = 2, .skipped = 0, .validated = 1});
}

TEST_F(SizeCountCheckpointValidatorTest, CreatedAndDroppedInBatchExcluded) {
    const UUID uuid = UUID::gen();
    const UUID otherUuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{otherUuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}})};
    applyBatch(
        validator,
        ops,
        {{.ts = kT0,
          .uuid = uuid,
          .delta =
              ReplicatedMetadataDelta{
                  .metadata = {.sizeCount = CollectionSizeCount{.size = 0, .count = 0},
                               .hash = kEmptyCollectionValidationHash},
                  .state = DDLState::kCreated}},
         {.ts = kT0, .uuid = uuid, .delta = ReplicatedMetadataDelta{.state = DDLState::kDropped}},
         {.ts = kT0,
          .uuid = otherUuid,
          .delta = ReplicatedMetadataDelta{
              .metadata = {.sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}}});
}

TEST_F(SizeCountCheckpointValidatorTest, DropOfKnownCollectionCheckedByMembership) {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 100, .count = 10},
                                  .hash = kEmptyCollectionValidationHash}}}),
        makeWatermarkEntry(kT3),
        makeCheckpointEntry(kT3, {{uuid, boost::none}})};
    applyBatch(
        validator,
        ops,
        {{.ts = kT0,
          .uuid = uuid,
          .delta =
              ReplicatedMetadataDelta{
                  .metadata = {.sizeCount = CollectionSizeCount{.size = 100, .count = 10},
                               .hash = kEmptyCollectionValidationHash},
                  .state = DDLState::kCreated}},
         {.ts = kT2, .uuid = uuid, .delta = ReplicatedMetadataDelta{.state = DDLState::kDropped}}});
}

// TODO(SERVER-138264): Convert this to a DEATH_TEST.
TEST_F(SizeCountCheckpointValidatorTest, DropOfUnknownCollectionExcluded) {
    const UUID uuid = UUID::gen();
    const UUID otherUuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{otherUuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}})};
    applyBatch(
        validator,
        ops,
        {{.ts = kT0, .uuid = uuid, .delta = ReplicatedMetadataDelta{.state = DDLState::kDropped}},
         {.ts = kT0,
          .uuid = otherUuid,
          .delta = ReplicatedMetadataDelta{
              .metadata = {.sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}}});
}

TEST_F(SizeCountCheckpointValidatorTest, CreatedCollectionStartsAtZero) {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(
            kT1,
            {{uuid,
              CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 0, .count = 0},
                                           .hash = kEmptyCollectionValidationHash}}})};
    applyBatch(validator,
               ops,
               {{.ts = kT0,
                 .uuid = uuid,
                 .delta = ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 0, .count = 0},
                                  .hash = kEmptyCollectionValidationHash},
                     .state = DDLState::kCreated}}});
}

TEST_F(SizeCountCheckpointValidatorTest, CreatedCollectionThenCrudOperationValidates) {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 15, .count = 2}}}})};
    applyBatch(validator,
               ops,
               {{.ts = kT0,
                 .uuid = uuid,
                 .delta =
                     ReplicatedMetadataDelta{
                         .metadata = {.sizeCount = CollectionSizeCount{.size = 0, .count = 0},
                                      .hash = kEmptyCollectionValidationHash},
                         .state = DDLState::kCreated}},
                {.ts = kT0,
                 .uuid = uuid,
                 .delta = ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 15, .count = 2}}}}});
}

TEST_F(SizeCountCheckpointValidatorTest, CreatedThenCrudThenDroppedExcluded) {
    const UUID uuid = UUID::gen();
    const UUID otherUuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{otherUuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}})};
    applyBatch(
        validator,
        ops,
        {{.ts = kT0,
          .uuid = uuid,
          .delta =
              ReplicatedMetadataDelta{
                  .metadata = {.sizeCount = CollectionSizeCount{.size = 0, .count = 0},
                               .hash = kEmptyCollectionValidationHash},
                  .state = DDLState::kCreated}},
         {.ts = kT0,
          .uuid = uuid,
          .delta =
              ReplicatedMetadataDelta{
                  .metadata = {.sizeCount = CollectionSizeCount{.size = 15, .count = 2}}}},
         {.ts = kT0, .uuid = uuid, .delta = ReplicatedMetadataDelta{.state = DDLState::kDropped}},
         {.ts = kT0,
          .uuid = otherUuid,
          .delta = ReplicatedMetadataDelta{
              .metadata = {.sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}}});
}

TEST_F(SizeCountCheckpointValidatorTest,
       CreatedThenDroppedAcrossIntervalsWithoutCheckpointExcluded) {
    const UUID uuid = UUID::gen();
    const UUID otherUuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeWatermarkEntry(kT3),
        makeCheckpointEntry(kT3,
                            {{otherUuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}})};
    applyBatch(
        validator,
        ops,
        {{.ts = kT0,
          .uuid = uuid,
          .delta =
              ReplicatedMetadataDelta{
                  .metadata = {.sizeCount = CollectionSizeCount{.size = 0, .count = 0},
                               .hash = kEmptyCollectionValidationHash},
                  .state = DDLState::kCreated}},
         {.ts = kT2, .uuid = uuid, .delta = ReplicatedMetadataDelta{.state = DDLState::kDropped}},
         {.ts = kT2,
          .uuid = otherUuid,
          .delta = ReplicatedMetadataDelta{
              .metadata = {.sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}}});
}

TEST_F(SizeCountCheckpointValidatorTest, DeltasPastNewestWatermarkStayOutOfCheckpoint) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    const UUID tailUuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}})};
    applyBatch(
        validator,
        ops,
        std::vector<ValidatorDeltaRecord>{
            {.ts = kT0,
             .uuid = uuid,
             .delta =
                 ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}},
            {.ts = kT3,
             .uuid = tailUuid,
             .delta = ReplicatedMetadataDelta{
                 .metadata = {.sizeCount = CollectionSizeCount{.size = 30, .count = 3}}}}});
    expectValidatorMetrics(capturer, {.recorded = 1, .evicted = 1, .skipped = 0, .validated = 1});
}

TEST_F(SizeCountCheckpointValidatorTest, ExcludesOplogUuidFromCheckpointPayloads) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID collB = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{oplogUuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 5, .count = 1}}},
                             {collB,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}})};
    applyBatch(validator,
               ops,
               {{.ts = kT0,
                 .uuid = collB,
                 .delta = ReplicatedMetadataDelta{
                     .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}}});
    expectValidatorMetrics(capturer, {.recorded = 1, .evicted = 1, .skipped = 0, .validated = 1});
}

TEST_F(SizeCountCheckpointValidatorTest, CheckpointsValidatedCounterIncrementsWhenParamOn) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}})};
    applyBatch(validator,
               ops,
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT0,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}}});
    expectValidatorMetrics(capturer, {.recorded = 1, .evicted = 1, .skipped = 0, .validated = 1});
}

TEST_F(SizeCountCheckpointValidatorTest, CheckpointsValidatedCounterIncrementsWhenParamOff) {
    unittest::ServerParameterGuard fassertOnPerCollectionContinuousInternodeValidationFailure{
        "fassertOnPerCollectionContinuousInternodeValidationFailure", false};
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}})};
    applyBatch(validator,
               ops,
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT0,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}}});
    // Eviction does not depend on the parameter, so memory is bounded even with validation off.
    expectValidatorMetrics(capturer, {.recorded = 1, .evicted = 1, .skipped = 0, .validated = 1});
}

TEST_F(SizeCountCheckpointValidatorTest, WatermarksWithoutCheckpointsAreNotEvicted) {
    otel::metrics::OtelMetricsCapturer capturer;
    applyBatch(validator, {makeWatermarkEntry(kT1), makeWatermarkEntry(kT2)}, {});
    applyBatch(validator, {makeWatermarkEntry(kT3)}, {});

    // The gap between recorded and evicted watermarks is the number of intervals the validator is
    // retaining in memory.
    expectValidatorMetrics(capturer, {.recorded = 3, .evicted = 0, .skipped = 0, .validated = 0});
}

TEST_F(SizeCountCheckpointValidatorTest, ResetWithPendingIntervalsIncrementsWatermarksEvicted) {
    otel::metrics::OtelMetricsCapturer capturer;
    applyBatch(validator, {makeWatermarkEntry(kT1), makeWatermarkEntry(kT2)}, {});
    applyBatch(validator, {makeWatermarkEntry(kT3)}, {});
    expectValidatorMetrics(capturer, {.recorded = 3, .evicted = 0, .skipped = 0, .validated = 0});

    validator.reset();

    expectValidatorMetrics(capturer, {.recorded = 3, .evicted = 3, .skipped = 0, .validated = 0});
}

TEST_F(SizeCountCheckpointValidatorTest, DeltaAfterNewestWatermarkCarriesIntoNextInterval) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    // The kT3 delta is after the newest watermark, so it stays out of the kT1 checkpoint.
    applyBatch(validator,
               {makeWatermarkEntry(kT1),
                makeCheckpointEntry(kT1, {{uuid, makeSizeCount({.size = 10, .count = 1})}})},
               {makeDelta(kT0, uuid, {.size = 10, .count = 1}),
                makeDelta(kT3, uuid, {.size = 5, .count = 1})});

    // The carried-over delta is folded into the next interval on top of the advanced baseline.
    applyBatch(validator,
               {makeWatermarkEntry(kEnd),
                makeCheckpointEntry(kEnd, {{uuid, makeSizeCount({.size = 15, .count = 2})}})},
               {});

    expectValidatorMetrics(capturer, {.recorded = 2, .evicted = 2, .skipped = 0, .validated = 2});
}

TEST_F(SizeCountCheckpointValidatorTest, ValidationUsesBaselineAdvancedWhileDisabled) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    {
        unittest::ServerParameterGuard disabled{
            "fassertOnPerCollectionContinuousInternodeValidationFailure", false};
        applyBatch(validator,
                   {makeWatermarkEntry(kT1),
                    makeCheckpointEntry(kT1, {{uuid, makeSizeCount({.size = 10, .count = 1})}})},
                   {makeDelta(kT0, uuid, {.size = 10, .count = 1})});
    }

    expectValidatorMetrics(capturer, {.recorded = 1, .evicted = 1, .skipped = 0, .validated = 1});

    // The expected contents are the baseline advanced while disabled plus the new delta. A baseline
    // that did not advance would not match the primary's (15, 2) and would fassert.
    applyBatch(validator,
               {makeWatermarkEntry(kT3),
                makeCheckpointEntry(kT3, {{uuid, makeSizeCount({.size = 15, .count = 2})}})},
               {makeDelta(kT2, uuid, {.size = 5, .count = 1})});

    expectValidatorMetrics(capturer, {.recorded = 2, .evicted = 2, .skipped = 0, .validated = 2});
}

TEST_F(SizeCountCheckpointValidatorTest, DeltasFromMultipleWorkerBuffersAreMerged) {
    otel::metrics::OtelMetricsCapturer capturer;
    const UUID uuid = UUID::gen();
    for (const auto& entry : std::vector<repl::OplogEntry>{
             makeWatermarkEntry(kT1),
             makeWatermarkEntry(kT3),
             makeCheckpointEntry(kT3, {{uuid, makeSizeCount({.size = 40, .count = 4})}})}) {
        validator.preprocessOplogEntry(entry);
    }

    // The same collection is written by two workers, with timestamps unsorted within and across
    // buffers. Only the sum across both intervals matches the checkpoint.
    const ValidatorBuffers buffers{
        {makeDelta(kT2, uuid, {.size = 10, .count = 1}),
         makeDelta(kT0, uuid, {.size = 20, .count = 2})},
        {makeDelta(kT0, uuid, {.size = 5, .count = 1}),
         makeDelta(kT2, uuid, {.size = 5, .count = 0})},
    };
    validator.mergeAndValidate(operationContext(), buffers, oplogUuid);

    expectValidatorMetrics(capturer, {.recorded = 2, .evicted = 2, .skipped = 0, .validated = 1});
}

class SizeCountCheckpointValidatorDeathTest : public SizeCountCheckpointValidatorTest {};

DEATH_TEST_F(SizeCountCheckpointValidatorDeathTest, WrongSizeInCheckpointFasserts, "13626005") {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 999, .count = 5}}}})};
    applyBatch(validator,
               ops,
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT0,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 50, .count = 5}}}}});
}

DEATH_TEST_F(SizeCountCheckpointValidatorDeathTest, WrongCountInCheckpointFasserts, "13626005") {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 50, .count = 999}}}})};
    applyBatch(validator,
               ops,
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT0,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 50, .count = 5}}}}});
}

DEATH_TEST_F(SizeCountCheckpointValidatorDeathTest,
             RecordedCheckpointWithoutWatermarkFasserts,
             "does not match any recorded watermark") {
    const UUID uuid = UUID::gen();
    applyBatch(
        validator,
        {makeCheckpointEntry(kT1,
                             {{uuid,
                               CollectionReplicatedMetadata{
                                   .sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}})},
        {});
}

DEATH_TEST_F(SizeCountCheckpointValidatorDeathTest, WrongHashInCheckpointFasserts, "13626005") {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(
            kT1,
            {{uuid,
              CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 50, .count = 5},
                                           .hash = 999}}})};
    applyBatch(validator,
               ops,
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT0,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 50, .count = 5},
                                     .hash = 7}}}});
}

TEST_F(SizeCountCheckpointValidatorTest, HashesFoldAcrossDeltasAndCheckpoints) {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(
            kT1,
            {{uuid,
              CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 30, .count = 2},
                                           .hash = 0b0011 ^ 0b0101}}}),
        makeWatermarkEntry(kT3),
        makeCheckpointEntry(
            kT3,
            {{uuid,
              CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 40, .count = 3},
                                           .hash = 0b0011 ^ 0b0101 ^ 0b1000}}})};
    applyBatch(validator,
               ops,
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT0,
                    .uuid = uuid,
                    .delta =
                        ReplicatedMetadataDelta{
                            .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1},
                                         .hash = 0b0011}}},
                   {.ts = Timestamp(Seconds(100), 1),
                    .uuid = uuid,
                    .delta =
                        ReplicatedMetadataDelta{
                            .metadata = {.sizeCount = CollectionSizeCount{.size = 20, .count = 1},
                                         .hash = 0b0101}}},
                   {.ts = kT2,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1},
                                     .hash = 0b1000}}}});
}

TEST_F(SizeCountCheckpointValidatorTest, HashesFoldAcrossIntervalsForLaterCheckpoint) {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeWatermarkEntry(kT3),
        makeCheckpointEntry(
            kT3,
            {{uuid,
              CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 30, .count = 2},
                                           .hash = 0b0011 ^ 0b0101}}})};
    applyBatch(validator,
               ops,
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT0,
                    .uuid = uuid,
                    .delta =
                        ReplicatedMetadataDelta{
                            .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1},
                                         .hash = 0b0011}}},
                   {.ts = kT2,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 20, .count = 1},
                                     .hash = 0b0101}}}});
}

TEST_F(SizeCountCheckpointValidatorTest, HashesFoldAcrossBatchesBeforeWatermark) {
    const UUID uuid = UUID::gen();
    applyBatch(validator,
               /*ops=*/{},
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT0,
                    .uuid = uuid,
                    .delta =
                        ReplicatedMetadataDelta{
                            .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1},
                                         .hash = 0b0011}}},
                   {.ts = Timestamp(Seconds(100), 1),
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 20, .count = 1},
                                     .hash = 0b0101}}}});
    // The delta past the watermark belongs to a later checkpoint.
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(
            kT1,
            {{uuid,
              CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 30, .count = 2},
                                           .hash = 0b0011 ^ 0b0101}}})};
    applyBatch(validator,
               ops,
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT2,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 40, .count = 1},
                                     .hash = 0b1000}}}});
}

TEST_F(SizeCountCheckpointValidatorTest, HashesValidatePerCollection) {
    const UUID uuidA = UUID::gen();
    const UUID uuidB = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(
            kT1,
            {{uuidA,
              CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 10, .count = 1},
                                           .hash = 0b0011}},
             {uuidB,
              CollectionReplicatedMetadata{.sizeCount = CollectionSizeCount{.size = 20, .count = 1},
                                           .hash = 0b0101}}})};
    applyBatch(validator,
               ops,
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT0,
                    .uuid = uuidA,
                    .delta =
                        ReplicatedMetadataDelta{
                            .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1},
                                         .hash = 0b0011}}},
                   {.ts = Timestamp(Seconds(100), 1),
                    .uuid = uuidB,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 20, .count = 1},
                                     .hash = 0b0101}}}});
}

DEATH_TEST_F(SizeCountCheckpointValidatorDeathTest,
             WatermarkTimestampIsMonotonicallyIncreasing,
             "Watermark timestamps should be monotonically increasing") {
    const std::vector<repl::OplogEntry> ops{makeWatermarkEntry(kT1), makeWatermarkEntry(kT1)};
    applyBatch(validator, ops, {});
}

DEATH_TEST_F(SizeCountCheckpointValidatorDeathTest,
             CollectionRecreateNotSupported,
             "Checkpoint validation does not support collection re-creation") {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{makeWatermarkEntry(kT1)};
    applyBatch(
        validator,
        ops,
        {{.ts = kT0, .uuid = uuid, .delta = ReplicatedMetadataDelta{.state = DDLState::kDropped}},
         {.ts = kT0, .uuid = uuid, .delta = ReplicatedMetadataDelta{.state = DDLState::kCreated}}});
}

DEATH_TEST_F(SizeCountCheckpointValidatorDeathTest,
             PrimaryWritesCollectionCreatedAndDroppedInIntervalFasserts,
             "13626001") {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}})};
    applyBatch(
        validator,
        ops,
        {{.ts = kT0,
          .uuid = uuid,
          .delta =
              ReplicatedMetadataDelta{
                  .metadata = {.sizeCount = CollectionSizeCount{.size = 0, .count = 0},
                               .hash = kEmptyCollectionValidationHash},
                  .state = DDLState::kCreated}},
         {.ts = kT0, .uuid = uuid, .delta = ReplicatedMetadataDelta{.state = DDLState::kDropped}}});
}

DEATH_TEST_F(SizeCountCheckpointValidatorDeathTest,
             PrimaryDropsCollectionCreatedAndDroppedInIntervalFasserts,
             "13626002") {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{makeWatermarkEntry(kT1),
                                            makeCheckpointEntry(kT1, {{uuid, boost::none}})};
    applyBatch(
        validator,
        ops,
        {{.ts = kT0,
          .uuid = uuid,
          .delta =
              ReplicatedMetadataDelta{
                  .metadata = {.sizeCount = CollectionSizeCount{.size = 0, .count = 0},
                               .hash = kEmptyCollectionValidationHash},
                  .state = DDLState::kCreated}},
         {.ts = kT0, .uuid = uuid, .delta = ReplicatedMetadataDelta{.state = DDLState::kDropped}}});
}

DEATH_TEST_F(SizeCountCheckpointValidatorDeathTest,
             PrimaryWritesCollectionDroppedAcrossIntervalsFasserts,
             "13626004") {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 100, .count = 10},
                                  .hash = kEmptyCollectionValidationHash}}}),
        makeWatermarkEntry(kT3),
        makeCheckpointEntry(kT3,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 110, .count = 11}}}})};
    applyBatch(
        validator,
        ops,
        {{.ts = kT0,
          .uuid = uuid,
          .delta =
              ReplicatedMetadataDelta{
                  .metadata = {.sizeCount = CollectionSizeCount{.size = 100, .count = 10},
                               .hash = kEmptyCollectionValidationHash},
                  .state = DDLState::kCreated}},
         {.ts = kT2, .uuid = uuid, .delta = ReplicatedMetadataDelta{.state = DDLState::kDropped}}});
}

DEATH_TEST_F(SizeCountCheckpointValidatorDeathTest,
             PrimaryOmitsCollectionDroppedAcrossIntervalsFasserts,
             "13626006") {
    const UUID uuid = UUID::gen();
    const UUID otherUuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 100, .count = 10},
                                  .hash = kEmptyCollectionValidationHash}}}),
        makeWatermarkEntry(kT3),
        makeCheckpointEntry(kT3,
                            {{otherUuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}})};
    applyBatch(
        validator,
        ops,
        {{.ts = kT0,
          .uuid = uuid,
          .delta =
              ReplicatedMetadataDelta{
                  .metadata = {.sizeCount = CollectionSizeCount{.size = 100, .count = 10},
                               .hash = kEmptyCollectionValidationHash},
                  .state = DDLState::kCreated}},
         {.ts = kT2, .uuid = uuid, .delta = ReplicatedMetadataDelta{.state = DDLState::kDropped}},
         {.ts = kT2,
          .uuid = otherUuid,
          .delta = ReplicatedMetadataDelta{
              .metadata = {.sizeCount = CollectionSizeCount{.size = 20, .count = 2}}}}});
}

DEATH_TEST_F(SizeCountCheckpointValidatorDeathTest,
             WatermarkTimestampIsNotUnique,
             "Timestamp of a replicated metadata delta") {
    const UUID uuid = UUID::gen();
    const std::vector<repl::OplogEntry> ops{makeWatermarkEntry(kT1)};
    applyBatch(validator,
               ops,
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT1,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 50, .count = 5}}}}});
}

}  // namespace
}  // namespace replicated_fast_count
}  // namespace mongo
