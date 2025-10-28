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

#include "mongo/s/write_ops/unified_write_executor/write_op_batcher.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_analyzer.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_producer.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace unified_write_executor {
namespace {

class WriteOpAnalyzerMock : public WriteOpAnalyzer {
public:
    WriteOpAnalyzerMock(std::map<WriteOpId, StatusWith<Analysis>> opAnalysis)
        : _opAnalysis(std::move(opAnalysis)) {}

    StatusWith<Analysis> analyze(OperationContext* opCtx,
                                 RoutingContext& routingCtx,
                                 const WriteOp& writeOp) override {
        auto it = _opAnalysis.find(writeOp.getId());
        tassert(
            10346702, "Write op id should be found in the analysis data", it != _opAnalysis.end());
        return it->second;
    }

    void setOpAnalysis(std::map<WriteOpId, StatusWith<Analysis>> opAnalysis) {
        _opAnalysis = std::move(opAnalysis);
    }

    std::map<WriteOpId, StatusWith<Analysis>> _opAnalysis;
};

class UnifiedWriteExecutorBatcherTest : public ServiceContextTest {
public:
    UnifiedWriteExecutorBatcherTest() : _opCtx(makeOperationContext()) {}

    const NamespaceString nss0 = NamespaceString::createNamespaceString_forTest("test", "coll0");
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest("test", "coll1");
    const ShardId shardId0 = ShardId("shard0");
    const ShardId shardId1 = ShardId("shard1");
    const ShardVersion shardVersionNss0Shard0 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(1, 0)}, CollectionPlacement(1, 0)));
    const ShardVersion shardVersionNss0Shard1 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(1, 0)}, CollectionPlacement(2, 0)));
    const ShardVersion shardVersionNss1Shard0 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(2, 0)}, CollectionPlacement(1, 0)));
    const ShardVersion shardVersionNss1Shard1 = ShardVersionFactory::make(
        ChunkVersion(CollectionGeneration{OID::gen(), Timestamp(2, 0)}, CollectionPlacement(2, 0)));
    const ShardEndpoint nss0Shard0 = ShardEndpoint(shardId0, shardVersionNss0Shard0, boost::none);
    const ShardEndpoint nss0Shard1 = ShardEndpoint(shardId1, shardVersionNss0Shard1, boost::none);
    const ShardEndpoint nss1Shard0 = ShardEndpoint(shardId0, shardVersionNss1Shard0, boost::none);
    const ShardEndpoint nss1Shard1 = ShardEndpoint(shardId1, shardVersionNss1Shard1, boost::none);

    void assertMultiShardSimpleWriteBatch(
        const WriteBatch& batch,
        WriteOpId expectedOpId,
        std::vector<ShardEndpoint> expectedShardVersions,
        boost::optional<analyze_shard_key::TargetedSampleId> expectedSampleId = boost::none) {
        ASSERT_TRUE(std::holds_alternative<SimpleWriteBatch>(batch.data));
        auto& simpleBatch = std::get<SimpleWriteBatch>(batch.data);

        ASSERT_EQ(expectedShardVersions.size(), simpleBatch.requestByShardId.size());
        for (auto& expectedShard : expectedShardVersions) {
            auto shardRequestIt = simpleBatch.requestByShardId.find(expectedShard.shardName);
            ASSERT_NOT_EQUALS(shardRequestIt, simpleBatch.requestByShardId.end());
            auto& shardRequest = shardRequestIt->second;
            ASSERT_EQ(shardRequest.ops.size(), 1);
            ASSERT_EQ(shardRequest.ops.front().getId(), expectedOpId);
            ASSERT_EQ(shardRequest.versionByNss.size(), 1);
            ASSERT_EQ(shardRequest.versionByNss.begin()->second, expectedShard);

            if (expectedSampleId) {
                if (expectedSampleId->isFor(expectedShard.shardName)) {
                    ASSERT_EQ(shardRequest.sampleIds.size(), 1);
                    auto it = shardRequest.sampleIds.find(expectedOpId);
                    ASSERT_TRUE(it != shardRequest.sampleIds.end());
                    ASSERT_EQ(it->second, expectedSampleId->getId());
                } else {
                    ASSERT_EQ(shardRequest.sampleIds.size(), 0);
                }
            } else {
                ASSERT_EQ(shardRequest.sampleIds.size(), 0);
            }
        }
    }

    void reprocessWriteOp(WriteOpBatcher& batcher,
                          WriteBatch& batch,
                          std::set<WriteOpId> reprocessOpIds) {
        ASSERT_TRUE(std::holds_alternative<SimpleWriteBatch>(batch.data));
        auto& simpleBatch = std::get<SimpleWriteBatch>(batch.data);
        for (auto& [shardId, request] : simpleBatch.requestByShardId) {
            for (auto& op : request.ops) {
                if (reprocessOpIds.contains(op.getId())) {
                    batcher.markOpReprocess({op});
                }
            }
        }
    }

    void assertNonTargetedWriteBatch(const WriteBatch& batch,
                                     WriteOpId expectedOpId,
                                     boost::optional<UUID> expectedSampleId = boost::none) {
        ASSERT_TRUE(std::holds_alternative<NonTargetedWriteBatch>(batch.data));
        auto& nonTargetedWriteBatch = std::get<NonTargetedWriteBatch>(batch.data);
        const auto& op = nonTargetedWriteBatch.op;
        ASSERT_EQ(op.getId(), expectedOpId);
        ASSERT_EQ(nonTargetedWriteBatch.sampleId, expectedSampleId);
    }

    void assertInternalTransactionBatch(const WriteBatch& batch,
                                        WriteOpId expectedOpId,
                                        boost::optional<UUID> expectedSampleId = boost::none) {
        ASSERT_TRUE(std::holds_alternative<InternalTransactionBatch>(batch.data));
        auto& internalTransactionBatch = std::get<InternalTransactionBatch>(batch.data);
        const auto& op = internalTransactionBatch.op;
        ASSERT_EQ(op.getId(), expectedOpId);
        ASSERT_EQ(internalTransactionBatch.sampleId, expectedSampleId);
    }

    void assertMultiWriteBlockingMigrationsBatch(
        const WriteBatch& batch,
        WriteOpId expectedOpId,
        boost::optional<UUID> expectedSampleId = boost::none) {
        ASSERT_TRUE(std::holds_alternative<MultiWriteBlockingMigrationsBatch>(batch.data));
        auto& multiWriteBlockingMigrations =
            std::get<MultiWriteBlockingMigrationsBatch>(batch.data);
        const auto& op = multiWriteBlockingMigrations.op;
        ASSERT_EQ(op.getId(), expectedOpId);
        ASSERT_EQ(multiWriteBlockingMigrations.sampleId, expectedSampleId);
    }

    void assertSampleIds(const std::map<WriteOpId, UUID>& sampleIds,
                         const std::map<WriteOpId, UUID>& expectedSampleIds) {
        ASSERT_EQ(sampleIds.size(), expectedSampleIds.size());
        for (auto& [opId, sampleId] : expectedSampleIds) {
            auto it = sampleIds.find(opId);
            ASSERT_TRUE(it != sampleIds.end());
            ASSERT_EQ(it->second, sampleId);
        }
    }

    OperationContext* getOperationContext() {
        return _opCtx.get();
    }

