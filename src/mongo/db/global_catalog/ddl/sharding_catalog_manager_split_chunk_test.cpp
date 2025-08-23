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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

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

        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->initializeIfNeeded(operationContext(), /* term */ 1);
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->interrupt();
        ConfigServerTestFixture::tearDown();
    }

    const NamespaceString _nss1 =
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl1");
    const NamespaceString _nss2 =
        NamespaceString::createNamespaceString_forTest("TestDB", "TestColl2");
    const KeyPattern _keyPattern{BSON("a" << 1)};
};

TEST_F(SplitChunkTest, SplitExistingChunkCorrectlyShouldSucceed) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collUuid);

        auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setRange({chunkMin, chunkMax});
        chunk.setOnCurrentShardSince(Timestamp(100, 0));
        chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), ShardId(_shardName)),
                          ChunkHistory(Timestamp(90, 0), ShardId("shardY"))});

        auto chunkSplitPoint = BSON("a" << 5);
        std::vector<BSONObj> splitPoints{chunkSplitPoint};

        setupCollection(nss, _keyPattern, {chunk});

        auto versions = assertGet(ShardingCatalogManager::get(operationContext())
                                      ->commitChunkSplit(operationContext(),
                                                         nss,
                                                         collEpoch,
                                                         collTimestamp,
                                                         ChunkRange(chunkMin, chunkMax),
                                                         splitPoints,
                                                         "shard0000"));
        auto collPlacementVersion = versions.collectionPlacementVersion;
        auto shardPlacementVersion = versions.shardPlacementVersion;

        ASSERT_EQ(std::partial_ordering::less, origVersion <=> shardPlacementVersion);
        ASSERT_EQ(collPlacementVersion, shardPlacementVersion);

        // Check for increment on mergedChunk's minor version
        auto expectedShardPlacementVersion =
            ChunkVersion({collEpoch, collTimestamp},
                         {origVersion.majorVersion(), origVersion.minorVersion() + 2});
        ASSERT_EQ(expectedShardPlacementVersion, shardPlacementVersion);
        ASSERT_EQ(shardPlacementVersion, collPlacementVersion);

        // First chunkDoc should have range [chunkMin, chunkSplitPoint]
        auto chunkDocStatus =
            getChunkDoc(operationContext(), collUuid, chunkMin, collEpoch, collTimestamp);
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
            getChunkDoc(operationContext(), collUuid, chunkSplitPoint, collEpoch, collTimestamp);
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
        ASSERT(chunkDoc.getOnCurrentShardSince().has_value());
        ASSERT_EQ(chunkDoc.getOnCurrentShardSince(), otherChunkDoc.getOnCurrentShardSince());
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, MultipleSplitsOnExistingChunkShouldSucceed) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collUuid);

        auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setRange({chunkMin, chunkMax});
        chunk.setOnCurrentShardSince(Timestamp(100, 0));
        chunk.setHistory({ChunkHistory(*chunk.getOnCurrentShardSince(), ShardId(_shardName)),
                          ChunkHistory(Timestamp(90, 0), ShardId("shardY"))});

        auto chunkSplitPoint = BSON("a" << 5);
        auto chunkSplitPoint2 = BSON("a" << 7);
        std::vector<BSONObj> splitPoints{chunkSplitPoint, chunkSplitPoint2};

        setupCollection(nss, _keyPattern, {chunk});

        uassertStatusOK(ShardingCatalogManager::get(operationContext())
                            ->commitChunkSplit(operationContext(),
                                               nss,
                                               collEpoch,
                                               collTimestamp,
                                               ChunkRange(chunkMin, chunkMax),
                                               splitPoints,
                                               "shard0000"));

        // First chunkDoc should have range [chunkMin, chunkSplitPoint]
        auto chunkDocStatus =
            getChunkDoc(operationContext(), collUuid, chunkMin, collEpoch, collTimestamp);
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
            getChunkDoc(operationContext(), collUuid, chunkSplitPoint, collEpoch, collTimestamp);
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
            getChunkDoc(operationContext(), collUuid, chunkSplitPoint2, collEpoch, collTimestamp);
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

        ASSERT(chunkDoc.getOnCurrentShardSince().has_value());
        ASSERT_EQ(chunkDoc.getOnCurrentShardSince(), midChunkDoc.getOnCurrentShardSince());
        ASSERT_EQ(midChunkDoc.getOnCurrentShardSince(), lastChunkDoc.getOnCurrentShardSince());
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, NewSplitShouldClaimHighestVersion) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();

        ChunkType chunk, chunk2;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collUuid);
        chunk2.setName(OID::gen());
        chunk2.setCollectionUUID(collUuid);

        // set up first chunk
        auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 2});
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setRange({chunkMin, chunkMax});

        std::vector<BSONObj> splitPoints;
        auto chunkSplitPoint = BSON("a" << 5);
        splitPoints.push_back(chunkSplitPoint);

        // set up second chunk (chunk2)
        auto competingVersion = ChunkVersion({collEpoch, collTimestamp}, {2, 1});
        chunk2.setVersion(competingVersion);
        chunk2.setShard(ShardId(_shardName));
        chunk2.setRange({BSON("a" << 10), BSON("a" << 20)});

        setupCollection(nss, _keyPattern, {chunk, chunk2});

        uassertStatusOK(ShardingCatalogManager::get(operationContext())
                            ->commitChunkSplit(operationContext(),
                                               nss,
                                               collEpoch,
                                               collTimestamp,
                                               ChunkRange(chunkMin, chunkMax),
                                               splitPoints,
                                               "shard0000"));

        // First chunkDoc should have range [chunkMin, chunkSplitPoint]
        auto chunkDocStatus =
            getChunkDoc(operationContext(), collUuid, chunkMin, collEpoch, collTimestamp);
        ASSERT_OK(chunkDocStatus.getStatus());

        auto chunkDoc = chunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkSplitPoint, chunkDoc.getMax());

        // Check for increment based on the competing chunk version
        ASSERT_EQ(competingVersion.majorVersion(), chunkDoc.getVersion().majorVersion());
        ASSERT_EQ(competingVersion.minorVersion() + 1, chunkDoc.getVersion().minorVersion());

        // Second chunkDoc should have range [chunkSplitPoint, chunkMax]
        auto otherChunkDocStatus =
            getChunkDoc(operationContext(), collUuid, chunkSplitPoint, collEpoch, collTimestamp);
        ASSERT_OK(otherChunkDocStatus.getStatus());

        auto otherChunkDoc = otherChunkDocStatus.getValue();
        ASSERT_BSONOBJ_EQ(chunkMax, otherChunkDoc.getMax());

        // Check for increment based on the competing chunk version
        ASSERT_EQ(competingVersion.majorVersion(), otherChunkDoc.getVersion().majorVersion());
        ASSERT_EQ(competingVersion.minorVersion() + 2, otherChunkDoc.getVersion().minorVersion());
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, Idempotency) {
    auto test = [&](const NamespaceString& nss,
                    const Timestamp& collTimestamp,
                    BSONObj chunkMin,
                    BSONObj chunkMax,
                    const std::vector<BSONObj>& splitPoints) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collUuid);

        auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        chunk.setRange({chunkMin, chunkMax});

        setupCollection(nss, _keyPattern, {chunk});

        const auto doSplit = [&]() {
            return ShardingCatalogManager::get(operationContext())
                ->commitChunkSplit(operationContext(),
                                   nss,
                                   collEpoch,
                                   collTimestamp,
                                   ChunkRange(chunkMin, chunkMax),
                                   splitPoints,
                                   "shard0000");
        };

        // Split.
        ASSERT_OK(doSplit());
        // Retry.
        ASSERT_OK(doSplit());

        const auto verifyChunk = [&](BSONObj min, BSONObj max) {
            auto chunkDocStatus =
                getChunkDoc(operationContext(), collUuid, min, collEpoch, collTimestamp);
            ASSERT_OK(chunkDocStatus.getStatus());

            auto chunkDoc = chunkDocStatus.getValue();
            ASSERT_BSONOBJ_EQ(max, chunkDoc.getMax());
        };

        // Sanity check.
        std::vector<BSONObj> expectedChunkBounds;
        expectedChunkBounds.push_back(chunkMin);
        expectedChunkBounds.insert(
            expectedChunkBounds.end(), splitPoints.begin(), splitPoints.end());
        expectedChunkBounds.push_back(chunkMax);

        for (auto minIt = expectedChunkBounds.begin(); minIt != expectedChunkBounds.end() - 1;
             ++minIt) {
            auto maxIt = minIt + 1;
            verifyChunk(*minIt, *maxIt);
        }
    };

    test(_nss1, Timestamp(42), BSON("a" << 1), BSON("a" << 10), {BSON("a" << 5)});
    test(_nss2, Timestamp(42), BSON("a" << 1), BSON("a" << 10), {BSON("a" << 3), BSON("a" << 7)});
}

