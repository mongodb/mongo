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

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/sharding_mongos_test_fixture.h"
#include "mongo/s/sharding_test_fixture_common.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/unified_write_executor/write_op_analyzer.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace unified_write_executor {
namespace {
struct WriteOpAnalyzerTest : public ShardingTestFixture {
    WriteOpAnalyzer analyzer;
    const ShardId kShard1Name = ShardId("shard1");
    const ShardId kShard2Name = ShardId("shard2");
    const NamespaceString kUntrackedNss =
        NamespaceString::createNamespaceString_forTest("test", "untracked");
    const NamespaceString kUnsplittableNss =
        NamespaceString::createNamespaceString_forTest("test", "unsplittable");
    ChunkManager createChunkManager(const UUID& uuid, const NamespaceString& nss) {
        ShardKeyPattern sk{fromjson("{x: 1, _id: 1}")};
        std::deque<DocumentSource::GetNextResult> configData{
            Document(fromjson("{_id: {x: {$minKey: 1}, _id: {$minKey: 1}}, max: {x: 0.0, _id: "
                              "0.0}, shard: 'shard1'}")),
            Document(fromjson("{_id: {x: 0.0, _id: 0.0}, max: {x: {$maxKey: 1}, _id: {$maxKey: "
                              "1}}, shard: 'shard2' }"))};
        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks;
        for (const auto& chunkData : configData) {
            const auto bson = chunkData.getDocument().toBson();
            ChunkRange range{bson.getField("_id").Obj().getOwned(),
                             bson.getField("max").Obj().getOwned()};
            ShardId shard{bson.getField("shard").valueStringDataSafe().toString()};
            chunks.emplace_back(uuid,
                                std::move(range),
                                ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                                std::move(shard));
        }

        auto rt = RoutingTableHistory::makeNew(nss,
                                               uuid,
                                               sk.getKeyPattern(),
                                               false, /* unsplittable */
                                               nullptr,
                                               false,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               false,
                                               chunks);

        return ChunkManager(
            ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(std::move(rt)),
            boost::none);
    }

    RoutingContext createRoutingContextSharded(
        std::vector<std::pair<UUID, NamespaceString>> uuidNssList) {
        stdx::unordered_map<NamespaceString, CollectionRoutingInfo> criMap;
        for (auto [uuid, nss] : uuidNssList) {
            criMap.emplace(
                nss,
                CollectionRoutingInfo(
                    createChunkManager(uuid, nss),
                    DatabaseTypeValueHandle(DatabaseType{
                        nss.dbName(), kShard1Name, DatabaseVersion(uuid, Timestamp{1, 1})})));
        }
        return RoutingContext::createForTest(criMap);
    }

    /**
     * Set up a routing context for testing analyze() with unsharded collections.
     */
    RoutingContext createRoutingContextUnsharded() {
        auto uuid = UUID::gen();
        auto dbVersion = DatabaseVersion(uuid, Timestamp{1, 1});
        return RoutingContext::createForTest(
            {{kUntrackedNss,
              CatalogCacheMock::makeCollectionRoutingInfoUntracked(
                  kUntrackedNss, kShard1Name, dbVersion)},
             {kUnsplittableNss,
              CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
                  kUnsplittableNss, kShard1Name, dbVersion, kShard1Name)}});
    }
};


TEST_F(WriteOpAnalyzerTest, SingleInserts) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            BulkWriteInsertOp(0, BSON("x" << -1 << "_id" << -1)),
            BulkWriteInsertOp(0, BSON("x" << 1 << "_id" << 1)),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = analyzer.analyze(operationContext(), rtx, op1);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(BatchType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = analyzer.analyze(operationContext(), rtx, op2);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(BatchType::kSingleShard, analysis.type);

    rtx.onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTest, MultiNSSingleInserts) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest("test", "coll2");
    UUID uuid2 = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}, {uuid2, nss2}});

    BulkWriteCommandRequest request(
        {
            BulkWriteInsertOp(0, BSON("x" << -1 << "_id" << -1)),
            BulkWriteInsertOp(1, BSON("x" << 1 << "_id" << 1)),
        },
        {NamespaceInfoEntry(nss), NamespaceInfoEntry(nss2)});

    WriteOp op1(request, 0);
    ASSERT_EQ(nss, op1.getNss());
    auto analysis = analyzer.analyze(operationContext(), rtx, op1);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(BatchType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    ASSERT_EQ(nss2, op2.getNss());
    analysis = analyzer.analyze(operationContext(), rtx, op2);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(BatchType::kSingleShard, analysis.type);

    rtx.onRequestSentForNss(nss);
    rtx.onRequestSentForNss(nss2);
}

