// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/write_ops/unified_write_executor/write_op_analyzer.h"

#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/router_role/routing_cache/catalog_cache.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/sharding_environment/sharding_test_fixture_common.h"
#include "mongo/db/topology/cluster_parameters/migration_blocking_operation_cluster_parameters_gen.h"
#include "mongo/db/topology/cluster_parameters/sharding_cluster_parameters_gen.h"
#include "mongo/s/refresh_query_analyzer_configuration_cmd_gen.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/tick_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace unified_write_executor {
namespace {
struct WriteOpAnalyzerTestImpl : public ShardingTestFixture {
    WriteOpAnalyzerTestImpl()
        : ShardingTestFixture(
              false /* withMockCatalogCache */,
              std::make_unique<ScopedGlobalServiceContextForTest>(ServiceContext::make(
                  nullptr, nullptr, std::make_unique<TickSourceMock<Nanoseconds>>()))) {}

    const ShardId shardId1 = ShardId("shard1");
    const ShardId shardId2 = ShardId("shard2");
    const int port = 12345;

    void setUp() override {
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(HostAndPort("config", port));

        std::vector<std::pair<ShardId, HostAndPort>> remoteShards{
            {shardId1, HostAndPort(shardId1.toString(), port)},
            {shardId2, HostAndPort(shardId2.toString(), port)},
        };

        std::vector<ShardType> shards;
        for (size_t i = 0; i < remoteShards.size(); i++) {
            ShardType shardType;
            shardType.setHandle(
                ShardHandle{ShardId(get<0>(remoteShards[i]).toString()), boost::none});
            shardType.setHost(get<1>(remoteShards[i]).toString());
            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(get<1>(remoteShards[i])));
            targeter->setFindHostReturnValue(get<1>(remoteShards[i]));
            targeterFactory()->addTargeterToReturn(ConnectionString(get<1>(remoteShards[i])),
                                                   std::move(targeter));
        }
        setupShards(shards);
    }

    Stats stats;
    WriteOpAnalyzerImpl analyzer = WriteOpAnalyzerImpl(stats);
    const ShardId kShard1Name = ShardId("shard1");
    const ShardId kShard2Name = ShardId("shard2");
    const NamespaceString kUntrackedNss =
        NamespaceString::createNamespaceString_forTest("test", "untracked");
    const NamespaceString kUnsplittableNss =
        NamespaceString::createNamespaceString_forTest("test", "unsplittable");

    CurrentChunkManager createChunkManager(
        const UUID& uuid,
        const NamespaceString& nss,
        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields = boost::none,
        bool isViewfulTimeseries = false) {
        const auto skeyPrefix = isViewfulTimeseries ? "meta" : "x";
        auto sk = ShardKeyPattern{BSON(skeyPrefix << 1 << "_id" << 1)};

        std::deque<DocumentSource::GetNextResult> configData;
        configData.push_back(Document(BSON("_id" << BSON(skeyPrefix << MINKEY << "_id" << MINKEY)
                                                 << "max" << BSON(skeyPrefix << 0.0 << "_id" << 0.0)
                                                 << "shard" << "shard1")));
        configData.push_back(Document(BSON("_id" << BSON(skeyPrefix << 0.0 << "_id" << 0.0) << "max"
                                                 << BSON(skeyPrefix << MAXKEY << "_id" << MAXKEY)
                                                 << "shard" << "shard2")));

        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks;
        for (const auto& chunkData : configData) {
            const auto bson = chunkData.getDocument().toBson();
            ChunkRange range{bson.getField("_id").Obj().getOwned(),
                             bson.getField("max").Obj().getOwned()};
            ShardId shard{std::string{bson.getField("shard").valueStringDataSafe()}};
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
                                               timeseriesFields,
                                               boost::none /* reshardingFields */,
                                               false,
                                               chunks);

        return CurrentChunkManager(
            ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(std::move(rt)));
    }

