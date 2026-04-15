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

#include "mongo/db/shard_role/shard_catalog/commit_collection_metadata_locally.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);

class MockCatalogClient final : public ShardingCatalogClientMock {
public:
    using ShardingCatalogClientMock::getCollection;

    void setCollectionMetadata(CollectionType coll, std::vector<ChunkType> chunks) {
        _coll = std::move(coll);
        _chunks = std::move(chunks);
    }

    CollectionType getCollection(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 repl::ReadConcernLevel readConcernLevel) override {
        return _coll;
    }

    StatusWith<std::vector<ChunkType>> getChunks(
        OperationContext* opCtx,
        const BSONObj& filter,
        const BSONObj& sort,
        boost::optional<int> limit,
        repl::OpTime* opTime,
        const OID& epoch,
        const Timestamp& timestamp,
        repl::ReadConcernLevel readConcern,
        const boost::optional<BSONObj>& hint = boost::none) override {
        return _chunks;
    }

    repl::OpTimeWith<std::vector<ShardType>> getAllShards(OperationContext* opCtx,
                                                          repl::ReadConcernLevel readConcern,
                                                          BSONObj filter = BSONObj()) override {
        return repl::OpTimeWith<std::vector<ShardType>>(std::vector<ShardType>{});
    }

private:
    CollectionType _coll;
    std::vector<ChunkType> _chunks;
};

struct CollectionAndChunksMetadata {
    CollectionType collType;
    std::vector<ChunkType> chunks;
};

CollectionAndChunksMetadata makeCollectionMetadata(int nChunks) {
    const UUID uuid = UUID::gen();
    const OID epoch = OID::gen();
    const Timestamp timestamp(Date_t::now());

    CollectionType collType{kTestNss, epoch, timestamp, Date_t::now(), uuid, kShardKeyPattern};

    std::vector<ChunkType> chunks;
    auto chunkVersion = ChunkVersion({epoch, timestamp}, {1, 0});
    for (int i = 0; i < nChunks; i++) {
        auto min = i == 0 ? BSON(kShardKey << MINKEY) : BSON(kShardKey << (i * 100));
        auto max =
            i == (nChunks - 1) ? BSON(kShardKey << MAXKEY) : BSON(kShardKey << ((i + 1) * 100));
        auto range = ChunkRange(min, max);
        auto& chunk = chunks.emplace_back(uuid, std::move(range), chunkVersion, ShardId("0"));
        chunk.setName(OID::gen());
        chunkVersion.incMajor();
    }

    return {std::move(collType), std::move(chunks)};
}

class CommitCollectionMetadataLocallyTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
        createTestCollection(operationContext(),
                             NamespaceString::kConfigShardCatalogCollectionsNamespace);
        createTestCollection(operationContext(),
                             NamespaceString::kConfigShardCatalogChunksNamespace);
    }

    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        auto client = std::make_unique<MockCatalogClient>();
        _mockCatalogClient = client.get();
        return client;
    }

    MockCatalogClient* mockCatalogClient() {
        return _mockCatalogClient;
    }

    long long countLocalDocs(const NamespaceString& nss) {
        DBDirectClient client(operationContext());
        return client.count(nss);
    }

    std::vector<BSONObj> findLocalDocs(const NamespaceString& nss, BSONObj query = BSONObj()) {
        DBDirectClient client(operationContext());
        FindCommandRequest findCmd(nss);
        findCmd.setFilter(std::move(query));
        auto cursor = client.find(std::move(findCmd));
        std::vector<BSONObj> results;
        while (cursor->more()) {
            results.push_back(cursor->next().getOwned());
        }
        return results;
    }

