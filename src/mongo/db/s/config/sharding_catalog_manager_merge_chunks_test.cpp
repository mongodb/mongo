/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/client/read_preference.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_cache_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {
namespace {

using unittest::assertGet;

class MergeChunkTest : public ConfigServerTestFixture {
protected:
    std::string _shardName = "shard0000";
    void setUp() override {
        ConfigServerTestFixture::setUp();
        ShardType shard;
        shard.setName(_shardName);
        shard.setHost(_shardName + ":12");
        setupShards({shard});

        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace.ns());
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace.ns(),
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        ConfigServerTestFixture::tearDown();
    }

    const ShardId _shardId{_shardName};
    const NamespaceString _nss1{"TestDB.TestColl1"};
    const NamespaceString _nss2{"TestDB.TestColl2"};
    const KeyPattern _keyPattern{BSON("x" << 1)};
};

TEST_F(MergeChunkTest, MergeExistingChunksCorrectlyShouldSucceed) {
    const auto collEpoch = OID::gen();
    const Timestamp collTimestamp(42);

    const auto collUuid = UUID::gen();
    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(collUuid);

    auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
    chunk.setVersion(origVersion);
    chunk.setShard(_shardId);

    // Construct chunk to be merged
    auto chunk2(chunk);
    chunk2.setName(OID::gen());

    auto chunkMin = BSON("a" << 1);
    auto chunkBound = BSON("a" << 5);
    auto chunkMax = BSON("a" << 10);
    // first chunk boundaries
    chunk.setMin(chunkMin);
    chunk.setMax(chunkBound);
    // second chunk boundaries
    chunk2.setMin(chunkBound);
    chunk2.setMax(chunkMax);

    setupCollection(_nss1, _keyPattern, {chunk, chunk2});

    Timestamp validAfter{100, 0};

    ChunkRange rangeToBeMerged(chunk.getMin(), chunk2.getMax());

    auto versions = assertGet(ShardingCatalogManager::get(operationContext())
                                  ->commitChunksMerge(operationContext(),
                                                      _nss1,
                                                      collEpoch,
                                                      collTimestamp,
                                                      collUuid,
                                                      rangeToBeMerged,
                                                      _shardId,
                                                      validAfter));

    auto collVersion = ChunkVersion::parse(versions["collectionVersion"]);
    auto shardVersion = ChunkVersion::parse(versions["shardVersion"]);

    ASSERT_TRUE(origVersion.isOlderThan(shardVersion));
    ASSERT_EQ(collVersion, shardVersion);

    // Check for increment on mergedChunk's minor version
    auto expectedShardVersion =
        ChunkVersion({origVersion.epoch(), origVersion.getTimestamp()},
                     {origVersion.majorVersion(), origVersion.minorVersion() + 1});
    ASSERT_EQ(expectedShardVersion, shardVersion);


    const auto query = BSON(ChunkType::collectionUUID() << collUuid);
    auto findResponse = uassertStatusOK(
        getConfigShard()->exhaustiveFindOnConfig(operationContext(),
                                                 ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 ChunkType::ConfigNS,
                                                 query,
                                                 BSON(ChunkType::lastmod << -1),
                                                 boost::none));

    const auto& chunksVector = findResponse.docs;

    // There should be exactly one chunk left in the collection
    ASSERT_EQ(1u, chunksVector.size());

    // MergedChunk should have range [chunkMin, chunkMax]
    auto mergedChunk = uassertStatusOK(ChunkType::parseFromConfigBSON(
        chunksVector.front(), collVersion.epoch(), collVersion.getTimestamp()));
    ASSERT_BSONOBJ_EQ(chunkMin, mergedChunk.getMin());
    ASSERT_BSONOBJ_EQ(chunkMax, mergedChunk.getMax());

    // Check that the shard version returned by the merge matches the CSRS one
    ASSERT_EQ(shardVersion, mergedChunk.getVersion());

    // Make sure history is there
    ASSERT_EQ(1UL, mergedChunk.getHistory().size());
    ASSERT_EQ(validAfter, mergedChunk.getHistory().front().getValidAfter());
}

TEST_F(MergeChunkTest, MergeSeveralChunksCorrectlyShouldSucceed) {
    const auto collEpoch = OID::gen();
    const Timestamp collTimestamp(42);
    const auto collUuid = UUID::gen();
    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(collUuid);

    auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
    chunk.setVersion(origVersion);
    chunk.setShard(_shardId);

    // Construct chunks to be merged
    auto chunk2(chunk);
    auto chunk3(chunk);
    chunk2.setName(OID::gen());
    chunk3.setName(OID::gen());

    auto chunkMin = BSON("a" << 1);
    auto chunkBound = BSON("a" << 5);
    auto chunkBound2 = BSON("a" << 7);
    auto chunkMax = BSON("a" << 10);
    // first chunk boundaries
    chunk.setMin(chunkMin);
    chunk.setMax(chunkBound);
    // second chunk boundaries
    chunk2.setMin(chunkBound);
    chunk2.setMax(chunkBound2);
    // third chunk boundaries
    chunk3.setMin(chunkBound2);
    chunk3.setMax(chunkMax);

    setupCollection(_nss1, _keyPattern, {chunk, chunk2, chunk3});
    ChunkRange rangeToBeMerged(chunk.getMin(), chunk3.getMax());

    Timestamp validAfter{100, 0};

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunksMerge(operationContext(),
                                      _nss1,
                                      collEpoch,
                                      collTimestamp,
                                      collUuid,
                                      rangeToBeMerged,
                                      _shardId,
                                      validAfter));

    const auto query BSON(ChunkType::collectionUUID() << collUuid);
    auto findResponse = uassertStatusOK(
        getConfigShard()->exhaustiveFindOnConfig(operationContext(),
                                                 ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 ChunkType::ConfigNS,
                                                 query,
                                                 BSON(ChunkType::lastmod << -1),
                                                 boost::none));

    const auto& chunksVector = findResponse.docs;

    // There should be exactly one chunk left in the collection
    ASSERT_EQ(1u, chunksVector.size());

    // MergedChunk should have range [chunkMin, chunkMax]
    auto mergedChunk = uassertStatusOK(
        ChunkType::parseFromConfigBSON(chunksVector.front(), collEpoch, collTimestamp));
    ASSERT_BSONOBJ_EQ(chunkMin, mergedChunk.getMin());
    ASSERT_BSONOBJ_EQ(chunkMax, mergedChunk.getMax());

    {
        // Check for increment on mergedChunk's minor version
        ASSERT_EQ(origVersion.majorVersion(), mergedChunk.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 1, mergedChunk.getVersion().minorVersion());
    }

    // Make sure history is there
    ASSERT_EQ(1UL, mergedChunk.getHistory().size());
    ASSERT_EQ(validAfter, mergedChunk.getHistory().front().getValidAfter());
}

