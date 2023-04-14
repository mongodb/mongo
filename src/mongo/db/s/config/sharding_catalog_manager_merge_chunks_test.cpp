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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/client/read_preference.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_changelog.h"
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
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
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
    const NamespaceString _nss1 =
        NamespaceString::createNamespaceString_forTest("TestDB.TestColl1");
    const NamespaceString _nss2 =
        NamespaceString::createNamespaceString_forTest("TestDB.TestColl2");
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

    // set histories
    chunk.setOnCurrentShardSince(Timestamp{100, 0});
    chunk2.setOnCurrentShardSince(Timestamp{200, 0});
    chunk.setHistory({ChunkHistory{*chunk.getOnCurrentShardSince(), _shardId}});
    chunk2.setHistory({ChunkHistory{*chunk2.getOnCurrentShardSince(), _shardId}});

    // set boundaries
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

    ChunkRange rangeToBeMerged(chunk.getMin(), chunk2.getMax());

    auto versions = assertGet(ShardingCatalogManager::get(operationContext())
                                  ->commitChunksMerge(operationContext(),
                                                      _nss1,
                                                      collEpoch,
                                                      collTimestamp,
                                                      collUuid,
                                                      rangeToBeMerged,
                                                      _shardId));

    auto collPlacementVersion = versions.collectionPlacementVersion;
    auto shardPlacementVersion = versions.shardPlacementVersion;

    ASSERT_TRUE(origVersion.isOlderThan(versions.shardPlacementVersion));
    ASSERT_EQ(shardPlacementVersion, collPlacementVersion);

    // Check for increment on mergedChunk's minor version
    auto expectedShardPlacementVersion =
        ChunkVersion({origVersion.epoch(), origVersion.getTimestamp()},
                     {origVersion.majorVersion(), origVersion.minorVersion() + 1});
    ASSERT_EQ(expectedShardPlacementVersion, shardPlacementVersion);


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
        chunksVector.front(), collPlacementVersion.epoch(), collPlacementVersion.getTimestamp()));
    ASSERT_BSONOBJ_EQ(chunkMin, mergedChunk.getMin());
    ASSERT_BSONOBJ_EQ(chunkMax, mergedChunk.getMax());

    // Check that the shard placement version returned by the merge matches the CSRS one
    ASSERT_EQ(shardPlacementVersion, mergedChunk.getVersion());

    // Make sure history is there
    ASSERT_EQ(1UL, mergedChunk.getHistory().size());
    ASSERT_EQ(chunk2.getHistory().front().getValidAfter(),
              mergedChunk.getHistory().front().getValidAfter());
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

    // set histories
    chunk.setOnCurrentShardSince(Timestamp{100, 10});
    chunk2.setOnCurrentShardSince(Timestamp{200, 1});
    chunk3.setOnCurrentShardSince(Timestamp{50, 0});
    chunk.setHistory({ChunkHistory{*chunk.getOnCurrentShardSince(), _shardId}});
    chunk2.setHistory({ChunkHistory{*chunk2.getOnCurrentShardSince(), _shardId}});
    chunk3.setHistory({ChunkHistory{*chunk3.getOnCurrentShardSince(), _shardId}});

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

    uassertStatusOK(ShardingCatalogManager::get(operationContext())
                        ->commitChunksMerge(operationContext(),
                                            _nss1,
                                            collEpoch,
                                            collTimestamp,
                                            collUuid,
                                            rangeToBeMerged,
                                            _shardId));

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
    ASSERT_EQ(chunk2.getHistory().front().getValidAfter(),
              mergedChunk.getHistory().front().getValidAfter());
    ASSERT_EQ(chunk2.getOnCurrentShardSince(), mergedChunk.getOnCurrentShardSince());
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

    // set histories
    chunk.setOnCurrentShardSince(Timestamp{100, 0});
    chunk2.setOnCurrentShardSince(Timestamp{200, 0});
    chunk.setHistory({ChunkHistory{*chunk.getOnCurrentShardSince(), _shardId}});
    chunk2.setHistory({ChunkHistory{*chunk2.getOnCurrentShardSince(), _shardId}});

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

    uassertStatusOK(ShardingCatalogManager::get(operationContext())
                        ->commitChunksMerge(operationContext(),
                                            _nss1,
                                            collEpoch,
                                            collTimestamp,
                                            collUuid,
                                            rangeToBeMerged,
                                            _shardId));

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
        // Check for minor increment on collection placement version
        ASSERT_EQ(competingVersion.majorVersion(), mergedChunk.getVersion().majorVersion());
        ASSERT_EQ(competingVersion.minorVersion() + 1, mergedChunk.getVersion().minorVersion());
    }

    // Make sure history is there
    ASSERT_EQ(1UL, mergedChunk.getHistory().size());
    ASSERT_EQ(chunk2.getHistory().front().getValidAfter(),
              mergedChunk.getHistory().front().getValidAfter());
    ASSERT_EQ(chunk2.getOnCurrentShardSince(), mergedChunk.getOnCurrentShardSince());
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

    // set histories
    chunk.setOnCurrentShardSince(Timestamp{100, 5});
    chunk2.setOnCurrentShardSince(Timestamp{200, 1});
    chunk.setHistory({ChunkHistory{*chunk.getOnCurrentShardSince(), shardId}});
    chunk2.setHistory({ChunkHistory{*chunk2.getOnCurrentShardSince(), shardId}});

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

    uassertStatusOK(ShardingCatalogManager::get(operationContext())
                        ->commitChunksMerge(operationContext(),
                                            _nss1,
                                            collEpoch,
                                            collTimestamp,
                                            collUuid,
                                            rangeToBeMerged,
                                            shardId));
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
    chunk.setShard(_shardId);
    chunk.setVersion(origVersion);

    // Construct chunk to be merged
    auto chunk2(chunk);

    // set history
    chunk.setOnCurrentShardSince(Timestamp{100, 0});
    chunk2.setOnCurrentShardSince(Timestamp{200, 0});
    chunk.setHistory({ChunkHistory{*chunk.getOnCurrentShardSince(), _shardId}});
    chunk2.setHistory({ChunkHistory{*chunk2.getOnCurrentShardSince(), _shardId}});

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

    ASSERT_THROWS(ShardingCatalogManager::get(operationContext())
                      ->commitChunksMerge(
                          operationContext(),
                          NamespaceString::createNamespaceString_forTest("TestDB.NonExistingColl"),
                          collEpoch,
                          collTimestamp,
                          collUuidAtRequest,
                          rangeToBeMerged,
                          _shardId),
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

    // set histories
    chunk.setOnCurrentShardSince(Timestamp{100, 0});
    chunk2.setOnCurrentShardSince(Timestamp{200, 0});
    chunk.setHistory({ChunkHistory{*chunk.getOnCurrentShardSince(), _shardId}});
    chunk2.setHistory({ChunkHistory{*chunk2.getOnCurrentShardSince(), _shardId}});

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

    auto mergeStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunksMerge(operationContext(),
                                               _nss1,
                                               collEpoch,
                                               collTimestamp,
                                               requestUuid,
                                               rangeToBeMerged,
                                               _shardId);
    ASSERT_EQ(ErrorCodes::InvalidUUID, mergeStatus.getStatus());
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
    mergedChunk.setOnCurrentShardSince(Timestamp{100, 0});
    mergedChunk.setHistory({ChunkHistory{*mergedChunk.getOnCurrentShardSince(), _shardId}});


    setupCollection(_nss1, _keyPattern, {mergedChunk});

    uassertStatusOK(ShardingCatalogManager::get(operationContext())
                        ->commitChunksMerge(operationContext(),
                                            _nss1,
                                            collEpoch,
                                            collTimestamp,
                                            collUuid,
                                            rangeToBeMerged,
                                            _shardId));

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

    // set histories
    chunk1.setOnCurrentShardSince(Timestamp{100, 9});
    chunk2.setOnCurrentShardSince(Timestamp{200, 5});
    chunk3.setOnCurrentShardSince(Timestamp{156, 1});
    chunk1.setHistory({ChunkHistory{*chunk1.getOnCurrentShardSince(), _shardId}});
    chunk2.setHistory({ChunkHistory{*chunk2.getOnCurrentShardSince(), _shardId}});
    chunk3.setHistory({ChunkHistory{*chunk3.getOnCurrentShardSince(), _shardId}});

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

    uassertStatusOK(ShardingCatalogManager::get(operationContext())
                        ->commitChunksMerge(operationContext(),
                                            _nss1,
                                            collEpoch,
                                            collTimestamp,
                                            collUuid,
                                            rangeToBeMerged,
                                            _shardId));

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
    ASSERT_EQ(chunk2.getHistory().front().getValidAfter(),
              mergedChunk.getHistory().front().getValidAfter());
    ASSERT_EQ(chunk2.getOnCurrentShardSince(), mergedChunk.getOnCurrentShardSince());
}

