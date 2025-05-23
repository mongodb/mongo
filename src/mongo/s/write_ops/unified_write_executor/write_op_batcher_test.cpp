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

#include "mongo/s/shard_version_factory.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_analyzer.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_producer.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace unified_write_executor {
namespace {

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

class UnifiedWriteExecutorBatcherTest : public unittest::Test {
public:
    const NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("test", "coll0");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test", "coll1");
    const ShardId shardId0 = ShardId("shard0");
    const ShardId shardId1 = ShardId("shard1");
    const ShardVersion shardVersionNss0Shard0 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(1, 0)}, CollectionPlacement(1, 0)),
        boost::optional<CollectionIndexes>(boost::none));
    const ShardVersion shardVersionNss0Shard1 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(1, 0)}, CollectionPlacement(2, 0)),
        boost::optional<CollectionIndexes>(boost::none));
    const ShardVersion shardVersionNss1Shard0 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(2, 0)}, CollectionPlacement(1, 0)),
        boost::optional<CollectionIndexes>(boost::none));
    const ShardVersion shardVersionNss1Shard1 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(2, 0)}, CollectionPlacement(2, 0)),
        boost::optional<CollectionIndexes>(boost::none));
    const ShardEndpoint nss0Shard0 = ShardEndpoint(shardId0, shardVersionNss0Shard0, boost::none);
    const ShardEndpoint nss0Shard1 = ShardEndpoint(shardId1, shardVersionNss0Shard1, boost::none);
    const ShardEndpoint nss1Shard0 = ShardEndpoint(shardId0, shardVersionNss1Shard0, boost::none);
    const ShardEndpoint nss1Shard1 = ShardEndpoint(shardId1, shardVersionNss1Shard1, boost::none);

    void assertMultiShardSimpleWriteBatch(const WriteBatch& batch,
                                          WriteOpId expectedOpId,
                                          std::vector<ShardEndpoint> expectedShards) {
        ASSERT_TRUE(std::holds_alternative<SimpleWriteBatch>(batch));
        auto& simpleBatch = std::get<SimpleWriteBatch>(batch);

        ASSERT_EQ(expectedShards.size(), simpleBatch.requestByShardId.size());
        for (auto& expectedShard : expectedShards) {
            auto shardRequestIt = simpleBatch.requestByShardId.find(expectedShard.shardName);
            ASSERT_NOT_EQUALS(shardRequestIt, simpleBatch.requestByShardId.end());
            auto& shardRequest = shardRequestIt->second;
            ASSERT_EQ(shardRequest.ops.size(), 1);
            ASSERT_EQ(shardRequest.ops.front().getId(), expectedOpId);
            ASSERT_EQ(shardRequest.versionByNss.size(), 1);
            ASSERT_EQ(shardRequest.versionByNss.begin()->second, expectedShard);
        }
    }

    void reprocessWriteOp(WriteOpBatcher& batcher,
                          WriteBatch& batch,
                          std::set<WriteOpId> reprocessOpIds) {
        ASSERT_TRUE(std::holds_alternative<SimpleWriteBatch>(batch));
        auto& simpleBatch = std::get<SimpleWriteBatch>(batch);
        for (auto& [shardId, request] : simpleBatch.requestByShardId) {
            for (auto& op : request.ops) {
                if (reprocessOpIds.contains(op.getId())) {
                    batcher.markOpReprocess({op});
                }
            }
        }
    }
};

class OrderedUnifiedWriteExecutorBatcherTest : public UnifiedWriteExecutorBatcherTest {
public:
    void assertSingleShardSimpleWriteBatch(const WriteBatch& batch,
                                           std::vector<WriteOpId> expectedOpIds,
                                           std::vector<ShardEndpoint> expectedShards) {
        ASSERT_TRUE(std::holds_alternative<SimpleWriteBatch>(batch));
        auto& simpleBatch = std::get<SimpleWriteBatch>(batch);
        ASSERT_EQ(1, simpleBatch.requestByShardId.size());
        const auto& shardRequest = simpleBatch.requestByShardId.begin()->second;
        ASSERT_EQ(shardRequest.ops.size(), expectedOpIds.size());
        ASSERT_EQ(shardRequest.ops.size(), expectedShards.size());
        for (size_t i = 0; i < shardRequest.ops.size(); i++) {
            const auto& op = shardRequest.ops[i];
            ASSERT_EQ(op.getId(), expectedOpIds[i]);
            auto opShard = shardRequest.versionByNss.find(op.getNss());
            ASSERT_TRUE(opShard != shardRequest.versionByNss.end());
            ASSERT_EQ(expectedShards[i], opShard->second);
        }
    }
};
class UnorderedUnifiedWriteExecutorBatcherTest : public UnifiedWriteExecutorBatcherTest {
public:
    const ShardVersion shardVersionNss0Shard0VersionIgnored = ShardVersionFactory::make(
        ChunkVersion::IGNORED(), boost::optional<CollectionIndexes>(boost::none));
    const ShardEndpoint nss0Shard0VersionIgnored =
        ShardEndpoint(shardId0, shardVersionNss0Shard0VersionIgnored, boost::none);

