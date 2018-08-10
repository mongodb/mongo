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
#include "mongo/s/transaction/at_cluster_time_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);

const ShardId shardOneId("shardOne");
const HostAndPort shardOne("shardOne:1234");

const ShardId shardTwoId("shardTwo");
const HostAndPort shardTwo("shardTwo:1234");

const NamespaceString kNss = NamespaceString("test", "coll");
const BSONObj kEmptyQuery;
const BSONObj kEmptyCollation;
const LogicalTime kInMemoryLogicalTime(Timestamp(3, 1));

class AtClusterTimeTest : public ShardingTestFixture {
protected:
    void setUp() {
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);
        std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
        shardInfos.push_back(std::make_tuple(shardOneId, shardOne));
        shardInfos.push_back(std::make_tuple(shardTwoId, shardTwo));

        ShardingTestFixture::addRemoteShards(shardInfos);

        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        // Set up a logical clock with an initial time.
        auto logicalClock = stdx::make_unique<LogicalClock>(getServiceContext());
        logicalClock->setClusterTimeFromTrustedSource(kInMemoryLogicalTime);
        LogicalClock::set(getServiceContext(), std::move(logicalClock));
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

    auto maxTime = at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, {shardOneId, shardTwoId}, kNss, kEmptyQuery, kEmptyCollation);
    // TODO: SERVER-31767
    // ASSERT_EQ(*maxTime, timeTwo);
    ASSERT_EQ(*maxTime, kInMemoryLogicalTime);
}

TEST_F(AtClusterTimeTest, ComputeValidInvalid) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    ASSERT_EQ(LogicalTime(), shardOne->getLastCommittedOpTime());

    auto shardTwo = shardRegistry()->getShardNoReload(shardTwoId);
    LogicalTime timeTwo(Timestamp(15, 1));
    shardTwo->updateLastCommittedOpTime(timeTwo);
    ASSERT_EQ(timeTwo, shardTwo->getLastCommittedOpTime());

    auto maxTime = at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, {shardOneId, shardTwoId}, kNss, kEmptyQuery, kEmptyCollation);
    // TODO: SERVER-31767
    // ASSERT_EQ(*maxTime, timeTwo);
    ASSERT_EQ(*maxTime, kInMemoryLogicalTime);
}

TEST_F(AtClusterTimeTest, ComputeInvalidInvalid) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    ASSERT_EQ(LogicalTime(), shardOne->getLastCommittedOpTime());

    auto shardTwo = shardRegistry()->getShardNoReload(shardTwoId);
    ASSERT_EQ(LogicalTime(), shardTwo->getLastCommittedOpTime());

    auto maxTime = at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, {shardOneId, shardTwoId}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT_EQ(*maxTime, kInMemoryLogicalTime);
}

TEST_F(AtClusterTimeTest, ComputeForOneShard) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);

    LogicalTime timeOne(Timestamp(10, 2));
    shardOne->updateLastCommittedOpTime(timeOne);
    ASSERT_EQ(timeOne, shardOne->getLastCommittedOpTime());

    auto atClusterTime =
        at_cluster_time_util::computeAtClusterTimeForOneShard(operationContext(), shardOneId);
    ASSERT_EQ(*atClusterTime, timeOne);
}

TEST_F(AtClusterTimeTest, ComputeForOneShardNoCachedOpTime) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    ASSERT_EQ(LogicalTime(), shardOne->getLastCommittedOpTime());

    auto atClusterTime =
        at_cluster_time_util::computeAtClusterTimeForOneShard(operationContext(), shardOneId);
    ASSERT_EQ(*atClusterTime, kInMemoryLogicalTime);
}

TEST_F(AtClusterTimeTest, ComputeForOneShardNoShard) {
    ASSERT_THROWS_CODE(at_cluster_time_util::computeAtClusterTimeForOneShard(operationContext(),
                                                                             ShardId("fakeShard")),
                       AssertionException,
                       ErrorCodes::ShardNotFound);
}

class AtClusterTimeTargetingTest : public CatalogCacheTestFixture {
protected:
    void setUp() {
        CatalogCacheTestFixture::setUp();
        CatalogCacheTestFixture::setupNShards(2);

        // Set up a logical clock with an initial time.
        auto logicalClock = stdx::make_unique<LogicalClock>(getServiceContext());
        logicalClock->setClusterTimeFromTrustedSource(kInMemoryLogicalTime);
        LogicalClock::set(getServiceContext(), std::move(logicalClock));
    }
};

// Verifies that the latest in-memory logical time is returned when one shard lastCommittedOpTime on
// one shard is not initialized.
TEST_F(AtClusterTimeTargetingTest, ReturnsLatestInMemoryTime) {
    auto routingInfo = loadRoutingTableWithTwoChunksAndTwoShards(kNss);
    auto query = BSON("find" << kNss.coll());
    auto collation = BSONObj();
    auto shards = getTargetedShardsForQuery(operationContext(), routingInfo, query, collation);

    LogicalTime time(Timestamp(2, 1));
    shardRegistry()->getShardNoReload(ShardId("0"))->updateLastCommittedOpTime(time);

    ASSERT_LT(time, kInMemoryLogicalTime);
    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
    ASSERT_EQ(kInMemoryLogicalTime,
              *at_cluster_time_util::computeAtClusterTime(
                  operationContext(), true, shards, kNss, query, collation));
}

