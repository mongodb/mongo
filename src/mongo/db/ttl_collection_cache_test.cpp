/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/ttl_collection_cache.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(TTLCollectionCacheTest, Basic) {
    TTLCollectionCache cache;
    ASSERT_EQ(cache.getTTLInfos().size(), 0);

    auto uuidCollA = UUID::gen();
    auto uuidCollB = UUID::gen();
    auto infoIndexA1 = TTLCollectionCache::Info{"collA_ttl_1", /*isExpireAfterSecondsNaN=*/false};
    auto infoClusteredA = TTLCollectionCache::Info{TTLCollectionCache::ClusteredId{}};
    auto infoIndexB1 = TTLCollectionCache::Info("collB_ttl_1", /*isExpireAfterSecondsNaN=*/true);

    // Confirm registerTTLInfo() behavior using getTTLInfo().
    cache.registerTTLInfo(uuidCollA, infoIndexA1);
    cache.registerTTLInfo(uuidCollA, infoClusteredA);
    cache.registerTTLInfo(uuidCollB, infoIndexB1);

    auto infos = cache.getTTLInfos();
    ASSERT_EQ(infos.size(), 2U);
    ASSERT_EQ(infos.count(uuidCollA), 1U);
    ASSERT_EQ(infos[uuidCollA].size(), 2U);
    ASSERT_FALSE(infos[uuidCollA][0].isClustered());
    ASSERT_EQ(infos[uuidCollA][0].getIndexName(), "collA_ttl_1");
    ASSERT_FALSE(infos[uuidCollA][0].isExpireAfterSecondsNaN());
    ASSERT(infos[uuidCollA][1].isClustered());

    ASSERT_EQ(infos.count(uuidCollB), 1U);
    ASSERT_EQ(infos[uuidCollB].size(), 1U);

    ASSERT_FALSE(infos[uuidCollB][0].isClustered());
    ASSERT_EQ(infos[uuidCollB][0].getIndexName(), "collB_ttl_1");
    ASSERT(infos[uuidCollB][0].isExpireAfterSecondsNaN());

    // Check that we can reset '_isExpireAfterSecondsNaN()' on the TTL index.
    cache.unsetTTLIndexExpireAfterSecondsNaN(uuidCollB, infoIndexB1.getIndexName());
    infos = cache.getTTLInfos();
    ASSERT_EQ(infos.size(), 2U);
    ASSERT_EQ(infos.count(uuidCollB), 1U);
    ASSERT_EQ(infos[uuidCollB].size(), 1U);
    ASSERT_EQ(infos[uuidCollB][0].getIndexName(), "collB_ttl_1");
    ASSERT_FALSE(infos[uuidCollB][0].isExpireAfterSecondsNaN());

    // Check deregisterTTLInfo(). TTLCollectionCache should clean up
    // UUIDs that no longer have any TTL infos registered.
    cache.deregisterTTLIndexByName(uuidCollB, infoIndexB1.getIndexName());
    infos = cache.getTTLInfos();
    ASSERT_EQ(infos.size(), 1U);
    ASSERT_EQ(infos.count(uuidCollA), 1U);
    ASSERT_EQ(infos[uuidCollA].size(), 2U);

    // Remove info for TTL index on collection A.
    cache.deregisterTTLIndexByName(uuidCollA, infoIndexA1.getIndexName());
    infos = cache.getTTLInfos();
    ASSERT_EQ(infos.size(), 1U);
    ASSERT_EQ(infos.count(uuidCollA), 1U);
    ASSERT_EQ(infos[uuidCollA].size(), 1U);
    ASSERT(infos[uuidCollA][0].isClustered());

    // Remove clustered info for collection A.
    cache.deregisterTTLClusteredIndex(uuidCollA);
    infos = cache.getTTLInfos();
    ASSERT_EQ(infos.size(), 0U);
}

}  // namespace
}  // namespace mongo
