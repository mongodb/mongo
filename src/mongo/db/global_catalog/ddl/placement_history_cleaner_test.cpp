/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/global_catalog/ddl/placement_history_cleaner.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_catalog_manager.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/global_catalog/type_namespace_placement_gen.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/session/logical_session_cache_noop.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/shard_role/ddl/ddl_lock_manager.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/uuid.h"

#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const ShardId kShard0{"shard0"};
const ShardId kShard1{"shard1"};
const HostAndPort kShard0Host{"shard0:12345"};
const HostAndPort kShard1Host{"shard1:12345"};

// Builds a skeleton for the 'serverStatus' response, containing just the 'earliestOpTime'
// field consumed by the PlacementHistoryCleaner.
BSONObj makeServerStatusOplogResponse(Timestamp earliestOptime) {
    return BSON("ok" << 1 << "oplog" << BSON("earliestOptime" << earliestOptime));
}

class DDLCoordinatorServiceMock : public DDLLockManager::Recoverable {
public:
    void waitForRecovery(OperationContext*) const override {}
};

// ---------------------------------------------------------------------------
// Main fixture: sharding state recovered, two shards registered.
// ---------------------------------------------------------------------------
class PlacementHistoryCleanerTest : public ConfigServerTestFixture {
public:
    // Aggregation pipelines in getHistoricalPlacement may need to spill to disk.
    PlacementHistoryCleanerTest() : ConfigServerTestFixture(Options{}.enableSpillEngine()) {}

    void setUp() override {
        ConfigServerTestFixture::setUp();

        // Mark sharding role recovery as completed so awaitClusterRoleRecovery() resolves
        // immediately inside runOnce().
        ShardingState::get(getServiceContext())
            ->setRecoveryCompleted({OID::gen(),
                                    {ClusterRole::ShardServer, ClusterRole::ConfigServer},
                                    ConnectionString::forLocal(),
                                    ShardId("config")});

        // Create indexes for config.placementHistory and other config collections.
        ASSERT_OK(ShardingCatalogManager::get(operationContext())
                      ->initializeConfigDatabaseIfNeeded(operationContext()));

        // The DDL lock manager must be "recovered" so that getHistoricalPlacement() can acquire
        // its shared lock on config.placementHistory.
        DDLLockManager::get(getServiceContext())->setRecoverable(_ddlRecoverable.get());

        // Services required by the transaction API used inside cleanUpPlacementHistory().
        DBDirectClient dbClient(operationContext());
        dbClient.createCollection(NamespaceString::kSessionTransactionsTableNamespace);
        dbClient.createIndexes(NamespaceString::kSessionTransactionsTableNamespace,
                               {MongoDSessionCatalog::getConfigTxnPartialIndexSpec()});
        ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());
        LogicalSessionCache::set(getServiceContext(), std::make_unique<LogicalSessionCacheNoop>());
        TransactionCoordinatorService::get(operationContext())
            ->initializeIfNeeded(operationContext(), /* term */ 1);
        WaitForMajorityService::get(getServiceContext()).startup(getServiceContext());

        // Populate the shard registry with two remote shards so that
        // getEarliestOpLogTimestampAmongAllShards() has recipients to query.
        //
        // On a ConfigServerTestFixture the catalog client reads config.shards locally, so
        // addRemoteShards() (which expects a mock-network find command) would deadlock.
        // The correct approach for this fixture is: insert shard docs, reload the registry
        // synchronously, then configure mock targeters on the resulting shard objects.
        ShardType shard0Doc;
        shard0Doc.setName(kShard0.toString());
        shard0Doc.setHost(kShard0Host.toString());
        ShardType shard1Doc;
        shard1Doc.setName(kShard1.toString());
        shard1Doc.setHost(kShard1Host.toString());
        setupShards({shard0Doc, shard1Doc});
        shardRegistry()->reload(operationContext());

