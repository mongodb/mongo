/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"

namespace mongo {
namespace {

class EnsureChunkVersionIsGreaterThanTest : public ConfigServerTestFixture {
protected:
    std::string _shardName = "shard0000";
    void setUp() override {
        ConfigServerTestFixture::setUp();
        ShardType shard;
        shard.setName(_shardName);
        shard.setHost(_shardName + ":12");
        setupShards({shard});
    }
    const NamespaceString _nss{"TestDB", "TestColl"};
    const UUID _collUuid = UUID::gen();
    const KeyPattern _keyPattern{BSON("x" << 1)};
};

ChunkType generateChunkType(const NamespaceString& nss,
                            const UUID& collUuid,
                            const ChunkVersion& chunkVersion,
                            const ShardId& shardId,
                            const BSONObj& minKey,
                            const BSONObj& maxKey) {
    ChunkType chunkType;
    chunkType.setName(OID::gen());
    chunkType.setCollectionUUID(collUuid);
    chunkType.setVersion(chunkVersion);
    chunkType.setShard(shardId);
    chunkType.setMin(minKey);
    chunkType.setMax(maxKey);
    chunkType.setHistory({ChunkHistory(Timestamp(100, 0), shardId)});
    return chunkType;
}

void assertChunkHasNotChanged(const ChunkType& chunkTypeBefore,
                              const StatusWith<ChunkType> swChunkTypeAfter) {
    ASSERT_OK(swChunkTypeAfter.getStatus());
    auto chunkTypeAfter = swChunkTypeAfter.getValue();
    ASSERT_BSONOBJ_EQ(chunkTypeBefore.toConfigBSON(), chunkTypeAfter.toConfigBSON());
}

void assertChunkVersionWasBumpedTo(const ChunkType& chunkTypeBefore,
                                   const StatusWith<ChunkType> swChunkTypeAfter,
                                   const ChunkVersion& newChunkVersion) {
    ASSERT_OK(swChunkTypeAfter.getStatus());
    auto chunkTypeAfter = swChunkTypeAfter.getValue();

    // The new chunk should have the new ChunkVersion.
    ASSERT_EQ(newChunkVersion, chunkTypeAfter.getVersion());

    // None of the chunk's other fields should have been changed.
    ASSERT_EQ(chunkTypeBefore.getName(), chunkTypeAfter.getName());
    ASSERT_EQ(chunkTypeBefore.getCollectionUUID(), chunkTypeAfter.getCollectionUUID());
    ASSERT_BSONOBJ_EQ(chunkTypeBefore.getMin(), chunkTypeAfter.getMin());
    ASSERT_BSONOBJ_EQ(chunkTypeBefore.getMax(), chunkTypeAfter.getMax());
    ASSERT(chunkTypeBefore.getHistory() == chunkTypeAfter.getHistory());
}

TEST_F(EnsureChunkVersionIsGreaterThanTest, IfNoCollectionFoundReturnsSuccess) {
    const auto requestedChunkType =
        generateChunkType(_nss,
                          _collUuid,
                          ChunkVersion(10, 2, OID::gen(), Timestamp(1, 1)),
                          ShardId(_shardName),
                          BSON("a" << 1),
                          BSON("a" << 10));

    ShardingCatalogManager::get(operationContext())
        ->ensureChunkVersionIsGreaterThan(operationContext(),
                                          _collUuid,
                                          requestedChunkType.getMin(),
                                          requestedChunkType.getMax(),
                                          requestedChunkType.getVersion());
}

TEST_F(EnsureChunkVersionIsGreaterThanTest, IfNoChunkWithMatchingMinKeyFoundReturnsSuccess) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    const auto requestedChunkType = generateChunkType(_nss,
                                                      _collUuid,
                                                      ChunkVersion(10, 2, collEpoch, collTimestamp),
                                                      ShardId(_shardName),
                                                      BSON("a" << 1),
                                                      BSON("a" << 10));

    ChunkType existingChunkType = requestedChunkType;
    // Min key is different.
    existingChunkType.setMin(BSON("a" << -1));
    setupCollection(_nss, _keyPattern, {existingChunkType});

    ShardingCatalogManager::get(operationContext())
        ->ensureChunkVersionIsGreaterThan(operationContext(),
                                          _collUuid,
                                          requestedChunkType.getMin(),
                                          requestedChunkType.getMax(),
                                          requestedChunkType.getVersion());

    assertChunkHasNotChanged(
        existingChunkType,
        getChunkDoc(operationContext(), existingChunkType.getMin(), collEpoch, collTimestamp));
}

TEST_F(EnsureChunkVersionIsGreaterThanTest, IfNoChunkWithMatchingMaxKeyFoundReturnsSuccess) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    const auto requestedChunkType = generateChunkType(_nss,
                                                      _collUuid,
                                                      ChunkVersion(10, 2, collEpoch, collTimestamp),
                                                      ShardId(_shardName),
                                                      BSON("a" << 1),
                                                      BSON("a" << 10));

    ChunkType existingChunkType = requestedChunkType;
    // Max key is different.
    existingChunkType.setMax(BSON("a" << 20));
    setupCollection(_nss, _keyPattern, {existingChunkType});

    ShardingCatalogManager::get(operationContext())
        ->ensureChunkVersionIsGreaterThan(operationContext(),
                                          _collUuid,
                                          requestedChunkType.getMin(),
                                          requestedChunkType.getMax(),
                                          requestedChunkType.getVersion());

