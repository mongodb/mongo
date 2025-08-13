/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/ddl_lock_manager.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/vector_clock/vector_clock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const Timestamp kDawnOfTime(0, 1);

class DDLCoordinatorServiceMock : public DDLLockManager::Recoverable {
public:
    void waitForRecovery(OperationContext*) const override {}
};

void assertSameHistoricalPlacement(HistoricalPlacement historicalPlacement,
                                   std::vector<std::string> expectedSet) {
    auto retrievedSet = historicalPlacement.getShards();
    ASSERT_EQ(retrievedSet.size(), expectedSet.size());
    std::sort(retrievedSet.begin(), retrievedSet.end());
    std::sort(expectedSet.begin(), expectedSet.end());
    for (size_t i = 0; i < retrievedSet.size(); i++) {
        ASSERT_EQ(retrievedSet[i], expectedSet[i]);
    }
    ASSERT_EQ(historicalPlacement.getStatus(), HistoricalPlacementStatus::OK);
}

class GetHistoricalPlacementTestFixture : public ConfigServerTestFixture {
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
        operationContext()->setAlwaysInterruptAtStepDownOrUp_UNSAFE();
        DDLLockManager::get(getServiceContext())->setRecoverable(_recoverable.get());


        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        client.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                             {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});

        ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());

        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->initializeIfNeeded(operationContext(), /* term */ 1);

        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->interrupt();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ConfigServerTestFixture::tearDown();
    }

    /* Generate and insert the entries into the config.shards collection */
    void setupConfigShard(OperationContext* opCtx,
                          int nShards,
                          Timestamp configTime = Timestamp(1000000, 0)) {
        for (auto& doc : _generateConfigShardSampleData(nShards)) {
            ASSERT_OK(
                insertToConfigCollection(opCtx, NamespaceString::kConfigsvrShardsNamespace, doc));
        }

        // Advance the config time to 'configTime'.
        VectorClock::get(opCtx)->advanceConfigTime_forTest(LogicalTime(configTime));
    }

    /* Insert the entries into the config.placementHistory collection
     *  Generate a unique random UUID for a collection namespace
     */
    void setupConfigPlacementHistory(OperationContext* opCtx,
                                     std::vector<PlacementDescriptor>&& entries,
                                     bool generateInitDocumentAtDawnOfTime = true) {
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

                if (nssToUuid.find(nss.toString_forTest()) == nssToUuid.end())
                    nssToUuid.emplace(nss.toString_forTest(), UUID::gen());

                const UUID& collUuid = nssToUuid.at(nss.toString_forTest());
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

        if (generateInitDocumentAtDawnOfTime) {
            // Insert the initial document at the dawn of time.
            NamespacePlacementType initialDoc(
                ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker,
                kDawnOfTime,
                {});
            ASSERT_OK(insertToConfigCollection(
                opCtx, NamespaceString::kConfigsvrPlacementHistoryNamespace, initialDoc.toBSON()));
        }
    }

    ShardingCatalogManager& shardingCatalogManager() {
        return *ShardingCatalogManager::get(operationContext());
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

    std::unique_ptr<DDLCoordinatorServiceMock> _recoverable =
        std::make_unique<DDLCoordinatorServiceMock>();
};

