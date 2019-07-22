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

#include "mongo/platform/basic.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/config_server_test_fixture.h"

namespace mongo {
namespace {

const NamespaceString kNamespace("TestDB", "TestColl");

using SplitChunkTest = ConfigServerTestFixture;

TEST_F(SplitChunkTest, SplitExistingChunkCorrectlyShouldSucceed) {
    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setNS(kNamespace);

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);
    chunk.setHistory({ChunkHistory(Timestamp(100, 0), ShardId("shard0000")),
                      ChunkHistory(Timestamp(90, 0), ShardId("shardY"))});

    auto chunkSplitPoint = BSON("a" << 5);
    std::vector<BSONObj> splitPoints{chunkSplitPoint};

    setupChunks({chunk});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunkSplit(operationContext(),
                                     kNamespace,
                                     origVersion.epoch(),
                                     ChunkRange(chunkMin, chunkMax),
                                     splitPoints,
                                     "shard0000"));

    // First chunkDoc should have range [chunkMin, chunkSplitPoint]
    auto chunkDocStatus = getChunkDoc(operationContext(), chunkMin);
    ASSERT_OK(chunkDocStatus.getStatus());

    auto chunkDoc = chunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

    // Check for increment on first chunkDoc's major version.
    ASSERT_EQ(origVersion.majorVersion() + 1, chunkDoc.getVersion().majorVersion());
    ASSERT_EQ(1u, chunkDoc.getVersion().minorVersion());

    // Make sure the history is there
    ASSERT_EQ(2UL, chunkDoc.getHistory().size());

    // Second chunkDoc should have range [chunkSplitPoint, chunkMax]
    auto otherChunkDocStatus = getChunkDoc(operationContext(), chunkSplitPoint);
    ASSERT_OK(otherChunkDocStatus.getStatus());

    auto otherChunkDoc = otherChunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkMax, otherChunkDoc.getMax());

    // Check for increment on second chunkDoc's minor version.
    ASSERT_EQ(origVersion.majorVersion() + 1, otherChunkDoc.getVersion().majorVersion());
    ASSERT_EQ(2u, otherChunkDoc.getVersion().minorVersion());

    // Make sure the history is there
    ASSERT_EQ(2UL, otherChunkDoc.getHistory().size());

    // Both chunks should have the same history
    ASSERT(chunkDoc.getHistory() == otherChunkDoc.getHistory());
}

TEST_F(SplitChunkTest, MultipleSplitsOnExistingChunkShouldSucceed) {
    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setNS(kNamespace);

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);
    chunk.setHistory({ChunkHistory(Timestamp(100, 0), ShardId("shard0000")),
                      ChunkHistory(Timestamp(90, 0), ShardId("shardY"))});

    auto chunkSplitPoint = BSON("a" << 5);
    auto chunkSplitPoint2 = BSON("a" << 7);
    std::vector<BSONObj> splitPoints{chunkSplitPoint, chunkSplitPoint2};

    setupChunks({chunk});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunkSplit(operationContext(),
                                     kNamespace,
                                     origVersion.epoch(),
                                     ChunkRange(chunkMin, chunkMax),
                                     splitPoints,
                                     "shard0000"));

    // First chunkDoc should have range [chunkMin, chunkSplitPoint]
    auto chunkDocStatus = getChunkDoc(operationContext(), chunkMin);
    ASSERT_OK(chunkDocStatus.getStatus());

    auto chunkDoc = chunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

    // Check for increment on first chunkDoc's major version.
    ASSERT_EQ(origVersion.majorVersion() + 1, chunkDoc.getVersion().majorVersion());
    ASSERT_EQ(1u, chunkDoc.getVersion().minorVersion());

    // Make sure the history is there
    ASSERT_EQ(2UL, chunkDoc.getHistory().size());

    // Second chunkDoc should have range [chunkSplitPoint, chunkSplitPoint2]
    auto midChunkDocStatus = getChunkDoc(operationContext(), chunkSplitPoint);
    ASSERT_OK(midChunkDocStatus.getStatus());

    auto midChunkDoc = midChunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkSplitPoint2, midChunkDoc.getMax());

    // Check for increment on second chunkDoc's minor version.
    ASSERT_EQ(origVersion.majorVersion() + 1, midChunkDoc.getVersion().majorVersion());
    ASSERT_EQ(2u, midChunkDoc.getVersion().minorVersion());

    // Make sure the history is there
    ASSERT_EQ(2UL, midChunkDoc.getHistory().size());

    // Third chunkDoc should have range [chunkSplitPoint2, chunkMax]
    auto lastChunkDocStatus = getChunkDoc(operationContext(), chunkSplitPoint2);
    ASSERT_OK(lastChunkDocStatus.getStatus());

    auto lastChunkDoc = lastChunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkMax, lastChunkDoc.getMax());

    // Check for increment on third chunkDoc's minor version.
    ASSERT_EQ(origVersion.majorVersion() + 1, lastChunkDoc.getVersion().majorVersion());
    ASSERT_EQ(3u, lastChunkDoc.getVersion().minorVersion());

    // Make sure the history is there
    ASSERT_EQ(2UL, lastChunkDoc.getHistory().size());

    // Both chunks should have the same history
    ASSERT(chunkDoc.getHistory() == midChunkDoc.getHistory());
    ASSERT(midChunkDoc.getHistory() == lastChunkDoc.getHistory());
}

