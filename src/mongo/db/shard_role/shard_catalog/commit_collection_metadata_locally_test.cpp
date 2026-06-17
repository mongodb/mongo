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
#include "mongo/db/shard_role/shard_catalog/collection_cache_recoverer.h"
#include "mongo/db/shard_role/shard_catalog/collection_metadata.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

const NamespaceString kTestNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "TestColl");
const NamespaceString kFromNss =
    NamespaceString::createNamespaceString_forTest("TestDB", "FromColl");
const NamespaceString kToNss = NamespaceString::createNamespaceString_forTest("TestDB", "ToColl");
const std::string kShardKey = "_id";
const BSONObj kShardKeyPattern = BSON(kShardKey << 1);

class MockCatalogClient final : public ShardingCatalogClientMock {
public:
    using ShardingCatalogClientMock::getCollection;

    void setCollectionMetadata(CollectionType coll, std::vector<ChunkType> chunks) {
        _notFound = false;
        _coll = std::move(coll);
        _chunks = std::move(chunks);
    }

    void setCollectionNotFound() {
        _notFound = true;
    }

    CollectionType getCollection(OperationContext* opCtx,
                                 const NamespaceString& nss,
                                 repl::ReadConcernLevel readConcernLevel) override {
        uassert(ErrorCodes::NamespaceNotFound, "Collection not found in mock", !_notFound);
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
    bool _notFound = false;
};

struct CollectionAndChunksMetadata {
    CollectionType collType;
    std::vector<ChunkType> chunks;
};

CollectionAndChunksMetadata makeCollectionMetadata(const NamespaceString& nss, int nChunks) {
    const UUID uuid = UUID::gen();
    const OID epoch = OID::gen();
    const Timestamp timestamp(Date_t::now());

    CollectionType collType{nss, epoch, timestamp, Date_t::now(), uuid, kShardKeyPattern};

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

CollectionAndChunksMetadata makeCollectionMetadata(int nChunks) {
    return makeCollectionMetadata(kTestNss, nChunks);
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

    void seedShardCatalog(const CollectionType& coll, const std::vector<ChunkType>& chunks) {
        DBDirectClient client(operationContext());
        client.insert(NamespaceString::kConfigShardCatalogCollectionsNamespace,
                      coll.asShardCatalogType().toBSON());
        for (const auto& chunk : chunks) {
            client.insert(NamespaceString::kConfigShardCatalogChunksNamespace,
                          chunk.toConfigBSON());
        }
    }

    long long countCommandOplogEntries(StringData commandField, const NamespaceString& nss) {
        const std::string objField = "o." + std::string{commandField};
        return findLocalDocs(NamespaceString::kRsOplogNamespace,
                             BSON("op" << "c" << objField << nss.coll()))
            .size();
    }

private:
    MockCatalogClient* _mockCatalogClient = nullptr;
};

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyPersistsCollectionAndChunks) {
    auto [collType, chunks] = makeCollectionMetadata(3);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

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

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->isSharded());
    ASSERT_EQ(metadata->getChunkManager()->getUUID(), collType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyIsIdempotent) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionPersistsCollectionAndChunks) {
    auto [collType, chunks] = makeCollectionMetadata(3);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

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

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->isSharded());
    ASSERT_EQ(metadata->getChunkManager()->getUUID(), collType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionIsIdempotent) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);
}