TEST_F(GetHistoricalPlacementTestFixture, queriesOnShardedCollectionReturnExpectedPlacement) {
    /*Querying the placementHistory for a sharded collection should return the shards that owned the
     * collection at the given clusterTime*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(2, 0), "db.collection1", {"shard1", "shard2"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard3", "shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    // 2 shards must own collection1
    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2"});

    // 2 shards must own collection2
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection2"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard3", "shard4"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_ShardedCollectionWithPrimary) {
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
    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_UnshardedCollection) {
    /*Quering the placementHistory must report the primary shard for unsharded or non-existing
     * collections*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db2", {"shard2"}},
                                 {Timestamp(3, 0), "db3", {"shard3"}}});

    setupConfigShard(opCtx, 3 /*nShards*/);

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db2.collection"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard2"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db3.collection"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard3"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_DifferentTimestamp) {
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
    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), kDawnOfTime);

    assertSameHistoricalPlacement(historicalPlacement, {});
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(2, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_SameTimestamp) {
    /*Having different namespaces for the same timestamp should not influence the expected result*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(1, 0), "db.collection", {"shard1", "shard2", "shard3"}},
         {Timestamp(1, 0), "db.collection2", {"shard1", "shard4", "shard5"}},
         {Timestamp(1, 0), "db2", {"shard6"}},
         {Timestamp(1, 0), "db2.collection", {"shard7", "shard8", "shard9"}}});

    setupConfigShard(opCtx, 9 /*nShards*/);

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection2"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard4", "shard5"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db2.collection"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard7", "shard8", "shard9"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_InvertedTimestampOrder) {
    /*Ordering of document insertion into config.placementHistory must not matter*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(4, 0), "db", {"shard1"}},
         {Timestamp(3, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(2, 0), "db2", {"shard5"}},
         {Timestamp(1, 0), "db2.collection2", {"shard6", "shard7", "shard8"}}});

    setupConfigShard(opCtx, 8 /*nShards*/);

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard2", "shard3", "shard4"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_ReturnPrimaryShardWhenNoShards) {
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

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection2"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1"});

    // Note: at timestamp 3 the collection's shard list is not empty
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection2"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_PrimaryShardNotIncludedWhenNotBearingData) {
    /*
     * The primary shard value must be excluded from the returned result when it is not included in
     * the set of data-bearing shards for the queried collection.
     */
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(2, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(3, 0), "db", {"shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(2, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard2", "shard3", "shard4"});

    // Note: the primary shard is shard5 at timestamp 3
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard2", "shard3", "shard4"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_WithMarkers) {
    auto opCtx = operationContext();
    const Timestamp previousInitializationTime(1, 0);
    const Timestamp latestInitializationTime(3, 0);
    PlacementDescriptor startFcvMarker = {
        previousInitializationTime,
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {"shard1", "shard2", "shard3", "shard4", "shard5"}};
    PlacementDescriptor endFcvMarker = {
        latestInitializationTime,
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {}};

    setupConfigPlacementHistory(
        opCtx,
        {// Entries set at initialization time
         startFcvMarker,
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(2, 0), "db", {"shard4"}},
         {Timestamp(2, 0), "db.collection2", {"shard1", "shard2", "shard3"}},
         endFcvMarker,
         // Entries set after initialization
         {Timestamp(4, 0), "db", {"shard1"}},
         {Timestamp(5, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(6, 0), "db.collection1", {}}},
        false /*generateInitDocumentAtDawnOfTime*/);

    setupConfigShard(opCtx, 4 /*nShards*/);

    // A query that predates the earliest initialization doc produces a 'NotAvailable' result.
    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), kDawnOfTime);
    ASSERT_EQ(historicalPlacement.getStatus(), HistoricalPlacementStatus::NotAvailable);
    ASSERT(historicalPlacement.getShards().empty());

    // Asking for a timestamp before the closing marker should return the shards from the first
    // marker of the fcv upgrade. As result, "isExact" is expected to be false
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(2, 0));
    assertSameHistoricalPlacement(historicalPlacement,
                                  {"shard1", "shard2", "shard3", "shard4", "shard5"});

    // Asking for a timestamp after the closing marker should return the expected shards
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(3, 0));
    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(6, 0));
    assertSameHistoricalPlacement(historicalPlacement, {"shard1"});
}

