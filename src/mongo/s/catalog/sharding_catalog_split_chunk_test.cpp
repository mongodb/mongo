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
#include "mongo/s/catalog/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/config_server_test_fixture.h"

namespace mongo {
namespace {

using SplitChunkTest = ConfigServerTestFixture;

TEST_F(SplitChunkTest, SplitExistingChunkCorrectlyShouldSucceed) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    auto chunkSplitPoint = BSON("a" << 5);
    std::vector<BSONObj> splitPoints{chunkSplitPoint};

    setupChunks({chunk}).transitional_ignore();

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunkSplit(operationContext(),
                                     NamespaceString("TestDB.TestColl"),
                                     origVersion.epoch(),
                                     ChunkRange(chunkMin, chunkMax),
                                     splitPoints,
                                     "shard0000"));

    // First chunkDoc should have range [chunkMin, chunkSplitPoint]
    auto chunkDocStatus = getChunkDoc(operationContext(), chunkMin);
    ASSERT_OK(chunkDocStatus.getStatus());

    auto chunkDoc = chunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

    // Check for increment on first chunkDoc's minor version
    ASSERT_EQ(origVersion.majorVersion(), chunkDoc.getVersion().majorVersion());
    ASSERT_EQ(origVersion.minorVersion() + 1, chunkDoc.getVersion().minorVersion());

    // Second chunkDoc should have range [chunkSplitPoint, chunkMax]
    auto otherChunkDocStatus = getChunkDoc(operationContext(), chunkSplitPoint);
    ASSERT_OK(otherChunkDocStatus.getStatus());

    auto otherChunkDoc = otherChunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkMax, otherChunkDoc.getMax());

    // Check for increment on second chunkDoc's minor version
    ASSERT_EQ(origVersion.majorVersion(), otherChunkDoc.getVersion().majorVersion());
    ASSERT_EQ(origVersion.minorVersion() + 2, otherChunkDoc.getVersion().minorVersion());
}

TEST_F(SplitChunkTest, MultipleSplitsOnExistingChunkShouldSucceed) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    auto chunkSplitPoint = BSON("a" << 5);
    auto chunkSplitPoint2 = BSON("a" << 7);
    std::vector<BSONObj> splitPoints{chunkSplitPoint, chunkSplitPoint2};

    setupChunks({chunk}).transitional_ignore();

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunkSplit(operationContext(),
                                     NamespaceString("TestDB.TestColl"),
                                     origVersion.epoch(),
                                     ChunkRange(chunkMin, chunkMax),
                                     splitPoints,
                                     "shard0000"));

    // First chunkDoc should have range [chunkMin, chunkSplitPoint]
    auto chunkDocStatus = getChunkDoc(operationContext(), chunkMin);
    ASSERT_OK(chunkDocStatus.getStatus());

    auto chunkDoc = chunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

    // Check for increment on first chunkDoc's minor version
    ASSERT_EQ(origVersion.majorVersion(), chunkDoc.getVersion().majorVersion());
    ASSERT_EQ(origVersion.minorVersion() + 1, chunkDoc.getVersion().minorVersion());

    // Second chunkDoc should have range [chunkSplitPoint, chunkSplitPoint2]
    auto midChunkDocStatus = getChunkDoc(operationContext(), chunkSplitPoint);
    ASSERT_OK(midChunkDocStatus.getStatus());

    auto midChunkDoc = midChunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkSplitPoint2, midChunkDoc.getMax());

    // Check for increment on second chunkDoc's minor version
    ASSERT_EQ(origVersion.majorVersion(), midChunkDoc.getVersion().majorVersion());
    ASSERT_EQ(origVersion.minorVersion() + 2, midChunkDoc.getVersion().minorVersion());

    // Third chunkDoc should have range [chunkSplitPoint2, chunkMax]
    auto lastChunkDocStatus = getChunkDoc(operationContext(), chunkSplitPoint2);
    ASSERT_OK(lastChunkDocStatus.getStatus());

    auto lastChunkDoc = lastChunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkMax, lastChunkDoc.getMax());

    // Check for increment on third chunkDoc's minor version
    ASSERT_EQ(origVersion.majorVersion(), lastChunkDoc.getVersion().majorVersion());
    ASSERT_EQ(origVersion.minorVersion() + 3, lastChunkDoc.getVersion().minorVersion());
}

