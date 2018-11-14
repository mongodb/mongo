
/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/logical_time.h"
#include "mongo/s/catalog_cache_test_fixture.h"
#include "mongo/s/client/shard_remote.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/session_catalog_router.h"
#include "mongo/s/shard_id.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/s/transaction_router.h"
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

const TxnNumber kTxnNumber = 3;

class AtClusterTimeTest : public ShardingTestFixture {
protected:
    void setUp() {
        ShardingTestFixture::setUp();
        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);
        std::vector<std::tuple<ShardId, HostAndPort>> shardInfos;
        shardInfos.push_back(std::make_tuple(shardOneId, shardOne));
        shardInfos.push_back(std::make_tuple(shardTwoId, shardTwo));

        ShardingTestFixture::addRemoteShards(shardInfos);

        // Set up a logical clock with an initial time.
        auto logicalClock = stdx::make_unique<LogicalClock>(getServiceContext());
        logicalClock->setClusterTimeFromTrustedSource(kInMemoryLogicalTime);
        LogicalClock::set(getServiceContext(), std::move(logicalClock));

        operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
        operationContext()->setTxnNumber(kTxnNumber);
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        _scopedSession.emplace(operationContext());
        txnRouter()->beginOrContinueTxn(operationContext(), kTxnNumber, true);
    }

    TransactionRouter* txnRouter() const {
        return TransactionRouter::get(operationContext());
    }

    LogicalTime getSelectedTime() {
        return txnRouter()->getAtClusterTime()->getTime();
    }

private:
    boost::optional<RouterOperationContextSession> _scopedSession;
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

    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, {shardOneId, shardTwoId}, kNss, kEmptyQuery, kEmptyCollation);
    // TODO SERVER-36312:
    // ASSERT_EQ(getSelectedTime(), timeTwo);
    ASSERT_EQ(getSelectedTime(), kInMemoryLogicalTime);
}

TEST_F(AtClusterTimeTest, ComputeValidInvalid) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    ASSERT_EQ(LogicalTime(), shardOne->getLastCommittedOpTime());

    auto shardTwo = shardRegistry()->getShardNoReload(shardTwoId);
    LogicalTime timeTwo(Timestamp(15, 1));
    shardTwo->updateLastCommittedOpTime(timeTwo);
    ASSERT_EQ(timeTwo, shardTwo->getLastCommittedOpTime());

    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, {shardOneId, shardTwoId}, kNss, kEmptyQuery, kEmptyCollation);
    // TODO SERVER-36312:
    // ASSERT_EQ(getSelectedTime(), timeTwo);
    ASSERT_EQ(getSelectedTime(), kInMemoryLogicalTime);
}

TEST_F(AtClusterTimeTest, ComputeInvalidInvalid) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    ASSERT_EQ(LogicalTime(), shardOne->getLastCommittedOpTime());

    auto shardTwo = shardRegistry()->getShardNoReload(shardTwoId);
    ASSERT_EQ(LogicalTime(), shardTwo->getLastCommittedOpTime());

    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, {shardOneId, shardTwoId}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT_EQ(getSelectedTime(), kInMemoryLogicalTime);
}

TEST_F(AtClusterTimeTest, ComputeForUnsharded) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);

    LogicalTime timeOne(Timestamp(10, 2));
    shardOne->updateLastCommittedOpTime(timeOne);
    ASSERT_EQ(timeOne, shardOne->getLastCommittedOpTime());

    txnRouter()->computeAndSetAtClusterTimeForUnsharded(operationContext(), shardOneId);
    // TODO SERVER-36312:
    // ASSERT_EQ(getSelectedTime(), timeOne);
    ASSERT_EQ(getSelectedTime(), kInMemoryLogicalTime);
}

TEST_F(AtClusterTimeTest, ComputeForUnshardedNoCachedOpTime) {
    auto shardOne = shardRegistry()->getShardNoReload(shardOneId);
    ASSERT_EQ(LogicalTime(), shardOne->getLastCommittedOpTime());

    txnRouter()->computeAndSetAtClusterTimeForUnsharded(operationContext(), shardOneId);
    ASSERT_EQ(getSelectedTime(), kInMemoryLogicalTime);
}

// TODO SERVER-36312: Verify computing atClusterTime for a single shard that does not exist returns
// an error.

