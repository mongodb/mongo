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
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/config/config_server_test_fixture.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/idl/server_parameter_test_util.h"
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
        PlacementDescriptor(Timestamp timestamp, std::string ns, std::vector<std::string> shardsIds)
            : timestamp(std::move(timestamp)), ns(std::move(ns)), shardsIds(std::move(shardsIds)) {}

        Timestamp timestamp;
        std::string ns;
        std::vector<std::string> shardsIds;
    };

    void setUp() override {
        ConfigServerTestFixture::setUp();

        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->onShardingInitialization(operationContext(), true);

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->onStepDown();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ConfigServerTestFixture::tearDown();
    }

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
            auto nss = NamespaceString::createNamespaceString_forTest(entry.ns);
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

    // Allows the usage of transactions.
    ReadWriteConcernDefaultsLookupMock _lookupMock;

};  // CatalogClientAggregationsTest

void assertSameHistoricalPlacement(HistoricalPlacement historicalPlacement,
                                   std::vector<std::string> expectedSet,
                                   bool expectedIsExact = true) {
    auto retrievedSet = historicalPlacement.getShards();
    ASSERT_EQ(retrievedSet.size(), expectedSet.size());
    std::sort(retrievedSet.begin(), retrievedSet.end());
    std::sort(expectedSet.begin(), expectedSet.end());
    for (size_t i = 0; i < retrievedSet.size(); i++) {
        ASSERT_EQ(retrievedSet[i], expectedSet[i]);
    }
    ASSERT_EQ(historicalPlacement.getIsExact(), expectedIsExact);
}
}  // namespace

// ######################## PlacementHistory: Query by collection ##########################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_ShardedCollection) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    /*Quering the placementHistory for a sharded collection should return the shards that owned the
     * collection at the given clusterTime*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(2, 0), "db.collection1", {"shard1", "shard2"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard3", "shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    // 2 shards must own collection1
    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2"});

    // 2 shards must own collection2
    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection2"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard3", "shard4"});
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
    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
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

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db2.collection"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard2"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db3.collection"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard3"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_DifferentTimestamp) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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
    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(0, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(2, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_SameTimestamp) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection2"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard4", "shard5"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db2.collection"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard6", "shard7", "shard8", "shard9"});
}

TEST_F(CatalogClientAggregationsTest,
       GetShardsThatOwnDataForCollAtClusterTime_InvertedTimestampOrder) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    /*Ordering of document insertion into config.placementHistory must not matter*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(4, 0), "db", {"shard1"}},
         {Timestamp(3, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(2, 0), "db2", {"shard5"}},
         {Timestamp(1, 0), "db2.collection2", {"shard6", "shard7", "shard8"}}});

    setupConfigShard(opCtx, 8 /*nShards*/);

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest,
       GetShardsThatOwnDataForCollAtClusterTime_ReturnPrimaryShardWhenNoShards) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection2"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1"});

    // Note: at timestamp 3 the collection's shard list is not empty
    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection2"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_AddPrimaryShard) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    /*Quering the placementHistory must report the primary shard in addition to the list of shards
     * related to db.collection. Primary shards must always be returned*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(3, 0), "db", {"shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(2, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});

    // Note: the primary shard is shard5 at timestamp 3
    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard5", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest,
       GetShardsThatOwnDataForCollAtClusterTime_AddPrimaryShardAtSameTimestamp) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForCollAtClusterTime_WithMarkers) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    auto opCtx = operationContext();
    PlacementDescriptor _startFcvMarker = {
        Timestamp(1, 0),
        NamespaceString::kConfigsvrPlacementHistoryFcvMarkerNamespace.ns().toString(),
        {"shard1", "shard2", "shard3", "shard4", "shard5"}};
    PlacementDescriptor _endFcvMarker = {
        Timestamp(3, 0),
        NamespaceString::kConfigsvrPlacementHistoryFcvMarkerNamespace.ns().toString(),
        {}};

    // initialization
    setupConfigPlacementHistory(
        opCtx,
        {_startFcvMarker,
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(2, 0), "db", {"shard4"}},
         {Timestamp(2, 0), "db.collection2", {"shard1", "shard2", "shard3"}},
         _endFcvMarker});

    // after initialization-
    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(4, 0), "db", {"shard1"}},
         {Timestamp(5, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(6, 0), "db.collection1", {}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    // Asking for a timestamp before the closing marker should return the shards from the first
    // marker of the fcv upgrade. As result, "isExact" is expected to be false
    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(2, 0));
    assertSameHistoricalPlacement(
        historicalPlacement, {"shard1", "shard2", "shard3", "shard4", "shard5"}, false);

    // Asking for a timestamp after the closing marker should return the expected shards
    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(3, 0));
    assertSameHistoricalPlacement(
        historicalPlacement, {"shard1", "shard2", "shard3", "shard4"}, true);

    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString("db.collection1"), Timestamp(6, 0));
    assertSameHistoricalPlacement(historicalPlacement, {"shard1"}, true);
}

