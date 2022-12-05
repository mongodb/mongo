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

// Extend the ConfigServerTestFixture with helper methods
class CatalogClientAggregationsTest : public ConfigServerTestFixture {
public:
    struct PlacementDescriptor {
        PlacementDescriptor(Timestamp&& timestamp,
                            std::string&& ns,
                            std::vector<std::string>&& shardsIds)
            : timestamp(std::move(timestamp)), ns(std::move(ns)), shardsIds(std::move(shardsIds)) {}

        Timestamp timestamp;
        std::string ns;
        std::vector<std::string> shardsIds;
    };

    /* Generate and insert the entries into the config.shards collection */
    void setupConfigShard(OperationContext* opCtx, int nShards) {
        for (auto& doc : _generateConfigShardSampleData(nShards)) {
            ASSERT_OK(
                insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
        }
    }

    /* Insert the entries into the config.placementHistory collection
     *  Generate a unique random UUID for a collection namespace
     */
    void setupConfigPlacementHistory(OperationContext* opCtx,
                                     std::vector<PlacementDescriptor>&& entries) {
        std::vector<BSONObj> placementData;
        stdx::unordered_map<std::string, UUID> nssToUuid;

        // Convert the entries into the format expected by the config.placementHistory collection
        for (const auto& entry : entries) {
            std::vector<ShardId> shardIds;
            for (const auto& shardId : entry.shardsIds) {
                shardIds.push_back(ShardId(shardId));
            }
            auto nss = NamespaceString(entry.ns);
            auto uuid = [&] {
                if (nss.coll().empty()) {
                    // no uuid for database
                    return boost::optional<UUID>(boost::none);
                }

                if (nssToUuid.find(nss.toString()) == nssToUuid.end())
                    nssToUuid.emplace(nss.toString(), UUID::gen());

                const UUID& collUuid = nssToUuid.at(nss.toString());
                return boost::optional<UUID>(collUuid);
            }();

            auto placementEntry = NamespacePlacementType(nss, entry.timestamp, shardIds);
            placementEntry.setUuid(uuid);
            placementData.push_back(placementEntry.toBSON());
        }

        // Insert the entries into the config.placementHistory collection
        for (auto& doc : placementData) {
            ASSERT_OK(insertToConfigCollection(
                opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, doc));
        }
    }

private:
    /**
    * Given the desired number of shards n, generates a vector of n ShardType objects (in BSON
    format) according to the following template,  Given the i-th element :
        - shard_id : shard<i>
        - host : localhost:3000<i>
        - state : always 1 (kShardAware)
    */
    std::vector<BSONObj> _generateConfigShardSampleData(int nShards) const {
        std::vector<BSONObj> configShardData;
        for (int i = 1; i <= nShards; i++) {
            const std::string shardName = "shard" + std::to_string(i);
            const std::string shardHost = "localhost:" + std::to_string(30000 + i);
            const auto& doc = BSON("_id" << shardName << "host" << shardHost << "state" << 1);

            configShardData.push_back(doc);
        }

        return configShardData;
    }
};  // CatalogClientAggregationsTest

void assertSameShardSet(std::vector<ShardId>& retrievedSet,
                        std::vector<std::string>&& expectedSet) {
    ASSERT_EQ(retrievedSet.size(), expectedSet.size());
    std::sort(retrievedSet.begin(), retrievedSet.end());
    std::sort(expectedSet.begin(), expectedSet.end());
    for (size_t i = 0; i < retrievedSet.size(); i++) {
        ASSERT_EQ(retrievedSet[i], expectedSet[i]);
    }
}
}  // namespace

