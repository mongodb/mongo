// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
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
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/version_context.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"
#include "mongo/util/version/releases.h"

#include <algorithm>
#include <cstddef>
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
        shard.setHandle(ShardHandle{ShardId(_shardName), boost::none});
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
        TransactionCoordinatorService::get(operationContext())->interruptForStepDown();
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

TEST_F(SplitChunkTest, RetryCommittedSplitSucceedsDuringFCVTransition) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);
    const auto collUuid = UUID::gen();

    const auto chunkMin = BSON("a" << 1);
    const auto splitPoint = BSON("a" << 5);
    const auto chunkMax = BSON("a" << 10);
    const std::vector<BSONObj> splitPoints{splitPoint};

    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(collUuid);
    chunk.setVersion(ChunkVersion({collEpoch, collTimestamp}, {1, 0}));
    chunk.setShard(ShardId(_shardName));
    chunk.setRange({chunkMin, chunkMax});

    setupCollection(_nss1, _keyPattern, {chunk});

    const auto doSplit = [&] {
        return ShardingCatalogManager::get(operationContext())
            ->commitChunkSplit(operationContext(),
                               _nss1,
                               collEpoch,
                               collTimestamp,
                               ChunkRange(chunkMin, chunkMax),
                               splitPoints,
                               _shardName);
    };

    ASSERT_OK(doSplit());

    const auto originalFCV =
        serverGlobalParams.featureCompatibility.acquireFCVSnapshot().getVersion();
    ScopeGuard restoreFCV([&] { serverGlobalParams.mutableFCV.setVersion(originalFCV); });
    // (Generic FCV reference): the retry carries a stable last LTS OFCV while server FCV
    // transitions.
    serverGlobalParams.mutableFCV.setVersion(multiversion::GenericFCV::kLastLTS);
    VersionContext::FixedOperationFCVRegion fixedOperationFCV(operationContext());
    serverGlobalParams.mutableFCV.setVersion(
        multiversion::GenericFCV::kUpgradingFromLastLTSToLatest);

    ASSERT_OK(doSplit());

    auto leftChunk = uassertStatusOK(
        getChunkDoc(operationContext(), collUuid, chunkMin, collEpoch, collTimestamp));
    ASSERT_BSONOBJ_EQ(splitPoint, leftChunk.getMax());

    auto rightChunk = uassertStatusOK(
        getChunkDoc(operationContext(), collUuid, splitPoint, collEpoch, collTimestamp));
    ASSERT_BSONOBJ_EQ(chunkMax, rightChunk.getMax());
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

// Returns the field names of 'obj' in sorted order. Used to compare the shape (not the values) of
// two chunk documents.
std::vector<std::string> sortedFieldNames(const BSONObj& obj) {
    std::vector<std::string> names;
    for (const auto& element : obj) {
        names.push_back(element.fieldName());
    }
    std::sort(names.begin(), names.end());
    return names;
}

class CommitSplitTest : public SplitChunkTest {
protected:
    // Returns the highest chunk version owned by 'shard', i.e. its shard placement version.
    ChunkVersion getShardVersion(const UUID& collUuid,
                                 const OID& collEpoch,
                                 const Timestamp& collTimestamp,
                                 const ShardId& shard) {
        auto doc = uassertStatusOK(findOneOnConfigCollection(
            operationContext(),
            NamespaceString::kConfigsvrChunksNamespace,
            BSON(ChunkType::collectionUUID << collUuid << ChunkType::shard(shard.toString())),
            BSON(ChunkType::lastmod << -1)));
        return uassertStatusOK(ChunkType::parseFromConfigBSON(doc, collEpoch, collTimestamp))
            .getVersion();
    }

    std::vector<ChunkType> commitSplit(const NamespaceString& nss,
                                       const ChunkVersion& shardVersionPreSplit,
                                       const ChunkRange& range,
                                       const std::vector<BSONObj>& splitPoints) {
        return assertGet(
            ShardingCatalogManager::get(operationContext())
                ->commitSplit(
                    operationContext(), nss, shardVersionPreSplit, range, splitPoints, _shardName));
    }

    boost::optional<ChunkType> findChangedChunkByMin(const std::vector<ChunkType>& chunks,
                                                     const BSONObj& min) {
        for (const auto& chunk : chunks) {
            if (chunk.getMin().woCompare(min) == 0) {
                return chunk;
            }
        }
        return boost::none;
    }

    // Asserts two changed-chunks lists describe the same chunks. The order can differ between a
    // commit and its idempotent retry, so the comparison is order-independent (keyed by chunk min).
    void assertSameChangedChunks(std::vector<ChunkType> lhs, std::vector<ChunkType> rhs) {
        ASSERT_EQ(lhs.size(), rhs.size());
        const auto byMin = [](const ChunkType& l, const ChunkType& r) {
            return l.getMin().woCompare(r.getMin()) < 0;
        };
        std::sort(lhs.begin(), lhs.end(), byMin);
        std::sort(rhs.begin(), rhs.end(), byMin);
        for (size_t i = 0; i < lhs.size(); ++i) {
            ASSERT_BSONOBJ_EQ(lhs[i].getMin(), rhs[i].getMin());
            ASSERT_BSONOBJ_EQ(lhs[i].getMax(), rhs[i].getMax());
            ASSERT_EQ(lhs[i].getShard(), rhs[i].getShard());
            ASSERT_EQ(lhs[i].getVersion(), rhs[i].getVersion());
        }
    }