class MergeAllChunksOnShardTest : public ConfigServerTestFixture {
protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();
        setupShards(_shards);

        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        ConfigServerTestFixture::tearDown();
    }

    /* Setup shareded collection randomly spreading chunks across shards */
    void setupCollectionWithRandomRoutingTable() {
        PseudoRandom random(SecureRandom().nextInt64());
        ChunkVersion collPlacementVersion{{_epoch, _ts}, {1, 0}};

        // Generate chunk with the provided parameters and increase current collection placement
        // version
        auto generateChunk = [&](ShardType& shard, BSONObj min, BSONObj max) -> ChunkType {
            ChunkType chunk;
            chunk.setCollectionUUID(_collUuid);
            chunk.setVersion(collPlacementVersion);
            collPlacementVersion.incMinor();
            chunk.setShard(shard.getName());
            chunk.setMin(min);
            chunk.setMax(max);

            // When `onCurrentShardSince` is set to "Timestamp(0, 1)", the chunk is mergeable
            // because the snapshot window passed. When it is set to "max", the chunk is not
            // mergeable because the snapshot window did not pass
            auto randomValidAfter = random.nextInt64() % 2 ? Timestamp(0, 1) : Timestamp::max();
            chunk.setOnCurrentShardSince(randomValidAfter);
            chunk.setHistory({ChunkHistory{randomValidAfter, shard.getName()}});

            // Rarely create a jumbo chunk (not mergeable)
            chunk.setJumbo(random.nextInt64() % 10 == 0);
            return chunk;
        };

        int numChunks = random.nextInt32(19) + 1;  // minimum 1 chunks, maximum 20 chunks
        std::vector<ChunkType> chunks;

        // Loop generating random routing table: [MinKey, 0), [1, 2), [2, 3), ... [x, MaxKey]
        int nextMin;
        for (int nextMax = 0; nextMax < numChunks; nextMax++) {
            auto randomShard = _shards.at(random.nextInt64() % _shards.size());
            // set min as `MinKey` during first iteration, otherwise next min
            auto min = nextMax == 0 ? _keyPattern.globalMin() : BSON("x" << nextMin);
            // set max as `MaxKey` during last iteration, otherwise next max
            auto max = nextMax == numChunks - 1 ? _keyPattern.globalMax() : BSON("x" << nextMax);
            auto chunk = generateChunk(randomShard, min, max);
            nextMin = nextMax;
            chunks.push_back(chunk);
        }

        setupCollection(_nss, _keyPattern, chunks);
    };

    /* Get routing table for the collection under testing */
    std::vector<ChunkType> getChunks() {
        const auto query = BSON(ChunkType::collectionUUID() << _collUuid);
        auto findResponse = uassertStatusOK(getConfigShard()->exhaustiveFindOnConfig(
            operationContext(),
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            repl::ReadConcernLevel::kLocalReadConcern,
            ChunkType::ConfigNS,
            query,
            BSON(ChunkType::min << 1),
            boost::none));

        std::vector<ChunkType> chunks;
        for (const auto& doc : findResponse.docs) {
            chunks.push_back(uassertStatusOK(ChunkType::parseFromConfigBSON(doc, _epoch, _ts)));
        }
        return chunks;
    }

    void assertConsistentRoutingTableWithNoContiguousMergeableChunksOnTheSameShard(
        std::vector<ChunkType> routingTable) {
        ASSERT_GTE(routingTable.size(), 0);

        ASSERT_EQ(routingTable.front().getMin().woCompare(_keyPattern.globalMin()), 0);

        for (size_t i = 1; i < routingTable.size(); i++) {
            const auto& prevChunk = routingTable.at(i - 1);
            const auto& currChunk = routingTable.at(i);
            ASSERT_EQ(prevChunk.getMax().woCompare(currChunk.getMin()), 0);

            // Chunks with the following carachteristics are not mergeable:
            // - Jumbo chunks
            // - Chunks with `onCurrentShardSince` higher than "now + snapshot window"
            // So it is excpected for them to potentially have a contiguous chunk on the same shard.
            if (!prevChunk.getJumbo() &&
                !(*(prevChunk.getOnCurrentShardSince()) == Timestamp::max()) &&
                !currChunk.getJumbo() &&
                !(*(currChunk.getOnCurrentShardSince()) == Timestamp::max())) {
                ASSERT_NOT_EQUALS(prevChunk.getShard().compare(currChunk.getShard()), 0);
            }
        }

        ASSERT(routingTable.back().getMax().woCompare(_keyPattern.globalMax()) == 0);
    }

    static void assertConsistentRoutingTableAfterMerges(
        const std::vector<ChunkType>& originalRoutingTable,
        const std::vector<ChunkType>& mergedRoutingTable) {
        ASSERT(originalRoutingTable.size() > 0);
        ASSERT(mergedRoutingTable.size() > 0);
        ASSERT(originalRoutingTable.size() >= mergedRoutingTable.size());

        size_t originalIndex = 0, mergedIndex = 0;
        auto currChunkOriginalRt = originalRoutingTable.at(originalIndex);
        auto currChunkMergedRt = mergedRoutingTable.at(mergedIndex);

        // Check that the merged routing table is compatible the original one
        while (currChunkMergedRt.getRange().covers(currChunkOriginalRt.getRange())) {
            ASSERT_EQ(currChunkOriginalRt.getShard(), currChunkMergedRt.getShard());
            originalIndex++;
            if (originalIndex == originalRoutingTable.size()) {
                break;
            }
            currChunkOriginalRt = originalRoutingTable.at(originalIndex);
            if (currChunkOriginalRt.getRange().getMax().woCompare(
                    currChunkMergedRt.getRange().getMax()) > 0) {
                mergedIndex++;
                currChunkMergedRt = mergedRoutingTable.at(mergedIndex);
            }
        }

        // Make sure all chunks have been checked (loop analyzed all chunks)
        ASSERT_EQ(originalIndex, originalRoutingTable.size());
        ASSERT_EQ(mergedIndex, mergedRoutingTable.size() - 1);
    }

    static void assertConsistentChunkVersionsAfterMerges(
        const std::vector<ChunkType>& originalRoutingTable,
        const std::vector<ChunkType>& mergedRoutingTable) {

        auto getMaxChunkVersion = [](const std::vector<ChunkType>& routingTable) -> ChunkVersion {
            auto maxCollPlacementVersion = routingTable.front().getVersion();
            for (const auto& chunk : routingTable) {
                if (maxCollPlacementVersion.isOlderThan(chunk.getVersion())) {
                    maxCollPlacementVersion = chunk.getVersion();
                }
            }
            return maxCollPlacementVersion;
        };

        const auto originalCollPlacementVersion = getMaxChunkVersion(originalRoutingTable);
        const auto mergedCollPlacementVersion = getMaxChunkVersion(mergedRoutingTable);

        // Calculate chunks that are in the merged routing table but aren't in the original one
        std::vector<ChunkType> chunksDiff = [&]() {
            std::vector<ChunkType> chunksDiff;
            std::set_difference(mergedRoutingTable.begin(),
                                mergedRoutingTable.end(),
                                originalRoutingTable.begin(),
                                originalRoutingTable.end(),
                                std::back_inserter(chunksDiff),
                                [](const ChunkType& l, const ChunkType& r) {
                                    return l.getRange().toBSON().woCompare(r.getRange().toBSON()) <
                                        0;
                                });
            return chunksDiff;
        }();

        const int nMerges = chunksDiff.size();

        // Merged max minor version = original max minor version + number of merges
        ASSERT_EQ(originalCollPlacementVersion.minorVersion() + nMerges,
                  mergedCollPlacementVersion.minorVersion());

        // Sort `chunksDiff` by minor version and check all intermediate versions are properly set
        std::sort(chunksDiff.begin(), chunksDiff.end(), [](ChunkType& l, ChunkType& r) {
            return l.getVersion().isOlderThan(r.getVersion());
        });

        int expectedMergedChunkMinorVersion = originalCollPlacementVersion.minorVersion() + 1;
        for (const auto& newChunk : chunksDiff) {
            ASSERT_EQ(newChunk.getVersion().getTimestamp(),
                      originalCollPlacementVersion.getTimestamp());
            ASSERT_EQ(newChunk.getVersion().majorVersion(),
                      originalCollPlacementVersion.majorVersion());
            ASSERT_EQ(newChunk.getVersion().minorVersion(), expectedMergedChunkMinorVersion++);
        }
    }

    void assertChangesWereLoggedAfterMerges(const NamespaceString& nss,
                                            const std::vector<ChunkType>& originalRoutingTable,
                                            const std::vector<ChunkType>& mergedRoutingTable) {

        const std::vector<ChunkType> chunksDiff = [&] {
            // Calculate chunks that are in the merged routing table but aren't in the original one
            std::vector<ChunkType> chunksDiff;
            std::set_difference(mergedRoutingTable.begin(),
                                mergedRoutingTable.end(),
                                originalRoutingTable.begin(),
                                originalRoutingTable.end(),
                                std::back_inserter(chunksDiff),
                                [](const ChunkType& l, const ChunkType& r) {
                                    return l.getRange().toBSON().woCompare(r.getRange().toBSON()) <
                                        0;
                                });
            return chunksDiff;
        }();

        size_t numMergedChunks = 0;
        size_t numMerges = 0;
        for (const auto& chunkDiff : chunksDiff) {
            BSONObjBuilder query;
            query << ChangeLogType::what("merge") << ChangeLogType::ns(nss.ns().toString());
            chunkDiff.getVersion().serialize("details.mergedVersion", &query);

            auto response = assertGet(getConfigShard()->exhaustiveFindOnConfig(
                operationContext(),
                ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                repl::ReadConcernLevel::kLocalReadConcern,
                ChangeLogType::ConfigNS,
                query.obj(),
                BSONObj(),
                1));

            ASSERT_EQ(1U, response.docs.size());
            auto logEntryBSON = response.docs.front();
            auto logEntry = assertGet(ChangeLogType::fromBSON(logEntryBSON));
            const auto& logEntryDetails = logEntry.getDetails();

            ASSERT_EQUALS(chunkDiff.getVersion(),
                          ChunkVersion::parse(logEntryDetails["mergedVersion"]));
            ASSERT_EQUALS(chunkDiff.getShard(), logEntryDetails["owningShard"].String());
            ASSERT_EQUALS(0,
                          chunkDiff.getRange().toBSON().woCompare(
                              ChunkRange(logEntryDetails["min"].Obj(), logEntryDetails["max"].Obj())
                                  .toBSON()));
            numMergedChunks += logEntryDetails["numChunks"].Int();
            numMerges++;
        }
        ASSERT_EQUALS(numMergedChunks,
                      originalRoutingTable.size() - mergedRoutingTable.size() + numMerges);
    }

    inline const static auto _shards =
        std::vector<ShardType>{ShardType{"shard0", "host0:123"}, ShardType{"shard1", "host1:123"}};

    const NamespaceString _nss = NamespaceString::createNamespaceString_forTest("test.coll");
    const UUID _collUuid = UUID::gen();

    const OID _epoch = OID::gen();
    const Timestamp _ts = Timestamp(42);

    const KeyPattern _keyPattern{BSON("x" << 1)};

    ReadWriteConcernDefaultsLookupMock _lookupMock;
};