// ######################## PlacementHistory: Query by collection ##########################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_ShardedCollection) {
    /*Quering the placementHistory for a sharded collection should return the shards that owned the
     * collection at the given clusterTime*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(2, 0), "db.collection1", {"shard1", "shard2"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard3", "shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    // 2 shards must own collection1
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(4, 0));

    assertSameShardSet(shards, {"shard1", "shard2"});

    // 2 shards must own collection2
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection2"), Timestamp(4, 0));

    assertSameShardSet(shards, {"shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest,
       GetShardsThatOwnDataForCollAtClusterTime_ShardedCollectionWithPrimary) {
    /*The primary shard associated to the parent database is already part of  the `shards` list of
     * the collection and it does not appear twice*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2"}},
         {Timestamp(3, 0), "db.collection1", {"shard1", "shard2", "shard3"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    // 3 shards must own collection1 at timestamp 4
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(4, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});
}

TEST_F(CatalogClientAggregationsTest,
       GetShardsThatOwnDataForCollAtClusterTime_UnshardedCollection) {
    /*Quering the placementHistory must report the primary shard for unsharded or non-existing
     * collections*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db2", {"shard2"}},
                                 {Timestamp(3, 0), "db3", {"shard3"}}});

    setupConfigShard(opCtx, 3 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection"), Timestamp(3, 0));

    assertSameShardSet(shards, {"shard1"});

    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db2.collection"), Timestamp(3, 0));

    assertSameShardSet(shards, {"shard2"});

    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db3.collection"), Timestamp(3, 0));

    assertSameShardSet(shards, {"shard3"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_DifferentTimestamp) {
    /*Query the placementHistory at different timestamp should return different results*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2"}},
         {Timestamp(4, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(5, 0), "db.collection1", {"shard1", "shard2", "shard3", "shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    // no shards at timestamp 0
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(0, 0));

    assertSameShardSet(shards, {});

    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(1, 0));

    assertSameShardSet(shards, {"shard1"});

    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(2, 0));

    assertSameShardSet(shards, {"shard1", "shard2"});

    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(4, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});

    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(5, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_SameTimestamp) {
    /*Having different namespaces for the same timestamp should not influece the expected result*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(1, 0), "db.collection", {"shard1", "shard2", "shard3"}},
         {Timestamp(1, 0), "db.collection2", {"shard1", "shard4", "shard5"}},
         {Timestamp(1, 0), "db2", {"shard6"}},
         {Timestamp(1, 0), "db2.collection", {"shard7", "shard8", "shard9"}}});

    setupConfigShard(opCtx, 9 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection"), Timestamp(1, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});

    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection2"), Timestamp(1, 0));

    assertSameShardSet(shards, {"shard1", "shard4", "shard5"});

    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db2.collection"), Timestamp(1, 0));

    assertSameShardSet(shards, {"shard6", "shard7", "shard8", "shard9"});
}

TEST_F(CatalogClientAggregationsTest,
       GetShardsThatOwnDataForCollAtClusterTime_InvertedTimestampOrder) {
    /*Ordering of document insertion into config.placementHistory must not matter*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(4, 0), "db", {"shard1"}},
         {Timestamp(3, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(2, 0), "db2", {"shard5"}},
         {Timestamp(1, 0), "db2.collection2", {"shard6", "shard7", "shard8"}}});

    setupConfigShard(opCtx, 8 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(4, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest,
       GetShardsThatOwnDataForCollAtClusterTime_ReturnPrimaryShardWhenNoShards) {
    /*Quering the placementHistory must report only the primary shard when an empty list of shards
     * is reported for the collection*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(3, 0), "db.collection2", {"shard1", "shard2", "shard3"}},
         {Timestamp(4, 0), "db.collection2", {}}});

    setupConfigShard(opCtx, 3 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection2"), Timestamp(4, 0));

    assertSameShardSet(shards, {"shard1"});

    // Note: at timestamp 3 the collection's shard list is not empty
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection2"), Timestamp(3, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_AddPrimaryShard) {
    /*Quering the placementHistory must report the primary shard in addition to the list of shards
     * related to db.collection. Primary shards must always be returned*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(3, 0), "db", {"shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(2, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3", "shard4"});

    // Note: the primary shard is shard5 at timestamp 3
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(3, 0));

    assertSameShardSet(shards, {"shard5", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest,
       GetShardsThatOwnDataForCollAtClusterTime_AddPrimaryShardAtSameTimestamp) {
    /*Quering the placementHistory must report the primary shard in addition to the list of shards
     * related to db.collection. Primary shards must always be returned*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(1, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(2, 0), "db2", {"shard5"}},
         {Timestamp(2, 0), "db2.collection2", {"shard6", "shard7", "shard8"}}});

    setupConfigShard(opCtx, 8 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(1, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3", "shard4"});
}

// ######################## PlacementHistory: Query by database ############################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_SingleDatabase) {
    /*Quering the placementHistory must report all the shards for every collection belonging to
     * the input db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection1", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard4", "shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(3, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3", "shard4", "shard5"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_MultipleDatabases) {
    /*Quering the placementHistory must report all the shards for every collection belonging to
     * the input db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db2", {"shard4"}},
                                 {Timestamp(4, 0), "db2.collection", {"shard5", "shard6"}},
                                 {Timestamp(5, 0), "db3", {"shard7"}}});

    setupConfigShard(opCtx, 7 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(5, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});

    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db2"), Timestamp(5, 0));

    assertSameShardSet(shards, {"shard4", "shard5", "shard6"});

    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db3"), Timestamp(5, 0));

    assertSameShardSet(shards, {"shard7"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_DifferentTimestamp) {
    /*Query the placementHistory at different timestamp should return different results*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2"}},
         {Timestamp(4, 0), "db.collection2", {"shard1", "shard2", "shard3"}},
         {Timestamp(5, 0), "db.collection3", {"shard1", "shard2", "shard3", "shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    // no shards at timestamp 0
    auto shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(0, 0));

    assertSameShardSet(shards, {});

    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(1, 0));

    assertSameShardSet(shards, {"shard1"});

    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(2, 0));

    assertSameShardSet(shards, {"shard1", "shard2"});

    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(4, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});

    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(5, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_SameTimestamp) {
    /*Having different namespaces for the same timestamp should not influece the expected result*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(1, 0), "db.collection", {"shard1", "shard2", "shard3"}},
         {Timestamp(1, 0), "db.collection2", {"shard1", "shard4", "shard5"}},
         {Timestamp(1, 0), "db2", {"shard6"}},
         {Timestamp(1, 0), "db2.collection", {"shard7", "shard8", "shard9"}}});

    setupConfigShard(opCtx, 9 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(1, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3", "shard4", "shard5"});

    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db2"), Timestamp(1, 0));

    assertSameShardSet(shards, {"shard6", "shard7", "shard8", "shard9"});
}

TEST_F(CatalogClientAggregationsTest,
       GetShardsThatOwnDataForDbAtClusterTime_InvertedTimestampOrder) {
    /*Ordering of document insertion into config.placementHistory must not matter*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(4, 0), "db", {"shard1"}},
         {Timestamp(3, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(2, 0), "db2", {"shard5"}},
         {Timestamp(1, 0), "db2.collection2", {"shard6", "shard7", "shard8"}}});

    setupConfigShard(opCtx, 8 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(4, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_NoShardsForDb) {
    /*Quering the placementHistory must report no shards if the list of shards belonging to every
     * collection and the db is empty*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(3, 0), "db", {}},
         {Timestamp(4, 0), "db.collection1", {}}});

    setupConfigShard(opCtx, 3 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(4, 0));

    assertSameShardSet(shards, {});

    // Note: at timestamp 3 the collection's shard list was not empty
    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(3, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_NewShardForDb) {
    /*Quering the placementHistory must correctly identify a new primary for the db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(3, 0), "db", {"shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(2, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});

    // At timestamp 3 the db shard list was updated with a new primary
    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(3, 0));

    assertSameShardSet(shards, {"shard4", "shard1", "shard2", "shard3"});
}

// ######################## PlacementHistory: Query the entire cluster ##################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_SingleDatabase) {
    /*Quering the placementHistory must report all the shards for every collection and db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection1", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard4", "shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(3, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3", "shard4", "shard5"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_MultipleDatabases) {
    /*Quering the placementHistory must report all the shards for every collection and db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db2", {"shard4"}},
                                 {Timestamp(4, 0), "db2.collection", {"shard5", "shard6"}},
                                 {Timestamp(5, 0), "db3", {"shard7"}}});

    setupConfigShard(opCtx, 7 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(5, 0));

    assertSameShardSet(shards,
                       {"shard1", "shard2", "shard3", "shard4", "shard5", "shard6", "shard7"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_DifferentTimestamp) {
    /*Query the placementHistory at different timestamp should return different results*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2"}},
         {Timestamp(4, 0), "db.collection2", {"shard1", "shard2", "shard3"}},
         {Timestamp(5, 0), "db.collection3", {"shard1", "shard2", "shard3", "shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    // no shards at timestamp 0
    auto shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(0, 0));

    assertSameShardSet(shards, {});

    shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(1, 0));

    assertSameShardSet(shards, {"shard1"});

    shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(2, 0));

    assertSameShardSet(shards, {"shard1", "shard2"});

    shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(4, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});

    shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(5, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_SameTimestamp) {
    /*Having different namespaces for the same timestamp should not influence the expected
     * result*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(1, 0), "db.collection", {"shard1", "shard2", "shard3"}},
         {Timestamp(1, 0), "db.collection2", {"shard1", "shard4", "shard5"}},
         {Timestamp(1, 0), "db2", {"shard6"}},
         {Timestamp(1, 0), "db2.collection", {"shard7", "shard8", "shard9"}}});

    setupConfigShard(opCtx, 9 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(1, 0));

    assertSameShardSet(
        shards,
        {"shard1", "shard2", "shard3", "shard4", "shard5", "shard6", "shard7", "shard8", "shard9"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_InvertedTimestampOrder) {
    /*Ordering of document insertion into config.placementHistory must not matter*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(4, 0), "db", {"shard1"}},
         {Timestamp(3, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(2, 0), "db2", {"shard5"}},
         {Timestamp(1, 0), "db2.collection2", {"shard6", "shard7", "shard8"}}});

    setupConfigShard(opCtx, 8 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(4, 0));

    assertSameShardSet(
        shards, {"shard1", "shard2", "shard3", "shard4", "shard5", "shard6", "shard7", "shard8"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_NoShards) {
    /*Quering the placementHistory must report no shards if the list of shards belonging to
     * every db.collection and db is empty*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(3, 0), "db", {}},
         {Timestamp(4, 0), "db.collection1", {}}});

    setupConfigShard(opCtx, 3 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(4, 0));

    assertSameShardSet(shards, {});

    // Note: at timestamp 3 the collection was still sharded
    shards = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(3, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});
}

// ######################## PlacementHistory: Regex Stage #####################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_RegexStage_ConfigSystem) {
    /*The regex stage must match correctly the config.system.namespaces collection*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "config", {"shard1"}},
         {Timestamp(2, 0), "config.system.collections", {"shard2", "shard3"}},
         {Timestamp(3, 0), "config.systemXcollections", {"shard4", "shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    // testing config.system.collections
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("config.system.collections"), Timestamp(7, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_RegexStage_NssWithPrefix) {
    /*The regex stage must match correctly the input namespaces*/
    auto opCtx = operationContext();

    // shards from 4, 5, 6 should never be returned
    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},

         {Timestamp(3, 0), "dbcollection1", {"shard4"}},
         {Timestamp(4, 0), "dbX", {"shard4", "shard5", "shard6"}},
         {Timestamp(5, 0), "dbX.collection1", {"shard4", "shard5", "shard6"}},
         {Timestamp(6, 0), "dbXcollection1", {"shard4", "shard5", "shard6"}},
         {Timestamp(7, 0), "Xdb.collection1", {"shard4", "shard5", "shard6"}},
         {Timestamp(8, 0), "Xdbcollection1", {"shard4", "shard5", "shard6"}},

         {Timestamp(9, 0), "db.Xcollection1", {"shard7", "shard8", "shard9"}},
         {Timestamp(10, 0), "db.collection1X", {"shard7", "shard8", "shard9"}},
         {Timestamp(11, 0), "db.collection1.X", {"shard7", "shard8", "shard9"}},
         {Timestamp(12, 0), "db.X.collection1", {"shard7", "shard8", "shard9"}}});

    setupConfigShard(opCtx, 9 /*nShards*/);

    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(12, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});

    // no data must be returned since the namespace is not found
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("d.collection1"), Timestamp(12, 0));

    assertSameShardSet(shards, {});

    // database exists
    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(12, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3", "shard7", "shard8", "shard9"});

    // database does not exist
    shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("d"), Timestamp(12, 0));

    assertSameShardSet(shards, {});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_RegexStage_DbWithSymbols) {
    /*The regex stage must correctly escape special character*/
    auto opCtx = operationContext();

    // shards >= 10 should never be returned
    setupConfigPlacementHistory(
        opCtx,

        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection", {"shard1", "shard2", "shard3"}},
#ifndef _WIN32
         {Timestamp(3, 0), "db*db", {"shard11"}},
         {Timestamp(4, 0), "db|db", {"shard12"}},
         {Timestamp(5, 0), "db*db.collection1", {"shard13"}},
         {Timestamp(6, 0), "db|db.collection1", {"shard14"}},
#endif
         {Timestamp(7, 0), "db_db", {"shard10"}},
         {Timestamp(8, 0), "db_db.collection1", {"shard11", "shard12", "shard13"}}});

    setupConfigShard(opCtx, 14 /*nShards*/);

    // db|db , db*db  etc... must not be found when quering by database
    auto shards = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(10, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});

    // db|db , db*db  etc... must not be found when quering by collection
    shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection"), Timestamp(10, 0));

    assertSameShardSet(shards, {"shard1", "shard2", "shard3"});
}

