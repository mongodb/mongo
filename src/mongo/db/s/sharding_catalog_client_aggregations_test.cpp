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
#include <vector>

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/s/catalog/sharding_catalog_client.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

// These tests are for the aggregations in the CatalogClient. They are here because the unittests in
// sharding_catalog_client_test.cpp are part of the s_test which does not have storage.

using CatalogClientAggregationsTest = ConfigServerTestFixture;

/**
 * Generates list of shards :
    _id : [shard1, shard2, shard3 ... shardN]
    hosts : [30001, ... 3000N]
    state : always 1

 */
std::vector<BSONObj> generateConfigShardSampleData(int nShards) {
    std::vector<BSONObj> configShardData;
    for (int i = 1; i <= nShards; i++) {
        const std::string shardName = "shard" + std::to_string(i);
        const std::string shardHost = "localhost:" + std::to_string(30000 + i);
        const auto& doc = BSON("_id" << shardName << "host" << shardHost << "state" << 1);

        configShardData.push_back(doc);
    }

    return configShardData;
}

std::vector<BSONObj> getPlacementDataSample() {
    std::vector<BSONObj> placementDataSample = {};
    const auto coll1Uuid = UUID::gen();
    const auto coll2Uuid = UUID::gen();
    // create database mock
    placementDataSample.push_back(BSON("_id' " << 1 << "nss"
                                               << "mock"
                                               << "timestamp" << Timestamp(1, 0) << "shards"
                                               << BSON_ARRAY("shard1")));
    // shard collection mock.collection1
    placementDataSample.push_back(BSON("_id' " << 2 << "nss"
                                               << "mock.collection1"
                                               << "uuid" << coll1Uuid << "timestamp"
                                               << Timestamp(2, 0) << "shards"
                                               << BSON_ARRAY("shard1"
                                                             << "shard2"
                                                             << "shard3")));
    // shard collection mock.collection2
    placementDataSample.push_back(BSON("_id' " << 3 << "nss"
                                               << "mock.collection2"
                                               << "uuid" << coll2Uuid << "timestamp"
                                               << Timestamp(3, 0) << "shards"
                                               << BSON_ARRAY("shard1"
                                                             << "shard2"
                                                             << "shard3")));
    // drop collection2
    placementDataSample.push_back(BSON("_id' " << 4 << "nss"
                                               << "mock.collection2"
                                               << "uuid" << coll2Uuid << "timestamp"
                                               << Timestamp(4, 0) << "shards"
                                               << BSONArrayBuilder().arr()));
    // move primary from shard1 to shard2
    placementDataSample.push_back(BSON("_id' " << 5 << "nss"
                                               << "mock"
                                               << "timestamp" << Timestamp(5, 0) << "shards"
                                               << BSON_ARRAY("shard4")));
    // move last chunk of collection 1 located in shard1 to shard4
    placementDataSample.push_back(BSON("_id' " << 6 << "nss"
                                               << "mock.collection1"
                                               << "uuid" << coll1Uuid << "timestamp"
                                               << Timestamp(6, 0) << "shards"
                                               << BSON_ARRAY("shard2"
                                                             << "shard3"
                                                             << "shard4")));

    return placementDataSample;
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_ShardedWithData) {

    auto opCtx = operationContext();

    // insert sample data into placementHistory collection
    for (auto& doc : getPlacementDataSample()) {
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
    }

    for (auto& doc : generateConfigShardSampleData(4)) {
        ASSERT_OK(insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
    }

    // 3 shards should own collection1 at timestamp 4
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("mock.collection1"), Timestamp(4, 0));

    ASSERT_EQ(3U, shards.size());
    std::sort(shards.begin(), shards.end());
    ASSERT(shards[0] == "shard1");
    ASSERT(shards[1] == "shard2");
    ASSERT(shards[2] == "shard3");

    // 3 shards should own collection1 at timestamp 5
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("mock.collection1"), Timestamp(5, 0));

    ASSERT_EQ(3U, shards.size());
    std::sort(shards.begin(), shards.end());
    ASSERT(shards[0] == "shard1");
    ASSERT(shards[1] == "shard2");
    ASSERT(shards[2] == "shard3");

    // 3 shards should own collection1 at timestamp 7
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("mock.collection1"), Timestamp(7, 0));

    ASSERT_EQ(3U, shards.size());
    std::sort(shards.begin(), shards.end());
    ASSERT(shards[0] == "shard2");
    ASSERT(shards[1] == "shard3");
    ASSERT(shards[2] == "shard4");
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_EmptyHistory) {

    auto opCtx = operationContext();

    // no shards should be returned
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("mock.collection1"), Timestamp(4, 0));

    ASSERT_EQ(0U, shards.size());
}  // namespace

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_NoData) {

    auto opCtx = operationContext();

    // insert sample data into placementHistory collection
    for (auto& doc : getPlacementDataSample()) {
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
    }

    for (auto& doc : generateConfigShardSampleData(4)) {
        ASSERT_OK(insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
    }

    // Collection was dropped: only primary shard should be returned
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("mock.collection2"), Timestamp(4, 0));

    ASSERT_EQ(1U, shards.size());
    ASSERT(shards[0] == "shard1");

    // no collection was sharded yet, but the database exists: only primary shard should be returned
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("mock.collection2"), Timestamp(1, 0));

    ASSERT_EQ(1, shards.size());
    ASSERT_EQ("shard1", shards[0]);
}


TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_RegexStageTest) {

    auto opCtx = operationContext();

    // insert sample data into placementHistory collection
    for (auto& doc : getPlacementDataSample()) {
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
    }

    for (auto& doc : generateConfigShardSampleData(4)) {
        ASSERT_OK(insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
    }

    // only primary shards should be returned since collectionX is not found
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("mock.collectionX"), Timestamp(7, 0));

    ASSERT_EQ(1U, shards.size());
    ASSERT_EQ("shard4", shards[0]);

    // no data should be returned since the namespace is not found
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("mockX.collection1"), Timestamp(7, 0));

    ASSERT_EQ(0U, shards.size());

    // Database and collection do not exist
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("mockX.collectionX"), Timestamp(7, 0));

    ASSERT_EQ(0U, shards.size());
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_SnapshotTooOld) {

    auto opCtx = operationContext();

    // insert sample data into placementHistory collection
    for (auto& doc : getPlacementDataSample()) {
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
    }

    // we force shard3 to miss
    for (auto& doc : generateConfigShardSampleData(2)) {
        ASSERT_OK(insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
    }

    // 3 shards should own collection1 at timestamp 4
    // however, since shard3 is missing, we should get an error
    ASSERT_THROWS_CODE(catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
                           opCtx, NamespaceString("mock.collection1"), Timestamp(4, 0)),
                       DBException,
                       ErrorCodes::SnapshotTooOld);
}

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
