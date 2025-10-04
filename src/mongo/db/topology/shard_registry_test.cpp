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
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/unittest/death_test.h"
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
    // The registry should reach out the the CSRS, cache was empty.
    {
        auto future =
            launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
        expectCSRSLookup();
        future.default_timed_get();
    }

    // A shard has been added, and the topologyTime gossiped.
    addShard({"1"}, kAdvanceTopologyTime);

    // The registry should reach out the the CSRS, cache was not empty.
    auto future = launchAsync([this] { assertShardIdsFromRegistry(getData()->getAllShardIds()); });
    expectCSRSLookup();
    future.default_timed_get();
}

TEST_F(ShardRegistryTest, GetDataDoesNoLookupWithoutNewTopologyTime) {
    // A shard has been added, and the topologyTime gossiped.
    addShard({"0"}, kAdvanceTopologyTime);

    // The registry should reach out the the CSRS.
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

}  // namespace
}  // namespace mongo