        RemoteCommandTargeterMock::get(
            shardRegistry()->getShard(operationContext(), kShard0).getValue()->getTargeter())
            ->setFindHostReturnValue(kShard0Host);
        RemoteCommandTargeterMock::get(
            shardRegistry()->getShard(operationContext(), kShard1).getValue()->getTargeter())
            ->setFindHostReturnValue(kShard1Host);
    }

    void tearDown() override {
        TransactionCoordinatorService::get(operationContext())->interruptForStepDown();
        WaitForMajorityService::get(getServiceContext()).shutDown();
        ConfigServerTestFixture::tearDown();
    }

    NamespacePlacementType insertPlacementChangeDoc(const std::string& nss,
                                                    const Timestamp& timestamp,
                                                    const std::vector<ShardId>& shards = {}) {
        NamespacePlacementType doc(
            NamespaceString::createNamespaceString_forTest(nss), timestamp, shards);
        doc.setUuid(UUID::gen());
        ASSERT_OK(insertToConfigCollection(operationContext(),
                                           NamespaceString::kConfigsvrPlacementHistoryNamespace,
                                           doc.toBSON()));
        return doc;
    }

    // Insert the operational initialization marker at the given timestamp.
    // `shards` is empty for the "real" init marker (the one used by getHistoricalPlacement to
    // determine that accurate data is available) and non-empty for the dawn-of-time approximation.
    void insertInitMarker(Timestamp ts, std::vector<ShardId> shards = {}) {
        NamespacePlacementType marker(
            ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker, ts, shards);
        ASSERT_OK(insertToConfigCollection(operationContext(),
                                           NamespaceString::kConfigsvrPlacementHistoryNamespace,
                                           marker.toBSON()));
    }

    int countPlacementDocs() {
        PersistentTaskStore<NamespacePlacementType> store(
            NamespaceString::kConfigsvrPlacementHistoryNamespace);
        return store.count(operationContext(), BSONObj());
    }

    std::vector<BSONObj> getInitMarkerDocs() {
        const auto initMarkerNssStr = NamespaceStringUtil::serialize(
            ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker,
            SerializationContext::stateDefault());

        DBDirectClient client(operationContext());
        FindCommandRequest req{NamespaceString::kConfigsvrPlacementHistoryNamespace};
        req.setFilter(BSON(NamespacePlacementType::kNssFieldName << initMarkerNssStr));
        req.setSort(BSON(NamespacePlacementType::kTimestampFieldName << 1));
        auto cursor = client.find(std::move(req));

        std::vector<BSONObj> docs;
        while (cursor->more()) {
            docs.push_back(cursor->nextSafe().getOwned());
        }
        return docs;
    }

    std::vector<BSONObj> getPlacementChangeDocs() {
        const auto initMarkerNssStr = NamespaceStringUtil::serialize(
            ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker,
            SerializationContext::stateDefault());

        DBDirectClient client(operationContext());
        FindCommandRequest req{NamespaceString::kConfigsvrPlacementHistoryNamespace};
        req.setFilter(
            BSON(NamespacePlacementType::kNssFieldName << BSON("$ne" << initMarkerNssStr)));
        req.setSort(BSON(NamespacePlacementType::kTimestampFieldName << 1));
        auto cursor = client.find(std::move(req));

        std::vector<BSONObj> docs;
        while (cursor->more()) {
            docs.push_back(cursor->nextSafe().getOwned());
        }
        return docs;
    }

    std::vector<BSONObj> getPlacementChangeDocsByNss(const std::string& nss) {
        const auto nssStr =
            NamespaceStringUtil::serialize(NamespaceString::createNamespaceString_forTest(nss),
                                           SerializationContext::stateDefault());

        DBDirectClient client(operationContext());
        FindCommandRequest req{NamespaceString::kConfigsvrPlacementHistoryNamespace};
        req.setFilter(BSON(NamespacePlacementType::kNssFieldName << nssStr));
        req.setSort(BSON(NamespacePlacementType::kTimestampFieldName << 1));
        auto cursor = client.find(std::move(req));

        std::vector<BSONObj> docs;
        while (cursor->more()) {
            docs.push_back(cursor->nextSafe().getOwned());
        }
        return docs;
    }

private:
    ReadWriteConcernDefaultsLookupMock _lookupMock;
    std::unique_ptr<DDLCoordinatorServiceMock> _ddlRecoverable =
        std::make_unique<DDLCoordinatorServiceMock>();
    // getHistoricalPlacement() requires this feature flag to return real placement data.
    // TODO (SERVER-98118): Remove once featureFlagChangeStreamPreciseShardTargeting is last-lts.
    RAIIServerParameterControllerForTest _preciseTargeting{
        "featureFlagChangeStreamPreciseShardTargeting", true};
};