// ######################## PlacementHistory: Query by database ############################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_SingleDatabase) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    /*Quering the placementHistory must report all the shards for every collection belonging to
     * the input db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection1", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard4", "shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement,
                                  {"shard1", "shard2", "shard3", "shard4", "shard5"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_MultipleDatabases) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db2"), Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard4", "shard5", "shard6"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db3"), Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard7"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_DifferentTimestamp) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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
    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(0, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(2, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_SameTimestamp) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement,
                                  {"shard1", "shard2", "shard3", "shard4", "shard5"});

    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db2"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard6", "shard7", "shard8", "shard9"});
}

TEST_F(CatalogClientAggregationsTest,
       GetShardsThatOwnDataForDbAtClusterTime_InvertedTimestampOrder) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    /*Ordering of document insertion into config.placementHistory must not matter*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(4, 0), "db", {"shard1"}},
         {Timestamp(3, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(2, 0), "db2", {"shard5"}},
         {Timestamp(1, 0), "db2.collection2", {"shard6", "shard7", "shard8"}}});

    setupConfigShard(opCtx, 8 /*nShards*/);

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_NoShardsForDb) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});

    // Note: at timestamp 3 the collection's shard list was not empty
    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_NewShardForDb) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    /*Quering the placementHistory must correctly identify a new primary for the db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(3, 0), "db", {"shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(2, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    // At timestamp 3 the db shard list was updated with a new primary
    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard4", "shard1", "shard2", "shard3"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataForDbAtClusterTime_WithMarkers) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    auto opCtx = operationContext();
    PlacementDescriptor _startFcvMarker = {
        Timestamp(1, 0),
        NamespaceString::kConfigsvrPlacementHistoryFcvMarkerNamespace.ns().toString(),
        {"shard1", "shard2", "shard3", "shard4", "shard5"}};
    PlacementDescriptor _endFcvMarker = {
        Timestamp(3, 0),
        NamespaceString::kConfigsvrPlacementHistoryFcvMarkerNamespace.ns().toString(),
        {}};

    // initialization
    setupConfigPlacementHistory(
        opCtx,
        {_startFcvMarker,
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(2, 0), "db", {"shard4"}},
         {Timestamp(2, 0), "db.collection2", {"shard1", "shard2", "shard3"}},
         _endFcvMarker});

    // after initialization-
    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(4, 0), "db", {"shard1"}},
         {Timestamp(5, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(6, 0), "db.collection1", {}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    // Asking for a timestamp before the closing marker should return the shards from the first
    // marker of the fcv upgrade. As result, "isExact" is expected to be false
    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(2, 0));
    assertSameHistoricalPlacement(
        historicalPlacement, {"shard1", "shard2", "shard3", "shard4", "shard5"}, false);

    // Asking for a timestamp after the closing marker should return the expected shards
    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(3, 0));
    assertSameHistoricalPlacement(
        historicalPlacement, {"shard1", "shard2", "shard3", "shard4"}, true);

    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString("db"), Timestamp(7, 0));
    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"}, true);
}

// ######################## PlacementHistory: Query the entire cluster ##################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_SingleDatabase) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    /*Quering the placementHistory must report all the shards for every collection and db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection1", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard4", "shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    auto historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement,
                                  {"shard1", "shard2", "shard3", "shard4", "shard5"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_MultipleDatabases) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    /*Quering the placementHistory must report all the shards for every collection and db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db2", {"shard4"}},
                                 {Timestamp(4, 0), "db2.collection", {"shard5", "shard6"}},
                                 {Timestamp(5, 0), "db3", {"shard7"}}});

    setupConfigShard(opCtx, 7 /*nShards*/);

    auto historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(5, 0));

    assertSameHistoricalPlacement(
        historicalPlacement,
        {"shard1", "shard2", "shard3", "shard4", "shard5", "shard6", "shard7"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_DifferentTimestamp) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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
    auto historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(0, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});

    historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1"});

    historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(2, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2"});

    historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_SameTimestamp) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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

    auto historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(1, 0));

    assertSameHistoricalPlacement(
        historicalPlacement,
        {"shard1", "shard2", "shard3", "shard4", "shard5", "shard6", "shard7", "shard8", "shard9"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_InvertedTimestampOrder) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    /*Ordering of document insertion into config.placementHistory must not matter*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(4, 0), "db", {"shard1"}},
         {Timestamp(3, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(2, 0), "db2", {"shard5"}},
         {Timestamp(1, 0), "db2.collection2", {"shard6", "shard7", "shard8"}}});

    setupConfigShard(opCtx, 8 /*nShards*/);

    auto historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(4, 0));

    assertSameHistoricalPlacement(
        historicalPlacement,
        {"shard1", "shard2", "shard3", "shard4", "shard5", "shard6", "shard7", "shard8"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_NoShards) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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

    auto historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});

    // Note: at timestamp 3 the collection was still sharded
    historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_WithMarkers) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    auto opCtx = operationContext();
    PlacementDescriptor _startFcvMarker = {
        Timestamp(1, 0),
        NamespaceString::kConfigsvrPlacementHistoryFcvMarkerNamespace.ns().toString(),
        {"shard1", "shard2", "shard3", "shard4"}};
    PlacementDescriptor _endFcvMarker = {
        Timestamp(3, 0),
        NamespaceString::kConfigsvrPlacementHistoryFcvMarkerNamespace.ns().toString(),
        {}};

    // initialization
    setupConfigPlacementHistory(
        opCtx,
        {_startFcvMarker,
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(2, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection2", {"shard2"}},
         _endFcvMarker});

    // after initialization-
    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(4, 0), "db", {"shard2"}},
         {Timestamp(5, 0), "db.collection2", {"shard1", "shard2", "shard3"}}});

    setupConfigShard(opCtx, 3 /*nShards*/);

    // Asking for a timestamp before the closing marker should return the shards from the first
    // marker of the fcv upgrade
    auto historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(2, 0));
    assertSameHistoricalPlacement(
        historicalPlacement, {"shard1", "shard2", "shard3", "shard4"}, false);

    // Asking for a timestamp after the closing marker should return the expected shards
    historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(3, 0));
    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"}, true);

    historicalPlacement =
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(5, 0));
    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"}, true);
}