TEST_F(SplitChunkTest, PreConditionFailErrors) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setRange({chunkMin, chunkMax});

        std::vector<BSONObj> splitPoints;
        auto chunkSplitPoint = BSON("a" << 5);
        splitPoints.push_back(chunkSplitPoint);

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  collTimestamp,
                                                  ChunkRange(chunkMin, BSON("a" << 7)),
                                                  splitPoints,
                                                  "shard0000"),
                           DBException,
                           ErrorCodes::BadValue);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, NonExisingNamespaceErrors) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setRange({chunkMin, chunkMax});

        std::vector<BSONObj> splitPoints{BSON("a" << 5)};

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_EQUALS(ShardingCatalogManager::get(operationContext())
                          ->commitChunkSplit(operationContext(),
                                             NamespaceString::createNamespaceString_forTest(
                                                 "TestDB.NonExistingColl"),
                                             collEpoch,
                                             Timestamp{50, 0},
                                             ChunkRange(chunkMin, chunkMax),
                                             splitPoints,
                                             "shard0000")
                          .getStatus()
                          .code(),
                      ErrorCodes::ConflictingOperationInProgress);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, NonMatchingEpochsOfChunkAndRequestErrors) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setRange({chunkMin, chunkMax});

        std::vector<BSONObj> splitPoints{BSON("a" << 5)};

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  OID::gen(),
                                                  Timestamp{50, 0},
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000"),
                           DBException,
                           ErrorCodes::StaleEpoch);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsOutOfOrderShouldFail) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setRange({chunkMin, chunkMax});

        std::vector<BSONObj> splitPoints{BSON("a" << 5), BSON("a" << 4)};

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  collTimestamp,
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000"),
                           DBException,
                           ErrorCodes::InvalidOptions);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsOutOfRangeAtMinShouldFail) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setRange({chunkMin, chunkMax});

        std::vector<BSONObj> splitPoints{BSON("a" << 0), BSON("a" << 5)};

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  collTimestamp,
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000"),
                           DBException,
                           ErrorCodes::InvalidOptions);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsOutOfRangeAtMaxShouldFail) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << 1);
        auto chunkMax = BSON("a" << 10);
        chunk.setRange({chunkMin, chunkMax});

        std::vector<BSONObj> splitPoints{BSON("a" << 5), BSON("a" << 15)};

        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->commitChunkSplit(operationContext(),
                                                  nss,
                                                  collEpoch,
                                                  collTimestamp,
                                                  ChunkRange(chunkMin, chunkMax),
                                                  splitPoints,
                                                  "shard0000"),
                           DBException,
                           ErrorCodes::InvalidOptions);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitPointsWithDollarPrefixShouldFail) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();

        ChunkType chunk;
        chunk.setCollectionUUID(UUID::gen());

        auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
        chunk.setVersion(origVersion);
        chunk.setShard(ShardId(_shardName));

        auto chunkMin = BSON("a" << kMinBSONKey);
        auto chunkMax = BSON("a" << kMaxBSONKey);
        chunk.setRange({chunkMin, chunkMax});
        setupCollection(nss, _keyPattern, {chunk});

        ASSERT_THROWS(ShardingCatalogManager::get(operationContext())
                          ->commitChunkSplit(operationContext(),
                                             nss,
                                             collEpoch,
                                             collTimestamp,
                                             ChunkRange(chunkMin, chunkMax),
                                             {BSON("a" << BSON("$minKey" << 1))},
                                             "shard0000"),
                      DBException);
        ASSERT_THROWS(ShardingCatalogManager::get(operationContext())
                          ->commitChunkSplit(operationContext(),
                                             nss,
                                             collEpoch,
                                             collTimestamp,
                                             ChunkRange(chunkMin, chunkMax),
                                             {BSON("a" << BSON("$maxKey" << 1))},
                                             "shard0000"),
                      DBException);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(SplitChunkTest, SplitJumboChunkShouldUnsetJumboFlag) {
    const auto& nss = _nss2;
    const auto collTimestamp = Timestamp(42);
    const auto collEpoch = OID::gen();
    const auto collUuid = UUID::gen();

    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(collUuid);

    auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId(_shardName));
    chunk.setJumbo(true);

    auto chunkMin = BSON("a" << 1);
    auto chunkMax = BSON("a" << 10);
    chunk.setRange({chunkMin, chunkMax});

    auto chunkSplitPoint = BSON("a" << 5);
    std::vector<BSONObj> splitPoints{chunkSplitPoint};

    setupCollection(nss, _keyPattern, {chunk});

    ASSERT_EQ(true, chunk.getJumbo());

    uassertStatusOK(ShardingCatalogManager::get(operationContext())
                        ->commitChunkSplit(operationContext(),
                                           nss,
                                           collEpoch,
                                           collTimestamp,
                                           ChunkRange(chunkMin, chunkMax),
                                           splitPoints,
                                           "shard0000"));

    // Both resulting chunks must not be jumbo
    auto chunkDocLeft =
        getChunkDoc(operationContext(), collUuid, chunkMin, collEpoch, collTimestamp);
    ASSERT_OK(chunkDocLeft.getStatus());

    auto chunkDocRight =
        getChunkDoc(operationContext(), collUuid, chunkSplitPoint, collEpoch, collTimestamp);
    ASSERT_OK(chunkDocRight.getStatus());

    ASSERT_EQ(false, chunkDocLeft.getValue().getJumbo());
    ASSERT_EQ(false, chunkDocRight.getValue().getJumbo());
}
}  // namespace
}  // namespace mongo