/*
 * Generate random routing tables covering all potential unit test cases (over multiple executions):
 * - Only one chunk [no merge possible]
 * - Multiple chunks:
 * --- None contiguous on same shard(s) [no merge possible]
 * --- Some contiguous on same shard(s) [some merges possible]
 *
 * Then make sure that:
 * - The routing table before merges is compatible with the routing table after merges
 * - There are no contiguous chunks on the same shard(s)
 * - The minor versions on chunks have been increased accordingly to the number of merges
 */
TEST_F(MergeAllChunksOnShardTest, AllMergeableChunksGetSquashed) {
    setupCollectionWithRandomRoutingTable();

    const auto chunksBeforeMerges = getChunks();

    for (const auto& shard : _shards) {
        uassertStatusOK(
            ShardingCatalogManager::get(operationContext())
                ->commitMergeAllChunksOnShard(operationContext(), _nss, shard.getName()));
    }

    const auto chunksAfterMerges = getChunks();

    try {
        assertConsistentRoutingTableWithNoContiguousMergeableChunksOnTheSameShard(
            chunksAfterMerges);
        assertConsistentRoutingTableAfterMerges(chunksBeforeMerges, chunksAfterMerges);
        assertConsistentChunkVersionsAfterMerges(chunksBeforeMerges, chunksAfterMerges);
        assertChangesWereLoggedAfterMerges(_nss, chunksBeforeMerges, chunksAfterMerges);
    } catch (...) {
        // Log original and merged routing tables only in case of error
        LOGV2_INFO(7161200,
                   "CHUNKS BEFORE MERGE",
                   "numberOfChunks"_attr = chunksBeforeMerges.size(),
                   "chunks"_attr = chunksBeforeMerges);
        LOGV2_INFO(7161201,
                   "CHUNKS AFTER MERGE",
                   "numberOfChunks"_attr = chunksAfterMerges.size(),
                   "chunks"_attr = chunksAfterMerges);
        throw;
    }
}

}  // namespace
}  // namespace mongo