// ######################## PlacementHistory: Regex Stage #####################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_RegexStage_ConfigSystem) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    /*The regex stage must match correctly the config.system.namespaces collection*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "config", {"shard1"}},
         {Timestamp(2, 0), "config.system.collections", {"shard2", "shard3"}},
         {Timestamp(3, 0), "config.systemXcollections", {"shard4", "shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    // testing config.system.collections
    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx,
        NamespaceString::createNamespaceString_forTest("config.system.collections"),
        Timestamp(7, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_RegexStage_NssWithPrefix) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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

    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(12, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    // no data must be returned since the namespace is not found
    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("d.collection1"), Timestamp(12, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});

    // database exists
    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(12, 0));

    assertSameHistoricalPlacement(historicalPlacement,
                                  {"shard1", "shard2", "shard3", "shard7", "shard8", "shard9"});

    // database does not exist
    historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("d"), Timestamp(12, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_RegexStage_DbWithSymbols) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
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
    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(10, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    // db|db , db*db  etc... must not be found when quering by collection
    historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection"), Timestamp(10, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

// ######################## PlacementHistory: EmptyHistory #####################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_EmptyHistory) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    // Setup a shard to perform a write into the config DB and initialize a committed OpTime
    // (required to perform a snapshot read of the placementHistory).
    setupShards({ShardType("shardName", "host01")});

    // Quering an empty placementHistory must return an empty vector
    auto opCtx = operationContext();

    // no shards must be returned
    auto historicalPlacement = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});

    // no shards must be returned
    auto historicalPlacement2 = catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(4, 0));

    ASSERT_EQ(0U, historicalPlacement.getShards().size());

    // no shards must be returned
    auto shards3 = catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(4, 0));

    ASSERT_EQ(0U, historicalPlacement.getShards().size());
}