TEST_F(SplitChunkTest, NewSplitShouldClaimHighestVersion) {
    ChunkType chunk, chunk2;
    chunk.setName(OID::gen());
    chunk.setNS(kNamespace);
    chunk2.setName(OID::gen());
    chunk2.setNS(kNamespace);
    auto collEpoch = OID::gen();

    // set up first chunk
    auto origVersion = ChunkVersion(1, 2, collEpoch);
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints;
    auto chunkSplitPoint = BSON("a" << 5);
    splitPoints.push_back(chunkSplitPoint);

    // set up second chunk (chunk2)
    auto competingVersion = ChunkVersion(2, 1, collEpoch);
    chunk2.setVersion(competingVersion);
    chunk2.setShard(ShardId("shard0000"));
    chunk2.setMin(BSON("a" << 10));
    chunk2.setMax(BSON("a" << 20));

    setupChunks({chunk, chunk2});

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunkSplit(operationContext(),
                                     kNamespace,
                                     collEpoch,
                                     ChunkRange(chunkMin, chunkMax),
                                     splitPoints,
                                     "shard0000"));

    // First chunkDoc should have range [chunkMin, chunkSplitPoint]
    auto chunkDocStatus = getChunkDoc(operationContext(), chunkMin);
    ASSERT_OK(chunkDocStatus.getStatus());

    auto chunkDoc = chunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

    // Check for major version increment based on the competing chunk version.
    ASSERT_EQ(competingVersion.majorVersion() + 1, chunkDoc.getVersion().majorVersion());
    // The minor version gets reset to 0 when the major version is incremented, and chunk splits
    // increment the minor version after incrementing the major version, so we expect the minor
    // version here to be 0 + 1 = 1.
    ASSERT_EQ(1u, chunkDoc.getVersion().minorVersion());

    // Second chunkDoc should have range [chunkSplitPoint, chunkMax]
    auto otherChunkDocStatus = getChunkDoc(operationContext(), chunkSplitPoint);
    ASSERT_OK(otherChunkDocStatus.getStatus());

    auto otherChunkDoc = otherChunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkMax, otherChunkDoc.getMax());

    // Check for increment based on the competing chunk version.
    ASSERT_EQ(competingVersion.majorVersion() + 1, otherChunkDoc.getVersion().majorVersion());
    // The minor version gets reset to 0 when the major version is incremented, and chunk splits
    // increment the minor version after incrementing the major version for the first chunk in the
    // split vector, so we expect the minor version here to be 0 + 1 + 1 = 2.
    ASSERT_EQ(2u, otherChunkDoc.getVersion().minorVersion());
}