    void assertUnorderedSingleShardSimpleWriteBatch(const WriteBatch& batch,
                                                    const SimpleWriteBatch& expectedBatch) {
        ASSERT_TRUE(std::holds_alternative<SimpleWriteBatch>(batch));
        auto& simpleBatch = std::get<SimpleWriteBatch>(batch);
        ASSERT_EQ(expectedBatch.requestByShardId.size(), simpleBatch.requestByShardId.size());

        for (const auto& [shardId, expectedShardRequest] : expectedBatch.requestByShardId) {
            auto shardRequestIt = simpleBatch.requestByShardId.find(shardId);
            ASSERT_NOT_EQUALS(shardRequestIt, simpleBatch.requestByShardId.end());

            auto& shardRequest = shardRequestIt->second;
            ASSERT_EQ(expectedShardRequest.ops.size(), shardRequest.ops.size());
            ASSERT_EQ(expectedShardRequest.versionByNss.size(), shardRequest.versionByNss.size());

            for (size_t i = 0; i < expectedShardRequest.ops.size(); i++) {
                const auto& op = shardRequest.ops[i];
                ASSERT_EQ(op.getId(), expectedShardRequest.ops[i].getId());

                auto opShard = shardRequest.versionByNss.find(op.getNss());
                ASSERT_TRUE(opShard != shardRequest.versionByNss.end());
                ASSERT_EQ(expectedBatch.requestByShardId.at(shardId).versionByNss.at(op.getNss()),
                          opShard->second);
            }
        }
    }
};


