// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/ttl/ttl_collection_cache.h"

#include "mongo/unittest/unittest.h"

#include <absl/container/node_hash_map.h>

namespace mongo {
namespace {

TEST(TTLCollectionCacheTest, Basic) {
    TTLCollectionCache cache;
    EXPECT_EQ(cache.getTTLInfos().size(), 0);

    auto uuidCollA = UUID::gen();
    auto uuidCollB = UUID::gen();
    auto uuidCollC = UUID::gen();
    auto infoIndexA1 = TTLCollectionCache::Info{
        "collA_ttl_1", TTLCollectionCache::Info::ExpireAfterSecondsType::kInt};
    auto infoClusteredA = TTLCollectionCache::Info{TTLCollectionCache::ClusteredId{}};
    auto infoIndexB1 = TTLCollectionCache::Info(
        "collB_ttl_1", TTLCollectionCache::Info::ExpireAfterSecondsType::kInvalid);
    auto infoIndexC1 = TTLCollectionCache::Info(
        "collC_ttl_1", TTLCollectionCache::Info::ExpireAfterSecondsType::kNonInt);

    // Confirm registerTTLInfo() behavior using getTTLInfo().
    cache.registerTTLInfo(uuidCollA, infoIndexA1);
    cache.registerTTLInfo(uuidCollA, infoClusteredA);
    cache.registerTTLInfo(uuidCollB, infoIndexB1);
    cache.registerTTLInfo(uuidCollC, infoIndexC1);

    auto infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 3U);
    ASSERT_TRUE(infos.contains(uuidCollA));
    ASSERT_EQ(infos[uuidCollA].size(), 2U);
    EXPECT_FALSE(infos[uuidCollA][0].isClustered());
    EXPECT_EQ(infos[uuidCollA][0].getIndexName(), "collA_ttl_1");
    EXPECT_FALSE(infos[uuidCollA][0].isExpireAfterSecondsInvalid());
    ASSERT(infos[uuidCollA][1].isClustered());

    ASSERT_TRUE(infos.contains(uuidCollB));
    ASSERT_EQ(infos[uuidCollB].size(), 1U);

    EXPECT_FALSE(infos[uuidCollB][0].isClustered());
    EXPECT_EQ(infos[uuidCollB][0].getIndexName(), "collB_ttl_1");
    ASSERT(infos[uuidCollB][0].isExpireAfterSecondsInvalid());

    // Check that we can reset _expireAfterSecondsType on the TTL index.
    cache.setTTLIndexExpireAfterSecondsType(uuidCollB,
                                            infoIndexB1.getIndexName(),
                                            TTLCollectionCache::Info::ExpireAfterSecondsType::kInt);
    infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 3U);
    ASSERT_TRUE(infos.contains(uuidCollB));
    ASSERT_EQ(infos[uuidCollB].size(), 1U);
    EXPECT_EQ(infos[uuidCollB][0].getIndexName(), "collB_ttl_1");
    EXPECT_FALSE(infos[uuidCollB][0].isExpireAfterSecondsInvalid());

    ASSERT_TRUE(infos.contains(uuidCollC));
    ASSERT_EQ(infos[uuidCollC].size(), 1U);

    EXPECT_FALSE(infos[uuidCollC][0].isClustered());
    EXPECT_EQ(infos[uuidCollC][0].getIndexName(), "collC_ttl_1");
    ASSERT(infos[uuidCollC][0].isExpireAfterSecondsNonInt());