TEST_F(MergeChunkTest, NewMergeShouldClaimHighestVersion) {
    const auto collEpoch = OID::gen();
    const Timestamp collTimestamp(42);
    const auto collUuid = UUID::gen();
    ChunkType chunk, otherChunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(collUuid);
    otherChunk.setName(OID::gen());
    otherChunk.setCollectionUUID(collUuid);

    auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 2});
    chunk.setVersion(origVersion);
    chunk.setShard(_shardId);

    // Construct chunk to be merged
    auto chunk2(chunk);
    chunk2.setName(OID::gen());

    auto chunkMin = BSON("a" << 1);
    auto chunkBound = BSON("a" << 5);
    auto chunkMax = BSON("a" << 10);
    // first chunk boundaries
    chunk.setMin(chunkMin);
    chunk.setMax(chunkBound);
    // second chunk boundaries
    chunk2.setMin(chunkBound);
    chunk2.setMax(chunkMax);

    // Record chunk boundaries for passing into commitChunksMerge
    ChunkRange rangeToBeMerged(chunk.getMin(), chunk2.getMax());

    // Set up other chunk with competing version
    auto competingVersion = ChunkVersion({collEpoch, collTimestamp}, {2, 1});
    otherChunk.setVersion(competingVersion);
    otherChunk.setShard(_shardId);
    otherChunk.setMin(BSON("a" << 10));
    otherChunk.setMax(BSON("a" << 20));

    setupCollection(_nss1, _keyPattern, {chunk, chunk2, otherChunk});

    Timestamp validAfter{100, 0};

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunksMerge(operationContext(),
                                      _nss1,
                                      collEpoch,
                                      collTimestamp,
                                      collUuid,
                                      rangeToBeMerged,
                                      _shardId,
                                      validAfter));

    const auto query = BSON(ChunkType::collectionUUID() << collUuid);
    auto findResponse = uassertStatusOK(
        getConfigShard()->exhaustiveFindOnConfig(operationContext(),
                                                 ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 ChunkType::ConfigNS,
                                                 query,
                                                 BSON(ChunkType::lastmod << -1),
                                                 boost::none));

    const auto& chunksVector = findResponse.docs;

    // There should be exactly two chunks left in the collection: one merged, one competing
    ASSERT_EQ(2u, chunksVector.size());

    // MergedChunk should have range [chunkMin, chunkMax]
    auto mergedChunk = uassertStatusOK(
        ChunkType::parseFromConfigBSON(chunksVector.front(), collEpoch, collTimestamp));
    ASSERT_BSONOBJ_EQ(chunkMin, mergedChunk.getMin());
    ASSERT_BSONOBJ_EQ(chunkMax, mergedChunk.getMax());

    {
        // Check for minor increment on collection version
        ASSERT_EQ(competingVersion.majorVersion(), mergedChunk.getVersion().majorVersion());
        ASSERT_EQ(competingVersion.minorVersion() + 1, mergedChunk.getVersion().minorVersion());
    }

    // Make sure history is there
    ASSERT_EQ(1UL, mergedChunk.getHistory().size());
    ASSERT_EQ(validAfter, mergedChunk.getHistory().front().getValidAfter());
}