TEST_F(WriteOpAnalyzerTest, EqUpdateOnes) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(0,
                              BSON("x" << -1 << "_id" << -1),
                              write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
            BulkWriteUpdateOp(0,
                              BSON("x" << 1 << "_id" << 1),
                              write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = analyzer.analyze(operationContext(), rtx, op1);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(BatchType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = analyzer.analyze(operationContext(), rtx, op2);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(BatchType::kSingleShard, analysis.type);

    rtx.onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTest, RangeUpdateOnes) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(0,
                              BSON("x" << BSON("$gt" << -1) << "_id" << BSON("$gt" << -1)),
                              write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
            BulkWriteUpdateOp(0,
                              BSON("x" << BSON("$lt" << 1) << "_id" << BSON("$lt" << 1)),
                              write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = analyzer.analyze(operationContext(), rtx, op1);
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(BatchType::kMultiShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = analyzer.analyze(operationContext(), rtx, op2);
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(BatchType::kMultiShard, analysis.type);

    rtx.onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTest, RangeUpdateManys) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            [&]() {
                auto op = BulkWriteUpdateOp(
                    0,
                    BSON("x" << BSON("$gt" << -1) << "_id" << BSON("$gt" << -1)),
                    write_ops::UpdateModification(BSON("$set" << BSON("a" << 1))));
                op.setMulti(true);
                return op;
            }(),
            [&]() {
                auto op = BulkWriteUpdateOp(
                    0,
                    BSON("x" << BSON("$lt" << 1) << "_id" << BSON("$lt" << 1)),
                    write_ops::UpdateModification(BSON("$set" << BSON("a" << 1))));
                op.setMulti(true);
                return op;
            }(),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = analyzer.analyze(operationContext(), rtx, op1);
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(BatchType::kMultiShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = analyzer.analyze(operationContext(), rtx, op2);
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(BatchType::kMultiShard, analysis.type);

    rtx.onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTest, SingleShardRangeUpdateOnes) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(0,
                              BSON("x" << BSON("$lt" << -1) << "_id" << BSON("$lt" << -1)),
                              write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
            BulkWriteUpdateOp(0,
                              BSON("x" << BSON("$gt" << 1) << "_id" << BSON("$gt" << 1)),
                              write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = analyzer.analyze(operationContext(), rtx, op1);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    // TODO SERVER-103780 this should be changed with 2 phase write support.
    ASSERT_EQ(BatchType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = analyzer.analyze(operationContext(), rtx, op2);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    // TODO SERVER-103780 this should be changed with 2 phase write support.
    ASSERT_EQ(BatchType::kSingleShard, analysis.type);

    rtx.onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTest, SingleShardRangeUpdateManys) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            [&]() {
                auto op = BulkWriteUpdateOp(
                    0,
                    BSON("x" << BSON("$lt" << -1) << "_id" << BSON("$lt" << -1)),
                    write_ops::UpdateModification(BSON("$set" << BSON("a" << 1))));
                op.setMulti(true);
                return op;
            }(),
            [&]() {
                auto op = BulkWriteUpdateOp(
                    0,
                    BSON("x" << BSON("$gt" << 1) << "_id" << BSON("$gt" << 1)),
                    write_ops::UpdateModification(BSON("$set" << BSON("a" << 1))));
                op.setMulti(true);
                return op;
            }(),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = analyzer.analyze(operationContext(), rtx, op1);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(BatchType::kMultiShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = analyzer.analyze(operationContext(), rtx, op2);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(BatchType::kMultiShard, analysis.type);

    rtx.onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTest, EqDeletes) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            BulkWriteDeleteOp(0, BSON("x" << -1 << "_id" << -1)),
            BulkWriteDeleteOp(0, BSON("x" << 1 << "_id" << 1)),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = analyzer.analyze(operationContext(), rtx, op1);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(BatchType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = analyzer.analyze(operationContext(), rtx, op2);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(BatchType::kSingleShard, analysis.type);

    rtx.onRequestSentForNss(nss);
}


TEST_F(WriteOpAnalyzerTest, RangeDeleteOnes) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            BulkWriteDeleteOp(0, BSON("x" << BSON("$gt" << -1) << "_id" << BSON("$gt" << -1))),
            BulkWriteDeleteOp(0, BSON("x" << BSON("$lt" << 1) << "_id" << BSON("$lt" << 1))),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = analyzer.analyze(operationContext(), rtx, op1);
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(BatchType::kMultiShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = analyzer.analyze(operationContext(), rtx, op2);
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(BatchType::kMultiShard, analysis.type);

    rtx.onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTest, SingleShardRangeDeleteManys) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            []() {
                auto op = BulkWriteDeleteOp(
                    0, BSON("x" << BSON("$lt" << -1) << "_id" << BSON("$lt" << -1)));
                op.setMulti(true);
                return op;
            }(),
            []() {
                auto op = BulkWriteDeleteOp(
                    0, BSON("x" << BSON("$gt" << 1) << "_id" << BSON("$gt" << 1)));
                op.setMulti(true);
                return op;
            }(),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = analyzer.analyze(operationContext(), rtx, op1);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(BatchType::kMultiShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = analyzer.analyze(operationContext(), rtx, op2);
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    // TODO SERVER-103780 this should be changed with 2 phase write support.
    ASSERT_EQ(BatchType::kMultiShard, analysis.type);

    rtx.onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTest, UnshardedUntracked) {
    auto rtx = createRoutingContextUnsharded();

    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(0,
                              BSON("x" << BSON("$lt" << -1) << "_id" << BSON("$lt" << -1)),
                              write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
            []() {
                auto op = BulkWriteUpdateOp(
                    0,
                    BSON("x" << BSON("$lt" << -1) << "_id" << BSON("$lt" << -1)),
                    write_ops::UpdateModification(BSON("$set" << BSON("a" << 1))));
                op.setMulti(true);
                return op;
            }(),
            BulkWriteDeleteOp(0, BSON("x" << BSON("$gt" << 1) << "_id" << BSON("$gt" << 1))),
            []() {
                auto op = BulkWriteDeleteOp(
                    0, BSON("x" << BSON("$gt" << 1) << "_id" << BSON("$gt" << 1)));
                op.setMulti(true);
                return op;
            }(),
        }  // namespace
        ,
        {NamespaceInfoEntry(kUntrackedNss)});

    for (size_t i = 0; i < request.getOps().size(); i++) {
        LOGV2(10346501, "request index", "i"_attr = i);
        WriteOp op(request, i);
        auto analysis = analyzer.analyze(operationContext(), rtx, op);
        ASSERT_EQ(1, analysis.shardsAffected.size());
        ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    }

    rtx.onRequestSentForNss(kUntrackedNss);
    rtx.onRequestSentForNss(kUnsplittableNss);
}

TEST_F(WriteOpAnalyzerTest, Unsplittable) {
    auto rtx = createRoutingContextUnsharded();

    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(0,
                              BSON("x" << BSON("$lt" << -1) << "_id" << BSON("$lt" << -1)),
                              write_ops::UpdateModification(BSON("$set" << BSON("a" << 1)))),
            []() {
                auto op = BulkWriteUpdateOp(
                    0,
                    BSON("x" << BSON("$lt" << -1) << "_id" << BSON("$lt" << -1)),
                    write_ops::UpdateModification(BSON("$set" << BSON("a" << 1))));
                op.setMulti(true);
                return op;
            }(),
            BulkWriteDeleteOp(0, BSON("x" << BSON("$gt" << 1) << "_id" << BSON("$gt" << 1))),
            []() {
                auto op = BulkWriteDeleteOp(
                    0, BSON("x" << BSON("$gt" << 1) << "_id" << BSON("$gt" << 1)));
                op.setMulti(true);
                return op;
            }(),
        }  // namespace
        ,
        {NamespaceInfoEntry(kUnsplittableNss)});

    for (size_t i = 0; i < request.getOps().size(); i++) {
        LOGV2(10346502, "request index", "i"_attr = i);
        WriteOp op(request, i);
        auto analysis = analyzer.analyze(operationContext(), rtx, op);
        ASSERT_EQ(1, analysis.shardsAffected.size());
        ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    }

    rtx.onRequestSentForNss(kUntrackedNss);
    rtx.onRequestSentForNss(kUnsplittableNss);
}

}  // namespace
}  // namespace unified_write_executor
}  // namespace mongo
