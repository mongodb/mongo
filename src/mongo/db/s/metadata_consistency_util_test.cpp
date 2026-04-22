/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/db/catalog/create_collection.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/metadata_consistency_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"


namespace mongo {
namespace {

const ShardId kShard0{"shard0"};
const ShardId kShard1{"shard1"};

ChunkType generateChunk(const UUID& collUuid,
                        const ShardId& shardId,
                        const BSONObj& minKey,
                        const BSONObj& maxKey,
                        const std::vector<ChunkHistory>& history) {
    const OID epoch = OID::gen();
    ChunkType chunkType;
    chunkType.setName(OID::gen());
    chunkType.setCollectionUUID(collUuid);
    chunkType.setVersion(ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}));
    chunkType.setShard(shardId);
    chunkType.setMin(minKey);
    chunkType.setMax(maxKey);
    chunkType.setOnCurrentShardSince(Timestamp(1, 0));
    chunkType.setHistory(history);
    return chunkType;
}

TagsType generateZone(const NamespaceString& nss, const BSONObj& minKey, const BSONObj& maxKey) {
    TagsType tagType;
    tagType.setTag(OID::gen().toString());
    tagType.setNS(nss);
    tagType.setMinKey(minKey);
    tagType.setMaxKey(maxKey);
    return tagType;
}

CollectionType generateCollectionType(const NamespaceString& nss,
                                      const UUID& uuid,
                                      const KeyPattern& keyPattern = KeyPattern(BSON("_id" << 1))) {
    return CollectionType{nss, OID::gen(), Timestamp(1), Date_t::now(), uuid, keyPattern};
}

void createLocalCollection(OperationContext* opCtx, const CreateCommand& cmd) {
    OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
        opCtx);
    Lock::GlobalLock lk(opCtx, MODE_X);
    uassertStatusOK(createCollection(opCtx, cmd));
}

std::vector<CollectionPtr> getLocalCatalogCollections(OperationContext* opCtx,
                                                      const NamespaceString& nss) {
    std::vector<CollectionPtr> localCatalogCollections;
    auto collCatalogSnapshot = [&] {
        AutoGetCollection coll(
            opCtx,
            nss,
            MODE_IS,
            AutoGetCollection::Options{}.viewMode(auto_get_collection::ViewMode::kViewsPermitted));
        return CollectionCatalog::get(opCtx);
    }();

    if (auto coll = collCatalogSnapshot->lookupCollectionByNamespace(opCtx, nss)) {
        localCatalogCollections.emplace_back(CollectionPtr(coll));
    }
    return localCatalogCollections;
}

class MetadataConsistencyTest : public ShardServerTestFixture {
protected:
    const ShardId _shardId = kShard0;
    const NamespaceString _nss{"TestDB", "TestColl"};
    const UUID _collUuid = UUID::gen();
    const KeyPattern _keyPattern{BSON("x" << 1)};
    const CollectionType _coll{
        _nss, OID::gen(), Timestamp(1), Date_t::now(), _collUuid, _keyPattern};

    void updateConfigChunks(const std::vector<ChunkType>& chunks) {
        DBDirectClient client(operationContext());

        // Remove all chunks
        const auto resRemove = client.remove(write_ops::DeleteCommandRequest{
            NamespaceString::kConfigsvrChunksNamespace,
            {write_ops::DeleteOpEntry{BSONObj(), true /* multi */}}});
        write_ops::checkWriteErrors(resRemove);

        // Insert the given chunks
        if (chunks.size()) {
            std::vector<BSONObj> docs;
            for (auto& chunk : chunks) {
                docs.emplace_back(chunk.toConfigBSON());
            }
            const auto resInsert = client.insert(
                write_ops::InsertCommandRequest{NamespaceString::kConfigsvrChunksNamespace, docs});
            write_ops::checkWriteErrors(resInsert);
        }
    }

    void assertOneInconsistencyFound(
        const MetadataInconsistencyTypeEnum& type,
        const NamespaceString& nss,
        const std::vector<MetadataInconsistencyItem>& inconsistencies) {
        ASSERT_EQ(1, inconsistencies.size());
        ASSERT_EQ(type, inconsistencies[0].getType());
    }

