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

#include "mongo/db/replicated_fast_count/size_count_checkpoint_buffer.h"

#include "mongo/bson/timestamp.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace mongo::replicated_fast_count {
namespace {

TEST(SizeCountCheckpointBufferTest, EmptyBufferStartsWithoutWork) {
    SizeCountCheckpointBuffer buffer;

    EXPECT_FALSE(buffer.hasPendingWork());
    EXPECT_FALSE(buffer.hasInFlightWork());
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST(SizeCountCheckpointBufferTest,
     MergeVisibleScanAccumulatesSizeCountDeltaAndCheckoutMovesBatchInFlight) {
    const Timestamp lastTs(3, 3);
    const UUID uuid = UUID::gen();

    SizeCountCheckpointBuffer buffer;
    buffer.mergeVisibleScan(OplogScanResult{
        .deltas = {{uuid,
                    SizeCountDelta{.sizeCount = CollectionSizeCount{.size = 25, .count = 2},
                                   .state = DDLState::kNone}}},
        .lastTimestamp = lastTs,
    });

    EXPECT_TRUE(buffer.hasPendingWork());
    EXPECT_FALSE(buffer.hasInFlightWork());

    auto batch = buffer.checkoutForFlush();
    ASSERT_TRUE(batch.has_value());

    ASSERT_TRUE(batch->lastTimestamp.has_value());
    EXPECT_EQ(*batch->lastTimestamp, lastTs);
    ASSERT_EQ(batch->deltas.size(), 1U);

    const auto& delta = batch->deltas.at(uuid);
    EXPECT_EQ(delta.sizeCount.size, 25);
    EXPECT_EQ(delta.sizeCount.count, 2);
    EXPECT_EQ(delta.state, DDLState::kNone);

    EXPECT_FALSE(buffer.hasPendingWork());
    EXPECT_TRUE(buffer.hasInFlightWork());
}

TEST(SizeCountCheckpointBufferTest, MergeVisibleScanCombinesMultipleVisibleDeltasForSameUuid) {
    const UUID uuid = UUID::gen();
    SizeCountCheckpointBuffer buffer;

    buffer.mergeVisibleScan(OplogScanResult{
        .deltas = {{uuid,
                    SizeCountDelta{.sizeCount = CollectionSizeCount{.size = 10, .count = 1},
                                   .state = DDLState::kNone}}},
        .lastTimestamp = Timestamp(2, 1),
    });
    buffer.mergeVisibleScan(OplogScanResult{
        .deltas = {{uuid,
                    SizeCountDelta{.sizeCount = CollectionSizeCount{.size = -3, .count = 0},
                                   .state = DDLState::kNone}}},
        .lastTimestamp = Timestamp(2, 2),
    });

    auto batch = buffer.checkoutForFlush();
    ASSERT_TRUE(batch.has_value());
    ASSERT_TRUE(batch->lastTimestamp.has_value());
    EXPECT_EQ(*batch->lastTimestamp, Timestamp(2, 2));

    const auto& delta = batch->deltas.at(uuid);
    EXPECT_EQ(delta.sizeCount.size, 7);
    EXPECT_EQ(delta.sizeCount.count, 1);
    EXPECT_EQ(delta.state, DDLState::kNone);
}

TEST(SizeCountCheckpointBufferTest, InFlightBatchIsRetriedUntilAcknowledged) {
    const UUID uuid = UUID::gen();
    SizeCountCheckpointBuffer buffer;

    buffer.mergeVisibleScan(OplogScanResult{
        .deltas = {{uuid,
                    SizeCountDelta{.sizeCount = CollectionSizeCount{.size = 40, .count = 4},
                                   .state = DDLState::kNone}}},
        .lastTimestamp = Timestamp(6, 6),
    });

    auto first = buffer.checkoutForFlush();
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(buffer.hasInFlightWork());

    auto retried = buffer.checkoutForFlush();
    ASSERT_TRUE(retried.has_value());

    EXPECT_EQ(retried->lastTimestamp, first->lastTimestamp);
    ASSERT_EQ(retried->deltas.size(), 1U);

    const auto& retriedDelta = retried->deltas.at(uuid);
    EXPECT_EQ(retriedDelta.sizeCount.size, 40);
    EXPECT_EQ(retriedDelta.sizeCount.count, 4);
    EXPECT_EQ(retriedDelta.state, DDLState::kNone);
}

TEST(SizeCountCheckpointBufferTest, AcknowledgeFlushSuccessClearsInFlight) {
    SizeCountCheckpointBuffer buffer;

    buffer.mergeVisibleScan(OplogScanResult{
        .deltas = {{UUID::gen(),
                    SizeCountDelta{.sizeCount = CollectionSizeCount{.size = 10, .count = 1},
                                   .state = DDLState::kNone}}},
        .lastTimestamp = Timestamp(2, 2),
    });

    ASSERT_TRUE(buffer.checkoutForFlush().has_value());
    ASSERT_TRUE(buffer.hasInFlightWork());

    buffer.acknowledgeFlushSuccess();

    EXPECT_FALSE(buffer.hasInFlightWork());
    EXPECT_FALSE(buffer.hasPendingWork());
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

TEST(SizeCountCheckpointBufferTest, DropThenCreateMergesIntoDroppedAndRecreated) {
    const UUID uuid = UUID::gen();
    SizeCountCheckpointBuffer buffer;

    buffer.mergeVisibleScan(OplogScanResult{
        .deltas = {{uuid,
                    SizeCountDelta{.sizeCount = CollectionSizeCount{},
                                   .state = DDLState::kDropped}}},
        .lastTimestamp = Timestamp(2, 1),
    });
    buffer.mergeVisibleScan(OplogScanResult{
        .deltas = {{uuid,
                    SizeCountDelta{.sizeCount = CollectionSizeCount{.size = 90, .count = 3},
                                   .state = DDLState::kCreated}}},
        .lastTimestamp = Timestamp(2, 2),
    });

    auto batch = buffer.checkoutForFlush();
    ASSERT_TRUE(batch.has_value());
    const auto& delta = batch->deltas.at(uuid);

    EXPECT_EQ(delta.state, DDLState::kDroppedAndRecreated);
    EXPECT_EQ(delta.sizeCount.size, 90);
    EXPECT_EQ(delta.sizeCount.count, 3);
    ASSERT_TRUE(batch->lastTimestamp.has_value());
    EXPECT_EQ(*batch->lastTimestamp, Timestamp(2, 2));
}

TEST(SizeCountCheckpointBufferTest, CreateThenDropCancelsPendingDeltaButRetainsWindowTimestamp) {
    const UUID uuid = UUID::gen();
    SizeCountCheckpointBuffer buffer;

    buffer.mergeVisibleScan(OplogScanResult{
        .deltas = {{uuid,
                    SizeCountDelta{.sizeCount = CollectionSizeCount{},
                                   .state = DDLState::kCreated}}},
        .lastTimestamp = Timestamp(2, 1),
    });
    buffer.mergeVisibleScan(OplogScanResult{
        .deltas = {{uuid,
                    SizeCountDelta{.sizeCount = CollectionSizeCount{},
                                   .state = DDLState::kDropped}}},
        .lastTimestamp = Timestamp(2, 2),
    });

    auto batch = buffer.checkoutForFlush();
    ASSERT_TRUE(batch.has_value());
    EXPECT_TRUE(batch->deltas.empty());
    ASSERT_TRUE(batch->lastTimestamp.has_value());
    EXPECT_EQ(*batch->lastTimestamp, Timestamp(2, 2));
}

TEST(SizeCountCheckpointBufferTest, EmptyScanDoesNotCreatePendingWork) {
    SizeCountCheckpointBuffer buffer;

    buffer.mergeVisibleScan(OplogScanResult{});

    EXPECT_FALSE(buffer.hasPendingWork());
    EXPECT_FALSE(buffer.hasInFlightWork());
    EXPECT_FALSE(buffer.checkoutForFlush().has_value());
}

}  // namespace
}  // namespace mongo::replicated_fast_count
