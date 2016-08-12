/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/client/read_preference.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/config_server_test_fixture.h"

namespace mongo {
namespace {

using MergeChunkTest = ConfigServerTestFixture;

TEST_F(MergeChunkTest, MergeExistingChunksCorrectlyShouldSucceed) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    // Construct chunk to be merged
    auto chunk2(chunk);

    auto chunkMin = BSON("a" << 1);
    auto chunkBound = BSON("a" << 5);
    auto chunkMax = BSON("a" << 10);
    // first chunk boundaries
    chunk.setMin(chunkMin);
    chunk.setMax(chunkBound);
    // second chunk boundaries
    chunk2.setMin(chunkBound);
    chunk2.setMax(chunkMax);

    std::vector<BSONObj> chunkBoundaries{chunkMin, chunkBound, chunkMax};

    setupChunks({chunk, chunk2});

    ASSERT_OK(catalogManager()->commitChunkMerge(operationContext(),
                                                 NamespaceString("TestDB.TestColl"),
                                                 origVersion.epoch(),
                                                 chunkBoundaries,
                                                 "shard0000"));

    auto findResponse = uassertStatusOK(
        getConfigShard()->exhaustiveFindOnConfig(operationContext(),
                                                 ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 NamespaceString(ChunkType::ConfigNS),
                                                 BSON(ChunkType::ns() << "TestDB.TestColl"),
                                                 BSON(ChunkType::DEPRECATED_lastmod << -1),
                                                 boost::none));

    const auto& chunksVector = findResponse.docs;

    // There should be exactly one chunk left in the collection
    ASSERT_EQ(1u, chunksVector.size());

    // MergedChunk should have range [chunkMin, chunkMax]
    auto mergedChunk = uassertStatusOK(ChunkType::fromBSON(chunksVector.front()));
    ASSERT_BSONOBJ_EQ(chunkMin, mergedChunk.getMin());
    ASSERT_BSONOBJ_EQ(chunkMax, mergedChunk.getMax());

    {
        // Check for increment on mergedChunk's minor version
        ASSERT_EQ(origVersion.majorVersion(), mergedChunk.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 1, mergedChunk.getVersion().minorVersion());
    }
}

TEST_F(MergeChunkTest, MergeSeveralChunksCorrectlyShouldSucceed) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    // Construct chunks to be merged
    auto chunk2(chunk);
    auto chunk3(chunk);

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

    // Record chunk boundaries for passing into commitChunkMerge
    std::vector<BSONObj> chunkBoundaries{chunkMin, chunkBound, chunkBound2, chunkMax};

    setupChunks({chunk, chunk2, chunk3});

    ASSERT_OK(catalogManager()->commitChunkMerge(operationContext(),
                                                 NamespaceString("TestDB.TestColl"),
                                                 origVersion.epoch(),
                                                 chunkBoundaries,
                                                 "shard0000"));

    auto findResponse = uassertStatusOK(
        getConfigShard()->exhaustiveFindOnConfig(operationContext(),
                                                 ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 NamespaceString(ChunkType::ConfigNS),
                                                 BSON(ChunkType::ns() << "TestDB.TestColl"),
                                                 BSON(ChunkType::DEPRECATED_lastmod << -1),
                                                 boost::none));

    const auto& chunksVector = findResponse.docs;

    // There should be exactly one chunk left in the collection
    ASSERT_EQ(1u, chunksVector.size());

    // MergedChunk should have range [chunkMin, chunkMax]
    auto mergedChunk = uassertStatusOK(ChunkType::fromBSON(chunksVector.front()));
    ASSERT_BSONOBJ_EQ(chunkMin, mergedChunk.getMin());
    ASSERT_BSONOBJ_EQ(chunkMax, mergedChunk.getMax());

    {
        // Check for increment on mergedChunk's minor version
        ASSERT_EQ(origVersion.majorVersion(), mergedChunk.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 1, mergedChunk.getVersion().minorVersion());
    }
}

TEST_F(MergeChunkTest, NewMergeShouldClaimHighestVersion) {
    ChunkType chunk, otherChunk;
    chunk.setNS("TestDB.TestColl");
    otherChunk.setNS("TestDB.TestColl");
    auto collEpoch = OID::gen();

    auto origVersion = ChunkVersion(1, 2, collEpoch);
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    // Construct chunk to be merged
    auto chunk2(chunk);

    auto chunkMin = BSON("a" << 1);
    auto chunkBound = BSON("a" << 5);
    auto chunkMax = BSON("a" << 10);
    // first chunk boundaries
    chunk.setMin(chunkMin);
    chunk.setMax(chunkBound);
    // second chunk boundaries
    chunk2.setMin(chunkBound);
    chunk2.setMax(chunkMax);

    // Record chunk boundaries for passing into commitChunkMerge
    std::vector<BSONObj> chunkBoundaries{chunkMin, chunkBound, chunkMax};

    // Set up other chunk with competing version
    auto competingVersion = ChunkVersion(2, 1, collEpoch);
    otherChunk.setVersion(competingVersion);
    otherChunk.setShard(ShardId("shard0000"));
    otherChunk.setMin(BSON("a" << 10));
    otherChunk.setMax(BSON("a" << 20));

    setupChunks({chunk, chunk2, otherChunk});

    ASSERT_OK(catalogManager()->commitChunkMerge(operationContext(),
                                                 NamespaceString("TestDB.TestColl"),
                                                 collEpoch,
                                                 chunkBoundaries,
                                                 "shard0000"));

    auto findResponse = uassertStatusOK(
        getConfigShard()->exhaustiveFindOnConfig(operationContext(),
                                                 ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 NamespaceString(ChunkType::ConfigNS),
                                                 BSON(ChunkType::ns() << "TestDB.TestColl"),
                                                 BSON(ChunkType::DEPRECATED_lastmod << -1),
                                                 boost::none));

    const auto& chunksVector = findResponse.docs;

    // There should be exactly two chunks left in the collection: one merged, one competing
    ASSERT_EQ(2u, chunksVector.size());

    // MergedChunk should have range [chunkMin, chunkMax]
    auto mergedChunk = uassertStatusOK(ChunkType::fromBSON(chunksVector.front()));
    ASSERT_BSONOBJ_EQ(chunkMin, mergedChunk.getMin());
    ASSERT_BSONOBJ_EQ(chunkMax, mergedChunk.getMax());

    {
        // Check for minor increment on collection version
        ASSERT_EQ(competingVersion.majorVersion(), mergedChunk.getVersion().majorVersion());
        ASSERT_EQ(competingVersion.minorVersion() + 1, mergedChunk.getVersion().minorVersion());
    }
}