private:
    ServiceContext::UniqueOperationContext _opCtx;
};

class OrderedUnifiedWriteExecutorBatcherTest : public UnifiedWriteExecutorBatcherTest {
public:
    void assertSingleShardSimpleWriteBatch(const WriteBatch& batch,
                                           std::vector<WriteOpId> expectedOpIds,
                                           std::vector<ShardEndpoint> expectedShardVersions,
                                           std::map<WriteOpId, UUID> expectedSampleIds = {}) {
        ASSERT_TRUE(std::holds_alternative<SimpleWriteBatch>(batch.data));
        auto& simpleBatch = std::get<SimpleWriteBatch>(batch.data);
        ASSERT_EQ(1, simpleBatch.requestByShardId.size());
        const auto& shardRequest = simpleBatch.requestByShardId.begin()->second;
        ASSERT_EQ(shardRequest.ops.size(), expectedOpIds.size());
        ASSERT_EQ(shardRequest.ops.size(), expectedShardVersions.size());
        for (size_t i = 0; i < shardRequest.ops.size(); i++) {
            const auto& op = shardRequest.ops[i];
            ASSERT_EQ(op.getId(), expectedOpIds[i]);
            auto opShard = shardRequest.versionByNss.find(op.getNss());
            ASSERT_TRUE(opShard != shardRequest.versionByNss.end());
            ASSERT_EQ(expectedShardVersions[i], opShard->second);
        }
        assertSampleIds(shardRequest.sampleIds, expectedSampleIds);
    }
};

class UnorderedUnifiedWriteExecutorBatcherTest : public UnifiedWriteExecutorBatcherTest {
public:
    const ShardVersion shardVersionNss0Shard0VersionIgnored =
        ShardVersionFactory::make(ChunkVersion::IGNORED());
    const ShardEndpoint nss0Shard0VersionIgnored =
        ShardEndpoint(shardId0, shardVersionNss0Shard0VersionIgnored, boost::none);

