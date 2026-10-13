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

const Timestamp kT0{Seconds(100), 0};
const Timestamp kT1{Seconds(200), 0};
const Timestamp kT2{Seconds(250), 0};
const Timestamp kT3{Seconds(300), 0};
const Timestamp kEnd{Seconds(1000), 0};

using test_helpers::makeCheckpointEntry;
using test_helpers::makeWatermarkEntry;

class SizeCountCheckpointValidatorTest : public unittest::Test {
protected:
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
        validator.mergeAndValidate({buffer});
    }

    SizeCountCheckpointValidator validator;

    unittest::ServerParameterGuard enableContinuousInternodePerCollectionValidation{
        "enableContinuousInternodePerCollectionValidation", true};
};

TEST_F(SizeCountCheckpointValidatorTest, NoWatermarkNoCheckpointPasses) {
    const UUID uuid = UUID::gen();
    applyBatch(validator,
               /*ops=*/{},
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT1,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}}});
}

TEST_F(SizeCountCheckpointValidatorTest, WatermarkThenCheckpointAcrossBatches) {
    const UUID uuid = UUID::gen();
    applyBatch(validator,
               /*ops=*/{},
               std::vector<ValidatorDeltaRecord>{
                   {.ts = kT1,
                    .uuid = uuid,
                    .delta = ReplicatedMetadataDelta{
                        .metadata = {.sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}}});
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeCheckpointEntry(kT1,
                            {{uuid,
                              CollectionReplicatedMetadata{
                                  .sizeCount = CollectionSizeCount{.size = 10, .count = 1}}}})};
    applyBatch(validator, ops, {});
}

TEST_F(SizeCountCheckpointValidatorTest, TwoWatermarksTwoCheckpoints) {
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
}

TEST_F(SizeCountCheckpointValidatorTest, TwoWatermarksCheckpointForSecondOnly) {
    const UUID uuidA = UUID::gen();
    const UUID uuidB = UUID::gen();
    const std::vector<repl::OplogEntry> ops{
        makeWatermarkEntry(kT1),
        makeWatermarkEntry(kT3),
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
}

TEST_F(SizeCountCheckpointValidatorTest, DeltasPastNewestWatermarkStayOutOfCheckpoint) {
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
             WatermarkTimestampIsMonotonicallyIncreasing,
             "Watermark timestamps should be monotonically increasing") {
    const std::vector<repl::OplogEntry> ops{makeWatermarkEntry(kT1), makeWatermarkEntry(kT1)};
    applyBatch(validator, ops, {});
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