// ######################## PlacementHistory: Query by database ############################
TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_SingleDatabase) {
    /*Quering the placementHistory must report all the shards for every collection belonging to
     * the input db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection1", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard4", "shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement,
                                  {"shard1", "shard2", "shard3", "shard4", "shard5"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_MultipleDatabases) {
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

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db2"), Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard4", "shard5", "shard6"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db3"), Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard7"});
}

TEST_F(GetHistoricalPlacementTestFixture, dbLevelSearch_DifferentTimestamp) {
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
    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), kDawnOfTime);

    assertSameHistoricalPlacement(historicalPlacement, {});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(2, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_SameTimestamp_repeated) {
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

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement,
                                  {"shard1", "shard2", "shard3", "shard4", "shard5"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db2"), Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard6", "shard7", "shard8", "shard9"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_InvertedTimestampOrder_repeated) {
    /*Ordering of document insertion into config.placementHistory must not matter*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(4, 0), "db", {"shard1"}},
         {Timestamp(3, 0), "db.collection1", {"shard2", "shard3", "shard4"}},
         {Timestamp(2, 0), "db2", {"shard5"}},
         {Timestamp(1, 0), "db2.collection2", {"shard6", "shard7", "shard8"}}});

    setupConfigShard(opCtx, 8 /*nShards*/);

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_NoShardsForDb) {
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

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});

    // Note: at timestamp 3 the collection's shard list was not empty
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_NewShardForDb) {
    /*Quering the placementHistory must correctly identify a new primary for the db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(3, 0), "db", {"shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(2, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    // At timestamp 3 the db shard list was updated with a new primary
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard4", "shard1", "shard2", "shard3"});
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_WithMarkers_repeated) {
    auto opCtx = operationContext();
    PlacementDescriptor _startFcvMarker = {
        Timestamp(1, 0),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {"shard1", "shard2", "shard3", "shard4", "shard5"}};
    PlacementDescriptor _endFcvMarker = {
        Timestamp(3, 0),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
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
    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(2, 0));
    assertSameHistoricalPlacement(historicalPlacement,
                                  {"shard1", "shard2", "shard3", "shard4", "shard5"});

    // Asking for a timestamp after the closing marker should return the expected shards
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(3, 0));
    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(7, 0));
    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

// ######################## PlacementHistory: Query the entire cluster ##################
TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_SingleDatabase) {
    /*Quering the placementHistory must report all the shards for every collection and db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection1", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard4", "shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    auto historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement,
                                  {"shard1", "shard2", "shard3", "shard4", "shard5"});
}

TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_MultipleDatabases) {
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
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(5, 0));

    assertSameHistoricalPlacement(
        historicalPlacement,
        {"shard1", "shard2", "shard3", "shard4", "shard5", "shard6", "shard7"});
}

TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_DifferentTimestamp) {
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
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, kDawnOfTime);

    assertSameHistoricalPlacement(historicalPlacement, {});

    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(1, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1"});

    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(2, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2"});

    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(5, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});
}

TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_SameTimestamp) {
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
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(1, 0));

    assertSameHistoricalPlacement(
        historicalPlacement,
        {"shard1", "shard2", "shard3", "shard4", "shard5", "shard6", "shard7", "shard8", "shard9"});
}

TEST_F(GetHistoricalPlacementTestFixture,
       GetShardsThatOwnDataAtClusterTime_InvertedTimestampOrder) {
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
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(4, 0));

    assertSameHistoricalPlacement(
        historicalPlacement,
        {"shard1", "shard2", "shard3", "shard4", "shard5", "shard6", "shard7", "shard8"});
}

TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_NoShards) {
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
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(4, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});

    // Note: at timestamp 3 the collection was still sharded
    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(3, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_WithMarkers) {
    auto opCtx = operationContext();
    PlacementDescriptor _startFcvMarker = {
        Timestamp(1, 0),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {"shard1", "shard2", "shard3", "shard4"}};
    PlacementDescriptor _endFcvMarker = {
        Timestamp(3, 0),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
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
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(2, 0));
    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3", "shard4"});

    // Asking for a timestamp after the closing marker should return the expected shards
    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(3, 0));
    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(5, 0));
    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

TEST_F(GetHistoricalPlacementTestFixture,
       GetShardsThatOwnDataAtClusterTime_RegexStage_NssWithPrefix) {
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

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection1"), Timestamp(12, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    // no data must be returned since the namespace is not found
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("d.collection1"), Timestamp(12, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});

    // database exists
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(12, 0));

    assertSameHistoricalPlacement(historicalPlacement,
                                  {"shard1", "shard2", "shard3", "shard7", "shard8", "shard9"});

    // database does not exist
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("d"), Timestamp(12, 0));

    assertSameHistoricalPlacement(historicalPlacement, {});
}

TEST_F(GetHistoricalPlacementTestFixture,
       GetShardsThatOwnDataAtClusterTime_RegexStage_DbWithSymbols) {
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
    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(10, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});

    // db|db , db*db  etc... must not be found when quering by collection
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx, NamespaceString::createNamespaceString_forTest("db.collection"), Timestamp(10, 0));

    assertSameHistoricalPlacement(historicalPlacement, {"shard1", "shard2", "shard3"});
}

// ######################## PlacementHistory: EmptyHistory #####################
TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_EmptyHistory) {
    // Setup a shard to perform a write into the config DB and initialize a committed OpTime
    // (required to perform a snapshot read of the placementHistory).
    setupShards({ShardType("shardName", "host01")});

    auto opCtx = operationContext();

    setupConfigShard(opCtx, 1 /* nShards */);

    // Querying an empty placementHistory must return a "NotAvailable" result for all kinds of
    // search. Collection-level query
    {
        auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            Timestamp(4, 0));
        ASSERT_EQ(historicalPlacement.getStatus(), HistoricalPlacementStatus::NotAvailable);
        ASSERT(historicalPlacement.getShards().empty());
    }

    // DB-level query
    {
        auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx, NamespaceString::createNamespaceString_forTest("db"), Timestamp(4, 0));

        ASSERT_EQ(0U, historicalPlacement.getShards().size());

        ASSERT_EQ(historicalPlacement.getStatus(), HistoricalPlacementStatus::NotAvailable);
        ASSERT(historicalPlacement.getShards().empty());
    }

    // Cluster-level query
    {
        auto historicalPlacement =
            shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(4, 0));

        ASSERT_EQ(historicalPlacement.getStatus(), HistoricalPlacementStatus::NotAvailable);
        ASSERT(historicalPlacement.getShards().empty());
    }
}

