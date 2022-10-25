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
#include "mongo/s/catalog/type_namespace_placement_gen.h"

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
/*Utility class to simplify the readibility of the placement history tests*/
class PlacementHistoryCollection {

public:
    PlacementHistoryCollection() = default;

    void insertEntryForShardCollection(const Timestamp& timestamp,
                                       const std::string& ns,
                                       const std::vector<std::string>& shards) {
        insertEntry(timestamp, ns, shards);
    }

    void insertEntryForDropDatabase(const Timestamp& timestamp, const std::string& ns) {
        insertEntry(timestamp, ns, {});
    }

    void insertEntryForDropCollection(const Timestamp& timestamp, const std::string& ns) {
        insertEntry(timestamp, ns, {});
    }

    void insertEntryForMovePrimary(const Timestamp& timestamp,
                                   const std::string& ns,
                                   const std::string& primaryShard) {
        insertEntry(timestamp, ns, {primaryShard});
    }

    void insertEntryForCreateDatabase(const Timestamp& timestamp,
                                      const std::string& ns,
                                      const std::string& primaryShard) {
        insertEntry(timestamp, ns, {primaryShard});
    }

    void insertEntryForCompleteMigration(const Timestamp& timestamp,
                                         const std::string& ns,
                                         const std::vector<std::string>& newShards) {
        insertEntry(timestamp, ns, newShards);
    }

    const std::vector<BSONObj>& getData() {
        return _placementData;
    }

private:
    void insertEntry(const Timestamp& timestamp,
                     const std::string& ns,
                     const std::vector<std::string>& shards) {
        std::vector<ShardId> shardIds;
        for (const auto& shard : shards) {
            shardIds.push_back(ShardId(shard));
        }
        auto nss = NamespaceString(ns);
        auto uuid = getUUID(nss);
        auto entry = NamespacePlacementType(nss, timestamp, shardIds);
        entry.setUuid(uuid);
        _placementData.push_back(entry.toBSON());
    }

    boost::optional<UUID> getUUID(const NamespaceString& nss) {
        if (nss.coll().empty()) {
            // no uuid for database
            return boost::none;
        }

        if (_uuidMap.find(nss.toString()) == _uuidMap.end())
            _uuidMap.emplace(nss.toString(), UUID::gen());

        const UUID& uuid = _uuidMap.at(nss.toString());
        return uuid;
    }
    std::vector<BSONObj> _placementData;
    stdx::unordered_map<std::string, UUID> _uuidMap;
};
}  // namespace


TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_ShardedWithData) {

    auto opCtx = operationContext();

    PlacementHistoryCollection placementHistoryCollection;
    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(1, 0), "db", "shard1");
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"});
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(3, 0), "db.collection2", {"shard1", "shard2", "shard3"});
    placementHistoryCollection.insertEntryForDropCollection(Timestamp(4, 0), "db.collection2");
    placementHistoryCollection.insertEntryForMovePrimary(Timestamp(5, 0), "db", "shard4");
    placementHistoryCollection.insertEntryForCompleteMigration(
        Timestamp(6, 0), "db.collection1", {"shard2", "shard3", "shard4"});

    // insert sample data into placementHistory collection
    for (auto& doc : placementHistoryCollection.getData()) {
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
    }

    for (auto& doc : generateConfigShardSampleData(4)) {
        ASSERT_OK(insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
    }

    // 3 shards should own collection1 at timestamp 4
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(4, 0));

    ASSERT_EQ(3U, shards.size());
    std::sort(shards.begin(), shards.end());
    ASSERT(shards[0] == "shard1");
    ASSERT(shards[1] == "shard2");
    ASSERT(shards[2] == "shard3");

    // 3 shards should own collection1 at timestamp 5
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(5, 0));

    ASSERT_EQ(3U, shards.size());
    std::sort(shards.begin(), shards.end());
    ASSERT(shards[0] == "shard1");
    ASSERT(shards[1] == "shard2");
    ASSERT(shards[2] == "shard3");

    // 3 shards should own collection1 at timestamp 7
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(7, 0));

    ASSERT_EQ(3U, shards.size());
    std::sort(shards.begin(), shards.end());
    ASSERT(shards[0] == "shard2");
    ASSERT(shards[1] == "shard3");
    ASSERT(shards[2] == "shard4");
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_EmptyHistory) {

    auto opCtx = operationContext();

    // no shards should be returned
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(4, 0));

    ASSERT_EQ(0U, shards.size());

    // no shards should be returned
    auto shards2 = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(4, 0));

    ASSERT_EQ(0U, shards2.size());

    // no shards should be returned
    auto shards3 = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(4, 0));

    ASSERT_EQ(0U, shards3.size());
}  // namespace

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_NoData) {

    auto opCtx = operationContext();

    PlacementHistoryCollection placementHistoryCollection;
    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(1, 0), "db", "shard1");
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"});
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(3, 0), "db.collection2", {"shard1", "shard2", "shard3"});
    placementHistoryCollection.insertEntryForDropCollection(Timestamp(4, 0), "db.collection2");

    // insert sample data into placementHistory collection
    for (auto& doc : placementHistoryCollection.getData()) {
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
    }

    for (auto& doc : generateConfigShardSampleData(4)) {
        ASSERT_OK(insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
    }

    // Collection was dropped: only primary shard should be returned
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection2"), Timestamp(4, 0));

    ASSERT_EQ(1U, shards.size());
    ASSERT(shards[0] == "shard1");

    // no collection was sharded yet, but the database exists: only primary shard should be returned
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection2"), Timestamp(1, 0));

    ASSERT_EQ(1, shards.size());
    ASSERT_EQ("shard1", shards[0]);
}


TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_RegexStageTest) {

    auto opCtx = operationContext();

    PlacementHistoryCollection placementHistoryCollection;
    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(1, 0), "config", "shard6");
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(1, 0), "config.system.collections", {"shard6", "shard7"});
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(1, 0), "config.systemXcollections", {"shard8"});

    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(1, 0), "db", "shard1");
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(2, 0), "db.collection1", {"shard1", "shard2"});
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(2, 0), "db.collection1.collection1", {"shard3", "shard4"});

    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(3, 0), "dbXX", "shard10");
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(3, 0), "dbXX.collection1", {"shard1", "shard2", "shard3", "shard4", "shard5"});

    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(4, 0), "db_db", "shard11");
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(4, 0), "db_db.collection1", {"shard1", "shard2", "shard3", "shard4", "shard5"});

#ifndef _WIN32
    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(5, 0), "db*db", "shard12");
    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(5, 0), "db|db", "shard13");
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(6, 0), "db*db.collection1", {"shard1", "shard2", "shard3", "shard4", "shard5"});
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(6, 0), "db|db.collection1", {"shard1", "shard2", "shard3", "shard4", "shard5"});
#endif


    placementHistoryCollection.insertEntryForCreateDatabase(
        Timestamp(4, 0), "dbXcollection1", "shard5");


    // insert sample data into placementHistory collection
    for (auto& doc : placementHistoryCollection.getData()) {
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
    }

    for (auto& doc : generateConfigShardSampleData(13)) {
        ASSERT_OK(insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
    }

    // there is only one db.collection1 which contains shard1 and shard2
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(7, 0));

    ASSERT_EQ(2U, shards.size());
    ASSERT_EQ("shard1", shards[0]);
    ASSERT_EQ("shard2", shards[1]);

    // testing config.system.collections
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("config.system.collections"), Timestamp(7, 0));

    ASSERT_EQ(2U, shards.size());
    ASSERT_EQ("shard6", shards[0]);
    ASSERT_EQ("shard7", shards[1]);

    // only primary shards should be returned since collectionX is not found
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collectionX"), Timestamp(6, 0));

    ASSERT_EQ(1U, shards.size());
    ASSERT_EQ("shard1", shards[0]);

    // no data should be returned since the namespace is not found
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("dbX.collection1"), Timestamp(6, 0));

    ASSERT_EQ(0U, shards.size());

    // Database and collection do not exist
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("dbX.collectionX"), Timestamp(6, 0));

    ASSERT_EQ(0U, shards.size());

    // Database does not exist
    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("dbX"), Timestamp(6, 0));

    ASSERT_EQ(0U, shards.size());

    // db|db , db*db  etc... should not be found
    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(6, 0));

    ASSERT_EQ(4U, shards.size());
    ASSERT_EQ("shard1", shards[0]);
    ASSERT_EQ("shard2", shards[1]);
    ASSERT_EQ("shard3", shards[2]);
    ASSERT_EQ("shard4", shards[3]);
}


TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_ShardedWithData) {

    auto opCtx = operationContext();

    PlacementHistoryCollection placementHistoryCollection;
    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(1, 0), "db", "shard1");
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"});
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(3, 0), "db.collection2", {"shard1", "shard2", "shard3"});
    placementHistoryCollection.insertEntryForDropCollection(Timestamp(4, 0), "db.collection2");
    placementHistoryCollection.insertEntryForMovePrimary(Timestamp(5, 0), "db", "shard5");
    placementHistoryCollection.insertEntryForCompleteMigration(
        Timestamp(6, 0), "db.collection1", {"shard2", "shard3", "shard4"});

    // insert sample data into placementHistory collection
    for (auto& doc : placementHistoryCollection.getData()) {
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
    }

    for (auto& doc : generateConfigShardSampleData(5)) {
        ASSERT_OK(insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
    }

    // 4 shards should own data for database db at timestamp 7
    auto shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(7, 0));

    ASSERT_EQ(4U, shards.size());
    std::sort(shards.begin(), shards.end());
    ASSERT(shards[0] == "shard2");
    ASSERT(shards[1] == "shard3");
    ASSERT(shards[2] == "shard4");
    ASSERT(shards[3] == "shard5");
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_DropDatabase) {

    PlacementHistoryCollection placementHistoryCollection;

    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(1, 0), "db", "shard1");
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(2, 0), "db.collection", {"shard1", "shard2", "shard3", "shard4"});
    placementHistoryCollection.insertEntryForDropCollection(Timestamp(3, 0), "db.collection");
    placementHistoryCollection.insertEntryForDropDatabase(Timestamp(4, 0), "db");

    auto opCtx = operationContext();

    // insert sample data into placementHistory collection
    for (auto& doc : placementHistoryCollection.getData()) {
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
    }

    for (auto& doc : generateConfigShardSampleData(4)) {
        ASSERT_OK(insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
    }

    auto shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(4, 0));

    ASSERT_EQ(0U, shards.size());
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_ShardedWithData) {

    auto opCtx = operationContext();

    PlacementHistoryCollection placementHistoryCollection;
    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(1, 0), "db", "shard1");
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"});
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(3, 0), "db.collection2", {"shard1", "shard2", "shard6"});
    placementHistoryCollection.insertEntryForDropCollection(Timestamp(4, 0), "db.collection2");
    placementHistoryCollection.insertEntryForMovePrimary(Timestamp(5, 0), "db", "shard5");
    placementHistoryCollection.insertEntryForCompleteMigration(
        Timestamp(6, 0), "db.collection1", {"shard2", "shard3", "shard4"});
    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(7, 0), "db2", "shard1");

    // insert sample data into placementHistory collection
    for (auto& doc : placementHistoryCollection.getData()) {
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
    }

    for (auto& doc : generateConfigShardSampleData(5)) {
        ASSERT_OK(insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
    }

    // 4 shards should own data for database db at timestamp 7
    auto shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(7, 0));

    ASSERT_EQ(5U, shards.size());
    std::sort(shards.begin(), shards.end());
    ASSERT(shards[0] == "shard1");
    ASSERT(shards[1] == "shard2");
    ASSERT(shards[2] == "shard3");
    ASSERT(shards[3] == "shard4");
    ASSERT(shards[4] == "shard5");
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_DropDatabase) {

    PlacementHistoryCollection placementHistoryCollection;

    placementHistoryCollection.insertEntryForCreateDatabase(Timestamp(1, 0), "db", "shard1");
    placementHistoryCollection.insertEntryForShardCollection(
        Timestamp(2, 0), "db.collection", {"shard1", "shard2", "shard3", "shard4"});
    placementHistoryCollection.insertEntryForDropCollection(Timestamp(3, 0), "db.collection");
    placementHistoryCollection.insertEntryForDropDatabase(Timestamp(4, 0), "db");

    auto opCtx = operationContext();

    // insert sample data into placementHistory collection
    for (auto& doc : placementHistoryCollection.getData()) {
        ASSERT_OK(insertToConfigCollection(
            opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
    }

    for (auto& doc : generateConfigShardSampleData(4)) {
        ASSERT_OK(insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
    }

    auto shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(4, 0));

    ASSERT_EQ(0U, shards.size());
}


// ############################# Indexes #############################
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
}  // namespace mongo