TEST_F(CommitCollectionMetadataLocallyTest, CreateCollectionReplacesStaleChunksOnReissuedOIDs) {
    // First pass: persist the initial chunks for the collection.
    auto [collType, chunksPass1] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunksPass1);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    // Second pass: same UUID and ranges, but with freshly generated chunk OIDs (mimicking the
    // unsplittable->sharded transition where the global catalog reissues chunk OIDs).
    auto chunksPass2 = chunksPass1;
    for (auto& chunk : chunksPass2) {
        chunk.setName(OID::gen());
    }
    mockCatalogClient()->setCollectionMetadata(collType, chunksPass2);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    // Only the second-pass chunks should remain; the first-pass rows must be deleted, not appended.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    auto chunkDocs = findLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace);
    std::set<OID> persistedNames;
    for (const auto& doc : chunkDocs) {
        persistedNames.insert(doc.getField(ChunkType::name.name()).OID());
    }
    for (const auto& chunk : chunksPass2) {
        ASSERT(persistedNames.count(chunk.getName()))
            << "expected new-OID chunk " << chunk.getName() << " to be persisted";
    }
    for (const auto& chunk : chunksPass1) {
        ASSERT(!persistedNames.count(chunk.getName()))
            << "stale chunk " << chunk.getName() << " should have been deleted";
    }
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunklessCollectionPersistsTokenToDisk) {
    auto [collType, chunks] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), collType.getUuid());

    // No chunk should be stored in the local chunks for a chunkless collection on the dbPrimary.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunklessCollectionUpdatesCSR) {
    auto [collType, chunks] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->isSharded());
    ASSERT_EQ(metadata->getChunkManager()->getUUID(), collType.getUuid());
    // The DB primary owns no real chunks for this collection, so its placement version is unset.
    ASSERT_FALSE(metadata->getShardPlacementVersion().isSet());
}

TEST_F(CommitCollectionMetadataLocallyTest, ChunklessCollectionIsIdempotent) {
    auto [collType, _] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType, {});

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, RefineShardKeyChunklessPersistsCollectionWithNewEpoch) {
    // Seed a chunkless tracked collection at (epoch1, ts1).
    auto [collType1, _] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType1, {});
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);

    // Simulate a refine to (epoch2, ts2) on the same UUID with an extended key pattern.
    const OID epoch2 = OID::gen();
    const Timestamp ts2 = collType1.getTimestamp() + 1;
    const BSONObj newKeyPattern = BSON("_id" << 1 << "extra" << 1);
    CollectionType collType2{
        kTestNss, epoch2, ts2, Date_t::now(), collType1.getUuid(), newKeyPattern};
    mockCatalogClient()->setCollectionMetadata(collType2, {});

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_BSONOBJ_EQ(collDocs[0].getObjectField("key"), newKeyPattern);
}

TEST_F(CommitCollectionMetadataLocallyTest,
       CommitChunklessUpdatesCatalogButPreservesExistingChunklessCSR) {
    auto [collType1, _] = makeCollectionMetadata(0);
    mockCatalogClient()->setCollectionMetadata(collType1, {});
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        ASSERT_TRUE(metadata->hasRoutingTable());
        ASSERT_FALSE(metadata->getShardPlacementVersion().isSet());
    }

    const OID epoch2 = OID::gen();
    const Timestamp ts2 = collType1.getTimestamp() + 1;
    const BSONObj newKeyPattern = BSON("_id" << 1 << "extra" << 1);
    CollectionType collType2{
        kTestNss, epoch2, ts2, Date_t::now(), collType1.getUuid(), newKeyPattern};
    mockCatalogClient()->setCollectionMetadata(collType2, {});

    shard_catalog_commit::commitChunklessCollectionMetadataLocally(operationContext(), kTestNss);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_BSONOBJ_EQ(collDocs[0].getObjectField("key"), newKeyPattern);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->hasRoutingTable());
    ASSERT_FALSE(metadata->getShardPlacementVersion().isSet());
    ASSERT_BSONOBJ_EQ(metadata->getChunkManager()->getShardKeyPattern().getKeyPattern().toBSON(),
                      kShardKeyPattern);
    ASSERT_EQ(metadata->getChunkManager()->getVersion().epoch(), collType1.getEpoch());
    ASSERT_EQ(metadata->getChunkManager()->getVersion().getTimestamp(), collType1.getTimestamp());
}