TEST_F(OrderedUnifiedWriteExecutorBatcherTest,
       OrderedBatcherBatchesSingleShardOpByShardIdGeneratingSingleOpBatches) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {nss0Shard0}}},
        {1, {kSingleShard, {nss0Shard1}}},
        {2, {kSingleShard, {nss0Shard0}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0], [1], [2]
    auto batch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch1.has_value());
    assertSingleShardSimpleWriteBatch(*batch1, {0}, {nss0Shard0});

    auto batch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch2.has_value());
    assertSingleShardSimpleWriteBatch(*batch2, {1}, {nss0Shard1});

    auto batch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch3.has_value());
    assertSingleShardSimpleWriteBatch(*batch3, {2}, {nss0Shard0});

    auto batch4 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(batch4.has_value());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest,
       OrderedBatcherBatchesSingleShardOpByShardIdGeneratingMultiOpBatches) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {nss0Shard0}}},
        {1, {kSingleShard, {nss1Shard0}}},
        {2, {kSingleShard, {nss0Shard1}}},
        {3, {kSingleShard, {nss1Shard1}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [2, 3]
    auto batch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch1.has_value());
    assertSingleShardSimpleWriteBatch(*batch1, {0, 1}, {nss0Shard0, nss1Shard0});

    auto batch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch2.has_value());
    assertSingleShardSimpleWriteBatch(*batch2, {2, 3}, {nss0Shard1, nss1Shard1});

    auto optBatch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(optBatch3.has_value());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest, OrderedBatcherBatchesSingleShardOpByWriteType) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {nss0Shard0}}},
        {1, {kSingleShard, {nss1Shard0}}},
        {2, {kMultiShard, {nss0Shard0, nss0Shard1}}},
        {3, {kSingleShard, {nss1Shard1}}},
        {4, {kSingleShard, {nss0Shard1}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [2], [3, 4]
    auto batch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch1.has_value());
    assertSingleShardSimpleWriteBatch(*batch1, {0, 1}, {nss0Shard0, nss1Shard0});

    auto batch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch2.has_value());
    assertMultiShardSimpleWriteBatch(*batch2, 2, {nss0Shard0, nss0Shard1});

    auto batch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch3.has_value());
    assertSingleShardSimpleWriteBatch(*batch3, {3, 4}, {nss1Shard1, nss0Shard1});

    auto batch4 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(batch4.has_value());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest, OrderedBatcherBatchesMultiShardOpSeparately) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSONObj()), BulkWriteInsertOp(0, BSONObj())},
        {NamespaceInfoEntry(nss0)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kMultiShard, {nss0Shard0, nss0Shard1}}},
        {1, {kMultiShard, {nss0Shard0, nss0Shard1}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0], [1]
    auto batch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch1.has_value());
    assertMultiShardSimpleWriteBatch(*batch1, 0, {nss0Shard0, nss0Shard1});

    auto batch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch2.has_value());
    assertMultiShardSimpleWriteBatch(*batch2, 1, {nss0Shard0, nss0Shard1});

    auto batch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(batch3.has_value());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest, OrderedBatcherReprocessesWriteOps) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {nss0Shard0}}},
        {1, {kSingleShard, {nss1Shard0}}},
        {2, {kSingleShard, {nss0Shard1}}},
        {3, {kSingleShard, {nss1Shard1}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [1(reprocess)], [2, 3]
    auto batch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch1.has_value());
    assertSingleShardSimpleWriteBatch(*batch1, {0, 1}, {nss0Shard0, nss1Shard0});

    reprocessWriteOp(batcher, *batch1, {1});

    auto batch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch2.has_value());
    assertSingleShardSimpleWriteBatch(*batch2, {1}, {nss1Shard0});

    auto batch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch3.has_value());
    assertSingleShardSimpleWriteBatch(*batch3, {2, 3}, {nss0Shard1, nss1Shard1});

    auto batch4 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(batch4.has_value());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest,
       OrderedBatcherReprocessesWriteOpsWithChunkMigration) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {nss0Shard0}}},
        {1, {kSingleShard, {nss1Shard0}}},
        {2, {kSingleShard, {nss0Shard1}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [1(reprocess), 2]
    auto batch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch1.has_value());
    assertSingleShardSimpleWriteBatch(*batch1, {0, 1}, {nss0Shard0, nss1Shard0});

    reprocessWriteOp(batcher, *batch1, {1});
    analyzer.setOpAnalysis({
        {0, {kSingleShard, {nss0Shard0}}},
        {1, {kSingleShard, {nss1Shard1}}},
        {2, {kSingleShard, {nss0Shard1}}},
    });

    auto batch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch2.has_value());
    assertSingleShardSimpleWriteBatch(*batch2, {1, 2}, {nss1Shard1, nss0Shard1});

    auto batch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(batch3.has_value());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest,
       UnorderedBatcherBatchesSingleShardOpsTargetingDifferentShardsInOneBatch) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {nss0Shard0}}},
        {1, {kSingleShard, {nss0Shard0}}},
        {2, {kSingleShard, {nss1Shard1}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);


    SimpleWriteBatch::ShardRequest shardRequest1{{{nss0, nss0Shard0}},
                                                 {WriteOp(request, 0), WriteOp(request, 1)}};
    SimpleWriteBatch::ShardRequest shardRequest2{{{nss1, nss1Shard1}}, {WriteOp(request, 2)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}, {shardId1, shardRequest2}}};

    // Output batch: [0, 1, 2]
    auto batch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch1.has_value());
    assertUnorderedSingleShardSimpleWriteBatch(*batch1, expectedBatch1);

    auto batch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(batch2.has_value());
}


TEST_F(UnorderedUnifiedWriteExecutorBatcherTest, UnorderedBatcherBatchesMultiShardOpsInOwnBatch) {
    BulkWriteCommandRequest request(
        {
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(1, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(1, BSONObj()),
        },
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kMultiShard, {nss0Shard0, nss0Shard1}}},
        {1, {kSingleShard, {nss0Shard0}}},
        {2, {kSingleShard, {nss1Shard1}}},
        {3, {kMultiShard, {{nss0Shard0, nss0Shard1}}}},
        {4, {kSingleShard, {nss0Shard0}}},
        {5, {kMultiShard, {{nss1Shard0, nss1Shard1}}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    SimpleWriteBatch::ShardRequest shardRequest1{{{nss0, nss0Shard0}, {nss1, nss1Shard0}},
                                                 {WriteOp(request, 0),
                                                  WriteOp(request, 1),
                                                  WriteOp(request, 3),
                                                  WriteOp(request, 4),
                                                  WriteOp(request, 5)}};
    SimpleWriteBatch::ShardRequest shardRequest2{
        {{nss0, nss0Shard1}, {nss1, nss1Shard1}},
        {WriteOp(request, 0), WriteOp(request, 2), WriteOp(request, 3), WriteOp(request, 5)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}, {shardId1, shardRequest2}}};
    auto batch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch1.has_value());
    assertUnorderedSingleShardSimpleWriteBatch(*batch1, expectedBatch1);

    auto batch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(batch2.has_value());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest,
       UnorderedBatcherMakesNewBatchForSameNamespaceWithDifferentShardVersion) {
    BulkWriteCommandRequest request(
        {
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(1, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
        },
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {nss0Shard0}}},
        {1, {kSingleShard, {nss1Shard0}}},
        {2, {kSingleShard, {nss0Shard0VersionIgnored}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    SimpleWriteBatch::ShardRequest shardRequest1{{{nss0, nss0Shard0}, {nss1, nss1Shard0}},
                                                 {WriteOp(request, 0), WriteOp(request, 1)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}}};
    auto batch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch1.has_value());
    assertUnorderedSingleShardSimpleWriteBatch(*batch1, expectedBatch1);

    shardRequest1 =
        SimpleWriteBatch::ShardRequest{{{nss0, nss0Shard0VersionIgnored}}, {WriteOp(request, 2)}};
    SimpleWriteBatch expectedBatch2{{{shardId0, shardRequest1}}};
    auto batch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch2.has_value());
    assertUnorderedSingleShardSimpleWriteBatch(*batch2, expectedBatch2);

    auto batch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(batch3.has_value());
}


