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
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/s/catalog/type_chunk.h"

namespace mongo {
namespace {
using unittest::assertGet;

class SplitChunkTest : public ConfigServerTestFixture {
protected:
    std::string _shardName = "shard0000";
    void setUp() override {
        ConfigServerTestFixture::setUp();
        ShardType shard;
        shard.setName(_shardName);
        shard.setHost(_shardName + ":12");
        setupShards({shard});
    }


    const NamespaceString _nss1{"TestDB", "TestColl1"};
    const NamespaceString _nss2{"TestDB", "TestColl2"};
    const KeyPattern _keyPattern{BSON("a" << 1)};
};

TEST_F(SplitChunkTest, SplitExistingChunkCorrectlyShouldSucceed) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setNS(nss);
        chunk.setCollectionUUID(collUuid);

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);
        chunk.setHistory({ChunkHistory(Timestamp(100, 0), ShardId(_shardName)),
                          ChunkHistory(Timestamp(90, 0), ShardId("shardY"))});

        auto chunkSplitPoint = BSON("a" << 5);
        std::vector<BSONObj> splitPoints{chunkSplitPoint};

        setupCollection(nss, _keyPattern, {chunk});

        auto versions = assertGet(ShardingCatalogManager::get(operationContext())
                                      ->commitChunkSplit(operationContext(),
                                                         nss,
                                                         collEpoch,
                                                         ChunkRange(chunkMin, chunkMax),
                                                         splitPoints,
                                                         "shard0000"));
        auto collVersion = assertGet(ChunkVersion::parseWithField(versions, "collectionVersion"));
        auto shardVersion = assertGet(ChunkVersion::parseWithField(versions, "shardVersion"));

        ASSERT_TRUE(origVersion.isOlderThan(shardVersion));
        ASSERT_EQ(collVersion, shardVersion);

        // Check for increment on mergedChunk's minor version
        auto expectedShardVersion = ChunkVersion(
            origVersion.majorVersion(), origVersion.minorVersion() + 2, collEpoch, collTimestamp);
        ASSERT_EQ(expectedShardVersion, shardVersion);
        ASSERT_EQ(shardVersion, collVersion);

        const auto nssOrUuid =
            collTimestamp ? NamespaceStringOrUUID(nss.db().toString(), collUuid) : nss;

        // First chunkDoc should have range [chunkMin, chunkSplitPoint]
        auto chunkDocStatus =
            getChunkDoc(operationContext(), nssOrUuid, chunkMin, collEpoch, collTimestamp);
        ASSERT_OK(chunkDocStatus.getStatus());

        auto chunkDoc = chunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

        // Check for increment on first chunkDoc's minor version
        ASSERT_EQ(origVersion.majorVersion(), chunkDoc.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 1, chunkDoc.getVersion().minorVersion());

        // Make sure the history is there
        ASSERT_EQ(2UL, chunkDoc.getHistory().size());

        // Second chunkDoc should have range [chunkSplitPoint, chunkMax]
        auto otherChunkDocStatus =
            getChunkDoc(operationContext(), nssOrUuid, chunkSplitPoint, collEpoch, collTimestamp);
        ASSERT_OK(otherChunkDocStatus.getStatus());

