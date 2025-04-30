/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include <queue>

#include "mongo/bson/json.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_analyzer.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_producer.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace unified_write_executor {
namespace {

const NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("test", "coll0");
const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test", "coll1");
const DatabaseVersion dbVersion(UUID::gen(), Timestamp(1, 0));
const ShardId shardId0("shard0");
const ShardId shardId1("shard1");
const ShardEndpoint shard0(shardId0, ShardVersion::UNSHARDED(), dbVersion);
const ShardEndpoint shard1(shardId1, ShardVersion::UNSHARDED(), dbVersion);

class MockWriteOpAnalyzer : public WriteOpAnalyzer {
public:
    MockWriteOpAnalyzer(std::map<WriteOpId, Analysis> opAnalysis)
        : _opAnalysis(std::move(opAnalysis)) {}

    Analysis analyze(OperationContext* opCtx,
                     const RoutingContext& routingCtx,
                     const WriteOp& writeOp) override {
        auto it = _opAnalysis.find(writeOp.getId());
        tassert(
            10346702, "Write op id should be found in the analysis data", it != _opAnalysis.end());
        return it->second;
    }

    void setOpAnalysis(std::map<WriteOpId, Analysis> opAnalysis) {
        _opAnalysis = std::move(opAnalysis);
    }

    std::map<WriteOpId, Analysis> _opAnalysis;
};

TEST(UnifiedWriteExecutorBatcherTest,
     OrderedBatcherBatchesSingleShardOpByShardIdGeneratingSingleOpBatches) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {shard0}}},
        {1, {kSingleShard, {shard1}}},
        {2, {kSingleShard, {shard0}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0], [1], [2]
    auto optBatch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch1.has_value());
    auto& batch1 = std::get<SingleShardWriteBatch>(*optBatch1);
    ASSERT_EQ(batch1.shard.shardName, shardId0);
    ASSERT_EQ(batch1.ops.size(), 1);
    ASSERT_EQ(batch1.ops[0].getId(), 0);

    auto optBatch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch2.has_value());
    auto& batch2 = std::get<SingleShardWriteBatch>(*optBatch2);
    ASSERT_EQ(batch2.shard.shardName, shardId1);
    ASSERT_EQ(batch2.ops.size(), 1);
    ASSERT_EQ(batch2.ops[0].getId(), 1);

    auto optBatch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch3.has_value());
    auto& batch3 = std::get<SingleShardWriteBatch>(*optBatch3);
    ASSERT_EQ(batch3.shard.shardName, shardId0);
    ASSERT_EQ(batch3.ops.size(), 1);
    ASSERT_EQ(batch3.ops[0].getId(), 2);

    auto optBatch4 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(optBatch4.has_value());
}

TEST(UnifiedWriteExecutorBatcherTest,
     OrderedBatcherBatchesSingleShardOpByShardIdGeneratingMultiOpBatches) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {shard0}}},
        {1, {kSingleShard, {shard0}}},
        {2, {kSingleShard, {shard1}}},
        {3, {kSingleShard, {shard1}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [2, 3]
    auto optBatch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch1.has_value());
    auto& batch1 = std::get<SingleShardWriteBatch>(*optBatch1);
    ASSERT_EQ(batch1.shard.shardName, shardId0);
    ASSERT_EQ(batch1.ops.size(), 2);
    ASSERT_EQ(batch1.ops[0].getId(), 0);
    ASSERT_EQ(batch1.ops[1].getId(), 1);

    auto optBatch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch2.has_value());
    auto& batch2 = std::get<SingleShardWriteBatch>(*optBatch2);
    ASSERT_EQ(batch2.shard.shardName, shardId1);
    ASSERT_EQ(batch2.ops.size(), 2);
    ASSERT_EQ(batch2.ops[0].getId(), 2);
    ASSERT_EQ(batch2.ops[1].getId(), 3);

    auto optBatch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(optBatch3.has_value());
}