    // Check that we can reset '_isExpireAfterSecondsInvalid()' on the TTL index.
    cache.setTTLIndexExpireAfterSecondsType(uuidCollC,
                                            infoIndexC1.getIndexName(),
                                            TTLCollectionCache::Info::ExpireAfterSecondsType::kInt);
    infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 3U);
    ASSERT_TRUE(infos.contains(uuidCollC));
    ASSERT_EQ(infos[uuidCollC].size(), 1U);
    EXPECT_EQ(infos[uuidCollC][0].getIndexName(), "collC_ttl_1");
    EXPECT_FALSE(infos[uuidCollC][0].isExpireAfterSecondsInvalid());
    EXPECT_FALSE(infos[uuidCollC][0].isExpireAfterSecondsNonInt());

    // Check deregisterTTLInfo(). TTLCollectionCache should clean up
    // UUIDs that no longer have any TTL infos registered.
    cache.deregisterTTLIndexByName(uuidCollC, infoIndexC1.getIndexName());
    infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 2U);
    ASSERT_TRUE(infos.contains(uuidCollB));
    EXPECT_EQ(infos[uuidCollB].size(), 1U);
    ASSERT_TRUE(infos.contains(uuidCollA));
    EXPECT_EQ(infos[uuidCollA].size(), 2U);

    cache.deregisterTTLIndexByName(uuidCollB, infoIndexB1.getIndexName());
    infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 1U);
    ASSERT_TRUE(infos.contains(uuidCollA));
    EXPECT_EQ(infos[uuidCollA].size(), 2U);

    // Remove info for TTL index on collection A.
    cache.deregisterTTLIndexByName(uuidCollA, infoIndexA1.getIndexName());
    infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 1U);
    ASSERT_TRUE(infos.contains(uuidCollA));
    ASSERT_EQ(infos[uuidCollA].size(), 1U);
    ASSERT(infos[uuidCollA][0].isClustered());

    // Remove clustered info for collection A.
    cache.deregisterTTLClusteredIndex(uuidCollA);
    infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 0U);
}

// Test that registering the same TTL index more than once does not create duplicate infos.
// This guards against SERVER-129760, where repeatedly creating and dropping the same TTL index
// caused the TTL job to run multiple times within a single cycle.
TEST(TTLCollectionCacheTest, RegisterSameIndexTwiceDoesNotDuplicate) {
    TTLCollectionCache cache;
    EXPECT_EQ(cache.getTTLInfos().size(), 0);

    auto uuid = UUID::gen();
    auto infoInvalid = TTLCollectionCache::Info{
        "collA_ttl_1", TTLCollectionCache::Info::ExpireAfterSecondsType::kInvalid};
    auto infoInt = TTLCollectionCache::Info{
        "collA_ttl_1", TTLCollectionCache::Info::ExpireAfterSecondsType::kInt};

    cache.registerTTLInfo(uuid, infoInvalid);
    // Re-registering the same index name replaces the existing info rather than duplicating it.
    cache.registerTTLInfo(uuid, infoInt);

    auto infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 1U);
    ASSERT_TRUE(infos.contains(uuid));
    ASSERT_EQ(infos[uuid].size(), 1U);
    EXPECT_EQ(infos[uuid][0].getIndexName(), "collA_ttl_1");
    // The most recent registration's expireAfterSecondsType wins.
    EXPECT_FALSE(infos[uuid][0].isExpireAfterSecondsInvalid());
}

// Test that registering a clustered TTL info more than once does not create duplicate infos.
TEST(TTLCollectionCacheTest, RegisterSameClusteredIndexTwiceDoesNotDuplicate) {
    TTLCollectionCache cache;
    EXPECT_EQ(cache.getTTLInfos().size(), 0);

    auto uuid = UUID::gen();
    auto infoClustered = TTLCollectionCache::Info{TTLCollectionCache::ClusteredId{}};

    cache.registerTTLInfo(uuid, infoClustered);
    cache.registerTTLInfo(uuid, infoClustered);

    auto infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 1U);
    ASSERT_TRUE(infos.contains(uuid));
    ASSERT_EQ(infos[uuid].size(), 1U);
    EXPECT_TRUE(infos[uuid][0].isClustered());
}