    std::unique_ptr<RoutingContext> createRoutingContextSharded(
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
        return RoutingContext::createSynthetic(criMap);
    }

    std::unique_ptr<RoutingContext> createRoutingContextShardedTimeseries(
        std::vector<std::pair<UUID, NamespaceString>> uuidNssList,
        bool isViewfulTimeseries = false) {
        stdx::unordered_map<NamespaceString, CollectionRoutingInfo> criMap;
        TypeCollectionTimeseriesFields tsFields;
        tsFields.setTimeField(std::string("ts"));
        tsFields.setMetaField(std::string("x"));

        for (auto [uuid, nss] : uuidNssList) {
            criMap.emplace(
                nss,
                CollectionRoutingInfo(
                    createChunkManager(uuid, nss, tsFields, isViewfulTimeseries),
                    DatabaseTypeValueHandle(DatabaseType{
                        nss.dbName(), kShard1Name, DatabaseVersion(uuid, Timestamp{1, 1})})));
        }
        return RoutingContext::createSynthetic(criMap);
    }

    /**
     * Set up a routing context for testing analyze() with unsharded collections.
     */
    std::unique_ptr<RoutingContext> createRoutingContextUnsharded() {
        auto uuid = UUID::gen();
        auto dbVersion = DatabaseVersion(uuid, Timestamp{1, 1});
        return RoutingContext::createSynthetic(
            {{kUntrackedNss,
              CatalogCacheMock::makeCollectionRoutingInfoUntracked(
                  kUntrackedNss, kShard1Name, dbVersion)},
             {kUnsplittableNss,
              CatalogCacheMock::makeCollectionRoutingInfoUnsplittable(
                  kUnsplittableNss, kShard1Name, dbVersion, kShard1Name)}});
    }
};


TEST_F(WriteOpAnalyzerTestImpl, SingleInserts) {
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
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, MultiNSSingleInserts) {
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
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    ASSERT_EQ(nss2, op2.getNss());
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    rtx->onRequestSentForNss(nss);
    rtx->onRequestSentForNss(nss2);
}

TEST_F(WriteOpAnalyzerTestImpl, EqUpdateOnes) {
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
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, EqUpdateWithAnalysisError) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    // Create write command containing a multi:true replacement update. This kind of update is not
    // supported by MongoDB. It should trigger an InvalidOptions error during analysis.
    BulkWriteCommandRequest request(
        {
            [&]() {
                auto op = BulkWriteUpdateOp(
                    0,
                    BSON("x" << -1 << "_id" << -1),
                    write_ops::UpdateModification(BSON("_id" << -1 << "x" << -1)));
                op.setMulti(true);
                return op;
            }(),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto swAnalysis = analyzer.analyze(operationContext(), *rtx, op1);

    ASSERT_FALSE(swAnalysis.isOK());
    ASSERT_EQ(swAnalysis.getStatus().code(), ErrorCodes::InvalidOptions);
}

TEST_F(WriteOpAnalyzerTestImpl, RangeUpdateOnes) {
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
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kTwoPhaseWrite, analysis.type);

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kTwoPhaseWrite, analysis.type);

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, RangeUpdateManys) {
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
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kMultiShard, analysis.type);
    ASSERT_EQ(ChunkVersion::IGNORED(), analysis.shardsAffected[0].shardVersion->placementVersion());
    ASSERT_EQ(ChunkVersion::IGNORED(), analysis.shardsAffected[1].shardVersion->placementVersion());

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kMultiShard, analysis.type);
    ASSERT_EQ(ChunkVersion::IGNORED(), analysis.shardsAffected[0].shardVersion->placementVersion());
    ASSERT_EQ(ChunkVersion::IGNORED(), analysis.shardsAffected[1].shardVersion->placementVersion());

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, SingleShardRangeUpdateOnes) {
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
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, SingleShardRangeUpdateManys) {
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
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, EqDeletes) {
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
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    rtx->onRequestSentForNss(nss);
}