    // Simulates a concurrent DDL disabling chunk operations on the collection by clearing the
    // allowChunkOperations flag on its config.collections document.
    void disallowChunkOperations(const NamespaceString& nss) {
        DBDirectClient client(operationContext());
        client.update(
            NamespaceString::kConfigsvrCollectionsNamespace,
            BSON(CollectionType::kNssFieldName
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
            BSON("$set" << BSON(CollectionType::kAllowChunkOperationsFieldName << false)));
    }
};

TEST_F(CommitSplitTest, CommitSplitReturnsNewSubChunks) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);
    const auto collUuid = UUID::gen();

    const auto chunkMin = BSON("a" << 1);
    const auto splitPoint = BSON("a" << 5);
    const auto chunkMax = BSON("a" << 10);

    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(collUuid);
    const auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId(_shardName));
    chunk.setRange({chunkMin, chunkMax});
    setupCollection(_nss1, _keyPattern, {chunk});

    auto changedChunks =
        commitSplit(_nss1, origVersion, ChunkRange(chunkMin, chunkMax), {splitPoint});

    ASSERT_EQ(2U, changedChunks.size());

    auto left = findChangedChunkByMin(changedChunks, chunkMin);
    ASSERT(left.has_value());
    ASSERT_BSONOBJ_EQ(splitPoint, left->getMax());
    ASSERT_EQ(ShardId(_shardName), left->getShard());

    auto right = findChangedChunkByMin(changedChunks, splitPoint);
    ASSERT(right.has_value());
    ASSERT_BSONOBJ_EQ(chunkMax, right->getMax());
    ASSERT_EQ(ShardId(_shardName), right->getShard());

    // Both sub-chunks carry a version produced by the split, greater than the pre-split version.
    ASSERT_EQ(std::partial_ordering::less, origVersion <=> left->getVersion());
    ASSERT_EQ(std::partial_ordering::less, origVersion <=> right->getVersion());
}

TEST_F(CommitSplitTest, IdempotentRetryReturnsSameChangedChunks) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);
    const auto collUuid = UUID::gen();

    const auto chunkMin = BSON("a" << 1);
    const auto splitPoint = BSON("a" << 5);
    const auto chunkMax = BSON("a" << 10);

    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(collUuid);
    const auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId(_shardName));
    chunk.setRange({chunkMin, chunkMax});
    setupCollection(_nss1, _keyPattern, {chunk});

    auto firstResult =
        commitSplit(_nss1, origVersion, ChunkRange(chunkMin, chunkMax), {splitPoint});
    const auto versionAfterFirst =
        getShardVersion(collUuid, collEpoch, collTimestamp, ShardId(_shardName));

    // Retry with the same arguments; the split is already committed.
    auto secondResult =
        commitSplit(_nss1, origVersion, ChunkRange(chunkMin, chunkMax), {splitPoint});

    assertSameChangedChunks(firstResult, secondResult);

    // The retry must not bump any version.
    ASSERT_EQ(versionAfterFirst,
              getShardVersion(collUuid, collEpoch, collTimestamp, ShardId(_shardName)));
}

// An idempotent retry of an already-applied commit must return OK even when chunk operations have
// since been disallowed on the collection. The allowChunkOperations check runs only on the
// non-retry path.
TEST_F(CommitSplitTest, IdempotentRetrySucceedsWhenChunkOperationsDisallowed) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);
    const auto collUuid = UUID::gen();

    const auto chunkMin = BSON("a" << 1);
    const auto splitPoint = BSON("a" << 5);
    const auto chunkMax = BSON("a" << 10);

    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(collUuid);
    const auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId(_shardName));
    chunk.setRange({chunkMin, chunkMax});
    setupCollection(_nss1, _keyPattern, {chunk});

    auto firstResult =
        commitSplit(_nss1, origVersion, ChunkRange(chunkMin, chunkMax), {splitPoint});

    disallowChunkOperations(_nss1);

    auto secondResult =
        commitSplit(_nss1, origVersion, ChunkRange(chunkMin, chunkMax), {splitPoint});

    assertSameChangedChunks(firstResult, secondResult);
}

// The commit returns chunks that participants store alongside chunks they load from the global
// catalog, so both must expose the same set of fields. This compares the field set (format), not
// the values, of each commit chunk against the durable chunk for the same range.
TEST_F(CommitSplitTest, ChangedChunksHaveSameFormatAsDurableChunks) {
    const auto collEpoch = OID::gen();
    const auto collTimestamp = Timestamp(42);
    const auto collUuid = UUID::gen();

    const auto chunkMin = BSON("a" << 1);
    const auto splitPoint = BSON("a" << 5);
    const auto chunkMax = BSON("a" << 10);

    ChunkType chunk;
    chunk.setName(OID::gen());
    chunk.setCollectionUUID(collUuid);
    const auto origVersion = ChunkVersion({collEpoch, collTimestamp}, {1, 0});
    chunk.setVersion(origVersion);
    chunk.setShard(ShardId(_shardName));
    chunk.setRange({chunkMin, chunkMax});
    chunk.setOnCurrentShardSince(Timestamp(100, 0));
    chunk.setHistory({ChunkHistory(Timestamp(100, 0), ShardId(_shardName))});
    setupCollection(_nss1, _keyPattern, {chunk});

    auto changedChunks =
        commitSplit(_nss1, origVersion, ChunkRange(chunkMin, chunkMax), {splitPoint});
    ASSERT_EQ(2U, changedChunks.size());

    for (const auto& changed : changedChunks) {
        auto fromCatalog = uassertStatusOK(
            getChunkDoc(operationContext(), collUuid, changed.getMin(), collEpoch, collTimestamp));
        ASSERT_EQ(sortedFieldNames(changed.toConfigBSON()),
                  sortedFieldNames(fromCatalog.toConfigBSON()));
    }
}
}  // namespace
}  // namespace mongo