class AtClusterTimeTargetingTest : public CatalogCacheTestFixture {
protected:
    void setUp() {
        CatalogCacheTestFixture::setUp();
        CatalogCacheTestFixture::setupNShards(2);

        // Set up a logical clock with an initial time.
        auto logicalClock = stdx::make_unique<LogicalClock>(getServiceContext());
        logicalClock->setClusterTimeFromTrustedSource(kInMemoryLogicalTime);
        LogicalClock::set(getServiceContext(), std::move(logicalClock));

        operationContext()->setLogicalSessionId(makeLogicalSessionIdForTest());
        operationContext()->setTxnNumber(kTxnNumber);
        repl::ReadConcernArgs::get(operationContext()) =
            repl::ReadConcernArgs(repl::ReadConcernLevel::kSnapshotReadConcern);

        _scopedSession.emplace(operationContext());
        txnRouter()->beginOrContinueTxn(operationContext(), kTxnNumber, true);
    }

    TransactionRouter* txnRouter() const {
        return TransactionRouter::get(operationContext());
    }

    LogicalTime getSelectedTime() {
        return txnRouter()->getAtClusterTime()->getTime();
    }

private:
    boost::optional<RouterOperationContextSession> _scopedSession;
};

// Verifies that the latest in-memory logical time is returned when lastCommittedOpTime on one
// targeted shard is not initialized.
TEST_F(AtClusterTimeTargetingTest, ReturnsLatestInMemoryTime) {
    auto routingInfo = loadRoutingTableWithTwoChunksAndTwoShards(kNss);
    auto query = BSON("find" << kNss.coll());
    auto collation = BSONObj();
    auto shards = getTargetedShardsForQuery(operationContext(), routingInfo, query, collation);

    LogicalTime time(Timestamp(2, 1));
    shardRegistry()->getShardNoReload(ShardId("0"))->updateLastCommittedOpTime(time);

    ASSERT_LT(time, kInMemoryLogicalTime);

    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, shards, kNss, query, collation);
    ASSERT_EQ(getSelectedTime(), kInMemoryLogicalTime);
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

    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, shards, kNss, query, collation);
    // TODO SERVER-36312:
    // ASSERT_EQ(getSelectedTime(), time2);
    ASSERT_EQ(getSelectedTime(), kInMemoryLogicalTime);
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
    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, {s0}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT_GTE(getSelectedTime(), afterClusterTime);

    txnRouter()->computeAndSetAtClusterTimeForUnsharded(operationContext(), s0);
    ASSERT_GTE(getSelectedTime(), afterClusterTime);

    // Target all shards.
    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, {s0, s1}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT_GTE(getSelectedTime(), afterClusterTime);

    // One shard has a last committed optime.

    LogicalTime time1(Timestamp(1, 1));
    shardRegistry()->getShardNoReload(s0)->updateLastCommittedOpTime(time1);
    ASSERT_LT(time1, afterClusterTime);

    // Target one shard.
    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, {s0}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT_GTE(getSelectedTime(), afterClusterTime);

    txnRouter()->computeAndSetAtClusterTimeForUnsharded(operationContext(), s0);
    ASSERT_GTE(getSelectedTime(), afterClusterTime);

    // Target all shards.
    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, {s0, s1}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT_GTE(getSelectedTime(), afterClusterTime);

    // Both shards have a last committed optime.

    LogicalTime time2(Timestamp(2, 1));
    shardRegistry()->getShardNoReload(s1)->updateLastCommittedOpTime(time2);
    ASSERT_LT(time2, afterClusterTime);

    // Target one shard.
    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, {s0}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT_GTE(getSelectedTime(), afterClusterTime);

    // Target all shards.
    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, {s0, s1}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT_GTE(getSelectedTime(), afterClusterTime);
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
    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, {s0}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT_EQ(getSelectedTime(), afterClusterTime);

    txnRouter()->computeAndSetAtClusterTimeForUnsharded(operationContext(), s0);
    ASSERT_EQ(getSelectedTime(), afterClusterTime);

    // Target one shard with a last committed optime less than afterClusterTime. The computed value
    // should still equal afterClusterTime.
    LogicalTime time1(Timestamp(1, 1));
    shardRegistry()->getShardNoReload(s0)->updateLastCommittedOpTime(time1);
    ASSERT_LT(time1, afterClusterTime);

    txnRouter()->computeAndSetAtClusterTime(
        operationContext(), true, {s0}, kNss, kEmptyQuery, kEmptyCollation);
    ASSERT_EQ(getSelectedTime(), afterClusterTime);

    txnRouter()->computeAndSetAtClusterTimeForUnsharded(operationContext(), s0);
    ASSERT_EQ(getSelectedTime(), afterClusterTime);
}

}  // namespace
}  // namespace mongo
