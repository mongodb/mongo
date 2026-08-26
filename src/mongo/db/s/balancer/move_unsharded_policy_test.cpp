// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/balancer/move_unsharded_policy.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/s/balancer/cluster_statistics_impl.h"
#include "mongo/db/s/balancer/migration_test_fixture.h"
#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <ostream>
#include <set>
#include <string>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

const DatabaseName kDbName = DatabaseName::createDatabaseName_forTest(boost::none, "TestDb");
const auto kNamespace = NamespaceString::createNamespaceString_forTest(kDbName, "TestColl");
const int kSizeOnDisk = 1;

class MoveUnshardedPolicyTest : public MigrationTestFixture {
protected:
    MoveUnshardedPolicyTest() : _clusterStats(std::make_unique<ClusterStatisticsImpl>()) {}

    std::vector<ClusterStatistics::ShardStatistics> getShardStats(OperationContext* opCtx) {
        return uassertStatusOK(_clusterStats.get()->getStats(opCtx));
    }

    stdx::unordered_set<ShardId> getAllShardIds(OperationContext* opCtx) {
        const auto& shards = shardRegistry()->getAllShardIds(opCtx);
        return stdx::unordered_set<ShardId>(shards.begin(), shards.end());
    }

    std::unique_ptr<ClusterStatistics> _clusterStats;
    MoveUnshardedPolicy _unshardedPolicy;
};


TEST_F(MoveUnshardedPolicyTest, MigrateUnsplittableCollection) {

    unittest::ServerParameterGuard serverParamController{"reshardingMinimumOperationDurationMillis",
                                                         5000};

    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);

    // Enable failpoint to return random collections
    FailPointEnableBlock fp("balancerShouldReturnRandomMigrations");

    // Override collections batch size to 4 for speeding up the test
    FailPointEnableBlock overrideBatchSizeGuard("overrideStatsForBalancingBatchSize",
                                                BSON("size" << 4));

    // Set up 1 unsplittable collections
    constexpr int numCollections = 1;
    std::vector<CollectionType> collections;
    for (auto i = 0; i < numCollections; ++i) {
        collections.emplace_back(setUpUnsplittableCollection(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl" + std::to_string(i)),
            kShardId0));
    }

    auto availableShards = getAllShardIds(operationContext());
    const auto& migrateInfoVector =
        _unshardedPolicy.selectCollectionsToMove(operationContext(),
                                                 getShardStats(operationContext()),
                                                 &availableShards,
                                                 true /*onlyTrackedCollections*/);
    ASSERT_EQ(1, migrateInfoVector.size());
    ASSERT_EQ(collections[0].getUuid(), migrateInfoVector[0].uuid);
}


TEST_F(MoveUnshardedPolicyTest, MigrateAnyCollectionFPOn) {

    unittest::ServerParameterGuard serverParamController{"reshardingMinimumOperationDurationMillis",
                                                         5000};

    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);


    // Enable failpoint to return random collections
    FailPointEnableBlock fp("balancerShouldReturnRandomMigrations");

    // Override collections batch size to 4 for speeding up the test
    FailPointEnableBlock overrideBatchSizeGuard("overrideStatsForBalancingBatchSize",
                                                BSON("size" << 4));

    const std::vector<CollectionType> collections = [&] {
        // Add three unsplittable (unsharded) collections
        std::vector<CollectionType> collections;
        collections.emplace_back(setUpUnsplittableCollection(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl_unsplittable_1"),
            kShardId0));
        collections.emplace_back(setUpUnsplittableCollection(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl_unsplittable_2"),
            kShardId0));
        collections.emplace_back(setUpUnsplittableCollection(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl_unsplittable_3"),
            kShardId0));
        return collections;
    }();

    std::set<NamespaceString> collectionsToCheck;
    for (auto& collection : collections) {
        collectionsToCheck.insert(collection.getNss());
    }

    int attemptsLeft = collectionsToCheck.size() * 50;
    while (!collectionsToCheck.empty() && attemptsLeft > 0) {

        auto availableShards = getAllShardIds(operationContext());

        const auto& migrateInfoVector =
            _unshardedPolicy.selectCollectionsToMove(operationContext(),
                                                     getShardStats(operationContext()),
                                                     &availableShards,
                                                     true /*onlyTrackedCollections*/);

        ASSERT_EQ(1, migrateInfoVector.size());
        std::cout << "Removing " << migrateInfoVector[0].nss.toString_forTest() << std::endl;
        collectionsToCheck.erase(migrateInfoVector[0].nss);

        attemptsLeft--;
    }
    // If we fail here, the balancer is (with very high probability) not picking randomly
    ASSERT(attemptsLeft > 0);
}