private:
    MockCatalogClient* _mockCatalogClient = nullptr;
};

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyPersistsCollectionAndChunks) {
    auto [collType, chunks] = makeCollectionMetadata(3);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitRefineShardKeyLocally(operationContext(), kTestNss);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), collType.getUuid());

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    ASSERT_EQ(chunkDocs.size(), 3u);
}

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyUpdatesCSR) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitRefineShardKeyLocally(operationContext(), kTestNss);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->isSharded());
    ASSERT_EQ(metadata->getChunkManager()->getUUID(), collType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyIsIdempotent) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitRefineShardKeyLocally(operationContext(), kTestNss);
    shard_catalog_commit::commitRefineShardKeyLocally(operationContext(), kTestNss);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionPersistsCollectionAndChunks) {
    auto [collType, chunks] = makeCollectionMetadata(3);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCreateCollectionLocally(operationContext(), kTestNss);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), collType.getUuid());

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    ASSERT_EQ(chunkDocs.size(), 3u);
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionUpdatesCSR) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCreateCollectionLocally(operationContext(), kTestNss);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->isSharded());
    ASSERT_EQ(metadata->getChunkManager()->getUUID(), collType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionIsIdempotent) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCreateCollectionLocally(operationContext(), kTestNss);
    shard_catalog_commit::commitCreateCollectionLocally(operationContext(), kTestNss);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionChunklessPersistsTokenToDisk) {
    auto [collType, chunks] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});

    shard_catalog_commit::commitCreateCollectionChunklessLocally(operationContext(), kTestNss);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), collType.getUuid());

    // The placeholder chunk is persisted so the token survives restarts.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 1);
    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    ASSERT_EQ(UUID::fromCDR(chunkDocs[0].getField(ChunkType::collectionUUID.name()).uuid()),
              collType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionChunklessUpdatesCSR) {
    auto [collType, chunks] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});

    shard_catalog_commit::commitCreateCollectionChunklessLocally(operationContext(), kTestNss);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->isSharded());
    ASSERT_FALSE(metadata->getShardPlacementVersion().isSet());
}

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyRemovesStaleChunks) {
    auto [collType, chunks] = makeCollectionMetadata(2);

    // Pre-populate with stale chunks that have a different number of shard key fields.
    {
        DBDirectClient client(operationContext());
        for (const auto& chunk : chunks) {
            auto bson = chunk.toConfigBSON();
            // Insert a stale chunk with a compound min key (2 fields vs the 1-field shard key).
            BSONObjBuilder staleDoc;
            for (const auto& elem : bson) {
                if (elem.fieldNameStringData() == "min") {
                    staleDoc.append("min", BSON("_id" << 1 << "extra" << 1));
                } else if (elem.fieldNameStringData() == "max") {
                    staleDoc.append("max", BSON("_id" << 2 << "extra" << 2));
                } else {
                    staleDoc.append(elem);
                }
            }
            client.insert(NamespaceString::kConfigShardCatalogChunksNamespace, staleDoc.obj());
        }
        ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);
    }

    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    shard_catalog_commit::commitRefineShardKeyLocally(operationContext(), kTestNss);

    // The stale chunks should have been removed and replaced with the correct ones.
    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    ASSERT_EQ(chunkDocs.size(), 2u);
    for (const auto& doc : chunkDocs) {
        ASSERT_EQ(doc.getObjectField("min").nFields(), 1);
    }
}

TEST_F(CommitCollectionMetadataLocallyTest, DropCollectionDeletesMetadata) {
    auto [collType, chunks] = makeCollectionMetadata(3);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    // First commit the metadata so there's something to drop.
    shard_catalog_commit::commitRefineShardKeyLocally(operationContext(), kTestNss);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    shard_catalog_commit::commitDropCollectionLocally(
        operationContext(), kTestNss, collType.getUuid());

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 0);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, DropCollectionClearsCSR) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitRefineShardKeyLocally(operationContext(), kTestNss);

    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
    }

    shard_catalog_commit::commitDropCollectionLocally(
        operationContext(), kTestNss, collType.getUuid());

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_FALSE(metadata) << "CSR should have no metadata after drop";
}

TEST_F(CommitCollectionMetadataLocallyTest, DropCollectionIsNoOpOnEmptyCatalog) {
    auto uuid = UUID::gen();
    shard_catalog_commit::commitDropCollectionLocally(operationContext(), kTestNss, uuid);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 0);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, DropCollectionOnlyDeletesTargetCollection) {
    auto [collType1, chunks1] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType1, chunks1);
    shard_catalog_commit::commitRefineShardKeyLocally(operationContext(), kTestNss);

    // Insert a second collection's data directly.
    auto otherNss = NamespaceString::createNamespaceString_forTest("TestDB", "OtherColl");
    auto [collType2, chunks2] = [&] {
        const UUID uuid = UUID::gen();
        const OID epoch = OID::gen();
        const Timestamp ts(Date_t::now());
        CollectionType coll{otherNss, epoch, ts, Date_t::now(), uuid, kShardKeyPattern};
        ChunkType chunk(uuid,
                        ChunkRange(BSON(kShardKey << MINKEY), BSON(kShardKey << MAXKEY)),
                        ChunkVersion({epoch, ts}, {1, 0}),
                        ShardId("0"));
        chunk.setName(OID::gen());
        return CollectionAndChunksMetadata{std::move(coll), {std::move(chunk)}};
    }();
    {
        DBDirectClient client(operationContext());
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace, collType2.toBSON());
        for (const auto& chunk : chunks2) {
            client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                          chunk.toConfigBSON());
        }
    }

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 2);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    shard_catalog_commit::commitDropCollectionLocally(
        operationContext(), kTestNss, collType1.getUuid());

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 1);

    auto remainingColls = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(UUID::fromCDR(remainingColls[0].getField("uuid").uuid()), collType2.getUuid());
}

}  // namespace
}  // namespace mongo