TEST(UnifiedWriteExecutorBatcherTest, OrderedBatcherBatchesSingleShardOpByWriteType) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {shard0}}},
        {1, {kSingleShard, {shard0}}},
        {2, {kMultiShard, {}}},
        {3, {kSingleShard, {shard1}}},
        {4, {kSingleShard, {shard1}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [2], [3, 4]
    auto optBatch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch1.has_value());
    auto& batch1 = std::get<SingleShardWriteBatch>(*optBatch1);
    ASSERT_EQ(batch1.shard.shardName, shardId0);
    ASSERT_EQ(batch1.ops.size(), 2);
    ASSERT_EQ(batch1.ops[0].getId(), 0);
    ASSERT_EQ(batch1.ops[1].getId(), 1);

    auto optBatch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch2.has_value());
    ASSERT_TRUE(std::holds_alternative<MultiShardWriteBatch>(*optBatch2));

    auto optBatch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch3.has_value());
    auto& batch3 = std::get<SingleShardWriteBatch>(*optBatch3);
    ASSERT_EQ(batch3.shard.shardName, shardId1);
    ASSERT_EQ(batch3.ops.size(), 2);
    ASSERT_EQ(batch3.ops[0].getId(), 3);
    ASSERT_EQ(batch3.ops[1].getId(), 4);

    auto optBatch4 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(optBatch4.has_value());
}

TEST(UnifiedWriteExecutorBatcherTest, OrderedBatcherReprocessesWriteOps) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {shard0}}},
        {1, {kSingleShard, {shard0}}},
        {2, {kSingleShard, {shard1}}},
        {3, {kSingleShard, {shard1}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [1(reprocess)], [2, 3]
    auto optBatch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch1.has_value());
    auto& batch1 = std::get<SingleShardWriteBatch>(*optBatch1);
    ASSERT_EQ(batch1.shard.shardName, shardId0);
    ASSERT_EQ(batch1.ops.size(), 2);
    ASSERT_EQ(batch1.ops[0].getId(), 0);
    ASSERT_EQ(batch1.ops[1].getId(), 1);

    batcher.markOpReprocess({batch1.ops[1]});

    auto optBatch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch2.has_value());
    auto& batch2 = std::get<SingleShardWriteBatch>(*optBatch2);
    ASSERT_EQ(batch2.shard.shardName, shardId0);
    ASSERT_EQ(batch2.ops.size(), 1);
    ASSERT_EQ(batch2.ops[0].getId(), 1);

    auto optBatch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch3.has_value());
    auto& batch3 = std::get<SingleShardWriteBatch>(*optBatch3);
    ASSERT_EQ(batch3.shard.shardName, shardId1);
    ASSERT_EQ(batch3.ops.size(), 2);
    ASSERT_EQ(batch3.ops[0].getId(), 2);
    ASSERT_EQ(batch3.ops[1].getId(), 3);

    auto optBatch4 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(optBatch4.has_value());
}

TEST(UnifiedWriteExecutorBatcherTest, OrderedBatcherReprocessesWriteOpsWithChunkMigration) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {shard0}}},
        {1, {kSingleShard, {shard0}}},
        {2, {kSingleShard, {shard1}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [1(reprocess), 2]
    auto optBatch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch1.has_value());
    auto& batch1 = std::get<SingleShardWriteBatch>(*optBatch1);
    ASSERT_EQ(batch1.shard.shardName, shardId0);
    ASSERT_EQ(batch1.ops.size(), 2);
    ASSERT_EQ(batch1.ops[0].getId(), 0);
    ASSERT_EQ(batch1.ops[1].getId(), 1);

    batcher.markOpReprocess({batch1.ops[1]});
    analyzer.setOpAnalysis({
        {0, {kSingleShard, {shard0}}},
        {1, {kSingleShard, {shard1}}},
        {2, {kSingleShard, {shard1}}},
    });

    auto optBatch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(optBatch2.has_value());
    auto& batch2 = std::get<SingleShardWriteBatch>(*optBatch2);
    ASSERT_EQ(batch2.shard.shardName, shardId1);
    ASSERT_EQ(batch2.ops.size(), 2);
    ASSERT_EQ(batch2.ops[0].getId(), 1);
    ASSERT_EQ(batch2.ops[1].getId(), 2);

    auto optBatch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(optBatch3.has_value());
}
}  // namespace
}  // namespace unified_write_executor
}  // namespace mongo