    void assertUnorderedSimpleWriteBatch(const WriteBatch& batch,
                                         const SimpleWriteBatch& expectedBatch) {
        ASSERT_TRUE(std::holds_alternative<SimpleWriteBatch>(batch.data));
        auto& simpleBatch = std::get<SimpleWriteBatch>(batch.data);
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

            assertSampleIds(shardRequest.sampleIds, expectedShardRequest.sampleIds);
        }
    }
};

TEST_F(OrderedUnifiedWriteExecutorBatcherTest,
       OrderedBatcherBatchesSingleShardOpByShardIdGeneratingSingleOpBatches) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss0Shard1}}},
        {2, Analysis{kSingleShard, {nss0Shard0}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0], [1], [2]
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result1.batch, {0}, {nss0Shard0});

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result2.batch, {1}, {nss0Shard1});

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result3.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result3.batch, {2}, {nss0Shard0});

    auto result4 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result4.batch.isEmptyBatch());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest,
       OrderedBatcherBatchesSingleShardOpByShardIdGeneratingMultiOpBatches) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard0}}},
        {2, Analysis{kSingleShard, {nss0Shard1}}},
        {3, Analysis{kSingleShard, {nss1Shard1}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [2, 3]
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result1.batch, {0, 1}, {nss0Shard0, nss1Shard0});

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result2.batch, {2, 3}, {nss0Shard1, nss1Shard1});

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result3.batch.isEmptyBatch());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest, OrderedBatcherBatchesSingleShardOpByWriteType) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard0}}},
        {2, Analysis{kMultiShard, {nss0Shard0, nss0Shard1}}},
        {3, Analysis{kSingleShard, {nss1Shard1}}},
        {4, Analysis{kSingleShard, {nss0Shard1}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [2], [3, 4]
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result1.batch, {0, 1}, {nss0Shard0, nss1Shard0});

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertMultiShardSimpleWriteBatch(result2.batch, 2, {nss0Shard0, nss0Shard1});

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result3.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result3.batch, {3, 4}, {nss1Shard1, nss0Shard1});

    auto result4 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result4.batch.isEmptyBatch());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest, OrderedBatcherBatchesMultiShardOpSeparately) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSONObj()), BulkWriteInsertOp(0, BSONObj())},
        {NamespaceInfoEntry(nss0)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kMultiShard, {nss0Shard0, nss0Shard1}}},
        {1, Analysis{kMultiShard, {nss0Shard0, nss0Shard1}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0], [1]
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertMultiShardSimpleWriteBatch(result1.batch, 0, {nss0Shard0, nss0Shard1});

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertMultiShardSimpleWriteBatch(result2.batch, 1, {nss0Shard0, nss0Shard1});

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result3.batch.isEmptyBatch());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest, OrderedBatcherBatchesQuaruntineOpsSeparately) {
    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(
                0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
            BulkWriteUpdateOp(
                0, BSON("y" << 1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
            BulkWriteUpdateOp(
                0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
            BulkWriteUpdateOp(
                0,
                BSON("meta" << BSON("a" << 1)),
                write_ops::UpdateModification(BSON("$set" << BSON("meta" << BSON("a" << "2"))))),
            BulkWriteUpdateOp(
                0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("g" << 1)))),
            BulkWriteUpdateOp(0,
                              BSON("x" << BSON("$gt" << -1) << "_id" << BSON("$gt" << -1)),
                              write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),

        },
        {NamespaceInfoEntry(nss0)});

    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kNonTargetedWrite, {nss0Shard0}}},
        {2, Analysis{kSingleShard, {nss0Shard0}}},
        {3, Analysis{kInternalTransaction, {nss0Shard0}}},
        {4, Analysis{kSingleShard, {nss0Shard0}}},
        {5, Analysis{kMultiWriteBlockingMigrations, {nss0Shard0, nss0Shard1}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0], [1], [2], [3]
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result1.batch, {0}, {nss0Shard0});

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertNonTargetedWriteBatch(result2.batch, 1);

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result3.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result3.batch, {2}, {nss0Shard0});

    auto result4 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result4.batch.isEmptyBatch());
    assertInternalTransactionBatch(result4.batch, 3);

    auto result5 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result5.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result5.batch, {4}, {nss0Shard0});

    auto result6 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result6.batch.isEmptyBatch());
    assertMultiWriteBlockingMigrationsBatch(result6.batch, 5);

    auto result7 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result7.batch.isEmptyBatch());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest, OrderedBatcherReprocessesWriteOps) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard0}}},
        {2, Analysis{kSingleShard, {nss0Shard1}}},
        {3, Analysis{kSingleShard, {nss1Shard1}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [1(reprocess)], [2, 3]
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result1.batch, {0, 1}, {nss0Shard0, nss1Shard0});

    reprocessWriteOp(batcher, result1.batch, {1});

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result2.batch, {1}, {nss1Shard0});

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result3.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result3.batch, {2, 3}, {nss0Shard1, nss1Shard1});

    auto result4 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result4.batch.isEmptyBatch());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest,
       OrderedBatcherReprocessesWriteOpsWithChunkMigration) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard0}}},
        {2, Analysis{kSingleShard, {nss0Shard1}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [1(reprocess), 2]
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result1.batch, {0, 1}, {nss0Shard0, nss1Shard0});

    reprocessWriteOp(batcher, result1.batch, {1});
    analyzer.setOpAnalysis({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard1}}},
        {2, Analysis{kSingleShard, {nss0Shard1}}},
    });

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result2.batch, {1, 2}, {nss1Shard1, nss0Shard1});

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result3.batch.isEmptyBatch());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest, OrderedBatcherStopsOnWriteError) {
    BulkWriteCommandRequest request(
        {BulkWriteInsertOp(0, BSONObj()), BulkWriteInsertOp(1, BSONObj())},
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard1}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0], <stop>
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result1.batch, {0}, {nss0Shard0});

    batcher.stopMakingBatches();

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result2.batch.isEmptyBatch());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest, OrderedBatcherAttachesSampleIdToBatches) {
    BulkWriteCommandRequest request(
        {BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
         BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
         BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
         BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1))))},
        {NamespaceInfoEntry(nss0)});
    WriteOpProducer producer(request);

    auto sampleId0 = UUID::gen();
    auto sampleId2 = UUID::gen();
    WriteOpAnalyzerMock analyzer({
        {0,
         Analysis{
             kSingleShard,
             {nss0Shard0},
             analyze_shard_key::TargetedSampleId(sampleId0, shardId0),
         }},
        {1,
         Analysis{
             kMultiShard,
             {nss0Shard0, nss0Shard1},
         }},
        {2,
         Analysis{
             kNonTargetedWrite,
             {nss0Shard0, nss0Shard1},
             analyze_shard_key::TargetedSampleId(sampleId2, shardId0),
         }},
        {3,
         Analysis{
             kInternalTransaction,
             {nss0Shard0, nss0Shard1},
         }},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0], [1], [2], [3], <stop>
    size_t expectedOpId = 0;
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(
        result1.batch, {expectedOpId}, {nss0Shard0}, {{expectedOpId, sampleId0}});
    expectedOpId++;

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertMultiShardSimpleWriteBatch(result2.batch, expectedOpId, {nss0Shard0, nss0Shard1});
    expectedOpId++;

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result3.batch.isEmptyBatch());
    assertNonTargetedWriteBatch(result3.batch, expectedOpId, sampleId2);
    expectedOpId++;

    auto result4 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result4.batch.isEmptyBatch());
    assertInternalTransactionBatch(result4.batch, expectedOpId);
    expectedOpId++;

    auto result5 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result5.batch.isEmptyBatch());
}

