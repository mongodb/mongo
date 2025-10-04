/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/json.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_test_fixture.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/pipeline/process_interface/stub_mongo_process_interface.h"

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

class ShardedAggTestFixture : public ShardCatalogCacheTestFixture {
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
