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
#include "mongo/db/global_catalog/sharding_catalog_client_mock.h"
#include "mongo/db/pipeline/change_stream_read_mode.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <algorithm>
#include <vector>

#include <boost/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const Timestamp kDawnOfTime(0, 1);

class DDLCoordinatorServiceMock : public DDLLockManager::Recoverable {
public:
    void waitForRecovery(OperationContext*) const override {}
};

// Helper struct to easily build 'HistoricalPlacement' objects for testing purposes, using a fluent
// interface.
struct ExpectedResponseBuilder {
    ExpectedResponseBuilder() : ExpectedResponseBuilder(HistoricalPlacementStatus::OK) {}
    explicit ExpectedResponseBuilder(HistoricalPlacementStatus status) {
        value.setStatus(status);
    }

    ExpectedResponseBuilder& setShards(std::vector<std::string> shards) {
        std::vector<ShardId> transformed;
        std::transform(shards.begin(),
                       shards.end(),
                       std::back_inserter(transformed),
                       [](const auto& value) { return ShardId(value); });
        value.setShards(std::move(transformed));
        return *this;
    }

    ExpectedResponseBuilder& setAnyRemovedShardDetected(
        const boost::optional<bool>& anyRemovedShardDetected) {
        value.setAnyRemovedShardDetected(anyRemovedShardDetected);
        return *this;
    }

    ExpectedResponseBuilder& setAnyRemovedShardDetected(bool value, ChangeStreamReadMode readMode) {
        if (readMode == ChangeStreamReadMode::kIgnoreRemovedShards) {
            setAnyRemovedShardDetected(value);
        }
        return *this;
    }

    ExpectedResponseBuilder& setOpenCursorAt(const boost::optional<Timestamp>& openCursorAt) {
        value.setOpenCursorAt(openCursorAt);
        return *this;
    }

    ExpectedResponseBuilder& setNextPlacementChangedAt(
        const boost::optional<Timestamp>& nextPlacementChanged) {
        value.setNextPlacementChangedAt(nextPlacementChanged);
        return *this;
    }

    HistoricalPlacement value;
};

// Check if the two placements are completely equal.
void assertPlacementsEqual(const HistoricalPlacement& expected, const HistoricalPlacement& actual) {
    auto sortShards = [](std::vector<ShardId> values) {
        std::sort(values.begin(), values.end());
        return values;
    };

    ASSERT_EQ(expected.getStatus(), actual.getStatus());
    ASSERT_EQ(sortShards(expected.getShards()), sortShards(actual.getShards()));
    ASSERT_EQ(expected.getAnyRemovedShardDetected(), actual.getAnyRemovedShardDetected());
    ASSERT_EQ(expected.getOpenCursorAt(), actual.getOpenCursorAt());
    ASSERT_EQ(expected.getNextPlacementChangedAt(), actual.getNextPlacementChangedAt());
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
        TransactionCoordinatorService::get(operationContext())->interruptForStepDown();
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

    /**
     * Overrides the sharding catalog client, so that we can inject the set of shards visible to the
     * shard registry during testing.
     */
    std::unique_ptr<ShardingCatalogClient> makeShardingCatalogClient() override {
        class StaticCatalogClient final : public ShardingCatalogClientMock {
        public:
            StaticCatalogClient(const std::vector<std::string>& shardIds) : _shardIds(shardIds) {}

            repl::OpTimeWith<std::vector<ShardType>> getAllShards(
                OperationContext* opCtx,
                repl::ReadConcernLevel readConcern,
                BSONObj filter) override {

                std::vector<ShardType> shards;
                for (const auto& shardId : _shardIds) {
                    ShardType shard;
                    shard.setName(shardId);
                    shard.setHost(shardId + ":12345");
                    shards.push_back(std::move(shard));
                }

                return repl::OpTimeWith<std::vector<ShardType>>(std::move(shards));
            }

        private:
            const std::vector<std::string>& _shardIds;
        };

        return std::make_unique<StaticCatalogClient>(GetHistoricalPlacementTestFixture::_shardIds);
    }

    /**
     * Store the configured shard ids in the shard registry.
     */
    void setShardIdsInShardRegistry(OperationContext* opCtx, std::vector<std::string> shardIds) {
        _shardIds = std::move(shardIds);
        Grid::get(opCtx)->shardRegistry()->reload(opCtx);
    }

    /**
     * Builds the expected value for the 'anyRemovedShardsDetected' field in the placement response.
     */
    boost::optional<bool> expectedValueForAnyRemovedShardDetected(bool ignoreRemovedShardsRequested,
                                                                  bool anyShardAbsent) const {
        if (ignoreRemovedShardsRequested) {
            return anyShardAbsent;
        }
        return boost::none;
    }

    /**
     * Retrieves the historical placement for the specified namespace and timestamp in
     * 'ignoreRemovedShards' mode.
     */
    HistoricalPlacement getHistoricalPlacementIgnoreRemovedShards(StringData nss, Timestamp ts) {
        return shardingCatalogManager().getHistoricalPlacement(
            operationContext(),
            nss.empty() ? boost::optional<NamespaceString>()
                        : boost::optional<NamespaceString>(
                              NamespaceString::createNamespaceString_forTest(nss)),
            ts,
            true /* checkIfPointInTimeIsInFuture */,
            true /* ignoreRemovedShards */);
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

    // This is used to communicate with the mock ShardingCatalogClient which shard ids should be
    // returned by the shard registry.
    std::vector<std::string> _shardIds;

    // Allows the usage of transactions.
    ReadWriteConcernDefaultsLookupMock _lookupMock;

    std::unique_ptr<DDLCoordinatorServiceMock> _recoverable =
        std::make_unique<DDLCoordinatorServiceMock>();
};

TEST_F(GetHistoricalPlacementTestFixture, queriesOnShardedCollectionReturnExpectedPlacement) {
    /* Querying the placementHistory for a sharded collection should return the shards that owned
     * the collection at the given clusterTime*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(2, 0), "db.collection1", {"shard1", "shard2"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard3", "shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3", "shard4"});

    for (bool ignoreRemovedShards : {true, false}) {
        // 2 shards must own collection1
        auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            Timestamp(4, 0),
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1", "shard2"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);
    }

    for (bool ignoreRemovedShards : {true, false}) {
        // 2 shards must own collection2
        auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection2"),
            Timestamp(4, 0),
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard3", "shard4"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);
    }
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_ShardedCollectionWithPrimary) {
    /* The primary shard associated to the parent database is already part of the `shards` list of
     * the collection and it does not appear twice*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2"}},
         {Timestamp(3, 0), "db.collection1", {"shard1", "shard2", "shard3"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3"});

    for (bool ignoreRemovedShards : {true, false}) {
        // 3 shards must own collection1 at timestamp 4
        auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            Timestamp(4, 0),
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1", "shard2", "shard3"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);
    }
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_UnshardedCollection) {
    /* Querying the placementHistory must report the primary shard for unsharded or non-existing
     * collections*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db2", {"shard2"}},
                                 {Timestamp(3, 0), "db3", {"shard3"}}});

    setupConfigShard(opCtx, 3 /*nShards*/);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3"});

    for (bool ignoreRemovedShards : {true, false}) {
        auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection"),
            Timestamp(3, 0),
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);

        historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db2.collection"),
            Timestamp(3, 0),
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard2"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);

        historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db3.collection"),
            Timestamp(3, 0),
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard3"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);
    }
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_DifferentTimestamp) {
    /* Query the placementHistory at different timestamp should return different results*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2"}},
         {Timestamp(4, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(5, 0), "db.collection1", {"shard1", "shard2", "shard3", "shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3", "shard4"});

    for (bool ignoreRemovedShards : {true, false}) {
        // no shards at timestamp 0
        auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            kDawnOfTime,
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);

        historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            Timestamp(1, 0),
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);

        historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            Timestamp(2, 0),
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1", "shard2"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);

        historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            Timestamp(4, 0),
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1", "shard2", "shard3"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);

        historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db.collection1"),
            Timestamp(5, 0),
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1", "shard2", "shard3", "shard4"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);
    }
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection"),
        Timestamp(1, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection2"),
        Timestamp(1, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard4", "shard5"}).value,
                          historicalPlacement);

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db2.collection"),
        Timestamp(1, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard7", "shard8", "shard9"}).value,
                          historicalPlacement);
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        Timestamp(4, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard2", "shard3", "shard4"}).value,
                          historicalPlacement);
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_ReturnPrimaryShardWhenNoShards) {
    /* Querying the placementHistory must report only the primary shard when an empty list of shards
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection2"),
        Timestamp(4, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1"}).value,
                          historicalPlacement);

    // Note: at timestamp 3 the collection's shard list is not empty
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection2"),
        Timestamp(3, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        Timestamp(2, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard2", "shard3", "shard4"}).value,
                          historicalPlacement);

    // Note: the primary shard is shard5 at timestamp 3
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        Timestamp(3, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard2", "shard3", "shard4"}).value,
                          historicalPlacement);
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        kDawnOfTime,
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    ASSERT_EQ(historicalPlacement.getStatus(), HistoricalPlacementStatus::NotAvailable);
    ASSERT(historicalPlacement.getShards().empty());

    // Asking for a timestamp before the closing marker should return the shards from the first
    // marker of the fcv upgrade. As result, "isExact" is expected to be false
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        Timestamp(2, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2", "shard3", "shard4", "shard5"})
                              .value,
                          historicalPlacement);

    // Asking for a timestamp after the closing marker should return the expected shards
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        Timestamp(3, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        Timestamp(6, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1"}).value,
                          historicalPlacement);
}

// Test 'ignoreRemovedShards' mode for a non-existing database.
TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_RemovedShards_DatabaseDoesNotExist) {
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}}, {Timestamp(2, 0), "db.collection1", {"shard1"}}});

    setupConfigShard(opCtx, 1 /*nShards*/);

    setShardIdsInShardRegistry(opCtx, {"shard1"});

    HistoricalPlacement historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db-does-not-exist.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db-does-not-exist.collection1", Timestamp(1, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db-does-not-exist.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
}