TEST_F(OrderedUnifiedWriteExecutorBatcherTest, OrderedBatcherSkipsDoneBatches) {
    BulkWriteCommandRequest request(
        {
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(1, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(1, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
        },
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss1Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard0}}},
        {2, Analysis{kMultiShard, {nss0Shard0, nss0Shard1}}},
        {3, Analysis{kSingleShard, {nss1Shard1}}},
        {4, Analysis{kMultiShard, {nss0Shard0, nss0Shard1}}},
        {5, Analysis{kSingleShard, {nss0Shard1}}},
        {6, Analysis{kMultiShard, {nss0Shard0, nss0Shard1}}},
        {7, Analysis{kSingleShard, {nss1Shard0}}},
        {8, Analysis{kMultiShard, {nss0Shard0, nss0Shard1}}},
        {9, Analysis{kSingleShard, {nss1Shard0}}},
        {10, Analysis{kMultiShard, {nss0Shard0, nss0Shard1}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = OrderedWriteOpBatcher(producer, analyzer);

    // Note a few operations to have all shards already successfully written to.
    const std::map<WriteOpId, std::set<ShardId>> successfulShardsToAdd{
        {WriteOpId(0), std::set<ShardId>{nss1Shard0.shardName}},
        {WriteOpId(4), std::set<ShardId>{nss0Shard0.shardName, nss0Shard1.shardName}},
        {WriteOpId(6), std::set<ShardId>{nss0Shard0.shardName, nss0Shard1.shardName}},
        {WriteOpId(7), std::set<ShardId>{nss1Shard0.shardName}},
        {WriteOpId(9), std::set<ShardId>{nss1Shard0.shardName}},
        {WriteOpId(10), std::set<ShardId>{nss0Shard0.shardName, nss0Shard1.shardName}}};
    batcher.noteSuccessfulShards(successfulShardsToAdd);

    // Output batches: [1], [2], [3, 5]
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result1.batch, {1}, {nss1Shard0});

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertMultiShardSimpleWriteBatch(result2.batch, 2, {nss0Shard0, nss0Shard1});

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result3.batch.isEmptyBatch());
    assertSingleShardSimpleWriteBatch(result3.batch, {3, 5}, {nss1Shard1, nss0Shard1});

    auto result4 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result4.batch.isEmptyBatch());
    assertMultiShardSimpleWriteBatch(result4.batch, 8, {nss0Shard0, nss0Shard1});

    auto result5 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result5.batch.isEmptyBatch());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest,
       UnorderedBatcherBatchesSingleShardOpsTargetingDifferentShardsInOneBatch) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss0Shard0}}},
        {2, Analysis{kSingleShard, {nss1Shard1}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    SimpleWriteBatch::ShardRequest shardRequest1{{{nss0, nss0Shard0}},
                                                 {WriteOp(request, 0), WriteOp(request, 1)}};
    SimpleWriteBatch::ShardRequest shardRequest2{{{nss1, nss1Shard1}}, {WriteOp(request, 2)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}, {shardId1, shardRequest2}}};

    // Output batch: [0, 1, 2]
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result1.batch, expectedBatch1);

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result2.batch.isEmptyBatch());
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
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kMultiShard, {nss0Shard0, nss0Shard1}}},
        {1, Analysis{kSingleShard, {nss0Shard0}}},
        {2, Analysis{kSingleShard, {nss1Shard1}}},
        {3, Analysis{kMultiShard, {{nss0Shard0, nss0Shard1}}}},
        {4, Analysis{kSingleShard, {nss0Shard0}}},
        {5, Analysis{kMultiShard, {{nss1Shard0, nss1Shard1}}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
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
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result1.batch, expectedBatch1);

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result2.batch.isEmptyBatch());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest, UnorderedBatcherBatchesQuaruntineOpSeparately) {
    BulkWriteCommandRequest request(
        {BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
         BulkWriteUpdateOp(
             0, BSON("y" << 1), write_ops::UpdateModification(BSON("$set" << BSON("b" << 1)))),
         BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("c" << 1)))),
         BulkWriteUpdateOp(
             0, BSON("y" << 1), write_ops::UpdateModification(BSON("$set" << BSON("d" << 1)))),
         BulkWriteUpdateOp(
             0, BSON("y" << 1), write_ops::UpdateModification(BSON("$set" << BSON("e" << 1)))),
         BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("f" << 1)))),
         BulkWriteUpdateOp(
             0,
             BSON("meta" << BSON("a" << 1)),
             write_ops::UpdateModification(BSON("$set" << BSON("meta" << BSON("a" << "2"))))),
         BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("g" << 1)))),
         BulkWriteUpdateOp(0,
                           BSON("x" << BSON("$gt" << -1) << "_id" << BSON("$gt" << -1)),
                           write_ops::UpdateModification(BSON("$set" << BSON("a" << 1))))},
        {NamespaceInfoEntry(nss0)});

    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kNonTargetedWrite, {nss0Shard0, nss0Shard1}}},
        {2, Analysis{kSingleShard, {nss0Shard0}}},
        {3, Analysis{kNonTargetedWrite, {nss0Shard0, nss0Shard1}}},
        {4, Analysis{kNonTargetedWrite, {nss0Shard0, nss0Shard1}}},
        {5, Analysis{kSingleShard, {nss0Shard0}}},
        {6, Analysis{kInternalTransaction, {nss0Shard0}}},
        {7, Analysis{kSingleShard, {nss0Shard0}}},
        {8, Analysis{kMultiWriteBlockingMigrations, {nss0Shard0, nss0Shard1}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    SimpleWriteBatch::ShardRequest shardRequest1{
        {{nss0, nss0Shard0}},
        {WriteOp(request, 0), WriteOp(request, 2), WriteOp(request, 5), WriteOp(request, 7)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}}};

    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result1.batch, expectedBatch1);

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertNonTargetedWriteBatch(result2.batch, 1);

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result3.batch.isEmptyBatch());
    assertNonTargetedWriteBatch(result3.batch, 3);

    auto result4 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result4.batch.isEmptyBatch());
    assertNonTargetedWriteBatch(result4.batch, 4);

    auto result5 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result5.batch.isEmptyBatch());
    assertInternalTransactionBatch(result5.batch, 6);

    auto result6 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result6.batch.isEmptyBatch());
    assertMultiWriteBlockingMigrationsBatch(result6.batch, 8);

    auto result7 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result7.batch.isEmptyBatch());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest, UnorderedBatcherTargetErrorsTransient) {
    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(
                0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
            BulkWriteUpdateOp(
                0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("b" << 1)))),
            BulkWriteUpdateOp(
                0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("c" << 1)))),
            BulkWriteUpdateOp(
                0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("d" << 1)))),
        },
        {NamespaceInfoEntry(nss0)});

    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Status{ErrorCodes::BadValue, "Bad Value"}},
        {2, Status{ErrorCodes::BadValue, "Bad Value"}},
        {3, Analysis{kSingleShard, {nss0Shard0}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    ASSERT_TRUE(batcher.getRetryOnTargetError());

    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);

    ASSERT_TRUE(result1.batch.isEmptyBatch());
    ASSERT_TRUE(result1.opsWithErrors.empty());
    ASSERT_FALSE(batcher.getRetryOnTargetError());

    analyzer.setOpAnalysis({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss0Shard0}}},
        {2, Analysis{kSingleShard, {nss0Shard0}}},
        {3, Analysis{kSingleShard, {nss0Shard0}}},
    });

    SimpleWriteBatch::ShardRequest shardRequest2{
        {{nss0, nss0Shard0}},
        {WriteOp(request, 0), WriteOp(request, 1), WriteOp(request, 2), WriteOp(request, 3)}};
    SimpleWriteBatch expectedBatch2{{{shardId0, shardRequest2}}};

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);

    assertUnorderedSimpleWriteBatch(result2.batch, expectedBatch2);
    ASSERT_TRUE(result2.opsWithErrors.empty());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest, UnorderedBatcherTargetErrorsNonTransient) {
    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(
                0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
            BulkWriteUpdateOp(
                0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("b" << 1)))),
            BulkWriteUpdateOp(
                0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("c" << 1)))),
            BulkWriteUpdateOp(
                0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("d" << 1)))),
        },
        {NamespaceInfoEntry(nss0)});

    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Status{ErrorCodes::BadValue, "Bad Value"}},
        {2, Status{ErrorCodes::BadValue, "Bad Value"}},
        {3, Analysis{kSingleShard, {nss0Shard0}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    ASSERT_TRUE(batcher.getRetryOnTargetError());

    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);

    ASSERT_TRUE(result1.batch.isEmptyBatch());
    ASSERT_TRUE(result1.opsWithErrors.empty());
    ASSERT_FALSE(batcher.getRetryOnTargetError());

    SimpleWriteBatch::ShardRequest shardRequest2{{{nss0, nss0Shard0}},
                                                 {WriteOp(request, 0), WriteOp(request, 3)}};
    SimpleWriteBatch expectedBatch2{{{shardId0, shardRequest2}}};

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);

    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result2.batch, expectedBatch2);

    ASSERT_TRUE(result2.opsWithErrors.size() == 2);
    ASSERT_TRUE(result2.opsWithErrors[0].first.getId() == 1);
    ASSERT_TRUE(result2.opsWithErrors[1].first.getId() == 2);
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
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard0}}},
        {2, Analysis{kSingleShard, {nss0Shard0VersionIgnored}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    SimpleWriteBatch::ShardRequest shardRequest1{{{nss0, nss0Shard0}, {nss1, nss1Shard0}},
                                                 {WriteOp(request, 0), WriteOp(request, 1)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}}};
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result1.batch, expectedBatch1);

    shardRequest1 =
        SimpleWriteBatch::ShardRequest{{{nss0, nss0Shard0VersionIgnored}}, {WriteOp(request, 2)}};
    SimpleWriteBatch expectedBatch2{{{shardId0, shardRequest1}}};
    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result2.batch, expectedBatch2);

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result3.batch.isEmptyBatch());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest, UnorderedBatcherReprocessesWriteOps) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    std::map<WriteOpId, StatusWith<Analysis>> ops = {
        {0, Analysis{kMultiShard, {nss0Shard0, nss0Shard1}}},
        {1, Analysis{kSingleShard, {nss0Shard0}}},
        {2, Analysis{kSingleShard, {nss0Shard0}}},
        {3, Analysis{kSingleShard, {nss1Shard1}}},
        {4, Analysis{kMultiShard, {nss1Shard0, nss1Shard1}}}};
    WriteOpAnalyzerMock analyzer(ops);

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    SimpleWriteBatch::ShardRequest shardRequest1{
        {{nss0, nss0Shard0}, {nss1, nss1Shard0}},
        {WriteOp(request, 0), WriteOp(request, 1), WriteOp(request, 2), WriteOp(request, 4)}};
    SimpleWriteBatch::ShardRequest shardRequest2{
        {{nss0, nss0Shard1}, {nss1, nss1Shard1}},
        {WriteOp(request, 0), WriteOp(request, 3), WriteOp(request, 4)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}, {shardId1, shardRequest2}}};
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result1.batch, expectedBatch1);

    reprocessWriteOp(batcher, result1.batch, {0});
    reprocessWriteOp(batcher, result1.batch, {3});

    shardRequest1 = SimpleWriteBatch::ShardRequest{{{nss0, nss0Shard0}}, {WriteOp(request, 0)}};
    shardRequest2 = SimpleWriteBatch::ShardRequest{{{nss0, nss0Shard1}, {nss1, nss1Shard1}},
                                                   {WriteOp(request, 0), WriteOp(request, 3)}};
    SimpleWriteBatch expectedBatch2{{{shardId0, shardRequest1}, {shardId1, shardRequest2}}};
    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result2.batch, expectedBatch2);

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result3.batch.isEmptyBatch());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest,
       UnorderedBatcherReprocessesWriteOpsWithChunkMigration) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard0}}},
        {2, Analysis{kSingleShard, {nss0Shard0}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1, 2], [1(reprocess)]
    SimpleWriteBatch::ShardRequest shardRequest1{
        {{nss0, nss0Shard0}, {nss1, nss1Shard0}},
        {WriteOp(request, 0), WriteOp(request, 1), WriteOp(request, 2)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}}};

    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result1.batch, expectedBatch1);

    reprocessWriteOp(batcher, result1.batch, {1});
    analyzer.setOpAnalysis({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard1}}},
        {2, Analysis{kSingleShard, {nss0Shard0}}},
    });

    shardRequest1 = SimpleWriteBatch::ShardRequest{{{nss1, nss1Shard1}}, {WriteOp(request, 1)}};
    SimpleWriteBatch expectedBatch2{{{shardId1, shardRequest1}}};
    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result2.batch, expectedBatch2);

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result3.batch.isEmptyBatch());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest, UnorderedBatcherReprocessBatch) {
    BulkWriteCommandRequest request({BulkWriteInsertOp(0, BSONObj()),
                                     BulkWriteInsertOp(1, BSONObj()),
                                     BulkWriteInsertOp(0, BSONObj())},
                                    {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard0}}},
        {2, Analysis{kSingleShard, {nss0Shard0}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1, 2], [0(reprocess), 1(reprocess), 2(reprocess)]
    SimpleWriteBatch::ShardRequest shardRequest1{
        {{nss0, nss0Shard0}, {nss1, nss1Shard0}},
        {WriteOp(request, 0), WriteOp(request, 1), WriteOp(request, 2)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}}};

    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result1.batch, expectedBatch1);

    batcher.markBatchReprocess(result1.batch);

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result2.batch, expectedBatch1);

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result3.batch.isEmptyBatch());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest, UnorderedBatcherDoesNotStopOnWriteError) {
    BulkWriteCommandRequest request(
        {
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(1, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
        },
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss0Shard0}}},
        {1, Analysis{kSingleShard, {nss1Shard0}}},
        {2, Analysis{kSingleShard, {nss0Shard0VersionIgnored}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [2]
    SimpleWriteBatch::ShardRequest shardRequest1{{{nss0, nss0Shard0}, {nss1, nss1Shard0}},
                                                 {WriteOp(request, 0), WriteOp(request, 1)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}}};
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result1.batch, expectedBatch1);

    shardRequest1 =
        SimpleWriteBatch::ShardRequest{{{nss0, nss0Shard0VersionIgnored}}, {WriteOp(request, 2)}};
    SimpleWriteBatch expectedBatch2{{{shardId0, shardRequest1}}};
    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result2.batch, expectedBatch2);

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result3.batch.isEmptyBatch());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest, UnorderedBatcherSkipsDoneBatches) {
    BulkWriteCommandRequest request(
        {
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
            BulkWriteInsertOp(0, BSONObj()),
        },
        {NamespaceInfoEntry(nss0), NamespaceInfoEntry(nss1)});
    WriteOpProducer producer(request);

    WriteOpAnalyzerMock analyzer({
        {0, Analysis{kSingleShard, {nss1Shard0}}},
        {1, Analysis{kSingleShard, {nss0Shard0}}},
        {2, Analysis{kMultiShard, {{nss0Shard0, nss0Shard1}}}},
        {3, Analysis{kMultiShard, {{nss0Shard0, nss0Shard1}}}},
        {4, Analysis{kNonTargetedWrite, {nss0Shard0}}},
        {5, Analysis{kMultiShard, {{nss0Shard0, nss0Shard1}}}},
        {6, Analysis{kSingleShard, {nss1Shard0}}},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    // Note a few operations to have all shards already successfully written to.
    const std::map<WriteOpId, std::set<ShardId>> successfulShardsToAdd{
        {WriteOpId(0), std::set<ShardId>{nss1Shard0.shardName}},
        {WriteOpId(2), std::set<ShardId>{nss0Shard0.shardName, nss0Shard1.shardName}},
        {WriteOpId(5), std::set<ShardId>{nss0Shard0.shardName, nss0Shard1.shardName}},
        {WriteOpId(6), std::set<ShardId>{nss1Shard0.shardName}}};
    batcher.noteSuccessfulShards(successfulShardsToAdd);

    SimpleWriteBatch::ShardRequest shardRequest1{{{nss0, nss0Shard0}},
                                                 {WriteOp(request, 1), WriteOp(request, 3)}};
    SimpleWriteBatch::ShardRequest shardRequest2{{{nss0, nss0Shard1}}, {WriteOp(request, 3)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}, {shardId1, shardRequest2}}};

    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    assertUnorderedSimpleWriteBatch(result1.batch, expectedBatch1);

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertNonTargetedWriteBatch(result2.batch, 4);

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result3.batch.isEmptyBatch());
}