    assertChunkHasNotChanged(
        existingChunkType,
        getChunkDoc(operationContext(), existingChunkType.getMin(), collEpoch, collTimestamp));
}

TEST_F(EnsureChunkVersionIsGreaterThanTest,
       IfChunkMatchingRequestedChunkFoundBumpsChunkVersionAndReturnsSuccess) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    const auto requestedChunkType = generateChunkType(_nss,
                                                      _collUuid,
                                                      ChunkVersion(10, 2, collEpoch, collTimestamp),
                                                      ShardId(_shardName),
                                                      BSON("a" << 1),
                                                      BSON("a" << 10));

    const auto existingChunkType = requestedChunkType;
    const auto highestChunkType = generateChunkType(_nss,
                                                    _collUuid,
                                                    ChunkVersion(20, 3, collEpoch, collTimestamp),
                                                    ShardId("shard0001"),
                                                    BSON("a" << 11),
                                                    BSON("a" << 20));
    setupCollection(_nss, _keyPattern, {existingChunkType, highestChunkType});

    ShardingCatalogManager::get(operationContext())
        ->ensureChunkVersionIsGreaterThan(operationContext(),
                                          _collUuid,
                                          requestedChunkType.getMin(),
                                          requestedChunkType.getMax(),
                                          requestedChunkType.getVersion());

    assertChunkVersionWasBumpedTo(
        existingChunkType,
        getChunkDoc(operationContext(), existingChunkType.getMin(), collEpoch, collTimestamp),
        ChunkVersion(
            highestChunkType.getVersion().majorVersion() + 1, 0, collEpoch, collTimestamp));
}

TEST_F(EnsureChunkVersionIsGreaterThanTest,
       IfChunkMatchingRequestedChunkFoundBumpsChunkVersionAndReturnsSuccessNew) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    const auto requestedChunkType = generateChunkType(_nss,
                                                      _collUuid,
                                                      ChunkVersion(10, 2, collEpoch, collTimestamp),
                                                      ShardId(_shardName),
                                                      BSON("a" << 1),
                                                      BSON("a" << 10));

    const auto existingChunkType = requestedChunkType;
    const auto highestChunkType = generateChunkType(_nss,
                                                    _collUuid,
                                                    ChunkVersion(20, 3, collEpoch, collTimestamp),
                                                    ShardId("shard0001"),
                                                    BSON("a" << 11),
                                                    BSON("a" << 20));
    setupCollection(_nss, _keyPattern, {existingChunkType, highestChunkType});

    ShardingCatalogManager::get(operationContext())
        ->ensureChunkVersionIsGreaterThan(operationContext(),
                                          _collUuid,
                                          requestedChunkType.getMin(),
                                          requestedChunkType.getMax(),
                                          requestedChunkType.getVersion());

    assertChunkVersionWasBumpedTo(
        existingChunkType,
        getChunkDoc(operationContext(), existingChunkType.getMin(), collEpoch, collTimestamp),
        ChunkVersion(
            highestChunkType.getVersion().majorVersion() + 1, 0, collEpoch, collTimestamp));
}

TEST_F(
    EnsureChunkVersionIsGreaterThanTest,
    IfChunkMatchingRequestedChunkFoundAndHasHigherChunkVersionReturnsSuccessWithoutBumpingChunkVersion) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    const auto requestedChunkType = generateChunkType(_nss,
                                                      _collUuid,
                                                      ChunkVersion(10, 2, collEpoch, collTimestamp),
                                                      ShardId(_shardName),
                                                      BSON("a" << 1),
                                                      BSON("a" << 10));

    ChunkType existingChunkType = requestedChunkType;
    existingChunkType.setVersion(ChunkVersion(11, 1, collEpoch, collTimestamp));
    setupCollection(_nss, _keyPattern, {existingChunkType});

    ShardingCatalogManager::get(operationContext())
        ->ensureChunkVersionIsGreaterThan(operationContext(),
                                          _collUuid,
                                          requestedChunkType.getMin(),
                                          requestedChunkType.getMax(),
                                          requestedChunkType.getVersion());

    assertChunkHasNotChanged(
        existingChunkType,
        getChunkDoc(operationContext(), existingChunkType.getMin(), collEpoch, collTimestamp));
}

TEST_F(
    EnsureChunkVersionIsGreaterThanTest,
    IfChunkMatchingRequestedChunkFoundAndHasHigherChunkVersionReturnsSuccessWithoutBumpingChunkVersionNew) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);

    const auto requestedChunkType = generateChunkType(_nss,
                                                      _collUuid,
                                                      ChunkVersion(10, 2, collEpoch, collTimestamp),
                                                      ShardId(_shardName),
                                                      BSON("a" << 1),
                                                      BSON("a" << 10));

    ChunkType existingChunkType = requestedChunkType;
    existingChunkType.setVersion(ChunkVersion(11, 1, collEpoch, collTimestamp));
    setupCollection(_nss, _keyPattern, {existingChunkType});

    ShardingCatalogManager::get(operationContext())
        ->ensureChunkVersionIsGreaterThan(operationContext(),
                                          _collUuid,
                                          requestedChunkType.getMin(),
                                          requestedChunkType.getMax(),
                                          requestedChunkType.getVersion());

    assertChunkHasNotChanged(
        existingChunkType,
        getChunkDoc(operationContext(), existingChunkType.getMin(), collEpoch, collTimestamp));
}

}  // namespace
}  // namespace mongo
