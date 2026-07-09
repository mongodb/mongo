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