TEST_F(CommitCollectionMetadataLocallyTest, CommitChunklessPreservesCSRWithOwnedChunks) {
    auto [collType1, chunks] = makeCollectionMetadata(2);
    for (auto& chunk : chunks) {
        chunk.setShard(ShardingState::get(operationContext())->shardId());
    }
    mockCatalogClient()->setCollectionMetadata(collType1, chunks);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, false);

    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        ASSERT_TRUE(metadata->getShardPlacementVersion().isSet());
    }

    const OID epoch2 = OID::gen();
    const Timestamp ts2 = collType1.getTimestamp() + 1;
    const BSONObj newKeyPattern = BSON("_id" << 1 << "extra" << 1);
    CollectionType collType2{
        kTestNss, epoch2, ts2, Date_t::now(), collType1.getUuid(), newKeyPattern};
    mockCatalogClient()->setCollectionMetadata(collType2, {});

    shard_catalog_commit::commitChunklessCollectionMetadataLocally(operationContext(), kTestNss);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(collDocs.size(), 1u);
    ASSERT_BSONOBJ_EQ(collDocs[0].getObjectField("key"), newKeyPattern);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    auto metadata = scopedCsr->getCurrentMetadataIfKnown();
    ASSERT_TRUE(metadata);
    ASSERT_TRUE(metadata->getShardPlacementVersion().isSet());
    ASSERT_BSONOBJ_EQ(metadata->getChunkManager()->getShardKeyPattern().getKeyPattern().toBSON(),
                      kShardKeyPattern);
    ASSERT_EQ(metadata->getChunkManager()->getVersion().epoch(), collType1.getEpoch());
    ASSERT_EQ(metadata->getChunkManager()->getVersion().getTimestamp(), collType1.getTimestamp());
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
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

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
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);
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

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

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

TEST_F(CommitCollectionMetadataLocallyTest, CommitNotifiesInFlightRecoverer) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

    // Simulate a recovery round that has already read from disk and is waiting to drain.
    auto recoverer = std::make_shared<CollectionCacheRecoverer>(
        kTestNss, CancellationToken::uncancelable(), CollectionMetadata::UNTRACKED());
    {
        auto scopedCsr = CollectionShardingRuntime::acquireExclusive(operationContext(), kTestNss);
        scopedCsr->setCollectionRecoverer(recoverer);
    }
    auto roundId = recoverer->start(operationContext(), nullptr);

    // Drop the collection. This should notify the recoverer about the drop.
    shard_catalog_commit::commitDropCollectionLocally(
        operationContext(), kTestNss, collType.getUuid());

    // The recoverer should force a new recovery instead of returning the metadata before the drop.
    ASSERT_FALSE(recoverer->drainAndApply(operationContext(), roundId));
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
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss);

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

TEST_F(CommitCollectionMetadataLocallyTest,
       DropCollectionMetadataInvalidationOplogEntryUsesCommandNamespace) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    seedShardCatalog(collType, chunks);

    shard_catalog_commit::commitDropCollectionLocally(
        operationContext(), kTestNss, collType.getUuid());

    auto oplogEntries =
        findLocalDocs(NamespaceString::kRsOplogNamespace,
                      BSON("op" << "c" << "o.invalidateCollectionMetadata" << kTestNss.coll()));
    ASSERT_EQ(oplogEntries.size(), 1u);
    ASSERT_EQ(oplogEntries.front().getStringField("ns"), kTestNss.getCommandNS().ns_forTest());
}

TEST_F(CommitCollectionMetadataLocallyTest, SetAllowChunkOperationsOplogEntryUsesCommandNamespace) {
    auto [collType, chunks] = makeCollectionMetadata(1);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);

    auto shardVersion = [&] {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        auto metadata = scopedCsr->getCurrentMetadataIfKnown();
        ASSERT_TRUE(metadata);
        return ShardVersionFactory::make(*metadata);
    }();
    ScopedSetShardRole scopedSetShardRole{
        operationContext(), kTestNss, shardVersion, boost::none /* databaseVersion */};

    shard_catalog_commit::commitSetAllowChunkOperationsLocally(operationContext(),
                                                               kTestNss,
                                                               false /* allowChunkOperations */,
                                                               collType.getUuid(),
                                                               true /* isPrimaryShard */);

    auto oplogEntries =
        findLocalDocs(NamespaceString::kRsOplogNamespace,
                      BSON("op" << "c" << "o.setAllowChunkOperations" << kTestNss.coll()));
    // TODO (SERVER-127444): there should be a single oplog entry.
    ASSERT_EQ(oplogEntries.size(), 2u);
    ASSERT_EQ(oplogEntries.back().getStringField("ns"), kTestNss.getCommandNS().ns_forTest());
}