// Test 'ignoreRemovedShards' mode for a non-existing collection.
TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_RemovedShards_CollectionDoesNotExist) {
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {
                                    {Timestamp(1, 0), "db", {"shard1"}},
                                    {Timestamp(2, 0), "db.collection1", {"shard1"}},
                                });

    setupConfigShard(opCtx, 1 /*nShards*/);

    setShardIdsInShardRegistry(opCtx, {"shard1"});

    HistoricalPlacement historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist", Timestamp(1, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);

    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist", Timestamp(2, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
}

// Test 'ignoreRemovedShards' mode with various combinations of shards being removed from the shard
// registry.
TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_RemovedShards) {
    auto opCtx = operationContext();

    PlacementDescriptor oldestClusterTimeSupportedMarker = {
        Timestamp(0, 5),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {}};

    setupConfigPlacementHistory(
        opCtx,
        {
            oldestClusterTimeSupportedMarker,
            {Timestamp(1, 0), "db", {"shard1"}},
            {Timestamp(2, 0), "db.collection1", {"shard1", "shard2"}},
            {Timestamp(4, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
            {Timestamp(5, 0), "db.collection1", {"shard1", "shard2", "shard3", "shard4"}},
            {Timestamp(6, 0), "db.collection1", {}},
            {Timestamp(7, 0), "db", {}},
        },
        true);

    setupConfigShard(opCtx, 5 /*nShards*/);

    setShardIdsInShardRegistry(opCtx, {"shard5"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    HistoricalPlacement historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setAnyRemovedShardDetected(false)
                              .setOpenCursorAt(Timestamp(7, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setAnyRemovedShardDetected(false)
                              .setOpenCursorAt(Timestamp(7, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setAnyRemovedShardDetected(false)
                              .setOpenCursorAt(Timestamp(7, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setAnyRemovedShardDetected(false)
                              .setOpenCursorAt(Timestamp(7, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setAnyRemovedShardDetected(false)
                              .setOpenCursorAt(Timestamp(7, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard1"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(2, 0))
                              .setNextPlacementChangedAt(Timestamp(4, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2"})
                              .setAnyRemovedShardDetected(false)
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3"});

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2"})
                              .setAnyRemovedShardDetected(false)
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2", "shard3"})
                              .setAnyRemovedShardDetected(false)
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2", "shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3", "shard4"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2"})
                              .setAnyRemovedShardDetected(false)
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2", "shard3"})
                              .setAnyRemovedShardDetected(false)
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2", "shard3", "shard4"})
                              .setAnyRemovedShardDetected(false)
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard4"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2"})
                              .setAnyRemovedShardDetected(false)
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2", "shard4"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard3"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(2, 0))
                              .setNextPlacementChangedAt(Timestamp(4, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard3", "shard4"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(2, 0))
                              .setNextPlacementChangedAt(Timestamp(4, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard3", "shard4"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard4"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(2, 0))
                              .setNextPlacementChangedAt(Timestamp(4, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard4"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value,
        historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard2"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(2, 0))
                              .setNextPlacementChangedAt(Timestamp(4, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(2, 0))
                              .setNextPlacementChangedAt(Timestamp(4, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setAnyRemovedShardDetected(false)
                              .setOpenCursorAt(Timestamp(7, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard2", "shard3"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(2, 0))
                              .setNextPlacementChangedAt(Timestamp(4, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(2, 0))
                              .setNextPlacementChangedAt(Timestamp(4, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2", "shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2", "shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setAnyRemovedShardDetected(false)
                              .setOpenCursorAt(Timestamp(7, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard2", "shard4"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(2, 0))
                              .setNextPlacementChangedAt(Timestamp(4, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(2, 0))
                              .setNextPlacementChangedAt(Timestamp(4, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard2", "shard4"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setAnyRemovedShardDetected(false)
                              .setOpenCursorAt(Timestamp(7, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard3"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setAnyRemovedShardDetected(false)
                              .setOpenCursorAt(Timestamp(7, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard3", "shard4"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard3"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(4, 0))
                              .setNextPlacementChangedAt(Timestamp(5, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard3", "shard4"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setAnyRemovedShardDetected(false)
                              .setOpenCursorAt(Timestamp(7, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);

    setShardIdsInShardRegistry(opCtx, {"shard4"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    historicalPlacement = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard4"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard4"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard4"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard4"})
                              .setAnyRemovedShardDetected(true)
                              .setOpenCursorAt(Timestamp(5, 0))
                              .setNextPlacementChangedAt(Timestamp(6, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setAnyRemovedShardDetected(false)
                              .setOpenCursorAt(Timestamp(7, 0))
                              .value,
                          historicalPlacement);
    historicalPlacement =
        getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(7, 0));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value,
                          historicalPlacement);
}

// Test 'openCursorAt' and 'nextPlacementChanged' for a collection with all shards still being
// present in the shard registry.
TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_Collection_AllShardsPresent) {
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {
                                    {Timestamp(1, 0), "db", {"shard2"}},
                                    {Timestamp(2, 0), "db.collection1", {"shard2", "shard3"}},
                                    {Timestamp(3, 0), "db.collection1", {"shard1"}},
                                    {Timestamp(4, 0), "db.collection1", {"shard1", "shard2"}},
                                    {Timestamp(5, 0), "db.collection1", {}},
                                    {Timestamp(6, 0), "db", {}},
                                });

    setupConfigShard(opCtx, 3 /*nShards*/);

    // All shards are still present.
    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3"});

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard2"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard2", "shard3"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(3, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard2"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard2"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

// Test 'openCursorAt' and 'nextPlacementChanged' for a collection with only some shards still
// being present in the shard registry.
TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_Collection_SomeShardsPresent) {
    auto opCtx = operationContext();

    PlacementDescriptor oldestClusterTimeSupportedMarker = {
        Timestamp(0, 5),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {}};

    setupConfigPlacementHistory(opCtx,
                                {
                                    oldestClusterTimeSupportedMarker,
                                    {Timestamp(1, 0), "db", {"shard2"}},
                                    {Timestamp(2, 0), "db.collection1", {"shard2", "shard3"}},
                                    {Timestamp(3, 0), "db.collection1", {"shard1"}},
                                    {Timestamp(4, 0), "db.collection1", {"shard1", "shard2"}},
                                    {Timestamp(5, 0), "db.collection1", {}},
                                    {Timestamp(6, 0), "db", {}},
                                },
                                true);

    setupConfigShard(opCtx, 3 /*nShards*/);

    // Only "shard1" is present.
    setShardIdsInShardRegistry(opCtx, {"shard1"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard1"})
                            .setOpenCursorAt(Timestamp(3, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard1"})
                            .setOpenCursorAt(Timestamp(3, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(3, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(4, 0))
                            .setNextPlacementChangedAt(Timestamp(5, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(6, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only "shard2" and "shard3" are present.
    setShardIdsInShardRegistry(opCtx, {"shard2", "shard3"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(1, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard2"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard2", "shard3"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard2"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(4, 0))
                            .setNextPlacementChangedAt(Timestamp(5, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(4, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard2"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(4, 0))
                            .setNextPlacementChangedAt(Timestamp(5, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(5, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection1", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

// Test 'openCursorAt' and 'nextPlacementChanged' for a dropped collection, for which the shard is
// removed after the placement history query timestamp.
TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_OpenCursorAt_Collection_Removed) {
    auto opCtx = operationContext();

    PlacementDescriptor oldestClusterTimeSupportedMarker = {
        Timestamp(0, 5),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {}};

    setupConfigPlacementHistory(opCtx,
                                {
                                    oldestClusterTimeSupportedMarker,
                                    {Timestamp(1, 0), "db", {"shard1"}},
                                    {Timestamp(2, 0), "db.collection", {"shard1"}},
                                    {Timestamp(4, 0), "db.collection", {}},
                                    {Timestamp(5, 0), "db", {}},
                                },
                                true);

    setupConfigShard(opCtx, 2 /*nShards*/);

    // Only "shard2" is present.
    setShardIdsInShardRegistry(opCtx, {"shard2"});

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(5, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }
}

// Test 'openCursorAt' and 'nextPlacementChanged' for a moved-and-then-dropped collection, for which
// the shard is removed after the placement history query timestamp.
TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_Collection_Moved_Removed_Then_Database_Removed) {
    auto opCtx = operationContext();

    PlacementDescriptor oldestClusterTimeSupportedMarker = {
        Timestamp(0, 5),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {}};

    setupConfigPlacementHistory(opCtx,
                                {
                                    oldestClusterTimeSupportedMarker,
                                    {Timestamp(1, 0), "db", {"shard1"}},
                                    {Timestamp(2, 0), "db.collection", {"shard2"}},
                                    {Timestamp(4, 0), "db.collection", {}},
                                    {Timestamp(5, 0), "db", {}},
                                },
                                true);

    setupConfigShard(opCtx, 3 /*nShards*/);

    // Only "shard3" is present.
    setShardIdsInShardRegistry(opCtx, {"shard3"});

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(5, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }
}

// Test 'openCursorAt' and 'nextPlacementChanged' for a non-existing collection in an existing
// database.
TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_NonExistingCollectionInExistingDatabase) {
    auto opCtx = operationContext();

    PlacementDescriptor oldestClusterTimeSupportedMarker = {
        Timestamp(0, 5),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {}};

    setupConfigPlacementHistory(
        opCtx,
        {
            oldestClusterTimeSupportedMarker,
            {Timestamp(1, 0), "db", {"shard2"}},
            {Timestamp(2, 0), "db.collection-unrelated", {"shard2", "shard3"}},
            {Timestamp(3, 0), "db", {}},
        },
        true);

    setupConfigShard(opCtx, 3 /*nShards*/);

    // All shards are present in the shard registry.
    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3"});

    {
        auto actual =
            getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist",
                                                                Timestamp(1, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard2"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist",
                                                                Timestamp(2, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard2"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist",
                                                                Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only "shard1" is present in the shard registry.
    setShardIdsInShardRegistry(opCtx, {"shard1"});

    {
        auto actual =
            getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist",
                                                                Timestamp(1, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(3, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist",
                                                                Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(3, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection-does-not-exist",
                                                                Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

// Test 'openCursorAt' and 'nextPlacementChanged' for a non-existing collection in non-existing
// database.
TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_NonExistingCollectionNonExistingDatabase) {
    auto opCtx = operationContext();

    PlacementDescriptor oldestClusterTimeSupportedMarker = {
        Timestamp(0, 5),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {}};

    setupConfigPlacementHistory(opCtx,
                                {
                                    oldestClusterTimeSupportedMarker,
                                    {Timestamp(1, 0), "db", {"shard1"}},
                                    {Timestamp(2, 0), "db", {}},
                                },
                                true);

    setupConfigShard(opCtx, 2 /*nShards*/);

    // All shards are present in the shard registry.
    setShardIdsInShardRegistry(opCtx, {"shard1"});

    for (auto ts : {kDawnOfTime,
                    Timestamp(0, 4),
                    Timestamp(0, 5),
                    Timestamp(0, 6),
                    Timestamp(1, 0),
                    Timestamp(2, 0),
                    Timestamp(3, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards(
            "db-does-not-exist.collection-does-not-exist", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only "shard2" is present in the shard registry.
    setShardIdsInShardRegistry(opCtx, {"shard2"});

    for (auto ts : {kDawnOfTime,
                    Timestamp(0, 4),
                    Timestamp(0, 5),
                    Timestamp(0, 6),
                    Timestamp(1, 0),
                    Timestamp(2, 0),
                    Timestamp(3, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards(
            "db-does-not-exist.collection-does-not-exist", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_Collection_ApproximatedResponseBeforeEntries) {
    auto opCtx = operationContext();

    // Insert the initial content
    setupConfigPlacementHistory(opCtx,
                                {
                                    {kDawnOfTime, "", {"shard1", "shard2"}},
                                    {Timestamp(0, 5), "", {}},
                                    {Timestamp(1, 0), "db", {"shard1"}},
                                    {Timestamp(2, 0), "db.collection", {"shard1"}},
                                    {Timestamp(3, 0), "db.collection", {"shard2"}},
                                    {Timestamp(4, 0), "db", {"shard2"}},
                                    {Timestamp(5, 0), "db.collection", {}},
                                    {Timestamp(6, 0), "db", {}},
                                });

    setupConfigShard(opCtx, 2 /*nShards*/);

    // All shards are still present.
    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2"});

    for (auto ts : {kDawnOfTime, Timestamp(0, 4)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard1", "shard2"})
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(1, 0), Timestamp(2, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard1"}).value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(3, 0), Timestamp(4, 0), Timestamp(5, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(6, 0), Timestamp(7, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only shard1 is still present.
    setShardIdsInShardRegistry(opCtx, {"shard1"});

    for (auto ts : {kDawnOfTime, Timestamp(0, 4)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard1"})
                            .setOpenCursorAt(ts)
                            .setNextPlacementChangedAt(Timestamp(0, 5))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(1, 0), Timestamp(2, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard1"}).value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(6, 0), Timestamp(7, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only shard2 is still present.
    setShardIdsInShardRegistry(opCtx, {"shard2"});

    for (auto ts : {kDawnOfTime, Timestamp(0, 4)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(ts)
                            .setNextPlacementChangedAt(Timestamp(0, 5))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(1, 0), Timestamp(2, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard2"})
                            .setOpenCursorAt(Timestamp(3, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(3, 0), Timestamp(4, 0), Timestamp(5, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(6, 0), Timestamp(7, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_Collection_ApproximatedResponseInMiddleOfEntries) {
    auto opCtx = operationContext();

    // Insert the initial content
    setupConfigPlacementHistory(opCtx,
                                {
                                    {kDawnOfTime, "", {"shard1", "shard2"}},
                                    {Timestamp(1, 0), "db", {"shard1"}},
                                    {Timestamp(2, 0), "db.collection", {"shard1"}},
                                    {Timestamp(3, 0), "db.collection", {"shard2"}},
                                    {Timestamp(3, 5), "", {}},
                                    {Timestamp(4, 0), "db", {"shard2"}},
                                    {Timestamp(5, 0), "db.collection", {}},
                                    {Timestamp(6, 0), "db", {}},
                                });

    setupConfigShard(opCtx, 2 /*nShards*/);

    // All shards are still present.
    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2"});

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard1", "shard2"})
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(1, 0), Timestamp(2, 0), Timestamp(3, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard1", "shard2"})
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(3, 5), Timestamp(4, 0), Timestamp(5, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(6, 0), Timestamp(7, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only shard1 is still present.
    setShardIdsInShardRegistry(opCtx, {"shard1"});

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard1"})
                            .setOpenCursorAt(kDawnOfTime)
                            .setNextPlacementChangedAt(Timestamp(3, 5))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(1, 0), Timestamp(2, 0), Timestamp(3, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard1"})
                            .setOpenCursorAt(ts)
                            .setNextPlacementChangedAt(Timestamp(3, 5))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(3, 5), Timestamp(4, 0), Timestamp(5, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(6, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(6, 0), Timestamp(7, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only shard2 is still present.
    setShardIdsInShardRegistry(opCtx, {"shard2"});

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(kDawnOfTime)
                            .setNextPlacementChangedAt(Timestamp(3, 5))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(1, 0), Timestamp(2, 0), Timestamp(3, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(ts)
                            .setNextPlacementChangedAt(Timestamp(3, 5))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(3, 5), Timestamp(4, 0), Timestamp(5, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(6, 0), Timestamp(7, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db.collection", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

// Test 'openCursorAt' and 'nextPlacementChanged' for a database with all shards still being
// present in the shard registry.
TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_Database_AllShardsPresent) {
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {
                                    {Timestamp(1, 0), "db", {"shard1"}},
                                    {Timestamp(2, 0), "db.collection1", {"shard2"}},
                                    {Timestamp(3, 0), "db.collection2", {"shard3"}},
                                    {Timestamp(4, 0), "db.collection1", {}},
                                    {Timestamp(5, 0), "db.collection2", {}},
                                    {Timestamp(6, 0), "db", {}},
                                });

    setupConfigShard(opCtx, 3 /*nShards*/);

    // All shards are still present.
    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3"});

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(1, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard2"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard2", "shard3"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(4, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard3"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(5, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

// Test 'openCursorAt' and 'nextPlacementChanged' for a database with only some shards still being
// present in the shard registry.
TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_Database_SomeShardsPresent) {
    auto opCtx = operationContext();

    PlacementDescriptor oldestClusterTimeSupportedMarker = {
        Timestamp(0, 5),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {}};

    setupConfigPlacementHistory(opCtx,
                                {
                                    oldestClusterTimeSupportedMarker,
                                    {Timestamp(1, 0), "db", {"shard1"}},
                                    {Timestamp(2, 0), "db.collection1", {"shard2"}},
                                    {Timestamp(3, 0), "db.collection2", {"shard3"}},
                                    {Timestamp(4, 0), "db.collection1", {}},
                                    {Timestamp(5, 0), "db.collection2", {}},
                                    {Timestamp(6, 0), "db", {}},
                                },
                                true);

    setupConfigShard(opCtx, 3 /*nShards*/);

    // Only shard2 and shard3 are still present.
    setShardIdsInShardRegistry(opCtx, {"shard2", "shard3"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(1, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard2"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(2, 0))
                            .setNextPlacementChangedAt(Timestamp(3, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard2"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(2, 0))
                            .setNextPlacementChangedAt(Timestamp(3, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard2", "shard3"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(3, 0))
                            .setNextPlacementChangedAt(Timestamp(4, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(4, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard3"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(4, 0))
                            .setNextPlacementChangedAt(Timestamp(5, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(5, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(6, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only shard1 is still present.
    setShardIdsInShardRegistry(opCtx, {"shard1"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(1, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(2, 0))
                            .setNextPlacementChangedAt(Timestamp(3, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(3, 0))
                            .setNextPlacementChangedAt(Timestamp(4, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(4, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(4, 0))
                            .setNextPlacementChangedAt(Timestamp(5, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(5, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

// Test 'openCursorAt' and 'nextPlacementChanged' for a non-existing database.
TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_OpenCursorAt_NonExistingDatabase) {
    auto opCtx = operationContext();

    PlacementDescriptor oldestClusterTimeSupportedMarker = {
        Timestamp(0, 5),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {}};

    setupConfigPlacementHistory(opCtx,
                                {
                                    oldestClusterTimeSupportedMarker,
                                    {Timestamp(1, 0), "db", {"shard2"}},
                                    {Timestamp(2, 0), "db.collection", {"shard2", "shard3"}},
                                    {Timestamp(3, 0), "db", {}},
                                },
                                true);

    setupConfigShard(opCtx, 3 /*nShards*/);

    // All shards are present in the shard registry.
    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3"});

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {kDawnOfTime, Timestamp(1, 0), Timestamp(2, 0), Timestamp(3, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db-does-not-exist", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only "shard1" is present in the shard registry.
    setShardIdsInShardRegistry(opCtx, {"shard1"});

    for (auto ts : {kDawnOfTime, Timestamp(1, 0), Timestamp(2, 0), Timestamp(3, 0)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db-does-not-exist", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_Database_AfterApproximatedResponse) {
    auto opCtx = operationContext();

    // Insert the initial content
    setupConfigPlacementHistory(opCtx,
                                {
                                    {kDawnOfTime, "", {"shard1", "shard2"}},
                                    {Timestamp(0, 5), "", {}},
                                    {Timestamp(1, 0), "db", {"shard1"}},
                                    {Timestamp(2, 0), "db.collection1", {"shard1"}},
                                    {Timestamp(3, 0), "db.collection1", {"shard2"}},
                                    {Timestamp(4, 0), "db", {"shard2"}},
                                    {Timestamp(5, 0), "db.collection2", {"shard1"}},
                                    {Timestamp(6, 0), "db.collection1", {}},
                                    {Timestamp(7, 0), "db.collection2", {"shard2"}},
                                    {Timestamp(8, 0), "db.collection2", {}},
                                    {Timestamp(9, 0), "db", {}},
                                });

    setupConfigShard(opCtx, 2 /*nShards*/);

    // All shards are still present.
    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2"});

    for (auto ts : {kDawnOfTime, Timestamp(0, 4)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard1", "shard2"})
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(1, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard1"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(2, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard1"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard1", "shard2"})
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(4, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(5, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard1", "shard2"})
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard1", "shard2"})
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(7, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(8, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(9, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only "shard1" is still present.
    setShardIdsInShardRegistry(opCtx, {"shard1"});

    for (auto ts : {kDawnOfTime, Timestamp(0, 4)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard1"})
                            .setOpenCursorAt(ts)
                            .setNextPlacementChangedAt(Timestamp(0, 5))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(1, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard1"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(2, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard1"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard1"})
                            .setOpenCursorAt(Timestamp(3, 0))
                            .setNextPlacementChangedAt(Timestamp(4, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(4, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard1"})
                            .setOpenCursorAt(Timestamp(5, 0))
                            .setNextPlacementChangedAt(Timestamp(6, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(5, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard1"})
                            .setOpenCursorAt(Timestamp(5, 0))
                            .setNextPlacementChangedAt(Timestamp(6, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard1"})
                            .setOpenCursorAt(Timestamp(6, 0))
                            .setNextPlacementChangedAt(Timestamp(7, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(7, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(9, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(8, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(9, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(9, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only "shard2" is still present.
    setShardIdsInShardRegistry(opCtx, {"shard2"});

    for (auto ts : {kDawnOfTime, Timestamp(0, 4)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(ts)
                            .setNextPlacementChangedAt(Timestamp(0, 5))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(1, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(Timestamp(3, 0))
                            .setNextPlacementChangedAt(Timestamp(4, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(Timestamp(3, 0))
                            .setNextPlacementChangedAt(Timestamp(4, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(Timestamp(3, 0))
                            .setNextPlacementChangedAt(Timestamp(4, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(4, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(5, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(Timestamp(5, 0))
                            .setNextPlacementChangedAt(Timestamp(6, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(Timestamp(6, 0))
                            .setNextPlacementChangedAt(Timestamp(7, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(7, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(8, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(9, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

// Test 'openCursorAt' and 'nextPlacementChanged' for the whole cluster with all shards still being
// present in the shard registry.
TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_WholeCluster_AllShardsPresent) {
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {
                                    {Timestamp(1, 0), "db1", {"shard1"}},
                                    {Timestamp(2, 0), "db1.collection1", {"shard2"}},
                                    {Timestamp(3, 0), "db2", {"shard3"}},
                                    {Timestamp(4, 0), "db2.collection2", {"shard1"}},
                                    {Timestamp(5, 0), "db1.collection1", {}},
                                    {Timestamp(6, 0), "db2.collection2", {}},
                                    {Timestamp(7, 0), "db1", {}},
                                    {Timestamp(8, 0), "db2", {}},
                                });

    setupConfigShard(opCtx, 3 /*nShards*/);

    // All shards are still present.
    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3"});

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(1, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard2"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard2", "shard3"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(4, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard2", "shard3"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(5, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard3"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard3"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(7, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard3"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(8, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

// Test 'openCursorAt' and 'nextPlacementChanged' for the whole cluster with only some shards still
// being present in the shard registry.
TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_WholeCluster_SomeShardsPresent) {
    auto opCtx = operationContext();

    PlacementDescriptor oldestClusterTimeSupportedMarker = {
        Timestamp(0, 5),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {}};

    setupConfigPlacementHistory(opCtx,
                                {
                                    oldestClusterTimeSupportedMarker,
                                    {Timestamp(1, 0), "db1", {"shard1"}},
                                    {Timestamp(2, 0), "db1.collection1", {"shard2"}},
                                    {Timestamp(3, 0), "db2", {"shard3"}},
                                    {Timestamp(4, 0), "db2.collection2", {"shard1"}},
                                    {Timestamp(5, 0), "db1.collection1", {}},
                                    {Timestamp(6, 0), "db2.collection2", {}},
                                    {Timestamp(7, 0), "db1", {}},
                                    {Timestamp(8, 0), "db2", {}},
                                },
                                true);

    setupConfigShard(opCtx, 3 /*nShards*/);

    // Only shard1 and shard2 are still present.
    setShardIdsInShardRegistry(opCtx, {"shard1", "shard3"});

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Query before, at, and directly after oldestClusterTimeSupported marker.
    for (auto ts : {Timestamp(0, 4), Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(1, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard1"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(2, 0))
                            .setNextPlacementChangedAt(Timestamp(3, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard3"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(3, 0))
                            .setNextPlacementChangedAt(Timestamp(4, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(4, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard3"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(4, 0))
                            .setNextPlacementChangedAt(Timestamp(5, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(5, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard3"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard1", "shard3"})
                            .setAnyRemovedShardDetected(false)
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(7, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard3"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(8, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only shard2 is still present.
    setShardIdsInShardRegistry(opCtx, {"shard2"});

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", kDawnOfTime);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Query before oldestClusterTimeSupported marker.
    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(0, 4));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Query at oldestClusterTimeSupported marker.
    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(0, 5));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Query directly after oldestClusterTimeSupported marker.
    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(0, 6));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(1, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(Timestamp(2, 0))
                            .setNextPlacementChangedAt(Timestamp(3, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard2"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(2, 0))
                            .setNextPlacementChangedAt(Timestamp(3, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(3, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard2"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(3, 0))
                            .setNextPlacementChangedAt(Timestamp(4, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(4, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setShards({"shard2"})
                            .setAnyRemovedShardDetected(true)
                            .setOpenCursorAt(Timestamp(4, 0))
                            .setNextPlacementChangedAt(Timestamp(5, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(5, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(8, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(6, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(8, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(7, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(8, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(8, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

TEST_F(GetHistoricalPlacementTestFixture,
       getHistoricalPlacement_OpenCursorAt_WholeCluster_AfterApproximatedResponse) {
    auto opCtx = operationContext();

    // Insert the initial content
    setupConfigPlacementHistory(opCtx,
                                {
                                    {kDawnOfTime, "", {"shard1", "shard2"}},
                                    {Timestamp(0, 5), "", {}},
                                    {Timestamp(1, 0), "db1", {"shard1"}},
                                    {Timestamp(2, 0), "db1.collection1", {"shard1"}},
                                    {Timestamp(3, 0), "db1.collection1", {"shard2"}},
                                    {Timestamp(4, 0), "db2", {"shard2"}},
                                    {Timestamp(5, 0), "db2.collection3", {"shard1"}},
                                    {Timestamp(6, 0), "db1.collection2", {"shard1"}},
                                    {Timestamp(7, 0), "db1.collection1", {}},
                                    {Timestamp(8, 0), "db1.collection2", {"shard2"}},
                                    {Timestamp(9, 0), "db1.collection2", {}},
                                    {Timestamp(10, 0), "db1", {}},
                                    {Timestamp(11, 0), "db2.collection3", {}},
                                    {Timestamp(12, 0), "db2", {}},
                                });

    setupConfigShard(opCtx, 2 /*nShards*/);

    // All shards are still present.
    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2"});

    for (auto ts : {kDawnOfTime, Timestamp(0, 4)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard1", "shard2"})
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(1, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard1"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(2, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard1"}).value;
        assertPlacementsEqual(expected, actual);
    }

    for (int i = 3; i <= 10; ++i) {
        auto ts = Timestamp(i, 0);

        auto actual = getHistoricalPlacementIgnoreRemovedShards("", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setShards({"shard1", "shard2"})
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(11, 0));
        auto expected =
            ExpectedResponseBuilder{}.setShards({"shard2"}).setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(12, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only "shard1" is still present.
    setShardIdsInShardRegistry(opCtx, {"shard1"});

    for (auto ts : {kDawnOfTime, Timestamp(0, 4)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard1"})
                            .setOpenCursorAt(ts)
                            .setNextPlacementChangedAt(Timestamp(0, 5))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(1, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard1"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(2, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard1"}).value;
        assertPlacementsEqual(expected, actual);
    }

    for (int i = 3; i <= 10; ++i) {
        auto ts = Timestamp(i, 0);

        auto actual = getHistoricalPlacementIgnoreRemovedShards("", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard1"})
                            .setOpenCursorAt(ts)
                            .setNextPlacementChangedAt(Timestamp(i + 1, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(11, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(false)
                            .setOpenCursorAt(Timestamp(12, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(12, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    // Only "shard2" is still present.
    setShardIdsInShardRegistry(opCtx, {"shard2"});

    for (auto ts : {kDawnOfTime, Timestamp(0, 4)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(ts)
                            .setNextPlacementChangedAt(Timestamp(0, 5))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (auto ts : {Timestamp(0, 5), Timestamp(0, 6)}) {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", ts);
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(1, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(Timestamp(3, 0))
                            .setNextPlacementChangedAt(Timestamp(4, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(2, 0));
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(Timestamp(3, 0))
                            .setNextPlacementChangedAt(Timestamp(4, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    for (int i = 3; i <= 10; ++i) {
        auto ts = Timestamp(i, 0);

        auto actual = getHistoricalPlacementIgnoreRemovedShards("", ts);
        auto expected = ExpectedResponseBuilder{}
                            .setAnyRemovedShardDetected(true)
                            .setShards({"shard2"})
                            .setOpenCursorAt(ts)
                            .setNextPlacementChangedAt(Timestamp(i + 1, 0))
                            .value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(11, 0));
        auto expected =
            ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).setShards({"shard2"}).value;
        assertPlacementsEqual(expected, actual);
    }

    {
        auto actual = getHistoricalPlacementIgnoreRemovedShards("", Timestamp(12, 0));
        auto expected = ExpectedResponseBuilder{}.setAnyRemovedShardDetected(false).value;
        assertPlacementsEqual(expected, actual);
    }
}

using GetHistoricalPlacementTestFixtureDeathTest = GetHistoricalPlacementTestFixture;

// Tests that a tassert is raised when the expected initialization marker for the oldest supported
// cluster time is missing in the placement history.
DEATH_TEST_REGEX_F(GetHistoricalPlacementTestFixtureDeathTest,
                   getHistoricalPlacement_OpenCursorAt_MissingOldestClusterTimeSupportedMarker,
                   "Tripwire assertion.*11314301") {
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {
                                    {Timestamp(1, 0), "db", {"shard1"}},
                                });

    setupConfigShard(opCtx, 2 /*nShards*/);

    // Only "shard2" is still present.
    setShardIdsInShardRegistry(opCtx, {"shard2"});

    ASSERT_THROWS_CODE(getHistoricalPlacementIgnoreRemovedShards("db.collection", Timestamp(1, 0)),
                       AssertionException,
                       11314301);
}

// Tests that a tassert is raised when the expected follow-up entry is missing in the placement
// history for a namespace which was present on a now-removed shard.
DEATH_TEST_REGEX_F(
    GetHistoricalPlacementTestFixtureDeathTest,
    getHistoricalPlacement_OpenCursorAt_MissingFollowUpPlacementHistoryEntryForNamespace,
    "Tripwire assertion.*11314303") {
    auto opCtx = operationContext();

    PlacementDescriptor oldestClusterTimeSupportedMarker = {
        Timestamp(0, 5),
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker.toString_forTest(),
        {}};

    setupConfigPlacementHistory(opCtx,
                                {
                                    oldestClusterTimeSupportedMarker,
                                    {Timestamp(1, 0), "db", {"shard1"}},
                                });

    setupConfigShard(opCtx, 2 /*nShards*/);

    // Only "shard2" is still present.
    setShardIdsInShardRegistry(opCtx, {"shard2"});

    ASSERT_THROWS_CODE(getHistoricalPlacementIgnoreRemovedShards("db", Timestamp(2, 0)),
                       AssertionException,
                       11314303);
}

// ######################## PlacementHistory: Query by database ############################
TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_SingleDatabase) {
    /* Querying the placementHistory must report all the shards for every collection belonging to
     * the input db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection1", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard4", "shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3", "shard4", "shard5"});

    for (bool ignoreRemovedShards : {true, false}) {
        auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db"),
            Timestamp(3, 0),
            true /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1", "shard2", "shard3", "shard4", "shard5"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);
    }
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_MultipleDatabases) {
    /* Querying the placementHistory must report all the shards for every collection belonging to
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(5, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db2"),
        Timestamp(5, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard4", "shard5", "shard6"}).value,
                          historicalPlacement);

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db3"),
        Timestamp(5, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard7"}).value,
                          historicalPlacement);
}

TEST_F(GetHistoricalPlacementTestFixture, dbLevelSearch_DifferentTimestamp) {
    /* Query the placementHistory at different timestamp should return different results*/
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        kDawnOfTime,
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(HistoricalPlacement{}, historicalPlacement);

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(1, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1"}).value,
                          historicalPlacement);

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(2, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2"}).value,
                          historicalPlacement);

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(4, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(5, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3", "shard4"}).value,
        historicalPlacement);
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_SameTimestamp_repeated) {
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(1, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2", "shard3", "shard4", "shard5"})
                              .value,
                          historicalPlacement);

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db2"),
        Timestamp(1, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard6", "shard7", "shard8", "shard9"}).value,
        historicalPlacement);
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(4, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3", "shard4"}).value,
        historicalPlacement);
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_NoShardsForDb) {
    /* Querying the placementHistory must report no shards if the list of shards belonging to every
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(4, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(HistoricalPlacement{}, historicalPlacement);

    // Note: at timestamp 3 the collection's shard list was not empty
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(3, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);
}

TEST_F(GetHistoricalPlacementTestFixture, getHistoricalPlacement_NewShardForDb) {
    /* Querying the placementHistory must correctly identify a new primary for the db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(
        opCtx,
        {{Timestamp(1, 0), "db", {"shard1"}},
         {Timestamp(2, 0), "db.collection1", {"shard1", "shard2", "shard3"}},
         {Timestamp(3, 0), "db", {"shard4"}}});

    setupConfigShard(opCtx, 4 /*nShards*/);

    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(2, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);

    // At timestamp 3 the db shard list was updated with a new primary
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(3, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard4", "shard1", "shard2", "shard3"}).value,
        historicalPlacement);
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(2, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1", "shard2", "shard3", "shard4", "shard5"})
                              .value,
                          historicalPlacement);

    // Asking for a timestamp after the closing marker should return the expected shards
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(3, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3", "shard4"}).value,
        historicalPlacement);

    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(7, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);
}

// ######################## PlacementHistory: Query the entire cluster ##################
TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_SingleDatabase) {
    /* Querying the placementHistory must report all the shards for every collection and db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection1", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db.collection2", {"shard4", "shard5"}}});

    setupConfigShard(opCtx, 5 /*nShards*/);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2", "shard3", "shard4", "shard5"});

    for (bool ignoreRemovedShards : {true, false}) {
        auto historicalPlacement =
            shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                            boost::none,
                                                            Timestamp(3, 0),
                                                            true /* checkIfPointInTimeIsInFuture */,
                                                            ignoreRemovedShards);

        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1", "shard2", "shard3", "shard4", "shard5"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);
    }
}

TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_MultipleDatabases) {
    /* Querying the placementHistory must report all the shards for every collection and db*/
    auto opCtx = operationContext();

    setupConfigPlacementHistory(opCtx,
                                {{Timestamp(1, 0), "db", {"shard1"}},
                                 {Timestamp(2, 0), "db.collection", {"shard2", "shard3"}},
                                 {Timestamp(3, 0), "db2", {"shard4"}},
                                 {Timestamp(4, 0), "db2.collection", {"shard5", "shard6"}},
                                 {Timestamp(5, 0), "db3", {"shard7"}}});

    setupConfigShard(opCtx, 7 /*nShards*/);

    auto historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(5, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);

    assertPlacementsEqual(
        ExpectedResponseBuilder{}
            .setShards({"shard1", "shard2", "shard3", "shard4", "shard5", "shard6", "shard7"})
            .value,
        historicalPlacement);
}

TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_DifferentTimestamp) {
    /* Query the placementHistory at different timestamp should return different results*/
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
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        kDawnOfTime,
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);

    assertPlacementsEqual(HistoricalPlacement{}, historicalPlacement);

    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(1, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1"}).value,
                          historicalPlacement);

    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(2, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2"}).value,
                          historicalPlacement);

    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(4, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);

    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(5, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);

    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3", "shard4"}).value,
        historicalPlacement);
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
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(1, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}
                              .setShards({"shard1",
                                          "shard2",
                                          "shard3",
                                          "shard4",
                                          "shard5",
                                          "shard6",
                                          "shard7",
                                          "shard8",
                                          "shard9"})
                              .value,
                          historicalPlacement);
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
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(4, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);

    assertPlacementsEqual(
        ExpectedResponseBuilder{}
            .setShards(
                {"shard1", "shard2", "shard3", "shard4", "shard5", "shard6", "shard7", "shard8"})
            .value,
        historicalPlacement);
}

TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_NoShards) {
    /* Querying the placementHistory must report no shards if the list of shards belonging to
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
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(4, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);

    assertPlacementsEqual(HistoricalPlacement{}, historicalPlacement);

    // Note: at timestamp 3 the collection was still sharded
    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(3, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);
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
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(2, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3", "shard4"}).value,
        historicalPlacement);

    // Asking for a timestamp after the closing marker should return the expected shards
    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(3, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);

    historicalPlacement =
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(5, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);
}

TEST_F(GetHistoricalPlacementTestFixture,
       GetShardsThatOwnDataAtClusterTime_RegexStage_NssWithPrefix) {
    /* The regex stage must match correctly the input namespaces*/
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
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        Timestamp(12, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);

    // no data must be returned since the namespace is not found
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("d.collection1"),
        Timestamp(12, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(HistoricalPlacement{}, historicalPlacement);

    // database exists
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(12, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(
        ExpectedResponseBuilder{}
            .setShards({"shard1", "shard2", "shard3", "shard7", "shard8", "shard9"})
            .value,
        historicalPlacement);

    // database does not exist
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("d"),
        Timestamp(12, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(HistoricalPlacement{}, historicalPlacement);
}

TEST_F(GetHistoricalPlacementTestFixture,
       GetShardsThatOwnDataAtClusterTime_RegexStage_DbWithSymbols) {
    /* The regex stage must correctly escape special character*/
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

    // db|db , db*db  etc... must not be found when querying by database
    auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db"),
        Timestamp(10, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);

    // db|db , db*db  etc... must not be found when querying by collection
    historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection"),
        Timestamp(10, 0),
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2", "shard3"}).value,
                          historicalPlacement);
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
            Timestamp(4, 0),
            true /* checkIfPointInTimeIsInFuture */,
            false /* ignoreRemovedShards */);
        assertPlacementsEqual(
            ExpectedResponseBuilder{HistoricalPlacementStatus::NotAvailable}.value,
            historicalPlacement);
    }

    // DB-level query
    {
        auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            NamespaceString::createNamespaceString_forTest("db"),
            Timestamp(4, 0),
            true /* checkIfPointInTimeIsInFuture */,
            false /* ignoreRemovedShards */);
        assertPlacementsEqual(
            ExpectedResponseBuilder{HistoricalPlacementStatus::NotAvailable}.value,
            historicalPlacement);
    }

    // Cluster-level query
    {
        auto historicalPlacement =
            shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                            boost::none,
                                                            Timestamp(4, 0),
                                                            true /* checkIfPointInTimeIsInFuture */,
                                                            false /* ignoreRemovedShards */);
        assertPlacementsEqual(
            ExpectedResponseBuilder{HistoricalPlacementStatus::NotAvailable}.value,
            historicalPlacement);
    }
}

// ######################## PlacementHistory: InvalidOptions #####################
TEST_F(GetHistoricalPlacementTestFixture, GetShardsThatOwnDataAtClusterTime_InvalidOptions) {
    /* Testing input validation*/
    auto opCtx = operationContext();

    // Invalid namespaces are rejected
    ASSERT_THROWS_CODE(shardingCatalogManager().getHistoricalPlacement(
                           opCtx,
                           NamespaceString::createNamespaceString_forTest(""),
                           kDawnOfTime,
                           true /* checkIfPointInTimeIsInFuture */,
                           false /* ignoreRemovedShards */),
                       DBException,
                       ErrorCodes::InvalidOptions);

    // 'config', 'local' and 'admin' namespaces are not supported.
    ASSERT_THROWS_CODE(
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        NamespaceString(DatabaseName::kAdmin),
                                                        kDawnOfTime,
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */),
        DBException,
        ErrorCodes::InvalidOptions);

    ASSERT_THROWS_CODE(
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        NamespaceString(DatabaseName::kLocal),
                                                        kDawnOfTime,
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */),
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
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards(approximatedPlacement).value,
                          shardingCatalogManager().getHistoricalPlacement(
                              opCtx,
                              NamespaceString::createNamespaceString_forTest("db"),
                              earliestClusterTime,
                              true /* checkIfPointInTimeIsInFuture */,
                              false /* ignoreRemovedShards */));
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards(approximatedPlacement).value,
                          shardingCatalogManager().getHistoricalPlacement(
                              opCtx,
                              NamespaceString::createNamespaceString_forTest("db"),
                              earliestClusterTime - 1,
                              true /* checkIfPointInTimeIsInFuture */,
                              false /* ignoreRemovedShards */));

    // db.collection1
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard2", "shard3", "shard4"}).value,
                          shardingCatalogManager().getHistoricalPlacement(
                              opCtx,
                              NamespaceString::createNamespaceString_forTest("db.collection1"),
                              earliestClusterTime,
                              true /* checkIfPointInTimeIsInFuture */,
                              false /* ignoreRemovedShards */));

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards(approximatedPlacement).value,
                          shardingCatalogManager().getHistoricalPlacement(
                              opCtx,
                              NamespaceString::createNamespaceString_forTest("db.collection1"),
                              earliestClusterTime - 1,
                              true /* checkIfPointInTimeIsInFuture */,
                              false /* ignoreRemovedShards */));

    // db.collection2
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard4"}).value,
                          shardingCatalogManager().getHistoricalPlacement(
                              opCtx,
                              NamespaceString::createNamespaceString_forTest("db.collection2"),
                              earliestClusterTime,
                              true /* checkIfPointInTimeIsInFuture */,
                              false /* ignoreRemovedShards */));

    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards(approximatedPlacement).value,
                          shardingCatalogManager().getHistoricalPlacement(
                              opCtx,
                              NamespaceString::createNamespaceString_forTest("db.collection2"),
                              Timestamp(11, 0),
                              true /* checkIfPointInTimeIsInFuture */,
                              false /* ignoreRemovedShards */));

    // Whole cluster
    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards(approximatedPlacement).value,
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        earliestClusterTime,
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */));

    assertPlacementsEqual(
        ExpectedResponseBuilder{}.setShards(approximatedPlacement).value,
        shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                        boost::none,
                                                        Timestamp(11, 0),
                                                        true /* checkIfPointInTimeIsInFuture */,
                                                        false /* ignoreRemovedShards */));
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
        earliestClusterTime - 1,
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    ShardingCatalogManager::get(opCtx)->cleanUpPlacementHistory(opCtx, earliestClusterTime);

    auto historicalPlacement_cleanup_coll1 = shardingCatalogManager().getHistoricalPlacement(
        opCtx,
        NamespaceString::createNamespaceString_forTest("db.collection1"),
        earliestClusterTime - 1,
        true /* checkIfPointInTimeIsInFuture */,
        false /* ignoreRemovedShards */);

    // before cleanup
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2"}).value,
                          historicalPlacement_coll1);

    // after cleanup
    assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2"}).value,
                          historicalPlacement_cleanup_coll1);
}

TEST_F(GetHistoricalPlacementTestFixture,
       Given_CurrentClusterTime_When_PlacementHistoryRequested_Then_ReturnPlacementHistory) {
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
    // placement history from the 'currentConfigTime' regardless of the
    // 'checkIfPointInTimeIsInFuture' value.
    {
        ASSERT_GREATER_THAN_OR_EQUALS(currentConfigTime, placementHistoryTs);

        auto collNss = NamespaceString::createNamespaceString_forTest("db.collection1");
        auto dbOnlyNss = NamespaceString::createNamespaceString_forTest("db");
        assertPlacementsEqual(
            ExpectedResponseBuilder{}.setShards({"shard1", "shard2"}).value,
            shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                            collNss,
                                                            currentConfigTime,
                                                            true /* checkIfPointInTimeIsInFuture */,
                                                            false /* ignoreRemovedShards */));

        assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2"}).value,
                              shardingCatalogManager().getHistoricalPlacement(
                                  opCtx,
                                  collNss,
                                  currentConfigTime,
                                  false /* checkIfPointInTimeIsInFuture */,
                                  false /* ignoreRemovedShards */));

        assertPlacementsEqual(
            ExpectedResponseBuilder{}.setShards({"shard1", "shard2"}).value,
            shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                            dbOnlyNss,
                                                            currentConfigTime,
                                                            true /* checkIfPointInTimeIsInFuture */,
                                                            false /* ignoreRemovedShards */));

        assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2"}).value,
                              shardingCatalogManager().getHistoricalPlacement(
                                  opCtx,
                                  dbOnlyNss,
                                  currentConfigTime,
                                  false /* checkIfPointInTimeIsInFuture */,
                                  false /* ignoreRemovedShards */));

        assertPlacementsEqual(
            ExpectedResponseBuilder{}.setShards({"shard1", "shard2"}).value,
            shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                            boost::none,
                                                            currentConfigTime,
                                                            true /* checkIfPointInTimeIsInFuture */,
                                                            false /* ignoreRemovedShards */));

        assertPlacementsEqual(ExpectedResponseBuilder{}.setShards({"shard1", "shard2"}).value,
                              shardingCatalogManager().getHistoricalPlacement(
                                  opCtx,
                                  boost::none,
                                  currentConfigTime,
                                  false /* checkIfPointInTimeIsInFuture */,
                                  false /* ignoreRemovedShards */));
    }
}

TEST_F(
    GetHistoricalPlacementTestFixture,
    Given_CurrentClusterTime_When_PlacementHistoryRequestedInTheFuture_Then_ReturnPlacementHistoryStatusDependingOnTheCheckIfPointInTimeIsInFutureFlag) {
    auto opCtx = operationContext();

    // Set up config shard and placement history information.
    Timestamp placementHistoryTs(10, 0);
    setupConfigPlacementHistory(opCtx,
                                {{placementHistoryTs, "db", {"shard1"}},
                                 {placementHistoryTs, "db.collection1", {"shard1", "shard2"}}});
    setupConfigShard(opCtx, 3 /*nShards*/);

    setShardIdsInShardRegistry(opCtx, {"shard1", "shard2"});

    const auto& vcTime = VectorClock::get(opCtx)->getTime();
    Timestamp currentConfigTime = vcTime.configTime().asTimestamp();

    // Ensure 'tsInTheFuture' is greater than 'currentConfigTime'.
    Timestamp timeInTheFuture = currentConfigTime + 1;
    ASSERT_GREATER_THAN(timeInTheFuture, currentConfigTime);

    auto collNss = NamespaceString::createNamespaceString_forTest("db.collection1");
    auto dbOnlyNss = NamespaceString::createNamespaceString_forTest("db");

    // Ensure that fetching placement history returns HistoricalPlacementStatus::FutureClusterTime,
    // when requesting placement history from the future config time if
    // 'checkIfPointInTimeIsInFuture' is set to true.
    for (bool ignoreRemovedShards : {true, false}) {
        // Collection-level query.
        auto historicalPlacement =
            shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                            collNss,
                                                            timeInTheFuture,
                                                            true /* checkIfPointInTimeIsInFuture */,
                                                            ignoreRemovedShards);
        assertPlacementsEqual(
            ExpectedResponseBuilder{HistoricalPlacementStatus::FutureClusterTime}.value,
            historicalPlacement);

        // Database-level query.
        historicalPlacement =
            shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                            dbOnlyNss,
                                                            timeInTheFuture,
                                                            true /* checkIfPointInTimeIsInFuture */,
                                                            ignoreRemovedShards);
        assertPlacementsEqual(
            ExpectedResponseBuilder{HistoricalPlacementStatus::FutureClusterTime}.value,
            historicalPlacement);

        // Whole-cluster query.
        historicalPlacement =
            shardingCatalogManager().getHistoricalPlacement(opCtx,
                                                            boost::none,
                                                            timeInTheFuture,
                                                            true /* checkIfPointInTimeIsInFuture */,
                                                            ignoreRemovedShards);
        assertPlacementsEqual(
            ExpectedResponseBuilder{HistoricalPlacementStatus::FutureClusterTime}.value,
            historicalPlacement);
    }

    // Ensure that fetching placement history does not return
    // HistoricalPlacementStatus::FutureClusterTime, when requesting placement history from the
    // future config time if 'checkIfPointInTimeIsInFuture' is set to false.
    for (bool ignoreRemovedShards : {true, false}) {
        auto historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            collNss,
            currentConfigTime,
            false /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);
        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1", "shard2"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);

        historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            dbOnlyNss,
            currentConfigTime,
            false /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);
        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1", "shard2"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);

        historicalPlacement = shardingCatalogManager().getHistoricalPlacement(
            opCtx,
            boost::none,
            currentConfigTime,
            false /* checkIfPointInTimeIsInFuture */,
            ignoreRemovedShards);
        assertPlacementsEqual(
            ExpectedResponseBuilder{}
                .setShards({"shard1", "shard2"})
                .setAnyRemovedShardDetected(false,
                                            ignoreRemovedShards
                                                ? ChangeStreamReadMode::kIgnoreRemovedShards
                                                : ChangeStreamReadMode::kStrict)
                .value,
            historicalPlacement);
    }
}

}  // unnamed namespace
}  // namespace mongo