TEST_F(MergeChunkTest, MergeLeavesOtherChunksAlone) {
    const auto collEpoch = OID::gen();
    const Timestamp collTimestamp(42);
    const auto collUuid = UUID::gen();
    ShardId shardId(_shardName);
    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(collUuid);

    auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 2});
    chunk.setVersion(origVersion);
    chunk.setShard(shardId);

    // Construct chunk to be merged
    auto chunk2(chunk);
    chunk2.setName(OID::gen());

    auto chunkMin = BSON("a" << 1);
    auto chunkBound = BSON("a" << 5);
    auto chunkMax = BSON("a" << 10);
    // first chunk boundaries
    chunk.setMin(chunkMin);
    chunk.setMax(chunkBound);
    // second chunk boundaries
    chunk2.setMin(chunkBound);
    chunk2.setMax(chunkMax);

    // Record chunk boundaries for passing into commitChunksMerge
    ChunkRange rangeToBeMerged(chunk.getMin(), chunk2.getMax());


    // Set up unmerged chunk
    auto otherChunk(chunk);
    otherChunk.setName(OID::gen());
    otherChunk.setMin(BSON("a" << 10));
    otherChunk.setMax(BSON("a" << 20));

    setupCollection(_nss1, _keyPattern, {chunk, chunk2, otherChunk});

    Timestamp validAfter{1};
    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunksMerge(operationContext(),
                                      _nss1,
                                      collEpoch,
                                      collTimestamp,
                                      collUuid,
                                      rangeToBeMerged,
                                      shardId,
                                      validAfter));
    const auto query = BSON(ChunkType::collectionUUID() << collUuid);
    auto findResponse = uassertStatusOK(
        getConfigShard()->exhaustiveFindOnConfig(operationContext(),
                                                 ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 ChunkType::ConfigNS,
                                                 query,
                                                 BSON(ChunkType::lastmod << -1),
                                                 boost::none));

    const auto& chunksVector = findResponse.docs;

    // There should be exactly two chunks left in the collection: one merged, one untouched
    ASSERT_EQ(2u, chunksVector.size());

    // MergedChunk should have range [chunkMin, chunkMax]
    auto mergedChunk = uassertStatusOK(
        ChunkType::parseFromConfigBSON(chunksVector.front(), collEpoch, collTimestamp));
    ASSERT_BSONOBJ_EQ(chunkMin, mergedChunk.getMin());
    ASSERT_BSONOBJ_EQ(chunkMax, mergedChunk.getMax());

    {
        // Check for increment on mergedChunk's minor version
        ASSERT_EQ(origVersion.majorVersion(), mergedChunk.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 1, mergedChunk.getVersion().minorVersion());
    }

    // OtherChunk should have been left alone
    auto foundOtherChunk = uassertStatusOK(
        ChunkType::parseFromConfigBSON(chunksVector.back(), collEpoch, collTimestamp));
    ASSERT_BSONOBJ_EQ(otherChunk.getMin(), foundOtherChunk.getMin());
    ASSERT_BSONOBJ_EQ(otherChunk.getMax(), foundOtherChunk.getMax());
}

