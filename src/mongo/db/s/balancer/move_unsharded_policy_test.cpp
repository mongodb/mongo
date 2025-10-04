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

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/string_data.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/balancer/cluster_statistics_impl.h"
#include "mongo/db/s/balancer/migration_test_fixture.h"
#include "mongo/db/s/balancer/move_unsharded_policy.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

#include <memory>
#include <ostream>
#include <set>
#include <string>

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

    RAIIServerParameterControllerForTest serverParamController{
        "reshardingMinimumOperationDurationMillis", 5000};

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

    RAIIServerParameterControllerForTest serverParamController{
        "reshardingMinimumOperationDurationMillis", 5000};

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
    RAIIServerParameterControllerForTest serverParamController{
        "reshardingMinimumOperationDurationMillis", 5001};

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

}  // namespace
}  // namespace mongo