TEST_F(SplitChunkTest, NewSplitShouldClaimHighestVersion) {
    ChunkType chunk, chunk2;
    chunk.setNS("TestDB.TestColl");
    chunk2.setNS("TestDB.TestColl");
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

    setupChunks({chunk, chunk2}).transitional_ignore();

    ASSERT_OK(ShardingCatalogManager::get(operationContext())
                  ->commitChunkSplit(operationContext(),
                                     NamespaceString("TestDB.TestColl"),
                                     collEpoch,
                                     ChunkRange(chunkMin, chunkMax),
                                     splitPoints,
                                     "shard0000"));

    // First chunkDoc should have range [chunkMin, chunkSplitPoint]
    auto chunkDocStatus = getChunkDoc(operationContext(), chunkMin);
    ASSERT_OK(chunkDocStatus.getStatus());

    auto chunkDoc = chunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

    // Check for increment based on the competing chunk version
    ASSERT_EQ(competingVersion.majorVersion(), chunkDoc.getVersion().majorVersion());
    ASSERT_EQ(competingVersion.minorVersion() + 1, chunkDoc.getVersion().minorVersion());

    // Second chunkDoc should have range [chunkSplitPoint, chunkMax]
    auto otherChunkDocStatus = getChunkDoc(operationContext(), chunkSplitPoint);
    ASSERT_OK(otherChunkDocStatus.getStatus());

    auto otherChunkDoc = otherChunkDocStatus.getValue();
    ASSERT_BSONOBJ_EQ(chunkMax, otherChunkDoc.getMax());

    // Check for increment based on the competing chunk version
    ASSERT_EQ(competingVersion.majorVersion(), otherChunkDoc.getVersion().majorVersion());
    ASSERT_EQ(competingVersion.minorVersion() + 2, otherChunkDoc.getVersion().minorVersion());
}

TEST_F(SplitChunkTest, PreConditionFailErrors) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

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

    setupChunks({chunk}).transitional_ignore();

    auto splitStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              NamespaceString("TestDB.TestColl"),
                                              origVersion.epoch(),
                                              ChunkRange(chunkMin, BSON("a" << 7)),
                                              splitPoints,
                                              "shard0000");
    ASSERT_EQ(ErrorCodes::BadValue, splitStatus);
}

TEST_F(SplitChunkTest, NonExisingNamespaceErrors) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints{BSON("a" << 5)};

    setupChunks({chunk}).transitional_ignore();

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
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints{BSON("a" << 5)};

    setupChunks({chunk}).transitional_ignore();

    auto splitStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              NamespaceString("TestDB.TestColl"),
                                              OID::gen(),
                                              ChunkRange(chunkMin, chunkMax),
                                              splitPoints,
                                              "shard0000");
    ASSERT_EQ(ErrorCodes::StaleEpoch, splitStatus);
}

TEST_F(SplitChunkTest, SplitPointsOutOfOrderShouldFail) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints{BSON("a" << 5), BSON("a" << 4)};

    setupChunks({chunk}).transitional_ignore();

    auto splitStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              NamespaceString("TestDB.TestColl"),
                                              origVersion.epoch(),
                                              ChunkRange(chunkMin, chunkMax),
                                              splitPoints,
                                              "shard0000");
    ASSERT_EQ(ErrorCodes::InvalidOptions, splitStatus);
}

TEST_F(SplitChunkTest, SplitPointsOutOfRangeAtMinShouldFail) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints{BSON("a" << 0), BSON("a" << 5)};

    setupChunks({chunk}).transitional_ignore();

    auto splitStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              NamespaceString("TestDB.TestColl"),
                                              origVersion.epoch(),
                                              ChunkRange(chunkMin, chunkMax),
                                              splitPoints,
                                              "shard0000");
    ASSERT_EQ(ErrorCodes::InvalidOptions, splitStatus);
}

TEST_F(SplitChunkTest, SplitPointsOutOfRangeAtMaxShouldFail) {
    ChunkType chunk;
    chunk.setNS("TestDB.TestColl");

    auto origVersion = ChunkVersion(1, 0, OID::gen());
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId("shard0000"));

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setMin(chunkMin);
    chunk.setMax(chunkMax);

    std::vector<BSONObj> splitPoints{BSON("a" << 5), BSON("a" << 15)};

    setupChunks({chunk}).transitional_ignore();

    auto splitStatus = ShardingCatalogManager::get(operationContext())
                           ->commitChunkSplit(operationContext(),
                                              NamespaceString("TestDB.TestColl"),
                                              origVersion.epoch(),
                                              ChunkRange(chunkMin, chunkMax),
                                              splitPoints,
                                              "shard0000");
    ASSERT_EQ(ErrorCodes::InvalidOptions, splitStatus);
}

}  // namespace
}  // namespace mongo