// Test that registering distinct TTL indexes on the same UUID keeps all of them.
TEST(TTLCollectionCacheTest, RegisterDistinctIndexesKeepsAll) {
    TTLCollectionCache cache;
    EXPECT_EQ(cache.getTTLInfos().size(), 0);

    auto uuid = UUID::gen();
    auto infoIndex1 = TTLCollectionCache::Info{
        "collA_ttl_1", TTLCollectionCache::Info::ExpireAfterSecondsType::kInt};
    auto infoIndex2 = TTLCollectionCache::Info{
        "collA_ttl_2", TTLCollectionCache::Info::ExpireAfterSecondsType::kInt};
    auto infoClustered = TTLCollectionCache::Info{TTLCollectionCache::ClusteredId{}};

    cache.registerTTLInfo(uuid, infoIndex1);
    cache.registerTTLInfo(uuid, infoIndex2);
    cache.registerTTLInfo(uuid, infoClustered);
    // Re-registering an existing index should not affect the others.
    cache.registerTTLInfo(uuid, infoIndex1);

    auto infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 1U);
    ASSERT_TRUE(infos.contains(uuid));
    ASSERT_EQ(infos[uuid].size(), 3U);
}

TEST(TTLCollectionCacheTest, DeregisterUntrackedUUIDDoesNotThrow) {
    TTLCollectionCache cache;
    EXPECT_EQ(cache.getTTLInfos().size(), 0);
    auto uuid = UUID::gen();
    cache.deregisterTTLIndexByName(uuid, "indexNameForUntrackedUUID");
    EXPECT_EQ(cache.getTTLInfos().size(), 0);
}

TEST(TTLCollectionCacheTest, DeregisterClusteredUntrackedUUIDDoesNotThrow) {
    TTLCollectionCache cache;
    EXPECT_EQ(cache.getTTLInfos().size(), 0);
    auto uuid = UUID::gen();
    cache.deregisterTTLClusteredIndex(uuid);
    EXPECT_EQ(cache.getTTLInfos().size(), 0);
}

TEST(TTLCollectionCacheTest, DeregisterUntrackedIndexDoesNotThrow) {
    TTLCollectionCache cache;
    EXPECT_EQ(cache.getTTLInfos().size(), 0);
    auto uuid = UUID::gen();
    // Register index A1 in the cache so the cache tracks an index for 'uuid'.
    auto infoIndexA1 = TTLCollectionCache::Info{
        "collA_ttl_1", TTLCollectionCache::Info::ExpireAfterSecondsType::kInt};
    cache.registerTTLInfo(uuid, infoIndexA1);
    EXPECT_EQ(cache.getTTLInfos().size(), 1);

    cache.deregisterTTLIndexByName(uuid, "untrackedIndexName");

    auto infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 1);
    ASSERT_TRUE(infos.contains(uuid));
    ASSERT_EQ(infos[uuid].size(), 1U);
    EXPECT_EQ(infos[uuid][0].getIndexName(), infoIndexA1.getIndexName());
}

// Test that de-registering an absent index from a UUID with a clustered TTL index does not result
// in an error.
TEST(TTLCollectionCacheTest, DeregisterUntrackedIndexWithClusteredIndexDoesNotThrow) {
    TTLCollectionCache cache;
    EXPECT_EQ(cache.getTTLInfos().size(), 0);
    auto uuid = UUID::gen();
    auto infoClusteredA = TTLCollectionCache::Info{TTLCollectionCache::ClusteredId{}};
    cache.registerTTLInfo(uuid, infoClusteredA);
    EXPECT_EQ(cache.getTTLInfos().size(), 1);

    cache.deregisterTTLIndexByName(uuid, "untrackedIndexName");

    auto infos = cache.getTTLInfos();
    EXPECT_EQ(infos.size(), 1);
    ASSERT_TRUE(infos.contains(uuid));
    ASSERT_EQ(infos[uuid].size(), 1U);
    EXPECT_TRUE(infos[uuid][0].isClustered());
}

}  // namespace
}  // namespace mongo