    void setShardedFilteringMetadata(OperationContext* opCtx, const UUID& uuid) {
        const OID epoch = OID::gen();
        auto chunk = ChunkType(uuid,
                               ChunkRange{_keyPattern.globalMin(), _keyPattern.globalMax()},
                               ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                               _shardId);
        ChunkManager cm(_shardId,
                        DatabaseVersion(UUID::gen(), Timestamp(1, 1)),
                        makeStandaloneRoutingTableHistory(
                            RoutingTableHistory::makeNew(_nss,
                                                         uuid,
                                                         _keyPattern.toBSON(),
                                                         nullptr /* defaultCollator */,
                                                         false /* unique */,
                                                         epoch,
                                                         Timestamp(1, 1),
                                                         boost::none /* timeseriesFields */,
                                                         boost::none /* reshardingFields */,
                                                         true /* allowMigrations */,
                                                         {std::move(chunk)})),
                        boost::none);

        AutoGetCollection autoColl(opCtx, _nss, LockMode::MODE_X);
        CollectionShardingRuntime::assertCollectionLockedAndAcquireExclusive(opCtx, _nss)
            ->setFilteringMetadata(opCtx, CollectionMetadata(std::move(cm), _shardId));
    }
};

void assertIncompatibleUniqueIndexFound(
    const std::vector<MetadataInconsistencyItem>& inconsistencies) {
    ASSERT_TRUE(std::any_of(inconsistencies.begin(), inconsistencies.end(), [](const auto& item) {
        return item.getType() ==
            MetadataInconsistencyTypeEnum::kIncompatibleUniqueIndexOnShardedCollection;
    }));
}

void assertNoIncompatibleUniqueIndexFound(
    const std::vector<MetadataInconsistencyItem>& inconsistencies) {
    ASSERT_TRUE(std::none_of(inconsistencies.begin(), inconsistencies.end(), [](const auto& item) {
        return item.getType() ==
            MetadataInconsistencyTypeEnum::kIncompatibleUniqueIndexOnShardedCollection;
    }));
}

TEST_F(MetadataConsistencyTest,
       UniqueIndexWithNonSimpleCollationOnShardKeyPrefixReportsInconsistency) {
    OperationContext* opCtx = operationContext();

    CreateCommand cmd(_nss);
    createLocalCollection(opCtx, cmd);

    DBDirectClient client(opCtx);
    client.createIndexes(_nss,
                         {BSON("key" << BSON("x" << 1) << "name"
                                     << "x_1_en_unique"
                                     << "unique" << true << "collation"
                                     << BSON("locale"
                                             << "en"))});

    const auto localCatalogCollections = getLocalCatalogCollections(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());
    setShardedFilteringMetadata(opCtx, localCatalogCollections[0]->uuid());

    auto configColl = generateCollectionType(
        _nss, localCatalogCollections[0]->uuid(), KeyPattern(BSON("x" << 1)));

    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataInconsistencies(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogCollections,
        true /*optionalCheckIndexes*/);

    assertIncompatibleUniqueIndexFound(inconsistencies);
}

TEST_F(MetadataConsistencyTest, NonUniqueIndexWithNonSimpleCollationDoesNotReportInconsistency) {
    OperationContext* opCtx = operationContext();

    CreateCommand cmd(_nss);
    createLocalCollection(opCtx, cmd);

    DBDirectClient client(opCtx);
    client.createIndexes(_nss,
                         {BSON("key" << BSON("x" << 1) << "name"
                                     << "x_1_en"
                                     << "collation"
                                     << BSON("locale"
                                             << "en"))});

    const auto localCatalogCollections = getLocalCatalogCollections(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());
    setShardedFilteringMetadata(opCtx, localCatalogCollections[0]->uuid());

    auto configColl = generateCollectionType(
        _nss, localCatalogCollections[0]->uuid(), KeyPattern(BSON("x" << 1)));

    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataInconsistencies(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogCollections,
        true /*optionalCheckIndexes*/);

    assertNoIncompatibleUniqueIndexFound(inconsistencies);
}