TEST_F(SplitChunkTest, SplitsOnShardWithLowerShardVersionDoesNotIncreaseCollectionVersion) {
    ChunkType chunk, chunk2;
    chunk.setName(OID::gen());
    chunk.setNS(kNamespace);
    chunk2.setName(OID::gen());
    chunk2.setNS(kNamespace);
    auto collEpoch = OID::gen();

    // Set up first chunk with lower version on shard0001. Its shard will not have shard version ==
    // collection version, so splits to it should give it the collection version plus a minor
    // version bump.
    auto origVersion = ChunkVersion(1, 2, collEpoch);
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));
    chunk.setMin(BSON("a" << 1));
    chunk.setMax(BSON("a" << 10));

    // Set up second chunk (chunk2) on shard0001. This has the higher version.
    auto competingVersion = ChunkVersion(2, 1, collEpoch);
    chunk2.setVersion(competingVersion);
    chunk2.setShard(ShardId("shard0001"));
    chunk2.setMin(BSON("a" << 10));
    chunk2.setMax(BSON("a" << 20));

    setupChunks({chunk, chunk2});

    std::vector<BSONObj> splitPoints;
    auto chunkSplitPoint = BSON("a" << 5);
    splitPoints.push_back(chunkSplitPoint);

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunkSplit(operationContext(),
                                     kNamespace,
                                     collEpoch,
                                     ChunkRange(chunk.getMin(), chunk.getMax()),
                                     splitPoints,
                                     chunk.getShard().toString()));

    // First chunkDoc should have range [chunk.getMin(), chunkSplitPoint]
    auto chunkDocStatus = getChunkDoc(operationContext(), chunk.getMin());
    auto chunkDoc = chunkDocStatus.getValue();

    // Check for major version increment based on the competing chunk version.
    ASSERT_EQ(competingVersion.majorVersion(), chunkDoc.getVersion().majorVersion());
    ASSERT_EQ(competingVersion.minorVersion() + 1u, chunkDoc.getVersion().minorVersion());

    // Second chunkDoc should have range [chunkSplitPoint, chunkMax]
    auto otherChunkDocStatus = getChunkDoc(operationContext(), chunkSplitPoint);
    auto otherChunkDoc = otherChunkDocStatus.getValue();
    // Check for increment based on the competing chunk version.
    ASSERT_EQ(competingVersion.majorVersion(), otherChunkDoc.getVersion().majorVersion());
    ASSERT_EQ(competingVersion.minorVersion() + 2u, otherChunkDoc.getVersion().minorVersion());
}

TEST_F(SplitChunkTest, SplitsOnShardWithHighestShardVersionIncreasesCollectionVersion) {
    ChunkType chunk, chunk2;
    chunk.setName(OID::gen());
    chunk.setNS(kNamespace);
    chunk2.setName(OID::gen());
    chunk2.setNS(kNamespace);
    auto collEpoch = OID::gen();

    // Set up first chunk with lower version on shard0001. Its shard will not have shard version ==
    // collection version.
    auto origVersion = ChunkVersion(1, 2, collEpoch);
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));
    chunk.setMin(BSON("a" << 1));
    chunk.setMax(BSON("a" << 10));

    // Set up second chunk (chunk2) on shard0001. This has the higher version, so its shard version
    // == collection version. When we split it, its major version should increase.
    auto competingVersion = ChunkVersion(2, 1, collEpoch);
    chunk2.setVersion(competingVersion);
    chunk2.setShard(ShardId("shard0001"));
    chunk2.setMin(BSON("a" << 10));
    chunk2.setMax(BSON("a" << 20));

    setupChunks({chunk, chunk2});

    std::vector<BSONObj> splitPoints;
    // This will split the second chunk.
    auto chunkSplitPoint = BSON("a" << 15);
    splitPoints.push_back(chunkSplitPoint);

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunkSplit(operationContext(),
                                     kNamespace,
                                     collEpoch,
                                     ChunkRange(chunk2.getMin(), chunk2.getMax()),
                                     splitPoints,
                                     chunk2.getShard().toString()));

    // First chunkDoc should have range [chunk2.getMin(), chunkSplitPoint]
    auto chunkDocStatus = getChunkDoc(operationContext(), chunk2.getMin());
    auto chunkDoc = chunkDocStatus.getValue();

    // Check for major version increment based on the competing chunk version.
    ASSERT_EQ(competingVersion.majorVersion() + 1u, chunkDoc.getVersion().majorVersion());
    ASSERT_EQ(1u, chunkDoc.getVersion().minorVersion());

    // Second chunkDoc should have range [chunkSplitPoint, chunk2.getMax()]
    auto otherChunkDocStatus = getChunkDoc(operationContext(), chunkSplitPoint);
    auto otherChunkDoc = otherChunkDocStatus.getValue();
    // Check for increment based on the competing chunk version.
    ASSERT_EQ(competingVersion.majorVersion() + 1u, otherChunkDoc.getVersion().majorVersion());
    ASSERT_EQ(2u, otherChunkDoc.getVersion().minorVersion());
}