// ######################## PlacementHistory: InvalidOptions #####################
TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_InvalidOptions) {
    /*Testing input validation*/
    auto opCtx = operationContext();

    // Invalid namespaces are rejected
    ASSERT_THROWS_CODE(shardingCatalogManager().getHistoricalPlacement(
                           opCtx, NamespaceString::createNamespaceString_forTest(""), kDawnOfTime),
                       DBException,
                       ErrorCodes::InvalidOptions);

    // 'config', 'local' and 'admin' namespaces are not supported.
    ASSERT_THROWS_CODE(shardingCatalogManager().getHistoricalPlacement(
                           opCtx, NamespaceString(DatabaseName::kAdmin), kDawnOfTime),
                       DBException,
                       ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(shardingCatalogManager().getHistoricalPlacement(
                           opCtx, NamespaceString(DatabaseName::kLocal), kDawnOfTime),
                       DBException,
                       ErrorCodes::InvalidOptions);
}

// ######################## PlacementHistory: Clean-up #####################
TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_CleanUp) {
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
        shardingCatalogManager().getHistoricalPlacement(
            opCtx, NamespaceString::createNamespaceString_forTest("db"), earliestClusterTime),
        {"shard1", "shard2", "shard3", "shard4"});
    assertSameHistoricalPlacement(
        shardingCatalogManager().getHistoricalPlacement(
            opCtx, NamespaceString::createNamespaceString_forTest("db"), earliestClusterTime - 1),
        approximatedPlacement);

    // db.collection1
    assertSameHistoricalPlacement(
        shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            earliestClusterTime),
        {"shard2", "shard3", "shard4"});
    assertSameHistoricalPlacement(
        shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            earliestClusterTime - 1),
        approximatedPlacement);

    // db.collection2
    assertSameHistoricalPlacement(
        shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection2"),
            earliestClusterTime),
        {"shard1", "shard4"});
    assertSameHistoricalPlacement(
        shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection2"),
            Timestamp(11, 0)),
        approximatedPlacement);

    // Whole cluster
    assertSameHistoricalPlacement(
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, earliestClusterTime),
        {"shard1", "shard2", "shard3", "shard4"});
    assertSameHistoricalPlacement(
        shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, Timestamp(11, 0)),
        approximatedPlacement);
}

TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_CleanUp_NewMarkers) {
    auto opCtx = operationContext();
    PlacementDescriptor startFcvMarker = {
        Timestamp(1, 0),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {"shard1", "shard2", "shard3", "shard4"}};
    PlacementDescriptor endFcvMarker = {
        Timestamp(3, 0),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
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
    auto historicalPlacement_coll1 = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        earliestClusterTime - 1);

    ShardingCatalogManager::get(opCtx)->cleanUpPlacementHistory(opCtx, earliestClusterTime);

    auto historicalPlacement_cleanup_coll1 = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        earliestClusterTime - 1);

    // before cleanup
    assertSameHistoricalPlacement(historicalPlacement_coll1, {"shard1", "shard2"});

    // after cleanup
    assertSameHistoricalPlacement(historicalPlacement_cleanup_coll1, {"shard1", "shard2"});
}

TEST_F(
    GetHistoricalPlacementTestFixture,
    Given_CurrentClusterTime_When_PlacementhHistoryRequestedInTheFuture_Then_ReturnPlacementHistoryStatusFutureClusterTime) {
    auto opCtx = operationContext();

    // Set up config shard and placement history information.
    Timestamp placementHistoryTs(10, 0);
    setupConfigPlacementHistory(opCtx,
                                {{placementHistoryTs, "db", {"shard1"}},
                                 {placementHistoryTs, "db.collection1", {"shard1", "shard2"}}});
    setupConfigShard(opCtx, 3 /*nShards*/);

    const auto& vcTime = VectorClock::get(opCtx)->getTime();
    Timestamp currentConfigTime = vcTime.configTime().asTimestamp();

    // Ensure that fetching placement history returns a set of active shards when requesting
    // placement history from the 'currentConfigTime'.
    {
        ASSERT_GREATER_THAN_OR_EQUALS(currentConfigTime, placementHistoryTs);

        auto collNss = NamespaceString::createNamespaceString_forTest("db.collection1");
        auto dbOnlyNss = NamespaceString::createNamespaceString_forTest("db");
        assertSameHistoricalPlacement(
            shardingCatalogManager().getHistoricalPlacement(opCtx, collNss, currentConfigTime),
            {"shard1", "shard2"});
        assertSameHistoricalPlacement(
            shardingCatalogManager().getHistoricalPlacement(opCtx, dbOnlyNss, currentConfigTime),
            {"shard1", "shard2"});
        assertSameHistoricalPlacement(
            shardingCatalogManager().getHistoricalPlacement(opCtx, boost::none, currentConfigTime),
            {"shard1", "shard2"});
    }

    // Ensure that fetching placement history returns HistoricalPlacementStatus::FutureClusterTime,
    // when requesting placement history from the future config time.
    {
        // Ensure 'tsInTheFuture' is greater than 'currentConfigTime'.
        Timestamp timeInTheFuture = currentConfigTime + 1;
        ASSERT_GREATER_THAN(timeInTheFuture, currentConfigTime);

        auto collNss = NamespaceString::createNamespaceString_forTest("db.collection1");
        auto dbOnlyNss = NamespaceString::createNamespaceString_forTest("db");
        ASSERT_EQ(shardingCatalogManager()
                      .getHistoricalPlacement(opCtx, collNss, timeInTheFuture)
                      .getStatus(),
                  HistoricalPlacementStatus::FutureClusterTime);
        ASSERT_EQ(shardingCatalogManager()
                      .getHistoricalPlacement(opCtx, dbOnlyNss, timeInTheFuture)
                      .getStatus(),
                  HistoricalPlacementStatus::FutureClusterTime);
        ASSERT_EQ(shardingCatalogManager()
                      .getHistoricalPlacement(opCtx, boost::none, timeInTheFuture)
                      .getStatus(),
                  HistoricalPlacementStatus::FutureClusterTime);
    }
}

}  // unnamed namespace
}  // namespace mongo