TEST_F(MetadataConsistencyTest, UniqueIndexWithSimpleCollationDoesNotReportInconsistency) {
    OperationContext* opCtx = operationContext();

    CreateCommand cmd(_nss);
    createLocalCollection(opCtx, cmd);

    DBDirectClient client(opCtx);
    client.createIndexes(_nss,
                         {BSON("key" << BSON("x" << 1) << "name"
                                     << "x_1_unique"
                                     << "unique" << true)});

    const auto localCatalogCollections = getLocalCatalogCollections(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());
    setShardedFilteringMetadata(opCtx, localCatalogCollections[0]->uuid());

    auto configColl = generateCollectionType(
        _nss, localCatalogCollections[0]->uuid(), KeyPattern(BSON("x" << 1)));

    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataInconsistencies(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogCollections,
        true /*optionalCheckIndexes*/);

    assertNoIncompatibleUniqueIndexFound(inconsistencies);
}

TEST_F(MetadataConsistencyTest,
       UniqueIndexWithNonSimpleCollationNotReportedWhenOptionalCheckIndexesFalse) {
    OperationContext* opCtx = operationContext();

    CreateCommand cmd(_nss);
    createLocalCollection(opCtx, cmd);

    DBDirectClient client(opCtx);
    client.createIndexes(_nss,
                         {BSON("key" << BSON("x" << 1) << "name"
                                     << "x_1_en_unique"
                                     << "unique" << true << "collation"
                                     << BSON("locale"
                                             << "en"))});

    const auto localCatalogCollections = getLocalCatalogCollections(opCtx, _nss);
    ASSERT_EQ(1, localCatalogCollections.size());
    setShardedFilteringMetadata(opCtx, localCatalogCollections[0]->uuid());

    auto configColl = generateCollectionType(
        _nss, localCatalogCollections[0]->uuid(), KeyPattern(BSON("x" << 1)));

    const auto inconsistencies = metadata_consistency_util::checkCollectionMetadataInconsistencies(
        opCtx,
        _shardId,
        _shardId,
        {configColl},
        localCatalogCollections,
        false /*optionalCheckIndexes*/);

    assertNoIncompatibleUniqueIndexFound(inconsistencies);
}

class MetadataConsistencyConfigTest : public MetadataConsistencyTest {
protected:
    void setUp() override {
        // The ShardingState must be set to 'config' to be able to call
        // metadata_consistency_util::checkChunksConsistency()
        _myShardName = ShardId::kConfigServerId;
        MetadataConsistencyTest::setUp();
    }
};

TEST_F(MetadataConsistencyConfigTest, FindRoutingTableRangeGapInconsistency) {
    const auto chunk1 = generateChunk(_collUuid,
                                      _shardId,
                                      _keyPattern.globalMin(),
                                      BSON("x" << 0),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto chunk2 = generateChunk(_collUuid,
                                      _shardId,
                                      BSON("x" << 1),
                                      _keyPattern.globalMax(),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});
    updateConfigChunks({chunk1, chunk2});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kRoutingTableRangeGap, _nss, inconsistencies);
}

TEST_F(MetadataConsistencyConfigTest, FindMissingChunkWithMaxKeyInconsistency) {
    const auto chunk = generateChunk(_collUuid,
                                     _shardId,
                                     _keyPattern.globalMin(),
                                     BSON("x" << 0),
                                     {ChunkHistory(Timestamp(1, 0), _shardId)});
    updateConfigChunks({chunk});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kRoutingTableMissingMaxKey, _nss, inconsistencies);
}

TEST_F(MetadataConsistencyConfigTest, FindMissingChunkWithMinKeyInconsistency) {
    const auto chunk = generateChunk(_collUuid,
                                     _shardId,
                                     BSON("x" << 0),
                                     _keyPattern.globalMax(),
                                     {ChunkHistory(Timestamp(1, 0), _shardId)});
    updateConfigChunks({chunk});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kRoutingTableMissingMinKey, _nss, inconsistencies);
}

TEST_F(MetadataConsistencyConfigTest, FindRoutingTableRangeOverlapInconsistency) {
    const auto chunk1 = generateChunk(_collUuid,
                                      _shardId,
                                      _keyPattern.globalMin(),
                                      BSON("x" << 0),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto chunk2 = generateChunk(_collUuid,
                                      _shardId,
                                      BSON("x" << -10),
                                      _keyPattern.globalMax(),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});
    updateConfigChunks({chunk1, chunk2});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kRoutingTableRangeOverlap, _nss, inconsistencies);
}