// ######################## PlacementHistory: InvalidOptions #####################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_InvalidOptions) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};
    /*Testing input validation*/
    auto opCtx = operationContext();

    // a namespace with collection must be provided
    ASSERT_THROWS_CODE(
        catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
            opCtx, NamespaceString::createNamespaceString_forTest(""), Timestamp(0, 0)),
        DBException,
        ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(
        catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
            opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(0, 0)),
        DBException,
        ErrorCodes::InvalidOptions);

    // a namespace with only db must be provided
    ASSERT_THROWS_CODE(catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
                           opCtx,
                           NamespaceString::createNamespaceString_forTest("db.collection"),
                           Timestamp(0, 0)),
                       DBException,
                       ErrorCodes::InvalidOptions);
    ASSERT_THROWS_CODE(
        catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
            opCtx, NamespaceString::createNamespaceString_forTest(""), Timestamp(0, 0)),
        DBException,
        ErrorCodes::InvalidOptions);
}

// ######################## PlacementHistory: Clean-up #####################
TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_CleanUp) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};

    auto opCtx = operationContext();

    // Insert the initial content
    setupConfigPlacementHistory(
        opCtx,
        {
            // One DB created before the time chosen for the cleanup
            {Timestamp(1, 0), "db", {"shard4"}},
            // One collection with entries before and after the chosen time of the cleanup
            {Timestamp(10, 0), "db.collection1", {"shard1"}},
            {Timestamp(20, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
            // One collection with multiple entries before the chosen time of the cleanup
            {Timestamp(11, 0), "db.collection2", {"shard2"}},
            {Timestamp(19, 0), "db.collection2", {"shard1", "shard4"}},
        });

    setupConfigShard(opCtx, 5 /*nShards*/);

    // Define the the earliest cluster time that needs to be preserved, then run the cleanup.
    const auto earliestClusterTime = Timestamp(20, 0);
    ShardingCatalogManager::get(opCtx)->cleanUpPlacementHistory(opCtx, earliestClusterTime);

    // Verify the behaviour of the API after the cleanup.
    // - Any query referencing a time >= earliestClusterTime is expected to return accurate data
    // based on the content inserted during the setup.
    // - Any query referencing a time < earliestClusterTime is expected to be answered with an
    // approximated value.
    const std::vector<std::string> approximatedPlacement{"shard1", "shard2", "shard3", "shard4"};

    // db
    assertSameHistoricalPlacement(
        catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
            opCtx, NamespaceString::createNamespaceString_forTest("db"), earliestClusterTime),
        {"shard1", "shard2", "shard3", "shard4"});
    assertSameHistoricalPlacement(
        catalogClient()->getShardsThatOwnDataForDbAtClusterTime(
            opCtx, NamespaceString::createNamespaceString_forTest("db"), earliestClusterTime - 1),
        approximatedPlacement,
        false /* expectedIsExact*/);

    // db.collection1
    assertSameHistoricalPlacement(
        catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            earliestClusterTime),
        {"shard2", "shard3", "shard4"});
    assertSameHistoricalPlacement(
        catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            earliestClusterTime - 1),
        approximatedPlacement,
        false /* expectedIsExact*/);

    // db.collection2
    assertSameHistoricalPlacement(
        catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection2"),
            earliestClusterTime),
        {"shard1", "shard4"});
    assertSameHistoricalPlacement(
        catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection2"),
            Timestamp(11, 0)),
        approximatedPlacement,
        false /* expectedIsExact*/);

    // Whole cluster
    assertSameHistoricalPlacement(
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, earliestClusterTime),
        {"shard1", "shard2", "shard3", "shard4"});
    assertSameHistoricalPlacement(
        catalogClient()->getShardsThatOwnDataAtClusterTime(opCtx, Timestamp(11, 0)),
        approximatedPlacement,
        false /* expectedIsExact*/);
}

