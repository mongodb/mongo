// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"
#include "mongo/db/router_role/routing_cache/catalog_cache_test_fixture.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * For the purposes of this test, assume every collection is sharded. Stages may ask this during
 * setup. For example, to compute its constraints, the $merge stage needs to know if the output
 * collection is sharded.
 */
class FakeMongoProcessInterface : public StubMongoProcessInterface {
public:
    using StubMongoProcessInterface::StubMongoProcessInterface;

    bool isSharded(OperationContext* opCtx, const NamespaceString& ns) override {
        return true;
    }
};

/**
 * TODO SERVER-111290 Remove external dependencies on this class.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardedAggTestFixture : public ShardCatalogCacheTestFixture {
public:
    const NamespaceString kTestAggregateNss =
        NamespaceString::createNamespaceString_forTest("unittests", "sharded_agg_test");

    void setUp() override {
        ShardCatalogCacheTestFixture::setUp();
        _expCtx = make_intrusive<ExpressionContextForTest>(operationContext(), kTestAggregateNss);
        _expCtx->setMongoProcessInterface(std::make_shared<FakeMongoProcessInterface>(executor()));
        _expCtx->setInRouter(true);
    }

    boost::intrusive_ptr<ExpressionContext> expCtx() {
        return _expCtx;
    }

    boost::intrusive_ptr<DocumentSource> parseStage(const std::string& json) {
        return parseStage(fromjson(json));
    }

    boost::intrusive_ptr<DocumentSource> parseStage(const BSONObj& spec) {
        auto stages = DocumentSource::parse(_expCtx, spec);
        ASSERT_EQ(stages.size(), 1UL);
        return stages.front();
    }

    std::vector<ChunkType> makeChunks(const UUID& uuid,
                                      const OID epoch,
                                      const Timestamp timestamp,
                                      std::vector<std::pair<ChunkRange, ShardId>> chunkInfos) {
        ChunkVersion version({epoch, timestamp}, {1, 0});
        std::vector<ChunkType> chunks;
        for (auto&& pair : chunkInfos) {
            chunks.emplace_back(uuid, pair.first, version, pair.second);
            chunks.back().setName(OID::gen());
            version.incMinor();
        }
        return chunks;
    }

    void loadRoutingTable(NamespaceString nss,
                          const OID epoch,
                          const Timestamp timestamp,
                          const ShardKeyPattern& shardKey,
                          const std::vector<ChunkType>& chunkDistribution) {
        auto future = scheduleRoutingInfoUnforcedRefresh(nss);

        // Mock the expected config server queries.
        expectGetDatabase(nss);
        expectCollectionAndChunksAggregation(
            nss, epoch, timestamp, UUID::gen(), shardKey, chunkDistribution);

        const auto cri = future.default_timed_get();
        ASSERT(cri->isSharded());
    }

protected:
    boost::intrusive_ptr<ExpressionContext> _expCtx;
};

}  // namespace mongo