TEST_F(WriteOpAnalyzerTestImpl, RangeDeleteOnes) {
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
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kTwoPhaseWrite, analysis.type);

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kTwoPhaseWrite, analysis.type);

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, RangeDeleteManys) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            [&]() {
                auto op = BulkWriteDeleteOp(
                    0, BSON("x" << BSON("$gt" << -1) << "_id" << BSON("$gt" << -1)));
                op.setMulti(true);
                return op;
            }(),
            [&]() {
                auto op = BulkWriteDeleteOp(
                    0, BSON("x" << BSON("$lt" << 1) << "_id" << BSON("$lt" << 1)));
                op.setMulti(true);
                return op;
            }(),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kMultiShard, analysis.type);
    ASSERT_EQ(ChunkVersion::IGNORED(), analysis.shardsAffected[0].shardVersion->placementVersion());
    ASSERT_EQ(ChunkVersion::IGNORED(), analysis.shardsAffected[1].shardVersion->placementVersion());

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kMultiShard, analysis.type);
    ASSERT_EQ(ChunkVersion::IGNORED(), analysis.shardsAffected[0].shardVersion->placementVersion());
    ASSERT_EQ(ChunkVersion::IGNORED(), analysis.shardsAffected[1].shardVersion->placementVersion());

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, SingleShardRangeDeleteOnes) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            BulkWriteDeleteOp(0, BSON("x" << BSON("$lt" << -1) << "_id" << BSON("$lt" << -1))),
            BulkWriteDeleteOp(0, BSON("x" << BSON("$gt" << 1) << "_id" << BSON("$gt" << 1))),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, SingleShardRangeDeleteManys) {
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
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(1, analysis.shardsAffected.size());
    ASSERT_EQ(kShard2Name, analysis.shardsAffected[0].shardName);
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, UnshardedUntracked) {
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
        auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op));
        ASSERT_EQ(1, analysis.shardsAffected.size());
        ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    }

    rtx->onRequestSentForNss(kUntrackedNss);
    rtx->onRequestSentForNss(kUnsplittableNss);
}

TEST_F(WriteOpAnalyzerTestImpl, Unsplittable) {
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
        auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op));
        ASSERT_EQ(1, analysis.shardsAffected.size());
        ASSERT_EQ(kShard1Name, analysis.shardsAffected[0].shardName);
    }

    rtx->onRequestSentForNss(kUntrackedNss);
    rtx->onRequestSentForNss(kUnsplittableNss);
}