// ---------------------------------------------------------------------------
// Fixture used specifically to test the recovery-failure branch of runOnce().
// In this fixture ShardingState recovery is intentionally failed, so
// awaitClusterRoleRecovery().get() throws and runOnce() returns early.
// ---------------------------------------------------------------------------
class PlacementHistoryCleanerRecoveryFailedTest : public ConfigServerTestFixture {
public:
    void setUp() override {
        ConfigServerTestFixture::setUp();
        ShardingState::get(getServiceContext())
            ->setRecoveryFailed({ErrorCodes::InternalError, "recovery failed for test"});
    }
};

// ---------------------------------------------------------------------------
// Doc count is at or below the minimum threshold ->
// early return, no network calls, placement history is unchanged.
// ---------------------------------------------------------------------------
TEST_F(PlacementHistoryCleanerTest, SkipsBelowThreshold) {
    insertInitMarker(Timestamp(0, 1), {kShard0, kShard1});
    insertInitMarker(Timestamp(0, 2), {});
    insertPlacementChangeDoc("db", Timestamp(1, 0), {kShard0});
    insertPlacementChangeDoc("db.coll", Timestamp(1, 1), {kShard0, kShard1});
    insertPlacementChangeDoc("db", Timestamp(10, 0), {kShard1});

    const auto initMarkersBefore = getInitMarkerDocs();
    const auto placementDocsBefore = getPlacementChangeDocs();
    // minEntries = 10 > 3 docs -> cleaner must skip without sending any network command.
    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        PlacementHistoryCleaner::runOnce(Client::getCurrent(), 10 /* minEntries */);
    });
    future.default_timed_get();

    const auto initMarkersAfter = getInitMarkerDocs();
    ASSERT_EQ(initMarkersBefore.size(), initMarkersAfter.size());
    for (size_t i = 0; i < initMarkersBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(initMarkersBefore[i], initMarkersAfter[i]);
    }

    const auto placementDocsAfter = getPlacementChangeDocs();
    ASSERT_EQ(placementDocsBefore.size(), placementDocsAfter.size());
    for (size_t i = 0; i < placementDocsBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(placementDocsBefore[i], placementDocsAfter[i]);
    }
}

// ---------------------------------------------------------------------------
// One shard returns an error from serverStatus ->
// getEarliestOpLogTimestampAmongAllShards returns boost::none -> early return.
// ---------------------------------------------------------------------------
TEST_F(PlacementHistoryCleanerTest, SkipsWhenShardResponseFails) {
    insertInitMarker(Timestamp(0, 1), {kShard0, kShard1});
    insertInitMarker(Timestamp(0, 2), {});
    insertPlacementChangeDoc("db", Timestamp(1, 0), {kShard0});
    insertPlacementChangeDoc("db.coll", Timestamp(1, 1), {kShard0, kShard1});
    insertPlacementChangeDoc("db", Timestamp(10, 0), {kShard1});

    const auto initMarkersBefore = getInitMarkerDocs();
    const auto placementDocsBefore = getPlacementChangeDocs();

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        PlacementHistoryCleaner::runOnce(Client::getCurrent(), 0 /* minEntries */);
    });

    // sendCommandToShards() dispatches to all shards asynchronously. Respond to both; the error
    // on one is enough for getEarliestOpLogTimestampAmongAllShards to return boost::none.
    onCommand([](const executor::RemoteCommandRequest&) {
        return BSON("ok" << 0 << "code" << static_cast<int>(ErrorCodes::TemporarilyUnavailable)
                         << "errmsg"
                         << "shard unreachable");
    });
    onCommand([](const executor::RemoteCommandRequest&) {
        return makeServerStatusOplogResponse(Timestamp(50, 0));
    });

    future.default_timed_get();

    const auto initMarkersAfter = getInitMarkerDocs();
    ASSERT_EQ(initMarkersBefore.size(), initMarkersAfter.size());
    for (size_t i = 0; i < initMarkersBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(initMarkersBefore[i], initMarkersAfter[i]);
    }

    const auto placementDocsAfter = getPlacementChangeDocs();
    ASSERT_EQ(placementDocsBefore.size(), placementDocsAfter.size());
    for (size_t i = 0; i < placementDocsBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(placementDocsBefore[i], placementDocsAfter[i]);
    }
}