        auto otherChunkDoc = otherChunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkMax, otherChunkDoc.getMax());

        // Check for increment on second chunkDoc's minor version
        ASSERT_EQ(origVersion.majorVersion(), otherChunkDoc.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 2, otherChunkDoc.getVersion().minorVersion());

        // Make sure the history is there
        ASSERT_EQ(2UL, otherChunkDoc.getHistory().size());

        // Both chunks should have the same history
        ASSERT(chunkDoc.getHistory() == otherChunkDoc.getHistory());
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, MultipleSplitsOnExistingChunkShouldSucceed) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setNS(nss);
        chunk.setCollectionUUID(collUuid);

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);
        chunk.setHistory({ChunkHistory(Timestamp(100, 0), ShardId(_shardName)),
                          ChunkHistory(Timestamp(90, 0), ShardId("shardY"))});

        auto chunkSplitPoint = BSON("a" << 5);
        auto chunkSplitPoint2 = BSON("a" << 7);
        std::vector<BSONObj> splitPoints{chunkSplitPoint, chunkSplitPoint2};

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_OK(ShardingCatalogManager::get(operationContext())
                      ->commitChunkSplit(operationContext(),
                                         nss,
                                         collEpoch,
                                         ChunkRange(chunkMin, chunkMax),
                                         splitPoints,
                                         "shard0000"));

        const auto nssOrUuid =
            collTimestamp ? NamespaceStringOrUUID(nss.db().toString(), collUuid) : nss;

        // First chunkDoc should have range [chunkMin, chunkSplitPoint]
        auto chunkDocStatus =
            getChunkDoc(operationContext(), nssOrUuid, chunkMin, collEpoch, collTimestamp);
        ASSERT_OK(chunkDocStatus.getStatus());

        auto chunkDoc = chunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

        // Check for increment on first chunkDoc's minor version
        ASSERT_EQ(origVersion.majorVersion(), chunkDoc.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 1, chunkDoc.getVersion().minorVersion());

        // Make sure the history is there
        ASSERT_EQ(2UL, chunkDoc.getHistory().size());

        // Second chunkDoc should have range [chunkSplitPoint, chunkSplitPoint2]
        auto midChunkDocStatus =
            getChunkDoc(operationContext(), nssOrUuid, chunkSplitPoint, collEpoch, collTimestamp);
        ASSERT_OK(midChunkDocStatus.getStatus());

        auto midChunkDoc = midChunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkSplitPoint2, midChunkDoc.getMax());

        // Check for increment on second chunkDoc's minor version
        ASSERT_EQ(origVersion.majorVersion(), midChunkDoc.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 2, midChunkDoc.getVersion().minorVersion());

        // Make sure the history is there
        ASSERT_EQ(2UL, midChunkDoc.getHistory().size());

        // Third chunkDoc should have range [chunkSplitPoint2, chunkMax]
        auto lastChunkDocStatus =
            getChunkDoc(operationContext(), nssOrUuid, chunkSplitPoint2, collEpoch, collTimestamp);
        ASSERT_OK(lastChunkDocStatus.getStatus());

        auto lastChunkDoc = lastChunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkMax, lastChunkDoc.getMax());

        // Check for increment on third chunkDoc's minor version
        ASSERT_EQ(origVersion.majorVersion(), lastChunkDoc.getVersion().majorVersion());
        ASSERT_EQ(origVersion.minorVersion() + 3, lastChunkDoc.getVersion().minorVersion());

        // Make sure the history is there
        ASSERT_EQ(2UL, lastChunkDoc.getHistory().size());

        // Both chunks should have the same history
        ASSERT(chunkDoc.getHistory() == midChunkDoc.getHistory());
        ASSERT(midChunkDoc.getHistory() == lastChunkDoc.getHistory());
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, NewSplitShouldClaimHighestVersion) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();

        ChunkType chunk, chunk2;
        chunk.setName(OID::gen());
        chunk.setNS(nss);
        chunk.setCollectionUUID(collUuid);
        chunk2.setName(OID::gen());
        chunk2.setNS(nss);
        chunk2.setCollectionUUID(collUuid);

        // set up first chunk
        auto origVersion = ChunkVersion(1, 2, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints;
        auto chunkSplitPoint = BSON("a" << 5);
        splitPoints.push_back(chunkSplitPoint);

        // set up second chunk (chunk2)
        auto competingVersion = ChunkVersion(2, 1, collEpoch, collTimestamp);
        chunk2.setVersion(competingVersion);
        chunk2.setShard(ShardId(_shardName));
        chunk2.setMin(BSON("a" << 10));
        chunk2.setMax(BSON("a" << 20));

        setupCollection(nss, _keyPattern, {chunk, chunk2});

        ASSERT_OK(ShardingCatalogManager::get(operationContext())
                      ->commitChunkSplit(operationContext(),
                                         nss,
                                         collEpoch,
                                         ChunkRange(chunkMin, chunkMax),
                                         splitPoints,
                                         "shard0000"));

        const auto nssOrUuid =
            collTimestamp ? NamespaceStringOrUUID(nss.db().toString(), collUuid) : nss;

        // First chunkDoc should have range [chunkMin, chunkSplitPoint]
        auto chunkDocStatus =
            getChunkDoc(operationContext(), nssOrUuid, chunkMin, collEpoch, collTimestamp);
        ASSERT_OK(chunkDocStatus.getStatus());

        auto chunkDoc = chunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

        // Check for increment based on the competing chunk version
        ASSERT_EQ(competingVersion.majorVersion(), chunkDoc.getVersion().majorVersion());
        ASSERT_EQ(competingVersion.minorVersion() + 1, chunkDoc.getVersion().minorVersion());

        // Second chunkDoc should have range [chunkSplitPoint, chunkMax]
        auto otherChunkDocStatus =
            getChunkDoc(operationContext(), nssOrUuid, chunkSplitPoint, collEpoch, collTimestamp);
        ASSERT_OK(otherChunkDocStatus.getStatus());

        auto otherChunkDoc = otherChunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkMax, otherChunkDoc.getMax());

        // Check for increment based on the competing chunk version
        ASSERT_EQ(competingVersion.majorVersion(), otherChunkDoc.getVersion().majorVersion());
        ASSERT_EQ(competingVersion.minorVersion() + 2, otherChunkDoc.getVersion().minorVersion());
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, PreConditionFailErrors) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setNS(nss);
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints;
        auto chunkSplitPoint = BSON("a" << 5);
        splitPoints.push_back(chunkSplitPoint);

        setupCollection(nss, _keyPattern, {chunk});

        auto splitStatus = ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  ChunkRange(chunkMin, BSON("a" << 7)),
                                                  splitPoints,
                                                  "shard0000");
        ASSERT_EQ(ErrorCodes::BadValue, splitStatus);
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, NonExisingNamespaceErrors) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setNS(nss);
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints{BSON("a" << 5)};

        setupCollection(nss, _keyPattern, {chunk});

        auto splitStatus = ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  NamespaceString("TestDB.NonExistingColl"),
                                                  collEpoch,
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000");
        ASSERT_NOT_OK(splitStatus);
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, NonMatchingEpochsOfChunkAndRequestErrors) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setNS(nss);
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints{BSON("a" << 5)};

        setupCollection(nss, _keyPattern, {chunk});

        auto splitStatus = ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  OID::gen(),
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000");
        ASSERT_EQ(ErrorCodes::StaleEpoch, splitStatus);
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsOutOfOrderShouldFail) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setNS(nss);
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints{BSON("a" << 5), BSON("a" << 4)};

        setupCollection(nss, _keyPattern, {chunk});

        auto splitStatus = ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000");
        ASSERT_EQ(ErrorCodes::InvalidOptions, splitStatus);
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsOutOfRangeAtMinShouldFail) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setNS(nss);
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints{BSON("a" << 0), BSON("a" << 5)};

        setupCollection(nss, _keyPattern, {chunk});

        auto splitStatus = ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000");
        ASSERT_EQ(ErrorCodes::InvalidOptions, splitStatus);
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsOutOfRangeAtMaxShouldFail) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setNS(nss);
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);

        std::vector<BSONObj> splitPoints{BSON("a" << 5), BSON("a" << 15)};

        setupCollection(nss, _keyPattern, {chunk});

        auto splitStatus = ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000");
        ASSERT_EQ(ErrorCodes::InvalidOptions, splitStatus);
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsWithDollarPrefixShouldFail) {
    auto test = [&](const NamespaceString& nss, const boost::optional<Timestamp>& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setNS(nss);
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion(1, 0, collEpoch, collTimestamp);
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << kMinBSONKey);
        auto chunkMax = BSON("a" << kMaxBSONKey);
        chunk.setMin(chunkMin);
        chunk.setMax(chunkMax);
        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_NOT_OK(ShardingCatalogManager::get(operationContext())
                          ->commitChunkSplit(operationContext(),
                                             nss,
                                             collEpoch,
                                             ChunkRange(chunkMin, chunkMax),
                                             {BSON("a" << BSON("$minKey" << 1))},
                                             "shard0000"));
        ASSERT_NOT_OK(ShardingCatalogManager::get(operationContext())
                          ->commitChunkSplit(operationContext(),
                                             nss,
                                             collEpoch,
                                             ChunkRange(chunkMin, chunkMax),
                                             {BSON("a" << BSON("$maxKey" << 1))},
                                             "shard0000"));
    };

    test(_nss1, boost::none /* timestamp */);
    test(_nss2, Timestamp(42));
}

}  // namespace
}  // namespace mongo
