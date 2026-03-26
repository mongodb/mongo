/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/unittest/log_test.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

class ShardRegistryTest : service_context_test::WithSetupTransportLayer,
                          public ShardingTestFixture {
protected:
    void setUp() override {
        // Setting the role is important for topologyTime gossiping.
        serverGlobalParams.clusterRole = {ClusterRole::RouterServer};
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(kConfigHostAndPort);
    }

    LogicalTime getLastKnownTopologyTime() {
        const auto time = VectorClock::get(operationContext())->getTime();
        return time.topologyTime();
    }

    ShardType shardIdToShardType(ShardId id) {
        ShardType shardType;
        shardType.setName(id.toString());
        const auto connString = ConnectionString::forReplicaSet(
            id.toString() + "-replset", {HostAndPort(id.toString(), kDummyPort)});
        shardType.setHost(connString.toString());
        return shardType;
    }

    // Adds a shard to the fixture. This involves creating the entry, and bumping the VectorClock
    // topology time if advanceTopologyTime.
    void addShard(ShardId id, bool advanceTopologyTime) {
        configsvrTopologyTime = Timestamp(addRemoveShardCounterForTopologyTime++, 0);

        auto shardType = shardIdToShardType(id);
        shardType.setTopologyTime(configsvrTopologyTime);
        shards.push_back(shardType);

        if (advanceTopologyTime) {
            VectorClock::get(operationContext())
                ->advanceTopologyTime_forTest(LogicalTime(configsvrTopologyTime));
        }
    }

    // Adds a shard with a custom ShardType (name and connection string). Used when testing
    // host-to-shard mapping with specific hosts (e.g. recovery after host reassignment).
    void addShard(ShardType shardType, bool advanceTopologyTime = true) {
        configsvrTopologyTime = Timestamp(addRemoveShardCounterForTopologyTime++, 0);
        shardType.setTopologyTime(configsvrTopologyTime);
        shards.push_back(shardType);
        if (advanceTopologyTime) {
            VectorClock::get(operationContext())
                ->advanceTopologyTime_forTest(LogicalTime(configsvrTopologyTime));
        }
    }

    // Clears the fixture's shard list so a new topology can be set up (e.g. to simulate
    // config.shards change before recovery).
    void clearShards() {
        shards.clear();
    }

    // Adds a shard to the fixture, simulating a config.shards with no topologyTime in any of the
    // entries. Old versions did not have the topologyTime, when upgrading the shard registry must
    // still populate the cache.
    void addShardWithoutTopologyTime(ShardId id) {
        auto shardType = shardIdToShardType(id);
        shards.push_back(shardType);
    }

    // Removes a shard from the fixture. Drops the entry, and updates another entry's topologyTime.
    // If advanceTopologyTime is true, advances the VectorClock.
    void removeShard(ShardId id, bool advanceTopologyTime) {
        invariant(shards.size() >= 2);
        configsvrTopologyTime = Timestamp(addRemoveShardCounterForTopologyTime++, 0);

        shards.erase(
            std::remove_if(shards.begin(),
                           shards.end(),
                           [&](ShardType& shard) { return shard.getName() == id.toString(); }),
            shards.end());

        // Recreate ShardType, resetting topologyTime is not allowed.
        auto& topologyTimeUpdateShard = shards[0];
        auto shardWithUpdatedTopologyTime = shardIdToShardType(topologyTimeUpdateShard.getName());
        shardWithUpdatedTopologyTime.setTopologyTime(configsvrTopologyTime);

        topologyTimeUpdateShard = shardWithUpdatedTopologyTime;
        if (advanceTopologyTime) {
            VectorClock::get(operationContext())
                ->advanceTopologyTime_forTest(LogicalTime(configsvrTopologyTime));
        }
    }

    // Expect the ShardRegistry to query the config server.
    void expectCSRSLookup() {
        expectGetShards(shards);
    }

    // Expect the ShardRegistry to send a ping to the config server.
    void expectPing() {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            ASSERT_EQ(request.target, kConfigHostAndPort);
            ASSERT(request.cmdObj.hasField("ping"));

            // Gossip topology time.
            BSONObjBuilder bob;
            bob.append(VectorClock::kTopologyTimeFieldName, configsvrTopologyTime);
            return bob.obj();
        });
    }

    void assertShardIdsFromRegistry(const std::vector<ShardId>& fromShardRegistry) {
        ASSERT_EQ(fromShardRegistry.size(), shards.size());

        auto registryCopy = fromShardRegistry;
        std::sort(registryCopy.begin(), registryCopy.end(), [](auto& a, auto& b) {
            return a.toString() < b.toString();
        });

        auto shardsCopy = shards;
        std::sort(shardsCopy.begin(), shardsCopy.end(), [](auto& a, auto& b) {
            return a.getName() < b.getName();
        });

        for (size_t i = 0; i < shards.size(); i++) {
            ASSERT_EQ(registryCopy[i].toString(), shardsCopy[i].getName());
        }
    }

    void reloadAndWait() {
        auto future = launchAsync([this] { shardRegistry()->reload(operationContext()); });
        // If there was no hit to CSRS, this would hang.
        expectCSRSLookup();
        future.default_timed_get();
    }

    // ShardRegistry functions attempt an extra refresh when no shards are found. To simplify our
    // life, we test the core _getData function directly.
    auto getData() {
        return shardRegistry()->_getData(operationContext());
    }

    auto makeTimeForForceReload() {
        return ShardRegistry::Time::makeForForcedReload();
    }

    auto makeTimeLatestKnown() {
        return ShardRegistry::Time::makeLatestKnown(getServiceContext());
    }

    auto makeTimeWithLookup(std::function<Timestamp(void)>&& lookupFn) {
        return ShardRegistry::Time::makeWithLookup(std::move(lookupFn));
    }

    const int kDummyPort{12345};
    const HostAndPort kConfigHostAndPort{"DummyConfig", kDummyPort};

    std::vector<ShardType> shards;
    int addRemoveShardCounterForTopologyTime = 1;

    const bool kAdvanceTopologyTime = true;
    const bool kDoNotAdvanceTopologyTime = false;
    Timestamp configsvrTopologyTime;

    unittest::MinimumLoggedSeverityGuard logSeverityGuard{logv2::LogComponent::kSharding,
                                                          logv2::LogSeverity::Debug(2)};

    size_t getLatestConnStringsSize() {
        return shardRegistry()->_latestConnStrings.size();
    }

    void clearForRecovery() {
        shardRegistry()->_clearForRecovery(operationContext());
    }

    // Sets up the host reassignment scenario: initial topology has shard1 (host1,2,3) and shard2
    // (host4,5,6). Then it simulates a config change where host3 moves to shard2. After this, the
    // mock config has the new topology but the registry still has the old view in memory.
    void setupHostReassignmentScenario() {
        addShard(ShardType("shard1", "shard1RS/host1:27017,host2:27017,host3:27017"));
        addShard(ShardType("shard2", "shard2RS/host4:27017,host5:27017,host6:27017"));

        auto future = launchAsync([this] { shardRegistry()->getAllShardIds(operationContext()); });
        expectCSRSLookup();
        future.default_timed_get();

        ASSERT_EQ(2u, shardRegistry()->getAllShardIds(operationContext()).size());
        auto shardForHost3 = shardRegistry()->getShardForHostNoReload(HostAndPort("host3", 27017));
        ASSERT(shardForHost3) << "host3 should be found after initial load";
        ASSERT_EQ(ShardId("shard1"), shardForHost3->getId())
            << "host3 should initially belong to shard1";

        // Simulate topology change: host3 moves from shard1 to shard2 in config.shards.
        clearShards();
        addShard(ShardType("shard1", "shard1RS/host1:27017,host2:27017"));
        addShard(ShardType("shard2", "shard2RS/host3:27017,host4:27017,host5:27017,host6:27017"));

        auto shardForHost3AfterReassignment =
            shardRegistry()->getShardForHostNoReload(HostAndPort("host3", 27017));
        ASSERT(shardForHost3AfterReassignment) << "host3 should be found after reassignment";
        ASSERT_EQ(ShardId("shard1"), shardForHost3AfterReassignment->getId())
            << "host3 should still belong to shard1 after reassignment";
    }
};