TEST_F(MergeChunkTest, MergeLeavesOtherChunksAlone) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 2, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    // Construct chunk to be merged
    auto chunk2(chunk);

    auto chunkMin = BSON("a" << 1);
    auto chunkBound = BSON("a" << 5);
    auto chunkMax = BSON("a" << 10);
    // first chunk boundaries
    chunk.setMin(chunkMin);
    chunk.setMax(chunkBound);
    // second chunk boundaries
    chunk2.setMin(chunkBound);
    chunk2.setMax(chunkMax);

    // Record chunk boundaries for passing into commitChunkMerge
    std::vector<BSONObj> chunkBoundaries{chunkMin, chunkBound, chunkMax};

    // Set up unmerged chunk
    auto otherChunk(chunk);
    otherChunk.setMin(BSON("a" << 10));
    otherChunk.setMax(BSON("a" << 20));

    setupChunks({chunk, chunk2, otherChunk});

    ASSERT_OK(catalogManager()->commitChunkMerge(operationContext(),
                                                 NamespaceString("TestDB.TestColl"),
                                                 origVersion.epoch(),
                                                 chunkBoundaries,
                                                 "shard0000"));

    auto findResponse = uassertStatusOK(
        getConfigShard()->exhaustiveFindOnConfig(operationContext(),
                                                 ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                                 repl::ReadConcernLevel::kLocalReadConcern,
                                                 NamespaceString(ChunkType::ConfigNS),
                                                 BSON(ChunkType::ns() << "TestDB.TestColl"),
                                                 BSON(ChunkType::DEPRECATED_lastmod << -1),
                                                 boost::none));

    const auto& chunksVector = findResponse.docs;

    // There should be exactly two chunks left in the collection: one merged, one untouched
    ASSERT_EQ(2u, chunksVector.size());

    // MergedChunk should have range [chunkMin, chunkMax]
    auto mergedChunk = uassertStatusOK(ChunkType::fromBSON(chunksVector.front()));
    ASSERT_BSONOBJ_EQ(chunkMin, mergedChunk.getMin());
    ASSERT_BSONOBJ_EQ(chunkMax, mergedChunk.getMax());

    {
        // Check for increment on mergedChunk's minor version
        ASSERT_EQ(origVersion.majorVersion(), mergedChunk.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 1, mergedChunk.getVersion().minorVersion());
    }

    // OtherChunk should have been left alone
    auto foundOtherChunk = uassertStatusOK(ChunkType::fromBSON(chunksVector.back()));
    ASSERT_BSONOBJ_EQ(otherChunk.getMin(), foundOtherChunk.getMin());
    ASSERT_BSONOBJ_EQ(otherChunk.getMax(), foundOtherChunk.getMax());
}

TEST_F(MergeChunkTest, NonExistingNamespace) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

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

    // Record chunk boundaries for passing into commitChunkMerge
    std::vector<BSONObj> chunkBoundaries{chunkMin, chunkBound, chunkMax};

    setupChunks({chunk, chunk2});

    auto mergeStatus = catalogManager()->commitChunkMerge(operationContext(),
                                                          NamespaceString("TestDB.NonExistingColl"),
                                                          origVersion.epoch(),
                                                          chunkBoundaries,
                                                          "shard0000");
    ASSERT_EQ(ErrorCodes::IllegalOperation, mergeStatus);
}

TEST_F(MergeChunkTest, NonMatchingEpochsOfChunkAndRequestErrors) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

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

    // Record chunk baoundaries for passing into commitChunkMerge
    std::vector<BSONObj> chunkBoundaries{chunkMin, chunkBound, chunkMax};

    setupChunks({chunk, chunk2});

    auto mergeStatus = catalogManager()->commitChunkMerge(operationContext(),
                                                          NamespaceString("TestDB.TestColl"),
                                                          OID::gen(),
                                                          chunkBoundaries,
                                                          "shard0000");
    ASSERT_EQ(ErrorCodes::StaleEpoch, mergeStatus);
}

}  // namespace
}  // namespace mongo
