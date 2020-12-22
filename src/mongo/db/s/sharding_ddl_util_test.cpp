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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_collection.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class ShardingDDLUtilTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();

        // Create config.transactions collection
        auto opCtx = operationContext();
        DBDirectClient client(opCtx);
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
        client.createCollection(NamespaceString::kConfigReshardingOperationsNamespace.ns());
        client.createCollection(CollectionType::ConfigNS.ns());

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        ConfigServerTestFixture::tearDown();
    }
};

// Test that config.collection document and config.chunks documents are properly updated from source
// to destination collection metadata
TEST_F(ShardingDDLUtilTest, ShardedRenameMetadata) {
    auto opCtx = operationContext();
    DBDirectClient client(opCtx);

    ShardType shard0;
    shard0.setName("shard0");
    shard0.setHost("shard0:12");
    setupShards({shard0});

    const NamespaceString fromNss("test.from");
    const auto fromCollQuery = Query(BSON(CollectionType::kNssFieldName << fromNss.ns()));

    const NamespaceString toNss("test.to");
    const auto toCollQuery = Query(BSON(CollectionType::kNssFieldName << toNss.ns()));

    // Initialize FROM collection chunks
    const auto fromEpoch = OID::gen();
    const int nChunks = 10;
    std::vector<ChunkType> chunks;
    for (int i = 0; i < nChunks; i++) {
        ChunkVersion chunkVersion(1, i, fromEpoch, boost::none);
        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setNS(fromNss);
        chunk.setVersion(chunkVersion);
        chunk.setShard(shard0.getName());
        chunk.setHistory({ChunkHistory(Timestamp(1, i), shard0.getName())});
        chunk.setMin(BSON("a" << i));
        chunk.setMax(BSON("a" << i + 1));
        chunks.push_back(chunk);
    }

    setupCollection(fromNss, KeyPattern(BSON("x" << 1)), chunks);

    // TODO SERVER-53105 remove all nss chunk references from this test
    const auto nssChunkFieldName = "ns";
    const auto epochChunkFieldName = "lastmodEpoch";

    // Get FROM collection document and chunks
    auto fromDoc = client.findOne(CollectionType::ConfigNS.ns(), fromCollQuery);
    CollectionType fromCollection(fromDoc);
    auto fromChunksQuery = Query(BSON(epochChunkFieldName << fromEpoch)).sort(BSON("_id" << 1));
    std::vector<BSONObj> fromChunks;
    client.findN(fromChunks, ChunkType::ConfigNS.ns(), fromChunksQuery, nChunks);

    // Perform the metadata rename and get TO epoch
    auto toEpoch = sharding_ddl_util::shardedRenameMetadata(opCtx, fromNss, toNss);

    // Check that the FROM config.collections entry has been deleted
    ASSERT(client.findOne(CollectionType::ConfigNS.ns(), fromCollQuery).isEmpty());

    // Ensure no chunks are referring the FROM collection anymore
    ASSERT(client.findOne(ChunkType::ConfigNS.ns(), fromChunksQuery).isEmpty());

    // Get TO collection document and chunks
    const auto toChunksQuery = Query(BSON(epochChunkFieldName << toEpoch)).sort(BSON("_id" << 1));
    auto toDoc = client.findOne(CollectionType::ConfigNS.ns(), toCollQuery);
    CollectionType toCollection(toDoc);
    std::vector<BSONObj> toChunks;
    client.findN(toChunks, ChunkType::ConfigNS.ns(), toChunksQuery, nChunks);

    // Check that a new epoch is getting used in the FROM config.collections entry
    ASSERT(fromCollection.getEpoch() != toCollection.getEpoch());

    // Check that no other CollectionType field has been changed
    auto fromUnchangedFields = fromDoc.removeField(CollectionType::kNssFieldName)
                                   .removeField(CollectionType::kEpochFieldName);
    auto toUnchangedFields = toDoc.removeField(CollectionType::kNssFieldName)
                                 .removeField(CollectionType::kEpochFieldName);
    ASSERT_EQ(fromUnchangedFields.woCompare(toUnchangedFields), 0);

    // Check that epoch and nss have been updated in chunk documents
    for (int i = 0; i < nChunks; i++) {
        auto fromChunkDoc = fromChunks[i];
        auto toChunkDoc = toChunks[i];

        ASSERT(fromChunkDoc[epochChunkFieldName].woCompare(toChunkDoc[epochChunkFieldName]) != 0);
        ASSERT(fromChunkDoc[epochChunkFieldName].woCompare(toChunkDoc[epochChunkFieldName]) != 0);
        ASSERT_EQ(
            fromChunkDoc.removeField(epochChunkFieldName)
                .removeField(nssChunkFieldName)
                .woCompare(
                    toChunkDoc.removeField(epochChunkFieldName).removeField(nssChunkFieldName)),
            0);
    }
}

}  // namespace
}  // namespace mongo