TEST_F(UnorderedUnifiedWriteExecutorBatcherTest, UnorderedBatcherAttachesSampleIdToBatches) {
    BulkWriteCommandRequest request(
        {BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
         BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
         BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
         BulkWriteUpdateOp(
             0, BSON("x" << -1), write_ops::UpdateModification(BSON("$set" << BSON("a" << 1))))},
        {NamespaceInfoEntry(nss0)});
    WriteOpProducer producer(request);

    auto sampleId0 = UUID::gen();
    auto sampleId2 = UUID::gen();
    WriteOpAnalyzerMock analyzer({
        {0,
         Analysis{
             kSingleShard,
             {nss0Shard0},
             analyze_shard_key::TargetedSampleId(sampleId0, shardId0),
         }},
        {1,
         Analysis{
             kMultiShard,
             {nss0Shard0, nss0Shard1},
         }},
        {2,
         Analysis{
             kNonTargetedWrite,
             {nss0Shard0, nss0Shard1},
             analyze_shard_key::TargetedSampleId(sampleId2, shardId0),
         }},
        {3,
         Analysis{
             kInternalTransaction,
             {nss0Shard0, nss0Shard1},
         }},
    });

    auto routingCtx = RoutingContext::createSynthetic({});
    auto batcher = UnorderedWriteOpBatcher(producer, analyzer);

    // Output batches: [0, 1], [2], [3], <stop>
    auto result1 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result1.batch.isEmptyBatch());
    auto shardRequest1 = SimpleWriteBatch::ShardRequest{
        {{nss0, nss0Shard0}}, {WriteOp(request, 0), WriteOp(request, 1)}, {{0, sampleId0}}};
    auto shardRequest2 =
        SimpleWriteBatch::ShardRequest{{{nss0, nss0Shard1}}, {WriteOp(request, 1)}};
    SimpleWriteBatch expectedBatch1{{{shardId0, shardRequest1}, {shardId1, shardRequest2}}};
    assertUnorderedSimpleWriteBatch(result1.batch, expectedBatch1);

    auto result2 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result2.batch.isEmptyBatch());
    assertNonTargetedWriteBatch(result2.batch, 2, sampleId2);

    auto result3 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_FALSE(result3.batch.isEmptyBatch());
    assertInternalTransactionBatch(result3.batch, 3);

    auto result4 = batcher.getNextBatch(getOperationContext(), *routingCtx);
    ASSERT_TRUE(result4.batch.isEmptyBatch());
}
}  // namespace
}  // namespace unified_write_executor
}  // namespace mongo
