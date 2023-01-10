/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/logv2/log.h"
#include "mongo/s/global_index_cache.h"
#include "mongo/s/sharding_router_test_fixture.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

using ShardingGlobalIndexCacheTest = ShardingTestFixture;

TEST_F(ShardingGlobalIndexCacheTest, CheckGlobalIndexes) {
    const UUID collUUID = UUID::gen();
    IndexCatalogTypeMap indexMap;
    IndexOptionsType options;
    GlobalIndexesCache indexCache({collUUID, Timestamp(0, 0)}, std::move(indexMap));
    IndexCatalogType index("x_1", BSON("x" << 1), options.toBSON(), Timestamp(1, 0), collUUID);
    options.setGlobal(true);
    IndexCatalogType globalIndex(
        "x_1", BSON("x" << 1), options.toBSON(), Timestamp(2, 0), collUUID);
    indexCache.add(index, {collUUID, Timestamp(1, 0)});
    indexCache.add(globalIndex, {collUUID, Timestamp(2, 0)});

    indexCache.forEachIndex([&](const auto& idx) {
        ASSERT_TRUE(
            SimpleBSONObjComparator::kInstance.evaluate(idx.toBSON() == globalIndex.toBSON()) ||
            SimpleBSONObjComparator::kInstance.evaluate(idx.toBSON() == index.toBSON()));
        return true;
    });
    indexCache.forEachGlobalIndex([&](const auto& idx) {
        ASSERT_BSONOBJ_EQ(idx.toBSON(), globalIndex.toBSON());
        return true;
    });
}
}  // namespace
}  // namespace mongo