TEST_F(SplitChunkTest, PreConditionFailErrors) {
    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setNS(kNamespace);

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints;
    auto chunkSplitPoint = BSON("a" << 5);
    splitPoints.push_back(chunkSplitPoint);

    setupChunks({chunk});

    auto splitStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              kNamespace,
                                              origVersion.epoch(),
                                              ChunkRange(chunkMin, BSON("a" << 7)),
                                              splitPoints,
                                              "shard0000");
    ASSERT_EQ(ErrorCodes::BadValue, splitStatus);
}

TEST_F(SplitChunkTest, NonExisingNamespaceErrors) {
    ChunkType chunk;
    chunk.setNS(kNamespace);

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints{BSON("a" << 5)};

    setupChunks({chunk});

    auto splitStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              NamespaceString("TestDB.NonExistingColl"),
                                              origVersion.epoch(),
                                              ChunkRange(chunkMin, chunkMax),
                                              splitPoints,
                                              "shard0000");
    ASSERT_EQ(ErrorCodes::IllegalOperation, splitStatus);
}

TEST_F(SplitChunkTest, NonMatchingEpochsOfChunkAndRequestErrors) {
    ChunkType chunk;
    chunk.setNS(kNamespace);

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints{BSON("a" << 5)};

    setupChunks({chunk});

    auto splitStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              kNamespace,
                                              OID::gen(),
                                              ChunkRange(chunkMin, chunkMax),
                                              splitPoints,
                                              "shard0000");
    ASSERT_EQ(ErrorCodes::StaleEpoch, splitStatus);
}

TEST_F(SplitChunkTest, SplitPointsOutOfOrderShouldFail) {
    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setNS(kNamespace);

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints{BSON("a" << 5), BSON("a" << 4)};

    setupChunks({chunk});

    auto splitStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              kNamespace,
                                              origVersion.epoch(),
                                              ChunkRange(chunkMin, chunkMax),
                                              splitPoints,
                                              "shard0000");
    ASSERT_EQ(ErrorCodes::InvalidOptions, splitStatus);
}

TEST_F(SplitChunkTest, SplitPointsOutOfRangeAtMinShouldFail) {
    ChunkType chunk;
    chunk.setNS(kNamespace);

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints{BSON("a" << 0), BSON("a" << 5)};

    setupChunks({chunk});

    auto splitStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              kNamespace,
                                              origVersion.epoch(),
                                              ChunkRange(chunkMin, chunkMax),
                                              splitPoints,
                                              "shard0000");
    ASSERT_EQ(ErrorCodes::InvalidOptions, splitStatus);
}

TEST_F(SplitChunkTest, SplitPointsOutOfRangeAtMaxShouldFail) {
    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setNS(kNamespace);

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints{BSON("a" << 5), BSON("a" << 15)};

    setupChunks({chunk});

    auto splitStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              kNamespace,
                                              origVersion.epoch(),
                                              ChunkRange(chunkMin, chunkMax),
                                              splitPoints,
                                              "shard0000");
    ASSERT_EQ(ErrorCodes::InvalidOptions, splitStatus);
}

TEST_F(SplitChunkTest, SplitPointsWithDollarPrefixShouldFail) {
    ChunkType chunk;
    chunk.setNS(kNamespace);

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << kMinBSONKey);
    auto chunkMax = BSON("a" << kMaxBSONKey);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);
    setupChunks({chunk});

    ASSERT_NOT_OK(ShardingCatalogManager::get(operationContext())
                      ->commitChunkSplit(operationContext(),
                                         kNamespace,
                                         origVersion.epoch(),
                                         ChunkRange(chunkMin, chunkMax),
                                         {BSON("a" << BSON("$minKey" << 1))},
                                         "shard0000"));
    ASSERT_NOT_OK(ShardingCatalogManager::get(operationContext())
                      ->commitChunkSplit(operationContext(),
                                         kNamespace,
                                         origVersion.epoch(),
                                         ChunkRange(chunkMin, chunkMax),
                                         {BSON("a" << BSON("$maxKey" << 1))},
                                         "shard0000"));
}

}  // namespace
}  // namespace mongo