// ---------------------------------------------------------------------------
// All shards report Timestamp(0,0) as earliestOptime (oplog never rolled) ->
// getEarliestOpLogTimestampAmongAllShards returns boost::none -> early return.
// ---------------------------------------------------------------------------
TEST_F(PlacementHistoryCleanerTest, SkipsWhenAllShardsReturnZeroTimestamp) {
    insertInitMarker(Timestamp(0, 1), {kShard0, kShard1});
    insertInitMarker(Timestamp(0, 2), {});

    insertPlacementChangeDoc("db", Timestamp(1, 0), {kShard0});
    insertPlacementChangeDoc("db.coll", Timestamp(1, 1), {kShard0, kShard1});
    insertPlacementChangeDoc("db", Timestamp(10, 0), {kShard1});

    const auto initMarkersBefore = getInitMarkerDocs();
    const auto placementDocsBefore = getPlacementChangeDocs();

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        PlacementHistoryCleaner::runOnce(Client::getCurrent(), 0 /* minEntries */);
    });

    onCommand([](const executor::RemoteCommandRequest&) {
        return makeServerStatusOplogResponse(Timestamp(0, 0));
    });
    onCommand([](const executor::RemoteCommandRequest&) {
        return makeServerStatusOplogResponse(Timestamp(0, 0));
    });

    future.default_timed_get();

    const auto initMarkersAfter = getInitMarkerDocs();
    ASSERT_EQ(initMarkersBefore.size(), initMarkersAfter.size());
    for (size_t i = 0; i < initMarkersBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(initMarkersBefore[i], initMarkersAfter[i]);
    }

    const auto placementDocsAfter = getPlacementChangeDocs();
    ASSERT_EQ(placementDocsBefore.size(), placementDocsAfter.size());
    for (size_t i = 0; i < placementDocsBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(placementDocsBefore[i], placementDocsAfter[i]);
    }
}

// ---------------------------------------------------------------------------
// The initialization marker timestamp is >= earliestOptime:
// the match query finds a document -> early return (safe to skip because the
// oplog on at least one shard still covers the init point).
// ---------------------------------------------------------------------------
TEST_F(PlacementHistoryCleanerTest, SkipsWhenInitMarkerIsAheadOfEarliestOptime) {
    // Init marker at T=200; shards will report earliestOptime T=100. Since 200 >= 100 the
    // cleaner must skip.
    insertInitMarker(Timestamp(0, 1), {kShard0, kShard1});
    insertInitMarker(Timestamp(200, 0));
    insertPlacementChangeDoc("db", Timestamp(150, 0), {kShard0});
    insertPlacementChangeDoc("db.coll", Timestamp(200, 0), {kShard0, kShard1});
    insertPlacementChangeDoc("db", Timestamp(200, 1), {kShard1});

    const auto initMarkersBefore = getInitMarkerDocs();
    const auto placementDocsBefore = getPlacementChangeDocs();

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        PlacementHistoryCleaner::runOnce(Client::getCurrent(), 0 /* minEntries */);
    });

    onCommand([](const executor::RemoteCommandRequest&) {
        return makeServerStatusOplogResponse(Timestamp(100, 0));
    });
    onCommand([](const executor::RemoteCommandRequest&) {
        return makeServerStatusOplogResponse(Timestamp(100, 0));
    });

    future.default_timed_get();

    const auto initMarkersAfter = getInitMarkerDocs();
    ASSERT_EQ(initMarkersBefore.size(), initMarkersAfter.size());
    for (size_t i = 0; i < initMarkersBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(initMarkersBefore[i], initMarkersAfter[i]);
    }

    const auto placementDocsAfter = getPlacementChangeDocs();
    ASSERT_EQ(placementDocsBefore.size(), placementDocsAfter.size());
    for (size_t i = 0; i < placementDocsBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(placementDocsBefore[i], placementDocsAfter[i]);
    }
}

