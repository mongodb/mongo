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

#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_shard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

// These tests are for the aggregations in the CatalogClient. They are here because the unittests in
// sharding_catalog_client_test.cpp are part of the s_test which does not have storage.

using CatalogClientAggregationsTest = ConfigServerTestFixture;

TEST_F(CatalogClientAggregationsTest, TestCollectionAndIndexesAggregationWithNoIndexes) {
    const NamespaceString nss{"TestDB.TestColl"};
    const ChunkVersion placementVersion{{OID::gen(), Timestamp(1, 0)}, {1, 0}};
    const std::string shardName = "shard01";
    const UUID uuid{UUID::gen()};
    const KeyPattern shardKey{BSON("_id" << 1)};
    setupShards({ShardType(shardName, "host01")});
    setupCollection(nss,
                    shardKey,
                    {ChunkType(uuid,
                               ChunkRange(BSONObjBuilder().appendMinKey("_id").obj(),
                                          BSONObjBuilder().appendMaxKey("_id").obj()),
                               placementVersion,
                               ShardId("shard01"))});
    auto [collection, indexes] = catalogClient()->getCollectionAndGlobalIndexes(
        operationContext(), nss, {repl::ReadConcernLevel::kSnapshotReadConcern});

    ASSERT_EQ(indexes.size(), 0);
    ASSERT_EQ(collection.getEpoch(), placementVersion.epoch());
    ASSERT_EQ(collection.getTimestamp(), placementVersion.getTimestamp());
    ASSERT_EQ(collection.getUuid(), uuid);
}

TEST_F(CatalogClientAggregationsTest, TestCollectionAndIndexesWithIndexes) {
    const NamespaceString nss{"TestDB.TestColl"};
    const ChunkVersion placementVersion{{OID::gen(), Timestamp(1, 0)}, {1, 0}};
    const std::string shardName = "shard01";
    const UUID uuid{UUID::gen()};
    const KeyPattern shardKey{BSON("_id" << 1)};
    setupShards({ShardType(shardName, "host01")});
    setupCollection(nss,
                    shardKey,
                    {ChunkType(uuid,
                               ChunkRange(BSONObjBuilder().appendMinKey("_id").obj(),
                                          BSONObjBuilder().appendMaxKey("_id").obj()),
                               placementVersion,
                               ShardId("shard01"))});
    IndexCatalogType index1{"x_1", shardKey.toBSON(), {}, Timestamp(3, 0), uuid};
    uassertStatusOK(insertToConfigCollection(
        operationContext(), NamespaceString::kConfigsvrIndexCatalogNamespace, index1.toBSON()));
    IndexCatalogType index2{"y_1", shardKey.toBSON(), {}, Timestamp(4, 0), uuid};
    uassertStatusOK(insertToConfigCollection(
        operationContext(), NamespaceString::kConfigsvrIndexCatalogNamespace, index2.toBSON()));

    auto [collection, indexes] = catalogClient()->getCollectionAndGlobalIndexes(
        operationContext(), nss, {repl::ReadConcernLevel::kSnapshotReadConcern});

    ASSERT_EQ(indexes.size(), 2);
    ASSERT_EQ(collection.getEpoch(), placementVersion.epoch());
    ASSERT_EQ(collection.getTimestamp(), placementVersion.getTimestamp());
    ASSERT_EQ(collection.getUuid(), uuid);
}

TEST_F(CatalogClientAggregationsTest, TestCollectionAndIndexesWithMultipleCollections) {
    const NamespaceString nssColl1{"TestDB.Collection1"};
    const NamespaceString nssColl2{"TestDB.Collection2"};
    const ChunkVersion placementVersion{{OID::gen(), Timestamp(1, 0)}, {1, 0}};
    const std::string shardName = "shard01";
    const UUID uuidColl1{UUID::gen()};
    const UUID uuidColl2{UUID::gen()};
    const KeyPattern shardKey{BSON("_id" << 1)};
    setupShards({ShardType(shardName, "host01")});
    setupCollection(nssColl1,
                    shardKey,
                    {ChunkType(uuidColl1,
                               ChunkRange(BSONObjBuilder().appendMinKey("_id").obj(),
                                          BSONObjBuilder().appendMaxKey("_id").obj()),
                               placementVersion,
                               ShardId("shard01"))});
    setupCollection(nssColl2,
                    shardKey,
                    {ChunkType(uuidColl2,
                               ChunkRange(BSONObjBuilder().appendMinKey("_id").obj(),
                                          BSONObjBuilder().appendMaxKey("_id").obj()),
                               placementVersion,
                               ShardId("shard01"))});
    IndexCatalogType index1{"x_1", shardKey.toBSON(), {}, Timestamp(3, 0), uuidColl1};
    uassertStatusOK(insertToConfigCollection(
        operationContext(), NamespaceString::kConfigsvrIndexCatalogNamespace, index1.toBSON()));
    IndexCatalogType index2{"y_1", shardKey.toBSON(), {}, Timestamp(4, 0), uuidColl2};
    uassertStatusOK(insertToConfigCollection(
        operationContext(), NamespaceString::kConfigsvrIndexCatalogNamespace, index2.toBSON()));

    auto [collection, indexes] = catalogClient()->getCollectionAndGlobalIndexes(
        operationContext(), nssColl1, {repl::ReadConcernLevel::kSnapshotReadConcern});

    ASSERT_EQ(indexes.size(), 1);
    ASSERT_EQ(collection.getEpoch(), placementVersion.epoch());
    ASSERT_EQ(collection.getTimestamp(), placementVersion.getTimestamp());
    ASSERT_EQ(collection.getUuid(), uuidColl1);
}

}  // namespace
}  // namespace mongo