TEST_F(CatalogClientAggregationsTest, GetShardsThatOwnDataAtClusterTime_CleanUp_NewMarkers) {
    RAIIServerParameterControllerForTest featureFlagHistoricalPlacementShardingCatalog{
        "featureFlagHistoricalPlacementShardingCatalog", true};

    auto opCtx = operationContext();
    PlacementDescriptor startFcvMarker = {
        Timestamp(1, 0),
        NamespaceString::kConfigsvrPlacementHistoryFcvMarkerNamespace.ns().toString(),
        {"shard1", "shard2", "shard3", "shard4"}};
    PlacementDescriptor endFcvMarker = {
        Timestamp(3, 0),
        NamespaceString::kConfigsvrPlacementHistoryFcvMarkerNamespace.ns().toString(),
        {}};

    // initialization
    setupConfigPlacementHistory(
        opCtx,
        {startFcvMarker,
         endFcvMarker,
         {Timestamp(10, 0), "db2", {"shard1"}},
         {Timestamp(10, 0), "db.collection2", {"shard1", "shard2"}},
         {Timestamp(10, 0), "db.collection1", {"shard1", "shard2"}},
         {Timestamp(30, 0), "db.collection1", {"shard1", "shard2", "shard3"}}});

    setupConfigShard(opCtx, 3 /*nShards*/);

    // Initialization markers are replaced at the earliestClusterTime
    const auto earliestClusterTime = Timestamp(20, 0);
    auto historicalPlacement_coll1 = catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        earliestClusterTime - 1);

    ShardingCatalogManager::get(opCtx)->cleanUpPlacementHistory(opCtx, earliestClusterTime);

    auto historicalPlacement_cleanup_coll1 =
        catalogClient()->getShardsThatOwnDataForCollAtClusterTime(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            earliestClusterTime - 1);

    // before cleanup
    assertSameHistoricalPlacement(historicalPlacement_coll1, {"shard1", "shard2"}, true /*exact*/);
    // after cleanup
    assertSameHistoricalPlacement(
        historicalPlacement_cleanup_coll1, {"shard1", "shard2"}, false /*exact*/);
}

// ############################# Indexes #############################
TEST_F(CatalogClientAggregationsTest, TestCollectionAndIndexesAggregationWithNoIndexes) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("TestDB.TestColl");
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
    auto [collection, indexes] = catalogClient()->getCollectionAndShardingIndexCatalogEntries(
        operationContext(), nss, {repl::ReadConcernLevel::kSnapshotReadConcern});

    ASSERT_EQ(indexes.size(), 0);
    ASSERT_EQ(collection.getEpoch(), placementVersion.epoch());
    ASSERT_EQ(collection.getTimestamp(), placementVersion.getTimestamp());
    ASSERT_EQ(collection.getUuid(), uuid);
}

TEST_F(CatalogClientAggregationsTest, TestCollectionAndIndexesWithIndexes) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("TestDB.TestColl");
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

    auto [collection, indexes] = catalogClient()->getCollectionAndShardingIndexCatalogEntries(
        operationContext(), nss, {repl::ReadConcernLevel::kSnapshotReadConcern});

    ASSERT_EQ(indexes.size(), 2);
    ASSERT_EQ(collection.getEpoch(), placementVersion.epoch());
    ASSERT_EQ(collection.getTimestamp(), placementVersion.getTimestamp());
    ASSERT_EQ(collection.getUuid(), uuid);
}

TEST_F(CatalogClientAggregationsTest, TestCollectionAndIndexesWithMultipleCollections) {
    const NamespaceString nssColl1 =
        NamespaceString::createNamespaceString_forTest("TestDB.Collection1");
    const NamespaceString nssColl2 =
        NamespaceString::createNamespaceString_forTest("TestDB.Collection2");
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

    auto [collection, indexes] = catalogClient()->getCollectionAndShardingIndexCatalogEntries(
        operationContext(), nssColl1, {repl::ReadConcernLevel::kSnapshotReadConcern});

    ASSERT_EQ(indexes.size(), 1);
    ASSERT_EQ(collection.getEpoch(), placementVersion.epoch());
    ASSERT_EQ(collection.getTimestamp(), placementVersion.getTimestamp());
    ASSERT_EQ(collection.getUuid(), uuidColl1);
}
}  // namespace mongo
