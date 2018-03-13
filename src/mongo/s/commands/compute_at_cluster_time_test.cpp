/**
 * Copyright (C) 2018 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

const ShardId shardOneId("shardOne");
const HostAndPort shardOne("shardOne:1234");

const ShardId shardTwoId("shardTwo");
const HostAndPort shardTwo("shardTwo:1234");

class AtClusterTimeTest : public ShardingTestFixture {
protected:
    void setUp() {
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);
        std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
        shardInfos.push_back(std::make_tuple(shardOneId, shardOne));
        shardInfos.push_back(std::make_tuple(shardTwoId, shardTwo));

        ShardingTestFixture::addRemoteShards(shardInfos);
    }
};

TEST_F(AtClusterTimeTest, ComputeValidValid) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    LogicalTime timeOne(Timestamp(10, 2));
    shardOne->updateLastCommittedOpTime(timeOne);
    ASSERT_EQ(timeOne, shardOne->getLastCommittedOpTime());

    auto shardTwo = shardRegistry()->getShardNoReload(shardTwoId);
    LogicalTime timeTwo(Timestamp(15, 1));
    shardTwo->updateLastCommittedOpTime(timeTwo);
    ASSERT_EQ(timeTwo, shardTwo->getLastCommittedOpTime());

    auto maxTime = computeAtClusterTimeForShards(operationContext(), {shardOneId, shardTwoId});
    ASSERT_EQ(maxTime, timeTwo);
}

TEST_F(AtClusterTimeTest, ComputeValidInvalid) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    ASSERT_EQ(LogicalTime(), shardOne->getLastCommittedOpTime());

    auto shardTwo = shardRegistry()->getShardNoReload(shardTwoId);
    LogicalTime timeTwo(Timestamp(15, 1));
    shardTwo->updateLastCommittedOpTime(timeTwo);
    ASSERT_EQ(timeTwo, shardTwo->getLastCommittedOpTime());

    auto maxTime = computeAtClusterTimeForShards(operationContext(), {shardOneId, shardTwoId});
    ASSERT_EQ(maxTime, timeTwo);
}

TEST_F(AtClusterTimeTest, ComputeInvalidInvalid) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    ASSERT_EQ(LogicalTime(), shardOne->getLastCommittedOpTime());

    auto shardTwo = shardRegistry()->getShardNoReload(shardTwoId);
    ASSERT_EQ(LogicalTime(), shardTwo->getLastCommittedOpTime());

    auto maxTime = computeAtClusterTimeForShards(operationContext(), {shardOneId, shardTwoId});
    ASSERT_EQ(maxTime, LogicalTime());
}

const NamespaceString kNss = NamespaceString("test", "coll");

class AtClusterTimeTargetingTest : public CatalogCacheTestFixture {
protected:
    void setUp() {
        CatalogCacheTestFixture::setUp();
        CatalogCacheTestFixture::setupNShards(2);

        // Set up a logical clock with an initial time.
        auto logicalClock = stdx::make_unique<LogicalClock>(serviceContext());
        LogicalTime initialTime(Timestamp(10, 1));
        logicalClock->setClusterTimeFromTrustedSource(initialTime);
        LogicalClock::set(serviceContext(), std::move(logicalClock));
    }
};

// Verifies that the latest in-memory logical time is always returned.
//
// TODO SERVER-33767: Once the multi-versioned routing table is integrated into global snapshot
// reads, replace this test with one that verifies the latest known committed optime for the
// targeted shards is returned, unless different shards would be targeted at that time, in which
// case the latest in-memory logical time is returned.
TEST_F(AtClusterTimeTargetingTest, AlwaysReturnsLatestInMemoryTime) {
    auto routingInfo = loadRoutingTableWithTwoChunksAndTwoShards(kNss);
    auto query = BSON("find" << kNss.coll());
    auto collation = BSONObj();
    auto shards = getTargetedShardsForQuery(operationContext(), routingInfo, query, collation);

    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

    LogicalTime time(Timestamp(5, 1));
    shardRegistry()->getShardNoReload(ShardId("0"))->updateLastCommittedOpTime(time);

    // The latest lastCommittedOpTime for a targeted shard should be ignored, with the latest
    // in-memory logical time returned instead.
    ASSERT_NE(time,
              *computeAtClusterTime(operationContext(), routingInfo, shards, query, collation));
    ASSERT_EQ(LogicalClock::get(operationContext())->getClusterTime(),
              *computeAtClusterTime(operationContext(), routingInfo, shards, query, collation));
}

// Verifies that a null logical time is returned for all requests without snapshot readConcern.
TEST_F(AtClusterTimeTargetingTest, NonSnapshotReadConcern) {
    auto routingInfo = loadRoutingTableWithTwoChunksAndTwoShards(kNss);
    auto query = BSON("find" << kNss.coll());
    auto collation = BSONObj();
    auto shards = getTargetedShardsForQuery(operationContext(), routingInfo, query, collation);

    // Uninitialized read concern.
    ASSERT_FALSE(computeAtClusterTime(operationContext(), routingInfo, shards, query, collation));

    auto& readConcernArgs = repl::ReadConcernArgs::get(operationContext());

    // Local readConcern.
    readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_FALSE(computeAtClusterTime(operationContext(), routingInfo, shards, query, collation));

    // Majority readConcern.
    readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);
    ASSERT_FALSE(computeAtClusterTime(operationContext(), routingInfo, shards, query, collation));

    // Linearizable readConcern.
    readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kLinearizableReadConcern);
    ASSERT_FALSE(computeAtClusterTime(operationContext(), routingInfo, shards, query, collation));

    // Available readConcern.
    readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kAvailableReadConcern);
    ASSERT_FALSE(computeAtClusterTime(operationContext(), routingInfo, shards, query, collation));
}

}  // namespace
}  // namespace mongo