TEST_F(MergeChunkTest, NonExistingNamespace) {
    const auto collEpoch = OID::gen();
    const Timestamp collTimestamp(42);
    const auto collUuidAtRequest = UUID::gen();
    ChunkType chunk;
    chunk.setCollectionUUID(UUID::gen());

    auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
    chunk.setVersion(origVersion);

    // Construct chunk to be merged
    auto chunk2(chunk);

    auto chunkMin = BSON("a" << 1);
    auto chunkBound = BSON("a" << 5);
    auto chunkMax = BSON("a" << 10);
    // first chunk boundaries
    chunk.setMin(chunkMin);
    chunk.setMax(chunkBound);
    chunk2.setMin(chunkBound);
    chunk2.setMax(chunkMax);

    // Record chunk boundaries for passing into commitChunksMerge
    ChunkRange rangeToBeMerged(chunk.getMin(), chunk2.getMax());

    setupCollection(_nss1, _keyPattern, {chunk, chunk2});

    Timestamp validAfter{1};

    ASSERT_THROWS(ShardingCatalogManager::get(operationContext())
                      ->commitChunksMerge(operationContext(),
                                          NamespaceString("TestDB.NonExistingColl"),
                                          collEpoch,
                                          collTimestamp,
                                          collUuidAtRequest,
                                          rangeToBeMerged,
                                          _shardId,
                                          validAfter),
                  DBException);
}

TEST_F(MergeChunkTest, NonMatchingUUIDsOfChunkAndRequestErrors) {
    const auto collEpoch = OID::gen();
    const Timestamp collTimestamp(42);
    const auto collUuid = UUID::gen();
    const auto requestUuid = UUID::gen();
    ChunkType chunk;
    chunk.setCollectionUUID(collUuid);

    auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
    chunk.setVersion(origVersion);
    chunk.setShard(_shardId);

    // Construct chunk to be merged
    auto chunk2(chunk);

    auto chunkMin = BSON("a" << 1);
    auto chunkBound = BSON("a" << 5);
    auto chunkMax = BSON("a" << 10);
    // first chunk boundaries
    chunk.setMin(chunkMin);
    chunk.setMax(chunkBound);
    chunk2.setMin(chunkBound);
    chunk2.setMax(chunkMax);

    // Record chunk baoundaries for passing into commitChunksMerge
    ChunkRange rangeToBeMerged(chunk.getMin(), chunk2.getMax());

    setupCollection(_nss1, _keyPattern, {chunk, chunk2});

    Timestamp validAfter{1};

    auto mergeStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunksMerge(operationContext(),
                                               _nss1,
                                               collEpoch,
                                               collTimestamp,
                                               requestUuid,
                                               rangeToBeMerged,
                                               _shardId,
                                               validAfter);
    ASSERT_EQ(ErrorCodes::InvalidUUID, mergeStatus);
}