TEST_F(MetadataConsistencyConfigTest, FindCorruptedChunkShardKeyInconsistency) {
    const auto chunk1 = generateChunk(_collUuid,
                                      _shardId,
                                      _keyPattern.globalMin(),
                                      BSON("x" << 0),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});

    const auto chunk2 = generateChunk(_collUuid,
                                      _shardId,
                                      BSON("y" << 0),
                                      _keyPattern.globalMax(),
                                      {ChunkHistory(Timestamp(1, 0), _shardId)});
    updateConfigChunks({chunk1, chunk2});

    const auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    ASSERT_EQ(2, inconsistencies.size());
    ASSERT_EQ(MetadataInconsistencyTypeEnum::kCorruptedChunkShardKey, inconsistencies[0].getType());
    ASSERT_EQ(MetadataInconsistencyTypeEnum::kRoutingTableRangeGap, inconsistencies[1].getType());
}

TEST_F(MetadataConsistencyConfigTest, FindCorruptedZoneShardKeyInconsistency) {
    const auto zone1 = generateZone(_nss, _keyPattern.globalMin(), BSON("x" << 0));

    const auto zone2 = generateZone(_nss, BSON("y" << 0), _keyPattern.globalMax());

    const auto inconsistencies = metadata_consistency_util::checkZonesInconsistencies(
        operationContext(), _coll, {zone1, zone2});

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kCorruptedZoneShardKey, _nss, inconsistencies);
}

TEST_F(MetadataConsistencyConfigTest, FindZoneRangeOverlapInconsistency) {
    const auto zone1 = generateZone(_nss, _keyPattern.globalMin(), BSON("x" << 0));

    const auto zone2 = generateZone(_nss, BSON("x" << -10), _keyPattern.globalMax());

    const auto inconsistencies = metadata_consistency_util::checkZonesInconsistencies(
        operationContext(), _coll, {zone1, zone2});

    assertOneInconsistencyFound(
        MetadataInconsistencyTypeEnum::kZonesRangeOverlap, _nss, inconsistencies);
}

class MetadataConsistencyRandomRoutingTableTest : public MetadataConsistencyConfigTest {
protected:
    inline const static auto _shards = std::vector<ShardId>{kShard0, kShard1};
    PseudoRandom _random{SecureRandom().nextInt64()};

    std::vector<ChunkType> getRandomRoutingTable() {
        int numChunks = _random.nextInt32(9) + 1;  // minimum 1 chunks, maximum 10 chunks
        std::vector<ChunkType> chunks;

        // Loop generating random routing table: [MinKey, 0), [1, 2), [2, 3), ... [x, MaxKey]
        int nextMin;
        for (int nextMax = 0; nextMax < numChunks; nextMax++) {
            auto randomShard = _shards.at(_random.nextInt64(_shards.size()));
            // set min as `MinKey` during first iteration, otherwise next min
            auto min = nextMax == 0 ? _keyPattern.globalMin() : BSON("x" << nextMin);
            // set max as `MaxKey` during last iteration, otherwise next max
            auto max = nextMax == numChunks - 1 ? _keyPattern.globalMax() : BSON("x" << nextMax);
            auto chunk = generateChunk(
                _collUuid, randomShard, min, max, {ChunkHistory(Timestamp(1, 0), randomShard)});
            nextMin = nextMax;
            chunks.push_back(chunk);
        }

        return chunks;
    };
};

/*
 * Test function to check the correct behaviour of finding range gaps inconsistencies with random
 * ranges. In order to introduce inconsistencies, a random number of chunks are removed from the the
 * routing table to create range gaps.
 */