TEST_F(UnorderedUnifiedWriteExecutorBatcherTest, UnorderedBatcherReprocessesWriteOps) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    std::map<WriteOpId, Analysis> ops = {{0, {kMultiShard, {nss0Shard0, nss0Shard1}}},
                                         {1, {kSingleShard, {nss0Shard0}}},
                                         {2, {kSingleShard, {nss0Shard0}}},
                                         {3, {kSingleShard, {nss1Shard1}}},
                                         {4, {kMultiShard, {nss1Shard0, nss1Shard1}}}};
    MockWriteOpAnalyzer analyzer(ops);

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    SimpleWriteBatch::ShardRequest shardRequest1{
        {{nss0, nss0Shard0}, {nss1, nss1Shard0}},
        {WriteOp(request, 0), WriteOp(request, 1), WriteOp(request, 2), WriteOp(request, 4)}};
    SimpleWriteBatch::ShardRequest shardRequest2{
        {{nss0, nss0Shard1}, {nss1, nss1Shard1}},
        {WriteOp(request, 0), WriteOp(request, 3), WriteOp(request, 4)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}, {shardId1, shardRequest2}}};
    auto batch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch1.has_value());
    assertUnorderedSingleShardSimpleWriteBatch(*batch1, expectedBatch1);

    reprocessWriteOp(batcher, *batch1, {0});
    reprocessWriteOp(batcher, *batch1, {3});

    shardRequest1 = SimpleWriteBatch::ShardRequest{{{nss0, nss0Shard0}}, {WriteOp(request, 0)}};
    shardRequest2 = SimpleWriteBatch::ShardRequest{{{nss0, nss0Shard1}, {nss1, nss1Shard1}},
                                                   {WriteOp(request, 0), WriteOp(request, 3)}};
    SimpleWriteBatch expectedBatch2{{{shardId0, shardRequest1}, {shardId1, shardRequest2}}};
    auto batch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch2.has_value());
    assertUnorderedSingleShardSimpleWriteBatch(*batch2, expectedBatch2);

    auto batch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(batch3.has_value());
}


TEST_F(UnorderedUnifiedWriteExecutorBatcherTest,
       UnorderedBatcherReprocessesWriteOpsWithChunkMigration) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    BulkWriteOpProducer producer(request);

    MockWriteOpAnalyzer analyzer({
        {0, {kSingleShard, {nss0Shard0}}},
        {1, {kSingleShard, {nss1Shard0}}},
        {2, {kSingleShard, {nss0Shard0}}},
    });

    auto routingCtx = RoutingContext::createForTest({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1, 2], [1(reprocess)]
    SimpleWriteBatch::ShardRequest shardRequest1{
        {{nss0, nss0Shard0}, {nss1, nss1Shard0}},
        {WriteOp(request, 0), WriteOp(request, 1), WriteOp(request, 2)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}}};

    auto batch1 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch1.has_value());
    assertUnorderedSingleShardSimpleWriteBatch(*batch1, expectedBatch1);

    reprocessWriteOp(batcher, *batch1, {1});
    analyzer.setOpAnalysis({
        {0, {kSingleShard, {nss0Shard0}}},
        {1, {kSingleShard, {nss1Shard1}}},
        {2, {kSingleShard, {nss0Shard0}}},
    });

    shardRequest1 = SimpleWriteBatch::ShardRequest{{{nss1, nss1Shard1}}, {WriteOp(request, 1)}};
    SimpleWriteBatch expectedBatch2{{{shardId1, shardRequest1}}};
    auto batch2 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_TRUE(batch2.has_value());
    assertUnorderedSingleShardSimpleWriteBatch(*batch2, expectedBatch2);

    auto optBatch3 = batcher.getNextBatch(nullptr /* opCtx */, routingCtx);
    ASSERT_FALSE(optBatch3.has_value());
}


}  // namespace
}  // namespace unified_write_executor
}  // namespace mongo