namespace {

TEST_F(ShardRegistryTest, FirstGetDataDoesLookup) {
    auto future = launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
    expectCSRSLookup();
    future.default_timed_get();
}

TEST_F(ShardRegistryTest, SimpleAddShard) {
    // Reload when config.shards is empty.
    reloadAndWait();
    // Add a shard and make the topologyTime known to this node.
    addShard({"0"}, kAdvanceTopologyTime);

    auto future = launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
    expectCSRSLookup();
    future.default_timed_get();
}

TEST_F(ShardRegistryTest, SimpleRemoveShard) {
    addShard({"0"}, kAdvanceTopologyTime);
    addShard({"1"}, kAdvanceTopologyTime);
    reloadAndWait();

    // Remove a shard and make the topologyTime known to this node.
    removeShard({"1"}, kAdvanceTopologyTime);

    auto future = launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
    expectCSRSLookup();
    future.default_timed_get();
}

TEST_F(ShardRegistryTest, LookupGossipsInTopologyTime) {
    // A shard has been added, this node is not aware of the new topologyTime.
    addShard({"0"}, kDoNotAdvanceTopologyTime);

    auto topologyTimeBefore = getLastKnownTopologyTime();

    // Force reload, which discovers new topology. This should also cause the new topologyTime to be
    // gossiped in.
    reloadAndWait();

    // Sanity check that VectorClock was not advanced before reload.
    ASSERT_LT(topologyTimeBefore.asTimestamp(), configsvrTopologyTime);

    auto topologyTimeAfter = getLastKnownTopologyTime();
    ASSERT_EQ(topologyTimeAfter.asTimestamp(), configsvrTopologyTime);
}

TEST_F(ShardRegistryTest, ForceReloadDoesLookupWithoutNewTopologyTime) {
    addShard({"0"}, kDoNotAdvanceTopologyTime);

    // Case 1: cache is empty.
    reloadAndWait();
    assertShardIdsFromRegistry(shardRegistry()->getAllShardIds(operationContext()));

    addShard({"1"}, kDoNotAdvanceTopologyTime);

    // Case 2: cache is populated, a lookup is expected anyways.
    reloadAndWait();
    assertShardIdsFromRegistry(shardRegistry()->getAllShardIds(operationContext()));
}

TEST_F(ShardRegistryTest, GetDataDoesNoLookupAfterReloadWithNoShards) {
    reloadAndWait();
    // Given that we have already performed a force reload, even if the result from the CSRS was
    // empty, subsequent getData() should not block.
    getData();
}

TEST_F(ShardRegistryTest, GetDataDoesNoLookupAfterReloadWithShards) {
    // A shard has been added, this node is not aware of the new topologyTime.
    addShard({"0"}, kDoNotAdvanceTopologyTime);

    // Force reload, which discovers new topology.
    reloadAndWait();

    // Following get requests should not reach out to CSRS.
    assertShardIdsFromRegistry(getData()->getAllShardIds());
}

TEST_F(ShardRegistryTest, GetDataDoesLookupWithNewTopologyTime) {
    addShard({"0"}, kAdvanceTopologyTime);
    // The registry should reach out the CSRS, cache was empty.
    {
        auto future =
            launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
        expectCSRSLookup();
        future.default_timed_get();
    }

    // A shard has been added, and the topologyTime gossiped.
    addShard({"1"}, kAdvanceTopologyTime);

    // The registry should reach out the CSRS, cache was not empty.
    auto future = launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
    expectCSRSLookup();
    future.default_timed_get();
}

TEST_F(ShardRegistryTest, GetDataDoesNoLookupWithoutNewTopologyTime) {
    // A shard has been added, and the topologyTime gossiped.
    addShard({"0"}, kAdvanceTopologyTime);

    // The registry should reach out the CSRS.
    {
        auto future =
            launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
        expectCSRSLookup();
        future.default_timed_get();
    }

    // Subsequent getData calls should not lookup. A lookup to CSRS would hang the test.
    getData();
}

// There used to be a bug where an empty result would store Timestamp(0,0) in cache, and cause
// topologyTime updates to never advance the timeInStore.
TEST_F(ShardRegistryTest, GetDataWithNewTopologyTimeAfterEmptyLookup) {
    // Reload on when config.shards is empty.
    reloadAndWait();
    // Add a shard and make the topologyTime known to this node.
    addShard({"0"}, kAdvanceTopologyTime);

    // If the new topologyTime does not advance timeInStore, the test would hang because no lookup
    // would be done.
    auto future = launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
    expectCSRSLookup();
    future.default_timed_get();
}

TEST_F(ShardRegistryTest, PeriodicReloaderUpdatesTopologyTime) {
    // Add a shard and make the topologyTime known to this node.
    addShard({"0"}, kDoNotAdvanceTopologyTime);

    // Sanity check that VectorClock was not advanced before starting periodic reloader.
    auto topologyTimeBefore = getLastKnownTopologyTime();
    ASSERT_LT(topologyTimeBefore.asTimestamp(), configsvrTopologyTime);

    shardRegistry()->startupPeriodicReloader(operationContext());

    expectPing();

    auto topologyTimeAfter = getLastKnownTopologyTime();
    ASSERT_EQ(topologyTimeAfter.asTimestamp(), configsvrTopologyTime);

    expectCSRSLookup();
}

TEST_F(ShardRegistryTest, TopologyTimeMonotonicityViolation) {
    // Unless there's an entry in the cache, there is no timeInStore to compare.
    reloadAndWait();

    addShard({"0"}, kAdvanceTopologyTime);

    // Corrupt the topologyTime into the future.
    auto vc = VectorClock::get(operationContext());
    auto time = vc->getTime();
    auto topologyTime = time.topologyTime().asTimestamp();
    VectorClock::get(operationContext())
        ->advanceTopologyTime_forTest(LogicalTime(Timestamp(topologyTime.getSecs() + 100, 0)));

    auto future = launchAsync([this] { getData(); });
    expectCSRSLookup();
    ASSERT_THROWS_CODE(future.default_timed_get(),
                       DBException,
                       ErrorCodes::ReadThroughCacheTimeMonotonicityViolation);
}

TEST_F(ShardRegistryTest, ConfigShardsWithNoTopologyTimeDueToUpgrade) {
    // Add a shard without a topologyTime. This can be the case when coming from older versions.
    addShardWithoutTopologyTime({"0"});

    auto future = launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
    expectCSRSLookup();
    future.default_timed_get();
}

// TODO (SERVER-102087): remove or adapt.
TEST_F(ShardRegistryTest, ConfigShardsWithNoTopologyTimeWithGossipedTopologyTime) {
    // Add a shard without a topologyTime. This can be the case when coming from older versions.
    addShardWithoutTopologyTime({"0"});

    // When a key is not in the ReadThroughCache, timeInStore is Time(). Thus, the expected time
    // is T(0, 0), and the result is never considered inconsistent.
    reloadAndWait();

    // Corrupt the topologyTime.
    VectorClock::get(operationContext())
        ->advanceTopologyTime_forTest(LogicalTime(Timestamp(100, 0)));

    // To prevent a cluster impacted by SERVER-63742 from crashing the server in benign scenarios
    // (config.shards without any topologyTime), the ShardRegistry accepts the corrupted time.
    {
        auto future =
            launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
        expectCSRSLookup();
        future.default_timed_get();
    }

    reloadAndWait();

    // Usually, a force reload without subsequent topologyTime changes should not cause a lookup.
    // However, given that the forced lookup installs a timeInStore using the config.shards
    // topologyTime, which is lesser than the corrupted time in the vector clock, subsequent
    // ShardRegistry operation will cause a lookup.
    auto future = launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
    expectCSRSLookup();
    future.default_timed_get();
}

/////// ShardRegistry::Time

TEST_F(ShardRegistryTest, ForceReloadTimeGreaterThanLatestKnown) {
    auto latestKnown = makeTimeLatestKnown();
    auto forceReload = makeTimeForForceReload();
    ASSERT_GT(forceReload, latestKnown);
}

TEST_F(ShardRegistryTest, ForceReloadGreaterThanAnyLookup) {
    auto lookupTime = makeTimeWithLookup([]() { return Timestamp::max(); });
    auto forceReloadTime = makeTimeForForceReload();
    ASSERT_GT(forceReloadTime, lookupTime);
}

TEST_F(ShardRegistryTest, NewerForceReloadGreaterThanOlder) {
    auto olderTime = makeTimeForForceReload();
    auto newerTime = makeTimeForForceReload();
    ASSERT_GT(newerTime, olderTime);
}

TEST_F(ShardRegistryTest, LookupTimeGreaterThanPreviousForceReload) {
    auto forceReloadTime = makeTimeForForceReload();
    auto lookupTime =
        makeTimeWithLookup([]() { return VectorClock::kInitialComponentTime.asTimestamp(); });
    ASSERT_GT(lookupTime, forceReloadTime);
}

TEST_F(ShardRegistryTest, NewerLookupTimeGreaterThanOlder) {
    auto olderTime = makeTimeWithLookup([]() { return Timestamp(1, 1); });
    auto newerTime = makeTimeWithLookup([]() { return Timestamp(2, 1); });
    ASSERT_GT(newerTime, olderTime);
}

TEST_F(ShardRegistryTest, FlushShardRegistryClearsCachedConnectionStrings) {
    ASSERT_EQ(getLatestConnStringsSize(), 1);  // This is the config shard
    clearForRecovery();
    ASSERT_EQ(getLatestConnStringsSize(), 0);
}

TEST_F(ShardRegistryTest, FlushShardRegistryReloadForRecovery) {
    setupHostReassignmentScenario();

    auto future = launchAsync([this] {
        auto [cachedTimeBefore,
              forceReloadIncrementBefore,
              cachedTimeAfter,
              forceReloadIncrementAfter] = shardRegistry()->reloadForRecovery(operationContext());
        ASSERT_GT(forceReloadIncrementAfter, forceReloadIncrementBefore);
        ASSERT_GT(cachedTimeAfter, cachedTimeBefore);
    });
    expectCSRSLookup();
    future.default_timed_get();

    auto shardForHost3 = shardRegistry()->getShardForHostNoReload(HostAndPort("host3", 27017));
    ASSERT(shardForHost3) << "host3 should be found after recovery";
    ASSERT_EQ(ShardId("shard2"), shardForHost3->getId())
        << "host3 should now belong to shard2 after reassignment";
}

TEST_F(ShardRegistryTest, toBSONEmptyRegistry) {
    reloadAndWait();

    BSONObjBuilder builder;
    shardRegistry()->toBSON(&builder);
    auto result = builder.obj();

    ASSERT_TRUE(result.hasField("map"));
    ASSERT_TRUE(result.hasField("hosts"));
    ASSERT_TRUE(result.hasField("connStrings"));
    auto map = result["map"].Obj();
    // With no shards, the map should only contain the config shard
    ASSERT_EQ(map.nFields(), 1);

    ASSERT_TRUE(result.hasField("timeInStore"));
    std::string timeInStoreStr = result["timeInStore"].String();
    ASSERT_TRUE(timeInStoreStr.find("forceReloadIncrement") != std::string::npos);
    ASSERT_TRUE(timeInStoreStr.find("topologyTime") != std::string::npos);
}

TEST_F(ShardRegistryTest, toBSONWithShards) {
    addShard({"shard0"}, kAdvanceTopologyTime);
    addShard({"shard1"}, kAdvanceTopologyTime);

    auto future = launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
    expectCSRSLookup();
    future.default_timed_get();

    BSONObjBuilder builder;
    shardRegistry()->toBSON(&builder);
    auto result = builder.obj();

    ASSERT_TRUE(result.hasField("map"));
    ASSERT_TRUE(result.hasField("hosts"));
    ASSERT_TRUE(result.hasField("connStrings"));
    auto map = result["map"].Obj();
    ASSERT_GTE(map.nFields(), 2);
    ASSERT_TRUE(map.hasField("shard0"));
    ASSERT_TRUE(map.hasField("shard1"));

    ASSERT_TRUE(result.hasField("timeInStore"));
    std::string timeInStoreStr = result["timeInStore"].String();
    ASSERT_TRUE(timeInStoreStr.find("topologyTime") != std::string::npos);
}

}  // namespace
}  // namespace mongo
