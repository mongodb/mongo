// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/router_role/routing_cache/routing_information_cache.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

#include <string>

namespace mongo {
namespace {


const KeyPattern kKeyPattern(BSON("x" << 1));

class ClearJumboFlagTest : public ConfigServerTestFixture {
public:
    ChunkRange jumboChunk() {
        return ChunkRange(BSON("x" << MINKEY), BSON("x" << 0));
    }

    ChunkRange nonJumboChunk() {
        return ChunkRange(BSON("x" << 0), BSON("x" << MAXKEY));
    }

protected:
    void setUp() override {
        ConfigServerTestFixture::setUp();
        ShardType shard;
        shard.setHandle(ShardHandle{ShardId(_shardName), boost::none});
        shard.setHost("shard:12");
        setupShards({shard});
    }

    void makeCollection(const NamespaceString& nss,
                        const UUID& collUuid,
                        const OID& epoch,
                        const Timestamp& timestamp) {
        ChunkType chunk;
        chunk.setName(OID::gen());
        chunk.setCollectionUUID(collUuid);
        chunk.setVersion(ChunkVersion({epoch, timestamp}, {12, 7}));
        chunk.setShard(_shardName);
        chunk.setRange(jumboChunk());
        chunk.setJumbo(true);

        ChunkType otherChunk;
        otherChunk.setName(OID::gen());
        otherChunk.setCollectionUUID(collUuid);
        otherChunk.setVersion(ChunkVersion({epoch, timestamp}, {14, 7}));
        otherChunk.setShard(_shardName);
        otherChunk.setRange(nonJumboChunk());

        setupCollection(nss, kKeyPattern, {chunk, otherChunk});
    }

    const std::string _shardName = "shard";
    const NamespaceString _nss1 =
        NamespaceString::createNamespaceString_forTest("TestDB.TestColl1");
    const NamespaceString _nss2 =
        NamespaceString::createNamespaceString_forTest("TestDB.TestColl2");
};

TEST_F(ClearJumboFlagTest, ClearJumboShouldNotBumpVersion) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collUuid = UUID::gen();
        const auto collEpoch = OID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ShardingCatalogManager::get(operationContext())
            ->clearJumboFlag(operationContext(), nss, collEpoch, jumboChunk());

        auto chunkDoc = uassertStatusOK(getChunkDoc(
            operationContext(), collUuid, jumboChunk().getMin(), collEpoch, collTimestamp));
        ASSERT_FALSE(chunkDoc.getJumbo());
        // The persisted chunk version stays at the original {12, 7} — the clear is observed via
        // the in-memory routing cache, not via a placement-version bump.
        ASSERT_EQ(ChunkVersion({collEpoch, collTimestamp}, {12, 7}), chunkDoc.getVersion());
    };

    test(_nss2, Timestamp(42));
}

TEST_F(ClearJumboFlagTest, ClearJumboShouldNotBumpVersionIfChunkNotJumbo) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collUuid = UUID::gen();
        const auto collEpoch = OID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ShardingCatalogManager::get(operationContext())
            ->clearJumboFlag(operationContext(), nss, collEpoch, nonJumboChunk());

        auto chunkDoc = uassertStatusOK(getChunkDoc(
            operationContext(), collUuid, nonJumboChunk().getMin(), collEpoch, collTimestamp));
        ASSERT_FALSE(chunkDoc.getJumbo());
        ASSERT_EQ(ChunkVersion({collEpoch, collTimestamp}, {14, 7}), chunkDoc.getVersion());
    };

    test(_nss2, Timestamp(42));
}

TEST_F(ClearJumboFlagTest, AssertsOnEpochMismatch) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collUuid = UUID::gen();
        const auto collEpoch = OID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ASSERT_THROWS_CODE(ShardingCatalogManager::get(operationContext())
                               ->clearJumboFlag(operationContext(), nss, OID::gen(), jumboChunk()),
                           AssertionException,
                           ErrorCodes::StaleEpoch);
    };

    test(_nss2, Timestamp(42));
}

TEST_F(ClearJumboFlagTest, AssertsIfChunkCantBeFound) {
    auto test = [&](const NamespaceString& nss, const Timestamp& collTimestamp) {
        const auto collEpoch = OID::gen();
        const auto collUuid = UUID::gen();
        makeCollection(nss, collUuid, collEpoch, collTimestamp);

        ChunkRange imaginaryChunk(BSON("x" << 0), BSON("x" << 10));
        ASSERT_THROWS(ShardingCatalogManager::get(operationContext())
                          ->clearJumboFlag(operationContext(), nss, OID::gen(), imaginaryChunk),
                      AssertionException);
    };

    test(_nss2, Timestamp(42));
}

// Demonstrates that the balancer (which reads through the configsvr's
// RoutingInformationCache) observes a cleared jumbo flag even though clearJumboFlag does not bump
// the chunk's placement version. The mechanism is the in-memory ChunkInfo mutation performed by
// clearJumboFlag: an incremental cache refresh would otherwise not pick up the change because the
// loader filters chunks by {lastmod: {$gte: sinceVersion}}.
TEST_F(ClearJumboFlagTest, BalancerObservesClearedJumboFlagViaRoutingCache) {
    const auto collTimestamp = Timestamp(42);
    const auto collUuid = UUID::gen();
    const auto collEpoch = OID::gen();
    makeCollection(_nss2, collUuid, collEpoch, collTimestamp);

    // Reads jumbo for the chunk whose min == jumboChunk().getMin() through the same path the
    // balancer takes (RoutingInformationCache::getCollectionPlacementInfoWithRefresh).
    const auto isJumboInRoutingCache = [&]() {
        auto cm =
            uassertStatusOK(RoutingInformationCache::get(operationContext())
                                ->getCollectionPlacementInfoWithRefresh(operationContext(), _nss2));
        ASSERT_TRUE(cm.isSharded());

        bool found = false;
        bool jumbo = false;
        cm.forEachChunk([&](const auto& chunk) {
            if (chunk.getMin().woCompare(jumboChunk().getMin()) == 0) {
                found = true;
                jumbo = chunk.isJumbo();
                return false;
            }
            return true;
        });
        ASSERT_TRUE(found);
        return jumbo;
    };

    // Prime the routing cache: the chunk is jumbo on disk, so the cached ChunkInfo must reflect
    // that.
    ASSERT_TRUE(isJumboInRoutingCache());

    ShardingCatalogManager::get(operationContext())
        ->clearJumboFlag(operationContext(), _nss2, collEpoch, jumboChunk());

    // The persisted chunk's placement version is unchanged: any observation of the cleared flag
    // through the routing cache must come from the in-memory mutation, not from a version-driven
    // incremental refresh.
    auto chunkDoc = uassertStatusOK(
        getChunkDoc(operationContext(), collUuid, jumboChunk().getMin(), collEpoch, collTimestamp));
    ASSERT_FALSE(chunkDoc.getJumbo());
    ASSERT_EQ(ChunkVersion({collEpoch, collTimestamp}, {12, 7}), chunkDoc.getVersion());

    // The balancer's next read sees the cleared flag.
    ASSERT_FALSE(isJumboInRoutingCache());
}

}  // namespace
}  // namespace mongo