TEST_F(MergeChunkTest, MergeAlreadyHappenedSucceeds) {
    const auto collEpoch = OID::gen();
    const Timestamp collTimestamp(42);
    const auto collUuid = UUID::gen();

    // Construct chunk range to be merged
    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    ChunkRange rangeToBeMerged(chunkMin, chunkMax);

    // Store a chunk that matches the range that will be requested
    auto mergedVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
    mergedVersion.incMinor();
    ChunkType mergedChunk;
    mergedChunk.setVersion(mergedVersion);
    mergedChunk.setMin(chunkMin);
    mergedChunk.setMax(chunkMax);
    mergedChunk.setName(OID::gen());
    mergedChunk.setCollectionUUID(collUuid);
    mergedChunk.setShard(_shardId);


    setupCollection(_nss1, _keyPattern, {mergedChunk});

    Timestamp validAfter{1};

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunksMerge(operationContext(),
                                      _nss1,
                                      collEpoch,
                                      collTimestamp,
                                      collUuid,
                                      rangeToBeMerged,
                                      _shardId,
                                      validAfter));

    // Verify that no change to config.chunks happened.
    const auto query = BSON(ChunkType::collectionUUID() << collUuid);
    auto findResponse = uassertStatusOK(
        getConfigShard()->exhaustiveFindOnConfig(operationContext(),
                                                 ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 ChunkType::ConfigNS,
                                                 query,
                                                 BSON(ChunkType::lastmod << -1),
                                                 boost::none));

    const auto& chunksVector = findResponse.docs;

    // There should be exactly one chunk left in the collection
    ASSERT_EQ(1u, chunksVector.size());

    // MergedChunk should have range [chunkMin, chunkMax]
    ChunkType foundChunk = uassertStatusOK(
        ChunkType::parseFromConfigBSON(chunksVector.front(), collEpoch, collTimestamp));
    ASSERT_BSONOBJ_EQ(mergedChunk.toConfigBSON(), foundChunk.toConfigBSON());
}

TEST_F(MergeChunkTest, MergingChunksWithDollarPrefixShouldSucceed) {
    const auto collEpoch = OID::gen();
    const Timestamp collTimestamp(42);
    const auto collUuid = UUID::gen();
    ChunkType chunk1;
    chunk1.setName(OID::gen());
    chunk1.setCollectionUUID(collUuid);


    auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
    chunk1.setVersion(origVersion);
    chunk1.setShard(_shardId);

    auto chunk2(chunk1);
    auto chunk3(chunk1);
    chunk2.setName(OID::gen());
    chunk3.setName(OID::gen());

    auto chunkMin = BSON("a" << kMinBSONKey);
    auto chunkBound1 = BSON("a" << BSON("$maxKey" << 1));
    auto chunkBound2 = BSON("a" << BSON("$mixKey" << 1));
    auto chunkMax = BSON("a" << kMaxBSONKey);

    // first chunk boundaries
    chunk1.setMin(chunkMin);
    chunk1.setMax(chunkBound1);
    // second chunk boundaries
    chunk2.setMin(chunkBound1);
    chunk2.setMax(chunkBound2);
    // third chunk boundaries
    chunk3.setMin(chunkBound2);
    chunk3.setMax(chunkMax);

    setupCollection(_nss1, _keyPattern, {chunk1, chunk2, chunk3});

    // Record chunk boundaries for passing into commitChunksMerge
    ChunkRange rangeToBeMerged(chunk1.getMin(), chunk3.getMax());
    Timestamp validAfter{100, 0};

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunksMerge(operationContext(),
                                      _nss1,
                                      collEpoch,
                                      collTimestamp,
                                      collUuid,
                                      rangeToBeMerged,
                                      _shardId,
                                      validAfter));

    const auto query = BSON(ChunkType::collectionUUID() << collUuid);
    auto findResponse = uassertStatusOK(
        getConfigShard()->exhaustiveFindOnConfig(operationContext(),
                                                 ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 ChunkType::ConfigNS,
                                                 query,
                                                 BSON(ChunkType::lastmod << -1),
                                                 boost::none));

    const auto& chunksVector = findResponse.docs;

    // There should be exactly one chunk left in the collection
    ASSERT_EQ(1u, chunksVector.size());

    // MergedChunk should have range [chunkMin, chunkMax]
    auto mergedChunk = uassertStatusOK(
        ChunkType::parseFromConfigBSON(chunksVector.front(), collEpoch, collTimestamp));
    ASSERT_BSONOBJ_EQ(chunkMin, mergedChunk.getMin());
    ASSERT_BSONOBJ_EQ(chunkMax, mergedChunk.getMax());

    {
        // Check for increment on mergedChunk's minor version
        ASSERT_EQ(origVersion.majorVersion(), mergedChunk.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 1, mergedChunk.getVersion().minorVersion());
    }

    // Make sure history is there
    ASSERT_EQ(1UL, mergedChunk.getHistory().size());
    ASSERT_EQ(validAfter, mergedChunk.getHistory().front().getValidAfter());
}

}  // namespace
}  // namespace mongo