// Verifies that the greatest logical time is returned when all shard's lastCommittedOpTime values
// are initialized.
TEST_F(AtClusterTimeTargetingTest, ReturnsLatestTimeFromShard) {
    auto routingInfo = loadRoutingTableWithTwoChunksAndTwoShards(kNss);
    auto query = BSON("find" << kNss.coll());
    auto collation = BSONObj();
    auto shards = getTargetedShardsForQuery(operationContext(), routingInfo, query, collation);

    LogicalTime time1(Timestamp(2, 1));
    shardRegistry()->getShardNoReload(ShardId("0"))->updateLastCommittedOpTime(time1);

    LogicalTime time2(Timestamp(4, 1));
    shardRegistry()->getShardNoReload(ShardId("1"))->updateLastCommittedOpTime(time2);

    ASSERT_LT(time1, kInMemoryLogicalTime);
    ASSERT_GT(time2, kInMemoryLogicalTime);

    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);
    // TODO: SERVER-31767
    // ASSERT_EQ(time2,
    //           *at_cluster_time_util::computeAtClusterTime(operationContext(), true, shards, kNss,
    //           query, collation));
    ASSERT_EQ(kInMemoryLogicalTime,
              *at_cluster_time_util::computeAtClusterTime(
                  operationContext(), true, shards, kNss, query, collation));
}

// Verifies that a null logical time is returned for all requests without snapshot readConcern.
TEST_F(AtClusterTimeTargetingTest, NonSnapshotReadConcern) {
    auto routingInfo = loadRoutingTableWithTwoChunksAndTwoShards(kNss);
    auto query = BSON("find" << kNss.coll());
    auto collation = BSONObj();
    auto shards = getTargetedShardsForQuery(operationContext(), routingInfo, query, collation);

    // Uninitialized read concern.
    ASSERT_FALSE(at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, shards, kNss, query, collation));

    auto& readConcernArgs = repl::ReadConcernArgs::get(operationContext());

    // Local readConcern.
    readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kLocalReadConcern);
    ASSERT_FALSE(at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, shards, kNss, query, collation));

    // Majority readConcern.
    readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);
    ASSERT_FALSE(at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, shards, kNss, query, collation));

    // Linearizable readConcern.
    readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kLinearizableReadConcern);
    ASSERT_FALSE(at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, shards, kNss, query, collation));

    // Available readConcern.
    readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kAvailableReadConcern);
    ASSERT_FALSE(at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, shards, kNss, query, collation));
}

// Verifies that if atClusterTime is specified in the request, atClusterTime is always greater than
// or equal to it.
TEST_F(AtClusterTimeTargetingTest, AfterClusterTime) {
    const auto afterClusterTime = LogicalTime(Timestamp(50, 2));
    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(afterClusterTime, repl::ReadConcernLevel::kSnapshotReadConcern);

    // This cannot be true in a real cluster, but is done to verify that the chosen atClusterTime
    // cannot be less than afterClusterTime.
    ASSERT_GT(afterClusterTime, kInMemoryLogicalTime);

    const auto s0 = ShardId("0");
    const auto s1 = ShardId("1");

    // Neither shard has a last committed optime.

    // Target one shard.
    auto computedTime = at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, {s0}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT(computedTime);
    ASSERT_GTE(*computedTime, afterClusterTime);

    // Target all shards.
    computedTime = at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, {s0, s1}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT(computedTime);
    ASSERT_GTE(*computedTime, afterClusterTime);

    // One shard has a last committed optime.

    LogicalTime time1(Timestamp(1, 1));
    shardRegistry()->getShardNoReload(s0)->updateLastCommittedOpTime(time1);
    ASSERT_LT(time1, afterClusterTime);

    // Target one shard.
    computedTime = at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, {s0}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT(computedTime);
    ASSERT_GTE(*computedTime, afterClusterTime);

    // Target all shards.
    computedTime = at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, {s0, s1}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT(computedTime);
    ASSERT_GTE(*computedTime, afterClusterTime);

    // Both shards have a last committed optime.

    LogicalTime time2(Timestamp(2, 1));
    shardRegistry()->getShardNoReload(s1)->updateLastCommittedOpTime(time2);
    ASSERT_LT(time2, afterClusterTime);

    // Target one shard.
    computedTime = at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, {s0}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT(computedTime);
    ASSERT_GTE(*computedTime, afterClusterTime);

    // Target all shards.
    computedTime = at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, {s0, s1}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT(computedTime);
    ASSERT_GTE(*computedTime, afterClusterTime);
}

// Verify that when afterClusterTime is given, the smallest computed atClusterTime is equal to
// afterClusterTime.
TEST_F(AtClusterTimeTargetingTest, AfterClusterTimeLowerBound) {
    auto afterClusterTime = LogicalTime(kInMemoryLogicalTime);
    repl::ReadConcernArgs::get(operationContext()) =
        repl::ReadConcernArgs(afterClusterTime, repl::ReadConcernLevel::kSnapshotReadConcern);

    ASSERT_EQ(afterClusterTime, kInMemoryLogicalTime);

    const auto s0 = ShardId("0");

    // Target one shard without a last committed optime. The computed value should equal
    // afterClusterTime.
    auto computedTime = at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, {s0}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT(computedTime);
    ASSERT_EQ(*computedTime, afterClusterTime);

    // Target one shard with a last committed optime less than afterClusterTime. The computed value
    // should still equal afterClusterTime.
    LogicalTime time1(Timestamp(1, 1));
    shardRegistry()->getShardNoReload(s0)->updateLastCommittedOpTime(time1);
    ASSERT_LT(time1, afterClusterTime);

    computedTime = at_cluster_time_util::computeAtClusterTime(
        operationContext(), true, {s0}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT(computedTime);
    ASSERT_EQ(*computedTime, afterClusterTime);
}

}  // namespace
}  // namespace mongo