TEST_F(MetadataConsistencyRandomRoutingTableTest, FindRoutingTableRangeGapInconsistency) {
    auto chunks = getRandomRoutingTable();
    updateConfigChunks(chunks);

    // Check that there are no inconsistencies in the routing table
    auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);
    ASSERT_EQ(0, inconsistencies.size());

    // Remove randoms chunk from the routing table
    const auto kNumberOfChunksToRemove = _random.nextInt64(chunks.size()) + 1;
    for (int i = 0; i < kNumberOfChunksToRemove; i++) {
        const auto itChunkToRemove = _random.nextInt64(chunks.size());
        chunks.erase(chunks.begin() + itChunkToRemove);
    }
    updateConfigChunks(chunks);

    inconsistencies = metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    // Assert that there is at least one gap inconsistency
    try {
        ASSERT_LTE(1, inconsistencies.size());
        for (const auto& inconsistency : inconsistencies) {
            const auto type = inconsistency.getType();
            ASSERT_TRUE(type == MetadataInconsistencyTypeEnum::kRoutingTableRangeGap ||
                        type == MetadataInconsistencyTypeEnum::kRoutingTableMissingMinKey ||
                        type == MetadataInconsistencyTypeEnum::kRoutingTableMissingMaxKey ||
                        type == MetadataInconsistencyTypeEnum::kMissingRoutingTable);
        }
    } catch (...) {
        LOGV2_INFO(7424600,
                   "Expecting gap inconsistencies",
                   "numberOfInconsistencies"_attr = inconsistencies.size(),
                   "inconsistencies"_attr = inconsistencies);
        throw;
    }
}

/*
 * Test function to check the correct behaviour of finding range overlaps inconsistencies with
 * random ranges. In order to introduce inconsistencies, one chunk is randomly selected and its max
 * or min bound is set to a random bigger or lower value, respectively.
 */
TEST_F(MetadataConsistencyRandomRoutingTableTest, FindRoutingTableRangeOverlapInconsistency) {
    auto chunks = getRandomRoutingTable();
    updateConfigChunks(chunks);

    // Check that there are no inconsistencies in the routing table
    auto inconsistencies =
        metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);
    ASSERT_EQ(0, inconsistencies.size());

    // If there is only one chunk, we can't introduce an overlap
    if (chunks.size() == 1) {
        return;
    }

    const auto chunkIdx = static_cast<size_t>(_random.nextInt64(chunks.size()));
    auto& chunk = chunks.at(chunkIdx);

    auto overlapMax = [&]() {
        if (_random.nextInt64(10) == 0) {
            // With 1/10 probability, set max to MaxKey
            chunk.setMax(_keyPattern.globalMax());
        } else {
            // Otherwise, set max to a random value bigger than actual
            auto max = chunk.getMax()["x"].numberInt();
            chunk.setMax(BSON("x" << max + _random.nextInt64(10) + 1));
        }
    };

    auto overlapMin = [&]() {
        if (_random.nextInt64(10) == 0) {
            // With 1/10 probability, set min to MinKey
            chunk.setMin(_keyPattern.globalMin());
        } else {
            // Otherwise, set min to a random value smaller than actual
            auto min = chunk.getMin()["x"].numberInt();
            chunk.setMin(BSON("x" << min - _random.nextInt64(10) - 1));
        }
    };

    if (chunkIdx == 0) {
        overlapMax();
    } else if (chunkIdx == (chunks.size() - 1)) {
        overlapMin();
    } else {
        // With 1/2 probability, overlap min or max
        if (_random.nextInt64(2) == 0) {
            overlapMin();
        } else {
            overlapMax();
        }
    }

    updateConfigChunks(chunks);
    inconsistencies = metadata_consistency_util::checkChunksConsistency(operationContext(), _coll);

    // Assert that there is at least one overlap inconsistency
    try {
        ASSERT_LTE(1, inconsistencies.size());
        ASSERT_TRUE(std::any_of(
            inconsistencies.begin(), inconsistencies.end(), [](const auto& inconsistency) {
                return inconsistency.getType() ==
                    MetadataInconsistencyTypeEnum::kRoutingTableRangeOverlap;
            }));
    } catch (...) {
        LOGV2_INFO(7424601,
                   "Expecting overlap inconsistencies",
                   "numberOfInconsistencies"_attr = inconsistencies.size(),
                   "inconsistencies"_attr = inconsistencies);
        throw;
    }
}

}  // namespace
}  // namespace mongo