TEST_F(WriteOpAnalyzerTestImpl, TimeSeriesRetryable) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(TxnNumber(1));
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextShardedTimeseries({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            []() {
                auto op = BulkWriteUpdateOp(
                    0,
                    BSON("x" << -1),
                    write_ops::UpdateModification(BSON("$set" << BSON("x" << -10))));
                op.setMulti(true);
                return op;
            }(),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(AnalysisType::kInternalTransaction, analysis.type);

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, AnalysisContainsSampleIdWhenQuerySamplerConfigured) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    // Set up query sampler.
    serverGlobalParams.clusterRole = ClusterRole::RouterServer;
    operationContext()->setQuerySamplingOptions(OperationContext::QuerySamplingOptions::kOptIn);
    auto& sampler = analyze_shard_key::QueryAnalysisSampler::get(operationContext());
    sampler.refreshQueryStatsForTest();
    auto future = launchAsync([&]() { sampler.refreshConfigurationsForTest(operationContext()); });
    onCommand([&](const executor::RemoteCommandRequest& request) {
        auto now = getServiceContext()->getFastClockSource()->now();
        analyze_shard_key::CollectionQueryAnalyzerConfiguration configuration{nss, uuid, 1, now};

        RefreshQueryAnalyzerConfigurationResponse response;
        response.setConfigurations({configuration});
        return response.toBSON();
    });
    future.default_timed_get();
    // Advance time to make sure we will sample queries.
    dynamic_cast<TickSourceMock<Nanoseconds>*>(getServiceContext()->getTickSource())
        ->advance(Milliseconds(10000));

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
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op(request, 0);
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kMultiShard, analysis.type);

    ASSERT(analysis.targetedSampleId.has_value());
    ASSERT(analysis.targetedSampleId->isFor(kShard1Name) !=
           analysis.targetedSampleId->isFor(kShard2Name));

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, MultiWriteInATransaction) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    // Setup transaction.
    auto lsid = LogicalSessionId(UUID::gen(), SHA256Block());
    operationContext()->setLogicalSessionId(lsid);
    TxnNumber txnNumber = 0;
    operationContext()->setTxnNumber(txnNumber);

    // Necessary for TransactionRouter::get to be non-null for this opCtx.
    RouterOperationContextSession rocs(operationContext());

    BulkWriteCommandRequest request(
        {
            [&]() {
                auto op = BulkWriteDeleteOp(
                    0, BSON("x" << BSON("$gt" << -1) << "_id" << BSON("$gt" << -1)));
                op.setMulti(true);
                return op;
            }(),
            [&]() {
                auto op = BulkWriteDeleteOp(
                    0, BSON("x" << BSON("$lt" << 1) << "_id" << BSON("$lt" << 1)));
                op.setMulti(true);
                return op;
            }(),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kMultiShard, analysis.type);
    ASSERT(ChunkVersion::IGNORED() != analysis.shardsAffected[0].shardVersion->placementVersion());
    ASSERT(ChunkVersion::IGNORED() != analysis.shardsAffected[1].shardVersion->placementVersion());

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kMultiShard, analysis.type);
    ASSERT(ChunkVersion::IGNORED() != analysis.shardsAffected[0].shardVersion->placementVersion());
    ASSERT(ChunkVersion::IGNORED() != analysis.shardsAffected[1].shardVersion->placementVersion());

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl,
       MultiWriteWithOnlyTargetDataOwningShardsForMultiWritesParamEnabled) {
    OnlyTargetDataOwningShardsForMultiWritesParam onlyTargetParam;
    onlyTargetParam.setEnabled(true);
    unittest::ServerParameterGuard onlyTargetGuard("onlyTargetDataOwningShardsForMultiWrites",
                                                   onlyTargetParam);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {
            [&]() {
                auto op = BulkWriteDeleteOp(
                    0, BSON("x" << BSON("$gt" << -1) << "_id" << BSON("$gt" << -1)));
                op.setMulti(true);
                return op;
            }(),
            [&]() {
                auto op = BulkWriteDeleteOp(
                    0, BSON("x" << BSON("$lt" << 1) << "_id" << BSON("$lt" << 1)));
                op.setMulti(true);
                return op;
            }(),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kMultiShard, analysis.type);
    ASSERT(ChunkVersion::IGNORED() != analysis.shardsAffected[0].shardVersion->placementVersion());
    ASSERT(ChunkVersion::IGNORED() != analysis.shardsAffected[1].shardVersion->placementVersion());

    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kMultiShard, analysis.type);
    ASSERT(ChunkVersion::IGNORED() != analysis.shardsAffected[0].shardVersion->placementVersion());
    ASSERT(ChunkVersion::IGNORED() != analysis.shardsAffected[1].shardVersion->placementVersion());

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, PauseMigrationsDuringMultiUpdatesParamEnabledWithMultiUpdate) {
    migration_blocking_operation::PauseMigrationsDuringMultiUpdatesParam pauseParam;
    pauseParam.setEnabled(true);
    unittest::ServerParameterGuard pauseGuard("pauseMigrationsDuringMultiUpdates", pauseParam);

    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextSharded({{uuid, nss}});

    BulkWriteCommandRequest request(
        {[&]() {
            auto op =
                BulkWriteUpdateOp(0,
                                  BSON("x" << BSON("$gt" << -1) << "_id" << BSON("$gt" << -1)),
                                  write_ops::UpdateModification(BSON("$set" << BSON("a" << 1))));
            op.setMulti(true);
            return op;
        }()},
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(2, analysis.shardsAffected.size());
    ASSERT_EQ(AnalysisType::kMultiWriteBlockingMigrations, analysis.type);
    ASSERT(ChunkVersion::IGNORED() != analysis.shardsAffected[0].shardVersion->placementVersion());
    ASSERT(ChunkVersion::IGNORED() != analysis.shardsAffected[1].shardVersion->placementVersion());

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, ViewfulTimeSeriesSimple) {
    unittest::ServerParameterGuard enableTimeseriesUpdatesSupport(
        "featureFlagTimeseriesUpdatesSupport", true);
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    const NamespaceString nssBuckets = nss.makeTimeseriesBucketsNamespace();
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextShardedTimeseries({{uuid, nss}, {uuid, nssBuckets}},
                                                     true /* isViewfulTimeseries */);

    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(0,
                              BSON("x" << -1),
                              write_ops::UpdateModification(BSON("$set" << BSON("x" << -10)))),
            BulkWriteUpdateOp(0,
                              BSON("x" << -2),
                              write_ops::UpdateModification(BSON("$set" << BSON("x" << -11)))),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);
    ASSERT_TRUE(analysis.isViewfulTimeseries);

    // Checking against the same namespace to exercise buckets namespace caching.
    WriteOp op2(request, 1);
    analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op2));
    ASSERT_EQ(AnalysisType::kSingleShard, analysis.type);
    ASSERT_TRUE(analysis.isViewfulTimeseries);

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, ViewfulTimeSeriesNonTargeted) {
    unittest::ServerParameterGuard enableTimeseriesUpdatesSupport(
        "featureFlagTimeseriesUpdatesSupport", true);
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    const NamespaceString nssBuckets = nss.makeTimeseriesBucketsNamespace();
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextShardedTimeseries({{uuid, nss}, {uuid, nssBuckets}},
                                                     true /* isViewfulTimeseries */);

    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(0,
                              BSON("y" << -1),
                              write_ops::UpdateModification(BSON("$set" << BSON("y" << -10)))),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(AnalysisType::kTwoPhaseWrite, analysis.type);
    ASSERT_TRUE(analysis.isViewfulTimeseries);

    rtx->onRequestSentForNss(nss);
}