// ---------------------------------------------------------------------------
// Clone (setFCV) tests
// ---------------------------------------------------------------------------

// The clone persists the durable shard catalog but performs no in-memory or oplog side effects:
// no 'c' oplog entries (neither the invalidate nor the setAllowChunkOperations) and no in-memory
// CSR install.
TEST_F(CommitCollectionMetadataLocallyTest, CloneOnlyWritesDurableCatalogAndEmitsNoOplog) {
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);

    shard_catalog_commit::cloneCollectionMetadataLocally(
        operationContext(), kTestNss, true /* isDbPrimaryShard */);

    // The durable shard catalog is written.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    // No 'c' oplog entries are written during the clone.
    ASSERT_EQ(countCommandOplogEntries("invalidateCollectionMetadata", kTestNss), 0);
    ASSERT_EQ(countCommandOplogEntries("setAllowChunkOperations", kTestNss), 0);

    // The clone does not touch the in-memory CSR: with no prior state it stays unknown and
    // non-authoritative, to be recovered lazily from the durable catalog when next needed.
    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    ASSERT_FALSE(scopedCsr->getCurrentMetadataIfKnown());
    ASSERT(scopedCsr->getAuthoritativeState() ==
           CollectionShardingRuntime::AuthoritativeState::kNonAuthoritative);
}

// The clone must not clear an existing in-memory CSR when the shard owns no chunks and is not the
// DB primary; the on-disk entry is still removed.
TEST_F(CommitCollectionMetadataLocallyTest, CloneDoesNotClearCsrWhenShardOwnsNoChunks) {
    // Seed an existing CSR with known metadata, as if it had been populated by the legacy catalog
    // cache loader prior to cloning. The non-clone commit emits exactly one invalidate 'c' entry,
    // which lets the assertion below verify the clone itself emits none.
    auto [collType, chunks] = makeCollectionMetadata(2);
    mockCatalogClient()->setCollectionMetadata(collType, chunks);
    shard_catalog_commit::commitCollectionMetadataLocally(operationContext(), kTestNss, true);
    ASSERT_EQ(countCommandOplogEntries("invalidateCollectionMetadata", kTestNss), 1);
    {
        auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
        ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
    }

    // Clone as a non-primary shard that owns no chunks: the durable entry is removed but the
    // in-memory CSR is left untouched (no clear) and no further oplog entry is emitted.
    mockCatalogClient()->setCollectionMetadata(collType, {} /* no owned chunks */);
    shard_catalog_commit::cloneCollectionMetadataLocally(
        operationContext(), kTestNss, false /* isDbPrimaryShard */);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 0);
    // Still just the single invalidate from the non-clone seed; the clone added none.
    ASSERT_EQ(countCommandOplogEntries("invalidateCollectionMetadata", kTestNss), 1);

    auto scopedCsr = CollectionShardingRuntime::acquireShared(operationContext(), kTestNss);
    ASSERT_TRUE(scopedCsr->getCurrentMetadataIfKnown());
}

// ---------------------------------------------------------------------------
// Rename tests
// ---------------------------------------------------------------------------

TEST_F(CommitCollectionMetadataLocallyTest, RenameUnshardedToShardedReplacingIt) {
    // toNss holds a sharded collection; fromNss is unsharded (no shard catalog entry).
    auto [toCollType, toChunks] = makeCollectionMetadata(kToNss, 3);
    seedShardCatalog(toCollType, toChunks);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    // After rename, toNss holds the unsharded collection — CSRS has no entry for it.
    mockCatalogClient()->setCollectionNotFound();

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           boost::none,
                                                           kToNss,
                                                           toCollType.getUuid(),
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimary */);

    shard_catalog_commit::commitDropOfStaleChunksForRename(operationContext(),
                                                           toCollType.getUuid());

    // The renamed collection is unsharded so it has no catalog representation; the replaced
    // sharded collection is fully removed.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 0);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, RenameUnshardedToUnsharded) {
    // Both fromNss and toNss are unsharded — nothing to track in the shard catalog.
    mockCatalogClient()->setCollectionNotFound();

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           boost::none,
                                                           kToNss,
                                                           boost::none,
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimaryShard */);

    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 0);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 0);
}