TEST_F(MoveUnshardedPolicyTest, DontMigrateAnyCollectionIfReshardingMinimumDurationIsTooLarge) {
    unittest::ServerParameterGuard serverParamController{"reshardingMinimumOperationDurationMillis",
                                                         5001};

    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);

    // Enable failpoint to return random collections
    FailPointEnableBlock fp("balancerShouldReturnRandomMigrations");

    // Override collections batch size to 4 for speeding up the test
    FailPointEnableBlock overrideBatchSizeGuard("overrideStatsForBalancingBatchSize",
                                                BSON("size" << 4));

    // Set up 1 unsplittable collections
    constexpr int numCollections = 1;
    std::vector<CollectionType> collections;
    for (auto i = 0; i < numCollections; ++i) {
        collections.emplace_back(setUpUnsplittableCollection(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl" + std::to_string(i)),
            kShardId0));
    }

    auto availableShards = getAllShardIds(operationContext());
    const auto& migrateInfoVector =
        _unshardedPolicy.selectCollectionsToMove(operationContext(),
                                                 getShardStats(operationContext()),
                                                 &availableShards,
                                                 true /*onlyTrackedCollections*/);
    ASSERT_EQ(0, migrateInfoVector.size());
}

TEST_F(MoveUnshardedPolicyTest, SkipMoveCollectionThresholdOfOneAlwaysSkipsWhenShardedCollections) {
    unittest::ServerParameterGuard serverParamController{"reshardingMinimumOperationDurationMillis",
                                                         5000};

    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);

    // A threshold of 1.0 means the balancer should always skip moveCollection in favor of a chunk
    // migration whenever there are sharded collections that can be balanced.
    FailPointEnableBlock fp("balancerShouldReturnRandomMigrations",
                            BSON("skipMoveCollectionThreshold" << 1.0));

    // Override collections batch size to 4 for speeding up the test
    FailPointEnableBlock overrideBatchSizeGuard("overrideStatsForBalancingBatchSize",
                                                BSON("size" << 4));

    // Set up an unsplittable collection that could be moved.
    setUpUnsplittableCollection(
        NamespaceString::createNamespaceString_forTest(kDbName, "TestColl_unsplittable"),
        kShardId0);

    // Set up a sharded collection so there is something to balance via chunk migrations.
    const auto shardedNss =
        NamespaceString::createNamespaceString_forTest(kDbName, "TestColl_sharded");
    const auto shardedUUID = UUID::gen();
    const ChunkVersion version({OID::gen(), Timestamp(42)}, {2, 0});
    setUpCollection(shardedNss, shardedUUID, version);

    // Run several rounds to be confident the skip is deterministic and not random luck.
    for (int i = 0; i < 20; ++i) {
        auto availableShards = getAllShardIds(operationContext());
        const auto& migrateInfoVector =
            _unshardedPolicy.selectCollectionsToMove(operationContext(),
                                                     getShardStats(operationContext()),
                                                     &availableShards,
                                                     true /*onlyTrackedCollections*/);
        ASSERT_EQ(0, migrateInfoVector.size());
    }
}

TEST_F(MoveUnshardedPolicyTest, SkipMoveCollectionThresholdOfZeroNeverSkips) {
    unittest::ServerParameterGuard serverParamController{"reshardingMinimumOperationDurationMillis",
                                                         5000};

    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);

    // A threshold of 0.0 means the balancer should never skip moveCollection, even when there are
    // sharded collections that could be balanced.
    FailPointEnableBlock fp("balancerShouldReturnRandomMigrations",
                            BSON("skipMoveCollectionThreshold" << 0.0));

    // Override collections batch size to 4 for speeding up the test
    FailPointEnableBlock overrideBatchSizeGuard("overrideStatsForBalancingBatchSize",
                                                BSON("size" << 4));

    // Set up an unsplittable collection that should always be moved.
    const auto unsplittableColl = setUpUnsplittableCollection(
        NamespaceString::createNamespaceString_forTest(kDbName, "TestColl_unsplittable"),
        kShardId0);

    // Set up a sharded collection to ensure it is the threshold, not the absence of sharded
    // collections, that drives the behavior.
    const auto shardedNss =
        NamespaceString::createNamespaceString_forTest(kDbName, "TestColl_sharded");
    const auto shardedUUID = UUID::gen();
    const ChunkVersion version({OID::gen(), Timestamp(42)}, {2, 0});
    setUpCollection(shardedNss, shardedUUID, version);

    auto availableShards = getAllShardIds(operationContext());
    const auto& migrateInfoVector =
        _unshardedPolicy.selectCollectionsToMove(operationContext(),
                                                 getShardStats(operationContext()),
                                                 &availableShards,
                                                 true /*onlyTrackedCollections*/);
    ASSERT_EQ(1, migrateInfoVector.size());
    ASSERT_EQ(unsplittableColl.getUuid(), migrateInfoVector[0].uuid);
}

}  // namespace
}  // namespace mongo