TEST_F(WriteOpAnalyzerTestImpl, ViewfulTimeSeriesRetryableWrite) {
    unittest::ServerParameterGuard enableTimeseriesUpdatesSupport(
        "featureFlagTimeseriesUpdatesSupport", true);
    operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
    operationContext()->setTxnNumber(TxnNumber(1));
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    const NamespaceString nssBuckets = nss.makeTimeseriesBucketsNamespace();
    UUID uuid = UUID::gen();
    auto rtx = createRoutingContextShardedTimeseries({{uuid, nss}, {uuid, nssBuckets}},
                                                     true /* isViewfulTimeseries */);

    BulkWriteCommandRequest request(
        {
            BulkWriteUpdateOp(0,
                              BSON("x" << -1),
                              write_ops::UpdateModification(BSON("$set" << BSON("x" << -10)))),
        },
        {NamespaceInfoEntry(nss)});

    WriteOp op1(request, 0);
    auto analysis = uassertStatusOK(analyzer.analyze(operationContext(), *rtx, op1));
    ASSERT_EQ(AnalysisType::kInternalTransaction, analysis.type);
    // Retryable write does not mark viewful timeseries flag outside of the transaction.
    ASSERT_FALSE(analysis.isViewfulTimeseries);

    rtx->onRequestSentForNss(nss);
}
}  // namespace
}  // namespace unified_write_executor
}  // namespace mongo