TEST_F(CommitCollectionMetadataLocallyTest, RenameShardedToUnshardedReplacingIt) {
    // fromNss is sharded; toNss is unsharded (gets replaced). After the rename the sharded
    // collection lives at toNss.
    auto [fromCollType, fromChunks] = makeCollectionMetadata(kFromNss, 3);
    seedShardCatalog(fromCollType, fromChunks);

    // The CSRS now shows the renamed collection under toNss with the same UUID.
    CollectionType renamedColl{kToNss,
                               fromCollType.getEpoch(),
                               fromCollType.getTimestamp(),
                               Date_t::now(),
                               fromCollType.getUuid(),
                               kShardKeyPattern};
    mockCatalogClient()->setCollectionMetadata(renamedColl, fromChunks);

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           fromCollType.getUuid(),
                                                           kToNss,
                                                           boost::none,
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimary */);

    // Metadata now lives at toNss; fromNss entry is gone; chunks are unchanged.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 3);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), fromCollType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest, RenameShardedToShardedReplacingIt) {
    // fromNss is sharded; toNss is also sharded and gets replaced by the rename.
    auto [fromCollType, fromChunks] = makeCollectionMetadata(kFromNss, 2);
    auto [toCollType, toChunks] = makeCollectionMetadata(kToNss, 3);
    seedShardCatalog(fromCollType, fromChunks);
    seedShardCatalog(toCollType, toChunks);

    // The CSRS now shows the renamed collection under toNss with fromNss's UUID.
    CollectionType renamedColl{kToNss,
                               fromCollType.getEpoch(),
                               fromCollType.getTimestamp(),
                               Date_t::now(),
                               fromCollType.getUuid(),
                               kShardKeyPattern};
    mockCatalogClient()->setCollectionMetadata(renamedColl, fromChunks);

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           fromCollType.getUuid(),
                                                           kToNss,
                                                           toCollType.getUuid(),
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimary */);

    shard_catalog_commit::commitDropOfStaleChunksForRename(operationContext(),
                                                           toCollType.getUuid());

    // toNss entry reflects fromNss metadata; fromNss entry is gone; replaced toNss chunks removed.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), fromCollType.getUuid());
}

TEST_F(CommitCollectionMetadataLocallyTest, RenameShardedToShardedNoReplacement) {
    // fromNss is sharded; toNss did not exist before the rename.
    auto [fromCollType, fromChunks] = makeCollectionMetadata(kFromNss, 2);
    seedShardCatalog(fromCollType, fromChunks);

    // The CSRS now shows the renamed collection under toNss with fromNss's UUID.
    CollectionType renamedColl{kToNss,
                               fromCollType.getEpoch(),
                               fromCollType.getTimestamp(),
                               Date_t::now(),
                               fromCollType.getUuid(),
                               kShardKeyPattern};
    mockCatalogClient()->setCollectionMetadata(renamedColl, fromChunks);

    shard_catalog_commit::commitRenameOfCollectionMetadata(operationContext(),
                                                           kFromNss,
                                                           fromCollType.getUuid(),
                                                           kToNss,
                                                           boost::none,
                                                           boost::none,
                                                           false /* isUpgrading */,
                                                           false /* isDbPrimary */);

    // toNss entry reflects fromNss metadata; fromNss entry is gone.
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace), 1);
    ASSERT_EQ(countLocalDocs(NamespaceString::kConfigShardCatalogChunksNamespace), 2);

    auto collDocs = findLocalDocs(NamespaceString::kConfigShardCatalogCollectionsNamespace);
    ASSERT_EQ(UUID::fromCDR(collDocs[0].getField("uuid").uuid()), fromCollType.getUuid());
}

}  // namespace
}  // namespace mongo