// -------------------------------------------------------------
// All conditions are met: cleanUpPlacementHistory() is invoked,
// the initialization marker is advanced to earliestOptime
// -------------------------------------------------------------
TEST_F(PlacementHistoryCleanerTest, PerformsCleanupWhenAllConditionsAreMet) {
    Timestamp earliestOpTime(150, 1);
    Timestamp earliestOpTimePlusOne(150, 2);
    Timestamp earliestOpTimeMinusOne(150, 0);
    // Init marker at T=10 (earlier than earliestOptime T=150) so the guard query finds no doc
    // with timestamp >= T=150 -> cleanup proceeds.
    // The dawn-of-time approximation doc at Timestamp(0,1) lets getHistoricalPlacement() return
    // an approximated (non-NotAvailable) response for queries that pre-date the init point.
    insertInitMarker(Timestamp(10, 0));
    insertInitMarker(Timestamp(0, 1), {kShard0, kShard1});
    const auto initMarkersBefore = getInitMarkerDocs();

    // Will be deleted: the doc was created before the cut point.
    const auto _ = insertPlacementChangeDoc("db", Timestamp(1, 1), {kShard1});
    // Will be kept: the doc was creates before the cut point, but it represents the most recent
    // placement change for the namespace.
    const auto expectedDbPlacement =
        insertPlacementChangeDoc("db", earliestOpTimeMinusOne, {kShard0});
    // Will be deleted.
    const auto __ = insertPlacementChangeDoc("db.coll", earliestOpTimeMinusOne, {kShard0, kShard1});
    // Will be kept.
    const auto expectedCollPlacement =
        insertPlacementChangeDoc("db.coll", earliestOpTime, {kShard1});

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        PlacementHistoryCleaner::runOnce(Client::getCurrent(), 0 /* minEntries */);
    });

    // Set earliest op times on each shard (The minimum value will be the one setting the cut
    // point).
    onCommand([&](const executor::RemoteCommandRequest&) {
        return makeServerStatusOplogResponse(earliestOpTime);
    });
    onCommand([&](const executor::RemoteCommandRequest&) {
        return makeServerStatusOplogResponse(earliestOpTimePlusOne);
    });

    future.default_timed_get();

    // cleanUpPlacementHistory() keeps the 'dawn of time' information...
    const auto initMarkersAfter = getInitMarkerDocs();
    ASSERT_EQ(2U, initMarkersAfter.size());
    ASSERT_BSONOBJ_EQ(initMarkersBefore[0], initMarkersAfter[0]);
    // .. while bumping the timestamp of the 'operational boundary' metadata doc.
    const NamespacePlacementType expectedInitMarkerAfterCleanup(
        ShardingCatalogClient::kConfigPlacementHistoryInitializationMarker, earliestOpTime, {});
    ASSERT_EQ(expectedInitMarkerAfterCleanup, NamespacePlacementType::parse(initMarkersAfter[1]));

    // "db" at earliestOpTimeMinusOne survived: it is the most recent entry for an existing
    // namespace, so it must be preserved even though it predates the cut point.
    {
        const auto dbDocs = getPlacementChangeDocsByNss("db");
        ASSERT_EQ(1U, dbDocs.size());
        const auto dbPlacementAfterCleanup = NamespacePlacementType::parse(dbDocs[0]);
        ASSERT_EQ(dbPlacementAfterCleanup, expectedDbPlacement);
    }

    // "db.coll": only the entry at earliestOpTime survived; the earlier one was superseded and
    // predated the cut point, so it was deleted.
    {
        const auto dbCollDocs = getPlacementChangeDocsByNss("db.coll");
        ASSERT_EQ(1U, dbCollDocs.size());
        const auto collPlacementAfterCleanup = NamespacePlacementType::parse(dbCollDocs[0]);
        ASSERT_EQ(collPlacementAfterCleanup, expectedCollPlacement);
    }
}

