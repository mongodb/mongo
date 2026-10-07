// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/replicated_fast_count/size_count_checkpoint_validator.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/log_capture.h"
#include "mongo/unittest/unittest.h"

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

}  // namespace
}  // namespace replicated_fast_count
}  // namespace mongo
