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
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/balancer/cluster_statistics_impl.h"
#include "mongo/db/s/balancer/migration_test_fixture.h"
#include "mongo/db/s/balancer/move_unsharded_policy.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

const DatabaseName kDbName = DatabaseName::createDatabaseName_forTest(boost::none, "TestDb");
const auto kNamespace = NamespaceString::createNamespaceString_forTest(kDbName, "TestColl");
const int kSizeOnDisk = 1;

class MoveUnshardedPolicyTest : public MigrationTestFixture {
protected:
    MoveUnshardedPolicyTest() : _clusterStats(std::make_unique<ClusterStatisticsImpl>()) {}

    /**
     * Sets up mock network to expect a listDatabases command and returns a BSON response with
     * a dummy sizeOnDisk.
     */
    void expectListDatabasesCommand() {
        BSONObjBuilder resultBuilder;
        CommandHelpers::appendCommandStatusNoThrow(resultBuilder, Status::OK());

        onCommand([&resultBuilder](const RemoteCommandRequest& request) {
            ASSERT(request.cmdObj["listDatabases"]);
            std::vector<BSONObj> dbInfos;
            BSONObjBuilder b;
            b.append("name", kDbName.toString_forTest());
            b.append("sizeOnDisk", kSizeOnDisk);
            b.append("empty", kSizeOnDisk > 0);
            resultBuilder.append("databases", dbInfos);
            resultBuilder.append("totalSize", kSizeOnDisk);
            return resultBuilder.obj();
        });
    }

    /**
     * Sets up mock network for all the shards to expect the commands executed for computing cluster
     * stats, which include listDatabase and serverStatus.
     */
    void expectGetStatsCommands(int numShards) {
        for (int i = 0; i < numShards; i++) {
            expectListDatabasesCommand();
        }
    }


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

    auto future = launchAsync([&] {
        ThreadClient tc(getServiceContext()->getService());
        auto opCtx = Client::getCurrent()->makeOperationContext();
        auto availableShards = getAllShardIds(opCtx.get());

        const auto& migrateInfoVector = _unshardedPolicy.selectCollectionsToMove(
            opCtx.get(), getShardStats(opCtx.get()), &availableShards);
        ASSERT_EQ(1, migrateInfoVector.size());
        ASSERT_EQ(collections[0].getUuid(), migrateInfoVector[0].uuid);
    });

    expectGetStatsCommands(2 /*numShards*/);
    future.default_timed_get();
}


TEST_F(MoveUnshardedPolicyTest, MigrateAnyCollectionFPOn) {
    setupShards({kShard0, kShard1});
    setupDatabase(kDbName, kShardId0);


    // Enable failpoint to return random collections
    FailPointEnableBlock fp("balancerShouldReturnRandomMigrations");

    // Override collections batch size to 4 for speeding up the test
    FailPointEnableBlock overrideBatchSizeGuard("overrideStatsForBalancingBatchSize",
                                                BSON("size" << 4));

    const std::vector<CollectionType> collections = [&] {
        // Add a sharded and two unsplittable (unsharded) collections
        std::vector<CollectionType> collections;
        collections.emplace_back(setUpUnsplittableCollection(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl_unsplittable_1"),
            kShardId0));
        collections.emplace_back(setUpUnsplittableCollection(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl_unsplittable_3"),
            kShardId0));
        ChunkVersion defaultVersion({OID::gen(), Timestamp(42)}, {2, 0});
        ChunkRange keyRange{kKeyPattern.globalMin(), kKeyPattern.globalMax()};
        std::vector<ChunkType> chunks = {{UUID::gen(), keyRange, defaultVersion, kShardId0}};
        collections.emplace_back(setupCollection(
            NamespaceString::createNamespaceString_forTest(kDbName, "TestColl_single_chunk_2"),
            kKeyPattern,
            chunks));
        return collections;
    }();

    std::set<NamespaceString> collectionsToCheck;
    for (auto& collection : collections) {
        if (collection.getUnsplittable()) {
            collectionsToCheck.insert(collection.getNss());
        }
    }

    int attemptsLeft = collectionsToCheck.size() * 50;
    while (!collectionsToCheck.empty() && attemptsLeft > 0) {
        auto future = launchAsync([&] {
            ThreadClient tc(getServiceContext()->getService());
            auto opCtx = Client::getCurrent()->makeOperationContext();
            auto availableShards = getAllShardIds(opCtx.get());

            const auto& migrateInfoVector = _unshardedPolicy.selectCollectionsToMove(
                opCtx.get(), getShardStats(opCtx.get()), &availableShards);

            ASSERT_EQ(1, migrateInfoVector.size());
            std::cout << "Removing " << migrateInfoVector[0].nss.toString_forTest() << std::endl;
            collectionsToCheck.erase(migrateInfoVector[0].nss);
        });

        expectGetStatsCommands(2 /*numShards*/);
        future.default_timed_get();
        attemptsLeft--;
    }
    // If we fail here, the balancer is (with very high probability) not picking randomly
    ASSERT(attemptsLeft > 0);
}


}  // namespace
}  // namespace mongo