// ---------------------------------------------------------------------------
// cleanUpPlacementHistory() is reached but no empty-shards init marker exists:
// bumpInitializationTimeOnPlacementHistory() uasserts; runOnce() catches the
// error and leaves the collection unchanged.
// ---------------------------------------------------------------------------
TEST_F(PlacementHistoryCleanerTest, CleanupFailsWhenEmptyShardsInitMarkerIsMissing) {
    // Use a 'corrupted dawn of time doc' to allow
    insertInitMarker(Timestamp(150, 0), {kShard0, kShard1});
    const auto initMarkersBefore = getInitMarkerDocs();

    insertPlacementChangeDoc("db", Timestamp(10, 0), {kShard0});
    insertPlacementChangeDoc("db.coll", Timestamp(200, 0), {kShard0, kShard1});
    insertPlacementChangeDoc("db", Timestamp(200, 1), {kShard1});
    const auto placementDocsBefore = getPlacementChangeDocs();

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        PlacementHistoryCleaner::runOnce(Client::getCurrent(), 0 /* minEntries */);
    });

    // The dawn-of-time marker at T(0,1) does not satisfy timestamp >= T(100,0), so the
    // guard query finds nothing and runOnce proceeds to invoke cleanUpPlacementHistory().
    onCommand([](const executor::RemoteCommandRequest&) {
        return makeServerStatusOplogResponse(Timestamp(100, 0));
    });
    onCommand([](const executor::RemoteCommandRequest&) {
        return makeServerStatusOplogResponse(Timestamp(100, 0));
    });

    future.default_timed_get();

    // bumpInitializationTimeOnPlacementHistory() found no matching doc and uasserted;
    // runOnce() caught the exception and left the collection unchanged.
    const auto initMarkersAfter = getInitMarkerDocs();
    ASSERT_EQ(initMarkersBefore.size(), initMarkersAfter.size());
    for (size_t i = 0; i < initMarkersBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(initMarkersBefore[i], initMarkersAfter[i]);
    }

    const auto placementDocsAfter = getPlacementChangeDocs();
    ASSERT_EQ(placementDocsBefore.size(), placementDocsAfter.size());
    for (size_t i = 0; i < placementDocsBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(placementDocsBefore[i], placementDocsAfter[i]);
    }
}

// ---------------------------------------------------------------------------
// cleanUpPlacementHistory() is reached with two empty-shards init markers:
// the cleanup operation gets aborted by
// ShardingCatalogManager::bumpInitializationTimeOnPlacementHistory() and nothing gets cleaned up.
// ---------------------------------------------------------------------------
TEST_F(PlacementHistoryCleanerTest, CleanupSucceedsWithMultipleEmptyShardsInitMarkers) {
    const Timestamp earliestOpTime(100, 0);

    insertInitMarker(Timestamp(0, 1), {kShard0, kShard1});
    insertInitMarker(Timestamp(5, 0));
    insertInitMarker(Timestamp(8, 0));
    const auto initMarkersBefore = getInitMarkerDocs();
    insertPlacementChangeDoc("db", Timestamp(10, 0), {kShard0});
    insertPlacementChangeDoc("db.coll", Timestamp(200, 0), {kShard0, kShard1});
    insertPlacementChangeDoc("db", Timestamp(200, 1), {kShard1});
    const auto placementDocsBefore = getPlacementChangeDocs();

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        PlacementHistoryCleaner::runOnce(Client::getCurrent(), 0 /* minEntries */);
    });

    onCommand([&](const executor::RemoteCommandRequest&) {
        return makeServerStatusOplogResponse(earliestOpTime);
    });
    onCommand([&](const executor::RemoteCommandRequest&) {
        return makeServerStatusOplogResponse(earliestOpTime);
    });

    future.default_timed_get();

    const auto initMarkersAfter = getInitMarkerDocs();
    ASSERT_EQ(initMarkersBefore.size(), initMarkersAfter.size());
    for (size_t i = 0; i < initMarkersBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(initMarkersBefore[i], initMarkersAfter[i]);
    }

    const auto placementDocsAfter = getPlacementChangeDocs();
    ASSERT_EQ(placementDocsBefore.size(), placementDocsAfter.size());
    for (size_t i = 0; i < placementDocsBefore.size(); ++i) {
        ASSERT_BSONOBJ_EQ(placementDocsBefore[i], placementDocsAfter[i]);
    }
}

// ---------------------------------------------------------------------------
// Sharding state recovery has failed -> runOnce() catches the
// DBException from awaitClusterRoleRecovery() and returns immediately.
// ---------------------------------------------------------------------------
TEST_F(PlacementHistoryCleanerRecoveryFailedTest, SkipsWhenShardingStateRecoveryFailed) {
    // No DB setup or network mocks are needed: runOnce() must return before touching anything.
    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        PlacementHistoryCleaner::runOnce(Client::getCurrent(), 0 /* minEntries */);
    });
    // If runOnce() hangs or crashes the test fails; a clean return is the expected outcome.
    future.default_timed_get();
}

}  // namespace
}  // namespace mongo