// ######################## PlacementHistory: SnapshotTooOld #####################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_SnapshotTooOld) {
    /*Quering the placementHistory must throw SnapshotTooOld when the returned list of shards
    contains at least one shard no longer active*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(3, 0), "db.collection2", {"shard1", "shard2", "shard3"}}});

    setupConfigShard(opCtx, 2 /*nShards*/);

    ASSERT_THROWS_CODE(catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
                           opCtx, NamespaceString("db"), Timestamp(4, 0)),
                       DBException,
                       ErrorCodes::SnapshotTooOld);

    ASSERT_THROWS_CODE(catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
                           opCtx, NamespaceString("db.collection1"), Timestamp(4, 0)),
                       DBException,
                       ErrorCodes::SnapshotTooOld);

    ASSERT_THROWS_CODE(catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(4, 0)),
                       DBException,
                       ErrorCodes::SnapshotTooOld);
}

// ######################## PlacementHistory: EmptyHistory #####################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_EmptyHistory) {
    // Quering an empty placementHistory must return an empty vector
    auto opCtx = operationContext();

    // no shards must be returned
    auto shards = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(4, 0));

    assertSameShardSet(shards, {});

    // no shards must be returned
    auto shards2 = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(4, 0));

    ASSERT_EQ(0U, shards2.size());

    // no shards must be returned
    auto shards3 = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(4, 0));

    ASSERT_EQ(0U, shards3.size());
}

// ######################## PlacementHistory: InvalidOptions #####################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_InvalidOptions) {
    /*Testing input validation*/
    auto opCtx = operationContext();

    // a namespace with collection must be provided
    ASSERT_THROWS_CODE(catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
                           opCtx, NamespaceString(""), Timestamp(0, 0)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
                           opCtx, NamespaceString("db"), Timestamp(0, 0)),
                       DBException,
                       ErrorCodes::InvalidOptions);

    // a namespace with only db must be provided
    ASSERT_THROWS_CODE(catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
                           opCtx, NamespaceString("db.collection"), Timestamp(0, 0)),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
                           opCtx, NamespaceString(""), Timestamp(0, 0)),
                       DBException,
                       ErrorCodes::InvalidOptions);
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
