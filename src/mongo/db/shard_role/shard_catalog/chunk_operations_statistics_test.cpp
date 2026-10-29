// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/chunk_operations_statistics.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/unittest/unittest.h"

#include <functional>

namespace mongo {
namespace {

using ChunkOperationType = ChunkOperationsStatistics::ChunkOperationType;

BSONObj report(const ChunkOperationsStatistics& stats) {
    BSONObjBuilder builder;
    stats.report(builder);
    return builder.obj();
}

void registerTimes(std::function<void()> fn, int times) {
    for (int i = 0; i < times; ++i) {
        fn();
    }
}

TEST(ChunkOperationsStatisticsTest, FreshStatsReportAllFieldsAsZero) {
    ChunkOperationsStatistics stats;
    auto obj = report(stats);

    for (const auto* field : {"countSplitChunkStarted",
                              "countSplitChunkCommitted",
                              "countSplitChunkAborted",
                              "countMergeChunksStarted",
                              "countMergeChunksCommitted",
                              "countMergeChunksAborted",
                              "countMergeAllChunksStarted",
                              "countMergeAllChunksCommitted",
                              "countMergeAllChunksAborted",
                              "countMoveRangeStarted",
                              "countMoveRangeCommitted",
                              "countMoveRangeAborted",
                              "countSplitChunkResultingChunks",
                              "countMergeAllChunksMerged",
                              "countMoveRangeChunksMoved",
                              "countMoveRangeFirstChunkReceived",
                              "countLocalChunkOperationsMetadataCommits",
                              "countChunksCommittedToShardCatalog"}) {
        ASSERT_TRUE(obj.hasField(field)) << "missing field " << field;
        ASSERT_EQ(obj.getIntField(field), 0) << "field " << field;
    }
}

TEST(ChunkOperationsStatisticsTest, LifecycleCountersFanOutToTheCorrectType) {
    ChunkOperationsStatistics stats;

    // Distinct counts per (type, phase) so a wrong switch branch or a mismatched report() field
    // name is caught.
    registerTimes([&] { stats.registerStarted(ChunkOperationType::kSplitChunk); }, 1);
    registerTimes([&] { stats.registerCommitted(ChunkOperationType::kSplitChunk); }, 2);
    registerTimes([&] { stats.registerAborted(ChunkOperationType::kSplitChunk); }, 3);
    registerTimes([&] { stats.registerStarted(ChunkOperationType::kMergeChunks); }, 4);
    registerTimes([&] { stats.registerCommitted(ChunkOperationType::kMergeChunks); }, 5);
    registerTimes([&] { stats.registerAborted(ChunkOperationType::kMergeChunks); }, 6);
    registerTimes([&] { stats.registerStarted(ChunkOperationType::kMergeAllChunks); }, 7);
    registerTimes([&] { stats.registerCommitted(ChunkOperationType::kMergeAllChunks); }, 8);
    registerTimes([&] { stats.registerAborted(ChunkOperationType::kMergeAllChunks); }, 9);
    registerTimes([&] { stats.registerStarted(ChunkOperationType::kMoveRange); }, 10);
    registerTimes([&] { stats.registerCommitted(ChunkOperationType::kMoveRange); }, 11);
    registerTimes([&] { stats.registerAborted(ChunkOperationType::kMoveRange); }, 12);

    auto obj = report(stats);
    ASSERT_EQ(obj.getIntField("countSplitChunkStarted"), 1);
    ASSERT_EQ(obj.getIntField("countSplitChunkCommitted"), 2);
    ASSERT_EQ(obj.getIntField("countSplitChunkAborted"), 3);
    ASSERT_EQ(obj.getIntField("countMergeChunksStarted"), 4);
    ASSERT_EQ(obj.getIntField("countMergeChunksCommitted"), 5);
    ASSERT_EQ(obj.getIntField("countMergeChunksAborted"), 6);
    ASSERT_EQ(obj.getIntField("countMergeAllChunksStarted"), 7);
    ASSERT_EQ(obj.getIntField("countMergeAllChunksCommitted"), 8);
    ASSERT_EQ(obj.getIntField("countMergeAllChunksAborted"), 9);
    ASSERT_EQ(obj.getIntField("countMoveRangeStarted"), 10);
    ASSERT_EQ(obj.getIntField("countMoveRangeCommitted"), 11);
    ASSERT_EQ(obj.getIntField("countMoveRangeAborted"), 12);
}

TEST(ChunkOperationsStatisticsTest, VolumeCountersAccumulateAmounts) {
    ChunkOperationsStatistics stats;

    stats.registerSplitChunkResultingChunks(2);
    stats.registerSplitChunkResultingChunks(3);
    stats.registerMergeAllChunksMerged(10);
    stats.registerMoveRangeChunksMoved(1);
    stats.registerMoveRangeFirstChunkReceived();

    auto obj = report(stats);
    ASSERT_EQ(obj.getIntField("countSplitChunkResultingChunks"), 5);
    ASSERT_EQ(obj.getIntField("countMergeAllChunksMerged"), 10);
    ASSERT_EQ(obj.getIntField("countMoveRangeChunksMoved"), 1);
    ASSERT_EQ(obj.getIntField("countMoveRangeFirstChunkReceived"), 1);
}

TEST(ChunkOperationsStatisticsTest, LocalCommitBumpsBothCommitCountAndChunkTotal) {
    ChunkOperationsStatistics stats;

    // Each call bumps the commit count by 1 and the chunk total by the supplied amount.
    stats.registerLocalChunkOperationsMetadataCommit(3);
    stats.registerLocalChunkOperationsMetadataCommit(4);

    auto obj = report(stats);
    ASSERT_EQ(obj.getIntField("countLocalChunkOperationsMetadataCommits"), 2);
    ASSERT_EQ(obj.getIntField("countChunksCommittedToShardCatalog"), 7);
}

}  // namespace
}  // namespace mongo
